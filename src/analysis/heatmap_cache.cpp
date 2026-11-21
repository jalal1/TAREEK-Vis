// SPDX-License-Identifier: GPL-3.0-or-later

#include "heatmap_cache.h"

#include "core/logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <fstream>

namespace simvis {

namespace {

// "HMAP" — guards against reading a file that is not one of ours.
constexpr uint32_t kMagic = 0x484D4150;
// Raise this when the record layout or the meaning of a value changes, so old
// files are ignored rather than misread.
// v2: values are activities per metre. Before this they were divided by the
// point count, which made maps of different types incomparable.
constexpr uint32_t kVersion = 2;

// Must match HeatmapRenderer::kAnchorPercentile, so a scale read from the
// cache means the same as one computed from a map in memory.
constexpr double kAnchorPercentile = 0.99;

// The value a map's colors are anchored to: a high percentile of the positive
// densities, or 0 when the map is empty.
float anchorValueOf(const std::vector<NkdvNetwork::Lixel>& lixels) {
    std::vector<float> positive;
    positive.reserve(lixels.size());
    for (const auto& lixel : lixels)
        if (lixel.value > 0.0f) positive.push_back(lixel.value);
    if (positive.empty()) return 0.0f;

    const size_t index =
        static_cast<size_t>(static_cast<double>(positive.size()) * kAnchorPercentile);
    const size_t clamped = std::min(index, positive.size() - 1);
    std::nth_element(positive.begin(), positive.begin() + clamped, positive.end());
    return positive[clamped];
}

struct FileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t lixelCount;
    // Highest density in this map. Kept in the header so the shared color
    // scale can be found from the stored maps without reading all of them.
    // This slot was spare padding in earlier v2 files, where it is 0; a 0
    // reads as "unknown" and the caller falls back to loading the map.
    float peak;
};
static_assert(sizeof(FileHeader) == 16, "FileHeader must be 16 bytes");

} // namespace

QString HeatmapCache::Key::fileName() const {
    // An activity type is scenario-defined text, so it can hold characters a
    // file name cannot. Hash it rather than trying to escape it, and keep a
    // readable prefix so the directory is still browsable.
    const QByteArray hash = QCryptographicHash::hash(
        activityType.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);

    QString readable;
    for (QChar c : activityType) {
        if (c.isLetterOrNumber()) readable.append(c.toLower());
        if (readable.size() >= 12) break;
    }
    if (readable.isEmpty()) readable = "type";

    return QString("heatmap_%1_%2_l%3_b%4.bin")
        .arg(readable)
        .arg(QString::fromLatin1(hash))
        .arg(lixelLength)
        .arg(static_cast<long long>(bandwidth * 100));
}

QString HeatmapCache::pathFor(const Key& key) const {
    return cacheDir_ + "/" + key.fileName();
}

bool HeatmapCache::contains(const Key& key, const QDateTime& sourceModified) const {
    const QFileInfo info(pathFor(key));
    if (!info.exists() || info.size() < static_cast<qint64>(sizeof(FileHeader)))
        return false;
    // A cache entry written before the events cache describes older data.
    if (sourceModified.isValid() && info.lastModified() < sourceModified)
        return false;
    return true;
}

float HeatmapCache::peakOf(const Key& key,
                           const QDateTime& sourceModified) const {
    if (!contains(key, sourceModified)) return 0.0f;

    std::ifstream in(pathFor(key).toStdString(), std::ios::binary);
    if (!in) return 0.0f;

    FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good() || header.magic != kMagic || header.version != kVersion)
        return 0.0f;

    // Files written before the peak was stored carry 0 here. Fall back to
    // reading the map, so an older cache still gives the right scale.
    if (header.peak > 0.0f) return header.peak;

    std::vector<NkdvNetwork::Lixel> lixels;
    if (!load(key, sourceModified, lixels)) return 0.0f;
    return anchorValueOf(lixels);
}

float HeatmapCache::peakAcrossAll(const QStringList& activityTypes,
                                  int lixelLength, double bandwidth,
                                  const QDateTime& sourceModified) const {
    float peak = 0.0f;
    for (const QString& type : activityTypes) {
        const Key key{type, lixelLength, bandwidth};
        peak = std::max(peak, peakOf(key, sourceModified));
    }
    return peak;
}

bool HeatmapCache::load(const Key& key, const QDateTime& sourceModified,
                        std::vector<NkdvNetwork::Lixel>& out) const {
    out.clear();
    if (!contains(key, sourceModified)) return false;

    const QString path = pathFor(key);
    std::ifstream in(path.toStdString(), std::ios::binary);
    if (!in) return false;

    FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good() || header.magic != kMagic || header.version != kVersion)
        return false;

    // Guard before reserving, so a damaged count cannot request a huge block.
    constexpr uint32_t kMaxLixels = 200'000'000;
    if (header.lixelCount > kMaxLixels) return false;

    out.resize(header.lixelCount);
    if (header.lixelCount > 0) {
        const std::streamsize bytes =
            static_cast<std::streamsize>(header.lixelCount) *
            static_cast<std::streamsize>(sizeof(NkdvNetwork::Lixel));
        in.read(reinterpret_cast<char*>(out.data()), bytes);
        if (in.gcount() != bytes) {
            out.clear();
            return false;
        }
    }

    LOG_INFO(QString("HeatmapCache: loaded %1 lixels for %2")
        .arg(out.size()).arg(key.activityType));
    return true;
}

bool HeatmapCache::store(const Key& key,
                         const std::vector<NkdvNetwork::Lixel>& lixels) const {
    QDir dir(cacheDir_);
    if (!dir.exists() && !dir.mkpath(".")) {
        LOG_WARN(QString("HeatmapCache: cannot create %1").arg(cacheDir_));
        return false;
    }

    // Write to a temporary file and rename, so an interrupted write cannot
    // leave a half-written entry that later looks valid.
    const QString finalPath = pathFor(key);
    const QString tempPath = finalPath + ".tmp";

    {
        std::ofstream out(tempPath.toStdString(), std::ios::binary);
        if (!out) {
            LOG_WARN(QString("HeatmapCache: cannot write %1").arg(tempPath));
            return false;
        }

        FileHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        header.lixelCount = static_cast<uint32_t>(lixels.size());
        // Store the same high percentile the renderer anchors on, not the
        // maximum. A map's highest lixel can be many times the value of almost
        // every other, so a shared scale built from maxima is set by a handful
        // of extreme segments and leaves every map too dark.
        header.peak = anchorValueOf(lixels);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        if (!lixels.empty()) {
            out.write(reinterpret_cast<const char*>(lixels.data()),
                      static_cast<std::streamsize>(
                          lixels.size() * sizeof(NkdvNetwork::Lixel)));
        }
        out.flush();
        if (!out.good()) {
            LOG_WARN(QString("HeatmapCache: write error on %1").arg(tempPath));
            return false;
        }
    }

    QFile::remove(finalPath);  // rename fails on Windows if the target exists
    if (!QFile::rename(tempPath, finalPath)) {
        LOG_WARN(QString("HeatmapCache: cannot rename %1").arg(tempPath));
        QFile::remove(tempPath);
        return false;
    }

    LOG_INFO(QString("HeatmapCache: stored %1 lixels for %2 (%3 KB)")
        .arg(lixels.size()).arg(key.activityType)
        .arg(lixels.size() * sizeof(NkdvNetwork::Lixel) / 1024));
    return true;
}

} // namespace simvis
