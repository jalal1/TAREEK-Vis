#include "link_time_table.h"
#include "data/vehicle_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

namespace simvis {

namespace {

constexpr uint32_t kMagic = 0x4E49424C;   // "LBIN"
// v1: LinkBinEntry of 20 B (enters, travel, vehicle and queue time)
constexpr uint32_t kVersion = 1;

struct FileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t binSeconds;
    uint32_t binCount;
    uint32_t linkCount;
    uint32_t vidxVersion;
    uint64_t vidxBytes;
    int64_t vidxModifiedMs;
    uint64_t networkHash;
    uint64_t entryCount;
};
static_assert(sizeof(FileHeader) == 56, "FileHeader must be 56 bytes");

// The part of a segment after the enter bin, in one later bin
struct Spill {
    uint32_t link;
    uint32_t bin;
    float vehicleS;
    float queueS;
};

template <class F>
void parallelFor(int threads, size_t n, F f) {
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        const size_t a = n * static_cast<size_t>(t) / static_cast<size_t>(threads);
        const size_t b = n * static_cast<size_t>(t + 1) / static_cast<size_t>(threads);
        pool.emplace_back([=] { f(t, a, b); });
    }
    for (auto& th : pool) th.join();
}

// Overlap of [a, b) and [c, d), ms
inline uint32_t overlap(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t lo = std::max(a, c), hi = std::min(b, d);
    return hi > lo ? hi - lo : 0;
}

} // namespace

uint64_t LinkTimeTable::networkHash(const std::vector<LinkRecord>& links) {
    // FNV-1a over the fields that the table uses
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const uint64_t n = links.size();
    mix(&n, sizeof n);
    for (const LinkRecord& l : links) {
        mix(&l.length, sizeof l.length);
        mix(&l.freespeed, sizeof l.freespeed);
    }
    return h;
}

LinkTimeTable LinkTimeTable::build(const VehicleIndex& vehicles,
                                   const std::vector<LinkRecord>& links,
                                   uint32_t binSeconds, int threads) {
    const auto started = std::chrono::steady_clock::now();
    LinkTimeTable table;
    table.binSeconds_ = std::max<uint32_t>(1, binSeconds);
    table.linkCount_ = static_cast<uint32_t>(links.size());
    const uint32_t nLinks = table.linkCount_;
    const uint32_t binMs = table.binSeconds_ * 1000u;
    const uint32_t nBins = vehicles.maxTime() / binMs + 1;
    const int T = threads > 0 ? threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const size_t nVehicles = vehicles.vehicleCount();

    std::vector<uint32_t> fftMs(nLinks);
    for (uint32_t i = 0; i < nLinks; ++i)
        fftMs[i] = static_cast<uint32_t>(matsimFreeFlowSeconds(links[i]) * 1000.0f);

    auto enterBin = [&](const VehicleSegment& s) { return std::min(s.enterTime / binMs, nBins - 1); };
    auto lastBin = [&](const VehicleSegment& s) {
        return s.leaveTime > s.enterTime ? std::min((s.leaveTime - 1) / binMs, nBins - 1) : enterBin(s);
    };
    auto forSegments = [&](size_t v0, size_t v1, auto&& f) {
        for (size_t v = v0; v < v1; ++v) {
            const VehicleTrajectory* t = vehicles.trajectory(static_cast<VehicleId>(v));
            if (!t) continue;
            for (const VehicleSegment& s : t->segments)
                if (s.linkId < nLinks) f(s);
        }
    };

    // Pass 1: per thread, segments per enter bin and spill items per bin
    std::vector<std::vector<uint64_t>> enterHist(T, std::vector<uint64_t>(nBins, 0));
    std::vector<std::vector<uint64_t>> spillHist(T, std::vector<uint64_t>(nBins, 0));
    parallelFor(T, nVehicles, [&](int t, size_t v0, size_t v1) {
        auto& eh = enterHist[t];
        auto& sh = spillHist[t];
        forSegments(v0, v1, [&](const VehicleSegment& s) {
            const uint32_t b0 = enterBin(s), b1 = lastBin(s);
            ++eh[b0];
            for (uint32_t b = b0 + 1; b <= b1; ++b) ++sh[b];
        });
    });

    // Pass 2: the spill items, in bin order
    std::vector<uint64_t> spillStart(nBins + 1, 0);
    std::vector<std::vector<uint64_t>> spillCursor(T, std::vector<uint64_t>(nBins));
    for (uint32_t b = 0; b < nBins; ++b) {
        uint64_t c = spillStart[b];
        for (int t = 0; t < T; ++t) { spillCursor[t][b] = c; c += spillHist[t][b]; }
        spillStart[b + 1] = c;
    }
    std::vector<Spill> spills(spillStart[nBins]);
    parallelFor(T, nVehicles, [&](int t, size_t v0, size_t v1) {
        auto& cur = spillCursor[t];
        forSegments(v0, v1, [&](const VehicleSegment& s) {
            const uint32_t b0 = enterBin(s), b1 = lastBin(s);
            const uint32_t queueFrom = s.enterTime + fftMs[s.linkId];
            for (uint32_t b = b0 + 1; b <= b1; ++b) {
                const uint32_t bs = b * binMs, be = bs + binMs;
                spills[cur[b]++] = {s.linkId, b,
                                    overlap(s.enterTime, s.leaveTime, bs, be) / 1000.0f,
                                    overlap(queueFrom, s.leaveTime, bs, be) / 1000.0f};
            }
        });
    });

    std::vector<uint64_t> binTotal(nBins, 0);
    for (int t = 0; t < T; ++t)
        for (uint32_t b = 0; b < nBins; ++b) binTotal[b] += enterHist[t][b];

    // Groups of bins with about equal segment counts. The segments of one
    // group go into a buffer of pointers, bucketed by bin. 4 groups: the
    // buffer is about 1/4 of the segments (100 MB on Twin Cities), and each
    // group reads all segments one more time. With 8 groups the build took
    // 1.25-1.29 s, with 4 groups 0.99 s (docs/link-colors.md).
    constexpr int kRounds = 4;
    uint64_t allSegments = 0;
    for (uint64_t c : binTotal) allSegments += c;
    std::vector<uint32_t> groupStart{0};
    {
        uint64_t acc = 0;
        const uint64_t target = (allSegments + kRounds - 1) / kRounds;
        for (uint32_t b = 0; b < nBins; ++b) {
            acc += binTotal[b];
            if (acc >= target * groupStart.size() && groupStart.size() < static_cast<size_t>(kRounds))
                groupStart.push_back(b + 1);
        }
        groupStart.push_back(nBins);
    }

    table.bins_.resize(nBins);
    std::vector<const VehicleSegment*> bucket;
    for (size_t g = 0; g + 1 < groupStart.size(); ++g) {
        const uint32_t gb0 = groupStart[g], gb1 = groupStart[g + 1];
        if (gb0 >= gb1) continue;
        std::vector<uint64_t> binOff(gb1 - gb0 + 1, 0);
        for (uint32_t b = gb0; b < gb1; ++b) binOff[b - gb0 + 1] = binOff[b - gb0] + binTotal[b];
        bucket.assign(binOff.back(), nullptr);
        std::vector<std::vector<uint64_t>> cursor(T, std::vector<uint64_t>(gb1 - gb0));
        for (uint32_t b = gb0; b < gb1; ++b) {
            uint64_t c = binOff[b - gb0];
            for (int t = 0; t < T; ++t) { cursor[t][b - gb0] = c; c += enterHist[t][b]; }
        }
        // Pass 3: the segments of this group into their bin buckets
        parallelFor(T, nVehicles, [&](int t, size_t v0, size_t v1) {
            auto& cur = cursor[t];
            forSegments(v0, v1, [&](const VehicleSegment& s) {
                const uint32_t b = enterBin(s);
                if (b >= gb0 && b < gb1) bucket[cur[b - gb0]++] = &s;
            });
        });

        // Pass 4: per bin, add into a per-thread array over all links, then
        // write the touched links in link order
        std::atomic<uint32_t> nextBin{gb0};
        std::vector<std::thread> pool;
        for (int t = 0; t < T; ++t) {
            pool.emplace_back([&] {
                std::vector<LinkBinEntry> scratch(nLinks, LinkBinEntry{UINT32_MAX, 0, 0, 0, 0});
                std::vector<uint32_t> touched;
                for (uint32_t b; (b = nextBin++) < gb1;) {
                    touched.clear();
                    auto touch = [&](uint32_t l) -> LinkBinEntry& {
                        LinkBinEntry& e = scratch[l];
                        if (e.link == UINT32_MAX) { e.link = l; touched.push_back(l); }
                        return e;
                    };
                    const uint32_t bs = b * binMs, be = bs + binMs;
                    for (uint64_t k = binOff[b - gb0]; k < binOff[b - gb0 + 1]; ++k) {
                        const VehicleSegment& s = *bucket[k];
                        const uint32_t leave = std::max(s.leaveTime, s.enterTime);
                        LinkBinEntry& e = touch(s.linkId);
                        ++e.enters;
                        e.travelS += (leave - s.enterTime) / 1000.0f;
                        e.vehicleS += overlap(s.enterTime, leave, bs, be) / 1000.0f;
                        e.queueS += overlap(s.enterTime + fftMs[s.linkId], leave, bs, be) / 1000.0f;
                    }
                    for (uint64_t k = spillStart[b]; k < spillStart[b + 1]; ++k) {
                        LinkBinEntry& e = touch(spills[k].link);
                        e.vehicleS += spills[k].vehicleS;
                        e.queueS += spills[k].queueS;
                    }
                    std::sort(touched.begin(), touched.end());
                    auto& out = table.bins_[b];
                    out.reserve(touched.size());
                    for (uint32_t l : touched) {
                        out.push_back(scratch[l]);
                        scratch[l] = LinkBinEntry{UINT32_MAX, 0, 0, 0, 0};
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
    }

    table.buildSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return table;
}

uint32_t LinkTimeTable::binOf(double seconds) const {
    if (bins_.empty() || !(seconds > 0.0)) return 0;
    const double b = std::floor(seconds / binSeconds_);
    return static_cast<uint32_t>(std::min(b, static_cast<double>(bins_.size() - 1)));
}

const LinkBinEntry* LinkTimeTable::find(uint32_t link, uint32_t b) const {
    if (b >= bins_.size()) return nullptr;
    const auto& v = bins_[b];
    auto it = std::lower_bound(v.begin(), v.end(), link,
                               [](const LinkBinEntry& e, uint32_t l) { return e.link < l; });
    return it != v.end() && it->link == link ? &*it : nullptr;
}

std::vector<LinkSums> LinkTimeTable::linkSeries(uint32_t link) const {
    std::vector<LinkSums> series(bins_.size());
    for (uint32_t b = 0; b < bins_.size(); ++b)
        if (const LinkBinEntry* e = find(link, b)) series[b].add(*e);
    return series;
}

size_t LinkTimeTable::entryCount() const {
    size_t n = 0;
    for (const auto& b : bins_) n += b.size();
    return n;
}

size_t LinkTimeTable::memoryBytes() const {
    size_t n = sizeof(*this) + bins_.capacity() * sizeof(bins_[0]);
    for (const auto& b : bins_) n += b.capacity() * sizeof(LinkBinEntry);
    return n;
}

bool LinkTimeTable::save(const std::string& path, const Source& source) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        FileHeader h{kMagic, kVersion, binSeconds_, binCount(), linkCount_, source.vidxVersion,
                     source.vidxBytes, source.vidxModifiedMs, source.networkHash, entryCount()};
        out.write(reinterpret_cast<const char*>(&h), sizeof h);
        // Entries per bin, then the entries of all bins in bin order
        for (const auto& b : bins_) {
            const uint32_t n = static_cast<uint32_t>(b.size());
            out.write(reinterpret_cast<const char*>(&n), sizeof n);
        }
        for (const auto& b : bins_)
            out.write(reinterpret_cast<const char*>(b.data()),
                      static_cast<std::streamsize>(b.size() * sizeof(LinkBinEntry)));
        if (!out.good()) return false;
    }
    // Replace the old file only when the new one is complete
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool LinkTimeTable::load(const std::string& path, const Source& source) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    FileHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof h);
    if (!in || h.magic != kMagic || h.version != kVersion ||
        h.vidxBytes != source.vidxBytes || h.vidxModifiedMs != source.vidxModifiedMs ||
        h.vidxVersion != source.vidxVersion || h.networkHash != source.networkHash ||
        h.binSeconds == 0 || h.binCount == 0 || h.binCount > 1'000'000) {
        return false;
    }
    std::vector<uint32_t> counts(h.binCount);
    in.read(reinterpret_cast<char*>(counts.data()), counts.size() * sizeof(uint32_t));
    uint64_t total = 0;
    for (uint32_t c : counts) total += c;
    if (!in || total != h.entryCount) return false;
    std::vector<std::vector<LinkBinEntry>> bins(h.binCount);
    for (uint32_t b = 0; b < h.binCount; ++b) {
        bins[b].resize(counts[b]);
        in.read(reinterpret_cast<char*>(bins[b].data()),
                static_cast<std::streamsize>(counts[b] * sizeof(LinkBinEntry)));
    }
    if (!in) return false;
    binSeconds_ = h.binSeconds;
    linkCount_ = h.linkCount;
    bins_ = std::move(bins);
    buildSeconds_ = 0.0;
    return true;
}

} // namespace simvis
