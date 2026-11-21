// SPDX-License-Identifier: GPL-3.0-or-later

// Headless benchmark driver for TAREEK-Vis.
//
// The application proper is an interactive Qt/OpenGL desktop tool, which makes
// it awkward to measure on a compute node: it wants a display, it wants a GPU,
// and it does its work in response to menu clicks. This driver exposes the same
// pipeline as a batch program, so the measurements a paper needs can be taken
// on a headless machine and repeated exactly.
//
// It links QtCore only -- no Widgets, no OpenGL -- because every stage it
// exercises (parsing, indexing, density) already depends on nothing more. That
// is what makes this possible at all, and it is worth preserving: if this file
// ever needs a widget header, something has been put in the wrong layer.
//
//   tareek-bench preprocess --scenario DIR [--force]
//   tareek-bench stats      --scenario DIR
//   tareek-bench density    --scenario DIR [--type NAME] [--bandwidth M]
//                                          [--lixel M] [--repeat N] [--gather]
//
// Every subcommand prints one JSON object per line to stdout ("JSON Lines"),
// so results append cleanly across runs and parse without a schema. Progress
// and diagnostics go to stderr, so `tareek-bench ... > results.jsonl` is always
// valid.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

#include "analysis/nkde_gather.h"
#include "analysis/nkde_scatter.h"
#include "analysis/nkdv_network.h"
#include "analysis/link_metrics.h"
#include "analysis/link_time_table.h"
#include "renderer/link_palette.h"
#include <chrono>
#include <cmath>
#include <unordered_map>
#include "core/logger.h"
#include "data/network_index.h"
#include "core/modes.h"
#include "data/vehicle_index.h"
#include "parsers/preprocessor.h"

namespace {

// One JSON object per line. Flushed immediately so a long run's partial output
// survives a kill, which matters when a job hits a scheduler time limit.
void emitRecord(const QJsonObject& obj) {
    const QByteArray line =
        QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
}

void note(const QString& message) {
    fprintf(stderr, "%s\n", qUtf8Printable(message));
    fflush(stderr);
}

// Resident set size in kilobytes, or 0 where it cannot be read. Linux only,
// which is where the benchmarks run; on other platforms the field is simply
// absent rather than wrong.
//
// Read with readAll() rather than a readLine() loop: files under /proc report
// a size of 0, and QFile::atEnd() believes it, so a `while (!atEnd())` loop
// exits immediately and silently reports 0 for every scenario. readAll() keeps
// reading until the kernel returns nothing, which is what a procfs file needs.
qint64 residentKb() {
#ifdef Q_OS_LINUX
    QFile status("/proc/self/status");
    if (status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QList<QByteArray> lines = status.readAll().split('\n');
        for (const QByteArray& line : lines) {
            if (!line.startsWith("VmRSS:")) continue;
            return QString::fromLatin1(line)
                .remove(QRegularExpression("[^0-9]")).toLongLong();
        }
    }
#endif
    return 0;
}

// The scenario layout the desktop app discovers by globbing. Repeated here so
// the two agree on what "a scenario folder" means.
struct ScenarioFiles {
    QString network;
    QString events;
    QString transit;
    bool valid() const { return !network.isEmpty() && !events.isEmpty(); }
};

QString findOne(const QDir& dir, const QString& contains) {
    // Prefer an `output_` prefixed file when several match, matching the
    // desktop app's own preference so both open the same scenario.
    const QStringList names = dir.entryList(
        {"*.xml", "*.xml.gz"}, QDir::Files, QDir::Name);
    QString fallback;
    for (const QString& name : names) {
        if (!name.contains(contains, Qt::CaseInsensitive)) continue;
        if (name.startsWith("output_", Qt::CaseInsensitive))
            return dir.absoluteFilePath(name);
        if (fallback.isEmpty()) fallback = dir.absoluteFilePath(name);
    }
    return fallback;
}

ScenarioFiles discover(const QString& scenarioDir) {
    const QDir dir(scenarioDir);
    ScenarioFiles f;
    f.network = findOne(dir, "network");
    f.events  = findOne(dir, "events");
    f.transit = findOne(dir, "transitSchedule");
    return f;
}

QString cacheDirFor(const QString& scenarioDir) {
    return QDir(scenarioDir).absoluteFilePath(".tareek_cache");
}

qint64 fileSize(const QString& path) {
    return path.isEmpty() ? 0 : QFileInfo(path).size();
}

// Uncompressed size of a gzip file.
//
// The compression claim the paper makes is against the XML the simulator
// produced, not against its gzipped form -- reporting the latter understates
// the index by an order of magnitude, because it credits gzip's work to us.
//
// The cheap answer is the ISIZE field in the last four bytes, but it is stored
// modulo 2^32, and a metropolitan event log passes 4 GB easily (Chicago's is
// 20.7 GB, which ISIZE reports as 1.9 GB). So ISIZE is used only when it can be
// trusted, and otherwise the member is decompressed and counted. That costs
// seconds on a multi-gigabyte file and runs once per scenario, which is a fair
// price for a headline number in a paper being correct.
qint64 gzipUncompressedSize(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const qint64 onDisk = f.size();
    if (onDisk < 18) return 0;                   // smaller than a valid member

    qint64 isize = 0;
    if (f.seek(onDisk - 4)) {
        const QByteArray tail = f.read(4);
        if (tail.size() == 4) {
            isize = static_cast<qint64>(
                (static_cast<quint32>(static_cast<quint8>(tail[0])))       |
                (static_cast<quint32>(static_cast<quint8>(tail[1])) << 8)  |
                (static_cast<quint32>(static_cast<quint8>(tail[2])) << 16) |
                (static_cast<quint32>(static_cast<quint8>(tail[3])) << 24));
        }
    }
    f.close();

    // XML compresses by roughly 8-10x, so a member whose ISIZE is under about
    // 4x its compressed size has almost certainly wrapped. Below that bar,
    // count the bytes instead of trusting the field. The threshold only has to
    // separate "plausible" from "impossible"; the exact multiple is not
    // load-bearing, because a false positive merely costs a decompression pass.
    if (isize > onDisk * 4) return isize;

    gzFile gz = gzopen(QFile::encodeName(path).constData(), "rb");
    if (!gz) return 0;
    constexpr int kChunk = 1 << 20;              // 1 MB
    std::vector<char> buffer(kChunk);
    qint64 total = 0;
    int n = 0;
    while ((n = gzread(gz, buffer.data(), kChunk)) > 0) total += n;
    const bool ok = (n == 0);                    // 0 = clean EOF, -1 = error
    gzclose(gz);
    return ok ? total : 0;
}

// The size a file contributes to the "input" total: its uncompressed size when
// it is gzipped and that size is knowable, otherwise its size on disk.
qint64 logicalSize(const QString& path) {
    if (path.isEmpty()) return 0;
    if (path.endsWith(".gz", Qt::CaseInsensitive)) {
        const qint64 raw = gzipUncompressedSize(path);
        if (raw > 0) return raw;
    }
    return QFileInfo(path).size();
}

qint64 directorySize(const QString& path) {
    qint64 total = 0;
    for (const QFileInfo& fi : QDir(path).entryInfoList(QDir::Files))
        total += fi.size();
    return total;
}

// --- preprocess ---------------------------------------------------------------
// Times the XML-to-binary conversion, which is the "convert once" claim. With
// --force the cache is removed first, so the number is a cold conversion and
// not a cache hit.
int cmdPreprocess(const QString& scenarioDir, bool force) {
    const ScenarioFiles files = discover(scenarioDir);
    if (!files.valid()) {
        note("No network/events files found in " + scenarioDir);
        return 2;
    }

    const QString cacheDir = cacheDirFor(scenarioDir);
    if (force && QDir(cacheDir).exists()) {
        note("Removing cache: " + cacheDir);
        QDir(cacheDir).removeRecursively();
    }
    QDir().mkpath(cacheDir);
    simvis::Logger::redirectLogFile(cacheDir);

    simvis::Preprocessor pre;
    pre.setNetworkFile(files.network);
    pre.setEventsFile(files.events);
    if (!files.transit.isEmpty()) pre.setTransitScheduleFile(files.transit);
    pre.setOutputDirectory(cacheDir);

    // Two different measurements, and conflating them was the original bug in
    // this command: with a cache already present, calling processAll() anyway
    // re-parses the XML and reports a "warm" time indistinguishable from cold.
    //
    // Cold  = parse the XML and write the cache (the convert-once cost).
    // Warm  = read the cache back into the in-memory indices (what the user
    //         pays on every open after the first), which is what the desktop
    //         app does when it finds a valid cache.
    const bool wasCached = pre.hasCachedFiles();

    QElapsedTimer timer;
    timer.start();
    if (wasCached) {
        // Load the cache exactly as the application would, and touch the
        // indices so the timing covers the work, not a lazy open.
        simvis::NetworkIndex network;
        simvis::VehicleIndex vehicles;
        if (!network.loadFile(pre.networkBinaryPath()) ||
            !vehicles.loadFile(pre.vehicleIndexPath())) {
            note("Cache present but could not be read; re-run with --force.");
            return 2;
        }
    } else {
        // processAll() runs the whole conversion synchronously when called
        // directly, rather than through a worker thread as the GUI does.
        pre.processAll();
    }
    const qint64 elapsedMs = timer.elapsed();

    // Two different questions, so two different numbers. `input_bytes` is the
    // XML the simulator wrote, which is what the compression claim is about;
    // `input_bytes_gz` is what sits on disk, which is what the read cost is
    // about. Reporting only the second would understate the index by ~10x.
    const qint64 inputBytes =
        logicalSize(files.network) + logicalSize(files.events);
    const qint64 inputBytesGz = fileSize(files.network) + fileSize(files.events);
    const qint64 cacheBytes = directorySize(cacheDir);

    QJsonObject rec{
        {"experiment", "preprocess"},
        {"scenario", QDir(scenarioDir).dirName()},
        {"cold", !wasCached},
        {"measured", wasCached ? "cache_load" : "xml_conversion"},
        {"seconds", elapsedMs / 1000.0},
        {"input_bytes", inputBytes},
        {"input_bytes_gz", inputBytesGz},
        {"cache_bytes", cacheBytes},
        {"network_file", QFileInfo(files.network).fileName()},
        {"events_file", QFileInfo(files.events).fileName()},
        {"peak_rss_kb", residentKb()},
    };
    if (cacheBytes > 0) {
        rec["compression_ratio"] = double(inputBytes) / double(cacheBytes);
        rec["compression_ratio_gz"] = double(inputBytesGz) / double(cacheBytes);
    }
    emitRecord(rec);
    return 0;
}

// Loads the cached index, which every later command needs.
bool loadIndices(const QString& scenarioDir,
                 std::unique_ptr<simvis::NetworkIndex>& network,
                 std::unique_ptr<simvis::VehicleIndex>& vehicles) {
    const QString cacheDir = cacheDirFor(scenarioDir);
    simvis::Logger::redirectLogFile(cacheDir);

    simvis::Preprocessor pre;
    const ScenarioFiles files = discover(scenarioDir);
    pre.setNetworkFile(files.network);
    pre.setEventsFile(files.events);
    pre.setOutputDirectory(cacheDir);

    // A .bin of an older format, with a valid .vidx: make the .bin again
    // only, as the app does (Preprocessor::rebuildNetworkBinary)
    if (QFileInfo::exists(pre.networkBinaryPath()) &&
        simvis::NetworkParser::readBinaryHeader(pre.networkBinaryPath().toStdString()).magic !=
            simvis::NETWORK_FILE_MAGIC) {
        note("Network .bin is of an older format: rebuilding it from " + files.network);
        if (!simvis::Preprocessor::rebuildNetworkBinary(files.network, pre.networkBinaryPath()))
            return false;
    }

    network = std::make_unique<simvis::NetworkIndex>();
    if (!network->loadFile(pre.networkBinaryPath())) {
        note("Could not load " + pre.networkBinaryPath() +
             " -- run `preprocess` first.");
        return false;
    }
    vehicles = std::make_unique<simvis::VehicleIndex>();
    if (!vehicles->loadFile(pre.vehicleIndexPath())) {
        note("Could not load " + pre.vehicleIndexPath() +
             " -- run `preprocess` first.");
        return false;
    }
    return true;
}

// --- stats --------------------------------------------------------------------
// The scenario characteristics table: counts, extent, and the memory the
// indices occupy once resident.
int cmdStats(const QString& scenarioDir) {
    std::unique_ptr<simvis::NetworkIndex> network;
    std::unique_ptr<simvis::VehicleIndex> vehicles;
    if (!loadIndices(scenarioDir, network, vehicles)) return 2;

    size_t segments = 0;
    std::map<QString, qint64> vehiclesByMode, segmentsByMode;
    for (size_t v = 0; v < vehicles->vehicleCount(); ++v) {
        const auto* t = vehicles->trajectory(static_cast<uint32_t>(v));
        const QString mode = QString::fromUtf8(
            simvis::modes::get(vehicles->modeId(static_cast<uint32_t>(v))).key);
        ++vehiclesByMode[mode];
        if (t) {
            segments += t->segments.size();
            segmentsByMode[mode] += qint64(t->segments.size());
        }
    }
    QJsonObject vehicleModes, segmentModes;
    for (const auto& [mode, count] : vehiclesByMode) vehicleModes[mode] = count;
    for (const auto& [mode, count] : segmentsByMode) segmentModes[mode] = count;

    size_t trips = 0;
    for (size_t p = 0; p < vehicles->personCount(); ++p)
        if (const auto* t = vehicles->personTrips(static_cast<uint32_t>(p)))
            trips += t->size();

    std::map<QString, size_t> byType;
    for (const auto& act : vehicles->activities())
        ++byType[vehicles->actTypeString(act.actTypeId)];

    QJsonObject types;
    for (const auto& [name, count] : byType)
        types[name] = qint64(count);

    // Height range of the nodes and the air links (TAREEK-Air output)
    double zMin = 0.0, zMax = 0.0;
    if (!network->allNodes().empty()) {
        zMin = zMax = network->allNodes().front().z;
        for (const auto& node : network->allNodes()) {
            zMin = std::min<double>(zMin, node.z);
            zMax = std::max<double>(zMax, node.z);
        }
    }
    size_t airLinks = 0;
    for (const auto& link : network->allLinks())
        if (simvis::isAirRoadType(static_cast<simvis::RoadType>(link.roadType))) ++airLinks;

    // Air overlay events (TAREEK-Air): count by kind, and holds with a place
    qint64 losses = 0, groundHolds = 0, airHolds = 0, placedHolds = 0;
    for (const auto& ev : vehicles->overlayEvents()) {
        switch (static_cast<simvis::OverlayKind>(ev.kind)) {
            case simvis::OverlayKind::Loss:       ++losses; break;
            case simvis::OverlayKind::GroundHold: ++groundHolds; break;
            case simvis::OverlayKind::AirHold:    ++airHolds; break;
        }
        if (ev.flags & simvis::OVERLAY_FLAG_PLACED) ++placedHolds;
    }

    const auto& bounds = network->bounds();
    emitRecord(QJsonObject{
        {"experiment", "stats"},
        {"scenario", QDir(scenarioDir).dirName()},
        {"nodes", qint64(network->nodeCount())},
        {"links", qint64(network->linkCount())},
        {"vehicles", qint64(vehicles->vehicleCount())},
        {"persons", qint64(vehicles->personCount())},
        {"segments", qint64(segments)},
        {"vehicles_by_mode", vehicleModes},
        {"segments_by_mode", segmentModes},
        {"trips", qint64(trips)},
        {"activities", qint64(vehicles->activityCount())},
        {"activity_types", types},
        {"sim_start_s", vehicles->minTime() / 1000.0},
        {"sim_end_s", vehicles->maxTime() / 1000.0},
        {"extent_m_x", bounds.width()},
        {"extent_m_y", bounds.height()},
        {"z_min", zMin},
        {"z_max", zMax},
        {"air_links", qint64(airLinks)},
        {"losses", losses},
        {"ground_holds", groundHolds},
        {"air_holds", airHolds},
        {"holds_placed", placedHolds},
        {"overlay_bytes", qint64(vehicles->overlayEvents().size() * sizeof(simvis::OverlayEvent))},
        // Structure bytes, from the record sizes -- not allocator overhead, so
        // this will not sum to RSS. Both are reported so the gap is visible.
        {"segment_bytes", qint64(segments * sizeof(simvis::VehicleSegment))},
        {"trip_bytes", qint64(trips * sizeof(simvis::PersonTrip))},
        {"activity_bytes", qint64(vehicles->activityCount() * sizeof(simvis::ActivityRecord))},
        {"resident_kb", residentKb()},
    });
    return 0;
}

// --- density ------------------------------------------------------------------
// The measurement the paper's central claim rests on. Reports, per activity
// type, the counts that describe the workload -- how many edges carry a point
// out of how many exist -- alongside the runtime.
int cmdDensity(const QString& scenarioDir, const QString& onlyType,
               double bandwidth, int lixelLength, int repeats,
               bool alsoGather) {
    std::unique_ptr<simvis::NetworkIndex> network;
    std::unique_ptr<simvis::VehicleIndex> vehicles;
    if (!loadIndices(scenarioDir, network, vehicles)) return 2;

    simvis::NkdvNetwork net;
    std::string error;
    QElapsedTimer buildTimer;
    buildTimer.start();
    if (!net.build(*network, error)) {
        note("Cannot build the undirected graph: " + QString::fromStdString(error));
        return 2;
    }
    const double buildSeconds = buildTimer.elapsed() / 1000.0;

    // The graph conversion is itself a reportable number: it is what halves the
    // network before any density work begins.
    emitRecord(QJsonObject{
        {"experiment", "density_graph"},
        {"scenario", QDir(scenarioDir).dirName()},
        {"directed_links", qint64(network->linkCount())},
        {"undirected_edges", qint64(net.edgeCount())},
        {"nodes", qint64(net.nodeCount())},
        {"total_length_m", net.totalLength()},
        {"build_seconds", buildSeconds},
    });

    std::map<QString, std::vector<simvis::ActivityRecord>> byType;
    for (const auto& act : vehicles->activities()) {
        const QString type = vehicles->actTypeString(act.actTypeId);
        // A transit transfer is a routing artifact, not a real activity; the
        // GUI excludes it and so must this, or the numbers will not match.
        if (type.isEmpty() || type == "pt interaction") continue;
        if (!onlyType.isEmpty() && type != onlyType) continue;
        byType[type].push_back(act);
    }

    if (byType.empty()) {
        note("No activities to map" +
             (onlyType.isEmpty() ? QString() : " of type " + onlyType));
        return 2;
    }

    simvis::NkdeScatter::Params params;
    params.bandwidth = bandwidth;
    params.lixelLength = lixelLength;
    simvis::NkdeScatter::resetCancel();

    for (const auto& [type, points] : byType) {
        for (int run = 0; run < repeats; ++run) {
            note(QString("density: %1 (%2 activities), run %3/%4")
                 .arg(type).arg(points.size()).arg(run + 1).arg(repeats));

            const simvis::NkdeScatter::Result r =
                simvis::NkdeScatter::run(net, *network, points, params);

            if (!r.success) {
                note("  failed: " + r.errorMessage);
                continue;
            }

            QJsonObject rec{
                {"experiment", "density"},
                {"scenario", QDir(scenarioDir).dirName()},
                {"activity_type", type},
                {"run", run + 1},
                {"bandwidth_m", bandwidth},
                {"lixel_m", lixelLength},
                {"activities", qint64(points.size())},
                // Repeat visits by one person to one place count once; see
                // NkdeScatter::uniquePlaces().
                {"places", qint64(r.pointsInput - r.pointsMerged)},
                {"activities_merged", qint64(r.pointsMerged)},
                {"points_placed", qint64(r.pointsPlaced)},
                {"points_skipped", qint64(r.pointsSkipped)},
                {"source_edges", qint64(r.sourceEdges)},
                {"considered_edges", qint64(r.consideredEdges)},
                {"total_edges", qint64(net.edgeCount())},
                // Two distinct quantities, named apart because conflating them
                // understates the network by ~40 %: the sites the network
                // divides into, and the subset the kernel actually reached.
                {"lixels_total", qint64(r.lixelsTotal)},
                {"lixels_nonzero", qint64(r.lixels.size())},
                {"seconds", r.seconds},
                {"resident_kb", residentKb()},
            };
            // The ratio the paper's argument turns on, computed here rather
            // than downstream so it cannot be derived inconsistently.
            if (net.edgeCount() > 0) {
                rec["empty_edges"] = qint64(net.edgeCount() - r.sourceEdges);
                rec["empty_edge_pct"] =
                    100.0 * double(net.edgeCount() - r.sourceEdges) /
                    double(net.edgeCount());
            }
            emitRecord(rec);
        }

        if (!alsoGather) continue;

        // The baseline the paper's efficiency claim is measured against: the
        // same map computed in the standard lixel-driven direction. Run after
        // the scatter repeats so a failure here cannot cost the main numbers.
        simvis::NkdeGather::Params gatherParams;
        gatherParams.bandwidth = bandwidth;
        gatherParams.lixelLength = lixelLength;

        for (int run = 0; run < repeats; ++run) {
            note(QString("density (gather baseline): %1, run %2/%3")
                 .arg(type).arg(run + 1).arg(repeats));

            const simvis::NkdeGather::Result g =
                simvis::NkdeGather::run(net, *network, points, gatherParams);
            if (!g.success) {
                note("  failed: " + g.errorMessage);
                continue;
            }

            QJsonObject rec{
                {"experiment", "density_gather"},
                {"scenario", QDir(scenarioDir).dirName()},
                {"activity_type", type},
                {"run", run + 1},
                {"bandwidth_m", bandwidth},
                {"lixel_m", lixelLength},
                {"places", qint64(g.pointsInput - g.pointsMerged)},
                {"points_placed", qint64(g.pointsPlaced)},
                {"searched_edges", qint64(g.searchedEdges)},
                {"considered_edges", qint64(g.consideredEdges)},
                {"searches", qint64(g.searches)},
                {"total_edges", qint64(net.edgeCount())},
                {"lixels_total", qint64(g.lixelsTotal)},
                {"lixels_nonzero", qint64(g.lixels.size())},
                {"seconds", g.seconds},
                {"resident_kb", residentKb()},
            };

            // Confirm the two directions really do compute the same map. Only
            // the first run needs it; the maps do not vary between repeats.
            if (run == 0) {
                const simvis::NkdeScatter::Result s =
                    simvis::NkdeScatter::run(net, *network, points, params);
                if (s.success) {
                    const auto agree =
                        simvis::NkdeGather::verify(s.lixels, g.lixels);
                    rec["verify_comparable"] = agree.comparable;
                    rec["verify_lixels"] = qint64(agree.compared);
                    rec["verify_max_abs"] = agree.maxAbsolute;
                    rec["verify_max_rel"] = agree.maxRelative;
                    rec["scatter_seconds"] = s.seconds;
                    rec["scatter_searches"] = qint64(2 * s.sourceEdges);
                    note(QString("  agreement: %1 lixels, max rel %2")
                         .arg(agree.compared)
                         .arg(agree.maxRelative, 0, 'e', 2));
                }
            }
            emitRecord(rec);
        }
    }
    return 0;
}

} // namespace

// --- metrics ------------------------------------------------------------------
// The classes of the link color modes (analysis/link_metrics.h) as a Markdown
// table. help/link-colors.md holds this output, so the help page shows the
// same classes as the app. Do not edit those tables by hand: change
// src/analysis/link_metrics.cpp and run this command again. (No HTML comment
// in the help page: the help viewer shows comments as text.)
int cmdMetrics() {
    // Two tables: the network attributes, then the simulation results
    for (const bool results : {false, true}) {
        if (results) std::printf("\n");
        std::printf("| Color mode | Unit | What the value is | Classes | Gray, thin |\n");
        std::printf("|------------|------|-------------------|---------|------------|\n");
        for (const auto& m : simvis::linkmetrics::metrics()) {
            if (m.mode == simvis::linkmetrics::Mode::RoadType || m.timeDependent != results) continue;
            std::string classes;
            if (m.scale == simvis::linkmetrics::Scale::RobustMax) {
                classes = "5 equal classes up to the 98th percentile of the day";
            } else {
                for (const std::string& label : m.labels) {
                    if (!classes.empty()) classes += "; ";
                    classes += label;
                }
            }
            std::string what = m.description;
            if (m.sampleScaled) what += " Scaled to the full population.";
            std::printf("| %s | %s | %s | %s | %s |\n", m.name, m.unit[0] ? m.unit : "-",
                        what.c_str(), classes.c_str(), m.noDataLabel[0] ? m.noDataLabel : "-");
        }
    }

    // Road Type: the gray level and width of each road level, most important
    // first (renderer/link_palette.h)
    const auto& road = simvis::linkmetrics::metric(simvis::linkmetrics::Mode::RoadType);
    namespace lp = simvis::linkpalette;
    auto hex = [](float v) {
        const int c = static_cast<int>(std::lround(v * 255.0f));
        char buf[16];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c, c, c);
        return std::string(buf);
    };
    std::printf("\n| Road type | Width | Dark map, satellite | OSM, topo |\n");
    std::printf("|-----------|------:|---------------------|-----------|\n");
    for (size_t k = road.labels.size(); k-- > 0;) {
        std::printf("| %s | %.1f px | %s | %s |\n", road.labels[k].c_str(),
                    2.0 * lp::kRoadHalfWidthPx[k], hex(lp::kRoadGrayDark[k]).c_str(),
                    hex(lp::kRoadGrayLight[k]).c_str());
    }
    std::printf("| %s | %.1f px | thin, dim gray | thin, dim gray |\n", road.noDataLabel, 1.2);
    return 0;
}

// --- linkbins -------------------------------------------------------------------
// The per-link, per-bin table of item 9 (docs/link-colors.md): build time,
// size, cache save and load time, and the check against MATSim's linkstats
// file of the same iteration.

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// One row of a linkstats file: per hour 0..23, min and max of the volume and
// the travel time over the averaged iterations
struct LinkstatsRow {
    std::array<double, 24> volMin{}, volMax{}, ttMin{}, ttMax{};
};

bool readLinkstats(const QString& path, std::unordered_map<std::string, LinkstatsRow>& rows) {
    gzFile file = gzopen(path.toStdString().c_str(), "rb");
    if (!file) return false;
    std::string line;
    std::vector<char> buf(1 << 20);
    std::vector<int> volMinCol(24, -1), volMaxCol(24, -1), ttMinCol(24, -1), ttMaxCol(24, -1);
    bool header = true;
    while (gzgets(file, buf.data(), static_cast<int>(buf.size()))) {
        line.assign(buf.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        std::vector<std::string> cols;
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            cols.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (header) {
            for (size_t c = 0; c < cols.size(); ++c) {
                for (int h = 0; h < 24; ++h) {
                    const std::string hrs = "HRS" + std::to_string(h) + "-" + std::to_string(h + 1);
                    const std::string tt = "TRAVELTIME" + std::to_string(h) + "-" + std::to_string(h + 1);
                    if (cols[c] == hrs + "min") volMinCol[h] = static_cast<int>(c);
                    if (cols[c] == hrs + "max") volMaxCol[h] = static_cast<int>(c);
                    if (cols[c] == tt + "min") ttMinCol[h] = static_cast<int>(c);
                    if (cols[c] == tt + "max") ttMaxCol[h] = static_cast<int>(c);
                }
            }
            header = false;
            continue;
        }
        LinkstatsRow row;
        auto num = [&](int c) { return c >= 0 && c < static_cast<int>(cols.size()) ? std::atof(cols[c].c_str()) : 0.0; };
        for (int h = 0; h < 24; ++h) {
            row.volMin[h] = num(volMinCol[h]);
            row.volMax[h] = num(volMaxCol[h]);
            row.ttMin[h] = num(ttMinCol[h]);
            row.ttMax[h] = num(ttMaxCol[h]);
        }
        rows.emplace(cols[0], row);
    }
    gzclose(file);
    return true;
}

int cmdLinkbins(const QString& scenarioDir, int binSeconds, int repeat, int threads) {
    std::unique_ptr<simvis::NetworkIndex> network;
    std::unique_ptr<simvis::VehicleIndex> vehicles;
    if (!loadIndices(scenarioDir, network, vehicles)) return 2;
    const auto& links = network->allLinks();
    for (size_t i = 0; i < links.size(); ++i) {
        if (links[i].id != i) {
            note(QString("Link %1 has id %2: the table needs id == index").arg(i).arg(links[i].id));
            return 2;
        }
    }

    // Build
    QJsonArray buildTimes;
    simvis::LinkTimeTable table;
    for (int r = 0; r < std::max(1, repeat); ++r) {
        table = simvis::LinkTimeTable::build(*vehicles, links, static_cast<uint32_t>(binSeconds), threads);
        buildTimes.append(table.buildSeconds());
        note(QString("build %1: %2 s").arg(r + 1).arg(table.buildSeconds(), 0, 'f', 3));
    }

    // Save and load
    const ScenarioFiles files = discover(scenarioDir);
    simvis::Preprocessor pre;
    pre.setEventsFile(files.events);
    pre.setNetworkFile(files.network);
    pre.setOutputDirectory(cacheDirFor(scenarioDir));
    const QString vidx = pre.vehicleIndexPath();
    simvis::LinkTimeTable::Source source;
    source.vidxBytes = static_cast<uint64_t>(QFileInfo(vidx).size());
    source.vidxModifiedMs = QFileInfo(vidx).lastModified().toMSecsSinceEpoch();
    source.vidxVersion = simvis::VehicleIndex::fileVersion(vidx);
    source.networkHash = simvis::LinkTimeTable::networkHash(links);
    const QString cachePath = QDir(cacheDirFor(scenarioDir)).absoluteFilePath(
        QString("bench_%1.linkbins").arg(binSeconds));
    auto t0 = std::chrono::steady_clock::now();
    const bool saved = table.save(cachePath.toStdString(), source);
    const double saveS = secondsSince(t0);
    simvis::LinkTimeTable loaded;
    t0 = std::chrono::steady_clock::now();
    const bool loadedOk = loaded.load(cachePath.toStdString(), source);
    const double loadS = secondsSince(t0);
    const bool same = loadedOk && loaded.entryCount() == table.entryCount() &&
                      loaded.binCount() == table.binCount();
    simvis::LinkTimeTable::Source other = source;
    other.vidxModifiedMs += 1;
    simvis::LinkTimeTable rejected;
    const bool rejectsOtherVidx = !rejected.load(cachePath.toStdString(), other);
    const qint64 fileBytes = QFileInfo(cachePath).size();
    QFile::remove(cachePath);

    // Totals: every segment enters one time
    uint64_t enters = 0;
    double vehicleH = 0.0, queueH = 0.0;
    for (uint32_t b = 0; b < table.binCount(); ++b)
        for (const auto& e : table.bin(b)) {
            enters += e.enters;
            vehicleH += e.vehicleS / 3600.0;
            queueH += e.queueS / 3600.0;
        }
    uint64_t segments = 0;
    for (size_t v = 0; v < vehicles->vehicleCount(); ++v)
        if (const auto* t = vehicles->trajectory(static_cast<uint32_t>(v))) segments += t->segments.size();

    QJsonObject record{
        {"experiment", "linkbins"},
        {"scenario", QFileInfo(scenarioDir).fileName()},
        {"bin_s", binSeconds},
        {"threads", threads},
        {"bins", static_cast<qint64>(table.binCount())},
        {"entries", static_cast<qint64>(table.entryCount())},
        {"table_mb", static_cast<double>(table.memoryBytes()) / 1048576.0},
        {"build_s", buildTimes},
        {"file_mb", static_cast<double>(fileBytes) / 1048576.0},
        {"save_s", saveS},
        {"load_s", loadS},
        {"save_load_same", same && saved},
        {"rejects_other_vidx", rejectsOtherVidx},
        {"sum_enters", static_cast<qint64>(enters)},
        {"segments", static_cast<qint64>(segments)},
        {"vehicle_hours", vehicleH},
        {"queue_hours", queueH},
    };

    // Linkstats check: the newest *.linkstats.txt.gz in the scenario folder
    const QStringList statsFiles = QDir(scenarioDir).entryList({"*.linkstats.txt.gz"}, QDir::Files, QDir::Name);
    if (!statsFiles.isEmpty() && binSeconds > 0 && 3600 % binSeconds == 0) {
        const QString statsPath = QDir(scenarioDir).absoluteFilePath(statsFiles.last());
        std::unordered_map<std::string, LinkstatsRow> rows;
        if (readLinkstats(statsPath, rows)) {
            // Volumes by leave hour, from the raw segments, to see which event
            // the linkstats count. Hours after 24 go into hour 23, as MATSim's
            // VolumesAnalyzer puts every time after its end into its last bin.
            std::vector<uint32_t> leaveHour(links.size() * 24, 0);
            // The same with only the real link events: a segment that starts
            // a trip has no "entered link" event, and one that ends a trip
            // has no "left link" event. A segment starts a trip when the one
            // before it (same vehicle) did not end at its enter time.
            std::vector<uint32_t> leaveHourEvents(links.size() * 24, 0);
            std::vector<uint32_t> enterHourEvents(links.size() * 24, 0);
            std::vector<double> ttSumEvents(links.size() * 24, 0.0);
            std::vector<uint32_t> ttCountEvents(links.size() * 24, 0);
            // MATSim's linkstats travel time of hour h is the travel time of
            // the travel time bin (900 s) that holds h:00
            std::vector<double> ttSumQuarter(links.size() * 24, 0.0);
            std::vector<uint32_t> ttCountQuarter(links.size() * 24, 0);
            uint64_t tripStarts = 0, tripEnds = 0;
            for (size_t v = 0; v < vehicles->vehicleCount(); ++v) {
                const auto* t = vehicles->trajectory(static_cast<uint32_t>(v));
                if (!t) continue;
                const auto& segs = t->segments;
                for (size_t k = 0; k < segs.size(); ++k) {
                    const auto& s = segs[k];
                    if (s.linkId >= links.size()) continue;
                    const uint32_t h = std::min<uint32_t>(s.leaveTime / 3600000u, 23u);
                    ++leaveHour[static_cast<size_t>(s.linkId) * 24 + h];
                    const bool start = k == 0 || segs[k - 1].leaveTime != s.enterTime;
                    const bool end = k + 1 == segs.size() || segs[k + 1].enterTime != s.leaveTime;
                    tripStarts += start;
                    tripEnds += end;
                    if (!end) ++leaveHourEvents[static_cast<size_t>(s.linkId) * 24 + h];
                    if (!start) {
                        const uint32_t he = std::min<uint32_t>(s.enterTime / 3600000u, 23u);
                        ++enterHourEvents[static_cast<size_t>(s.linkId) * 24 + he];
                        if (!end) {
                            ttSumEvents[static_cast<size_t>(s.linkId) * 24 + he] +=
                                (s.leaveTime - s.enterTime) / 1000.0;
                            ++ttCountEvents[static_cast<size_t>(s.linkId) * 24 + he];
                            if (s.enterTime % 3600000u < 900000u && s.enterTime < 24u * 3600000u) {
                                ttSumQuarter[static_cast<size_t>(s.linkId) * 24 + he] +=
                                    (s.leaveTime - s.enterTime) / 1000.0;
                                ++ttCountQuarter[static_cast<size_t>(s.linkId) * 24 + he];
                            }
                        }
                    }
                }
            }
            uint64_t leaveEvMatch = 0, enterEvMatch = 0, ttEvPairs = 0, ttEvIn = 0;
            uint64_t overMax = 0, underMin = 0;
            uint64_t ttQPairs = 0, ttQIn = 0;
            double ttQAbs = 0.0;
            double ttEvAbs = 0.0, ttEvMax = 0.0;
            const int binsPerHour = 3600 / binSeconds;
            // Pairs (link, hour) with traffic in the linkstats or in the table
            uint64_t pairs = 0, enterMatch = 0, enterIn = 0, leaveMatch = 0, leaveIn = 0;
            uint64_t pairs23 = 0, leave23Match = 0, enter23Match = 0;
            double enterAbsDiff = 0.0, leaveAbsDiff = 0.0;
            uint64_t ttPairs = 0, ttIn = 0;
            double ttAbsDiff = 0.0, ttMaxDiff = 0.0;
            uint64_t linksMissing = 0;
            const auto& ids = network->linkIds();
            for (const auto& [id, row] : rows) {
                const uint32_t link = network->findLinkByOriginalId(id);
                if (link == UINT32_MAX) { ++linksMissing; continue; }
                for (int h = 0; h < 24; ++h) {
                    simvis::LinkSums sums;
                    for (int k = 0; k < binsPerHour; ++k) {
                        if (const auto* e = table.find(link, static_cast<uint32_t>(h * binsPerHour + k)))
                            sums.add(*e);
                    }
                    // Hour 23 of the enter count: everything from 23:00 on
                    if (h == 23) {
                        for (uint32_t b = 24 * binsPerHour; b < table.binCount(); ++b)
                            if (const auto* e = table.find(link, b)) sums.add(*e);
                    }
                    const double enterVol = sums.enters;
                    const double leaveVol = leaveHour[static_cast<size_t>(link) * 24 + h];
                    if (row.volMax[h] == 0 && enterVol == 0 && leaveVol == 0) continue;
                    ++pairs;
                    auto near = [&](double v) {
                        return std::min(std::abs(v - row.volMin[h]), std::abs(v - row.volMax[h]));
                    };
                    const bool em = near(enterVol) < 0.5, lm = near(leaveVol) < 0.5;
                    enterMatch += em;
                    leaveMatch += lm;
                    enterIn += enterVol >= row.volMin[h] - 0.5 && enterVol <= row.volMax[h] + 0.5;
                    leaveIn += leaveVol >= row.volMin[h] - 0.5 && leaveVol <= row.volMax[h] + 0.5;
                    enterAbsDiff += near(enterVol);
                    leaveAbsDiff += near(leaveVol);
                    if (h == 23) { ++pairs23; leave23Match += lm; enter23Match += em; }
                    const size_t ix = static_cast<size_t>(link) * 24 + h;
                    if (near(leaveHourEvents[ix]) >= 0.5) {
                        if (leaveHourEvents[ix] > row.volMax[h]) ++overMax;
                        if (leaveHourEvents[ix] < row.volMin[h]) ++underMin;
                    }
                    leaveEvMatch += near(leaveHourEvents[ix]) < 0.5;
                    enterEvMatch += near(enterHourEvents[ix]) < 0.5;
                    if (h < 23 && ttCountQuarter[ix] > 0) {
                        const double tt = ttSumQuarter[ix] / ttCountQuarter[ix];
                        const double d = tt < row.ttMin[h] ? row.ttMin[h] - tt
                                       : tt > row.ttMax[h] ? tt - row.ttMax[h] : 0.0;
                        ++ttQPairs;
                        ttQIn += d < 0.01;
                        ttQAbs += d;
                    }
                    if (h < 23 && ttCountEvents[ix] > 0) {
                        const double tt = ttSumEvents[ix] / ttCountEvents[ix];
                        const double d = tt < row.ttMin[h] ? row.ttMin[h] - tt
                                       : tt > row.ttMax[h] ? tt - row.ttMax[h] : 0.0;
                        ++ttEvPairs;
                        ttEvIn += d < 0.01;
                        ttEvAbs += d;
                        ttEvMax = std::max(ttEvMax, d);
                    }

                    // Mean travel time of the vehicles that entered in the hour
                    // (TravelTimeCalculator: by enter time). Hours 0..22 only.
                    if (h < 23 && sums.enters > 0) {
                        const double tt = sums.travelS / sums.enters;
                        const double d = tt < row.ttMin[h] ? row.ttMin[h] - tt
                                       : tt > row.ttMax[h] ? tt - row.ttMax[h] : 0.0;
                        ++ttPairs;
                        ttIn += d < 0.01;
                        ttAbsDiff += d;
                        ttMaxDiff = std::max(ttMaxDiff, d);
                    }
                }
            }
            (void)ids;
            record["linkstats_file"] = statsFiles.last();
            record["linkstats_links"] = static_cast<qint64>(rows.size());
            record["linkstats_links_missing"] = static_cast<qint64>(linksMissing);
            record["volume_pairs"] = static_cast<qint64>(pairs);
            record["volume_enter_equals_min_or_max"] = static_cast<double>(enterMatch) / pairs;
            record["volume_enter_inside_min_max"] = static_cast<double>(enterIn) / pairs;
            record["volume_enter_mean_abs_diff"] = enterAbsDiff / pairs;
            record["volume_leave_equals_min_or_max"] = static_cast<double>(leaveMatch) / pairs;
            record["volume_leave_inside_min_max"] = static_cast<double>(leaveIn) / pairs;
            record["volume_leave_mean_abs_diff"] = leaveAbsDiff / pairs;
            record["hour23_pairs"] = static_cast<qint64>(pairs23);
            record["hour23_enter_match"] = pairs23 ? static_cast<double>(enter23Match) / pairs23 : 0.0;
            record["hour23_leave_match"] = pairs23 ? static_cast<double>(leave23Match) / pairs23 : 0.0;
            record["traveltime_pairs"] = static_cast<qint64>(ttPairs);
            record["traveltime_inside_min_max"] = ttPairs ? static_cast<double>(ttIn) / ttPairs : 0.0;
            record["traveltime_mean_abs_diff_s"] = ttPairs ? ttAbsDiff / ttPairs : 0.0;
            record["traveltime_max_diff_s"] = ttMaxDiff;
            record["trip_start_segments"] = static_cast<qint64>(tripStarts);
            record["trip_end_segments"] = static_cast<qint64>(tripEnds);
            record["events_only_leave_equals_min_or_max"] = static_cast<double>(leaveEvMatch) / pairs;
            record["events_only_enter_equals_min_or_max"] = static_cast<double>(enterEvMatch) / pairs;
            record["events_only_traveltime_pairs"] = static_cast<qint64>(ttEvPairs);
            record["events_only_traveltime_inside"] = ttEvPairs ? static_cast<double>(ttEvIn) / ttEvPairs : 0.0;
            record["events_only_traveltime_mean_abs_diff_s"] = ttEvPairs ? ttEvAbs / ttEvPairs : 0.0;
            record["events_only_traveltime_max_diff_s"] = ttEvMax;
            record["events_only_leave_over_max"] = static_cast<qint64>(overMax);
            record["events_only_leave_under_min"] = static_cast<qint64>(underMin);
            record["first_quarter_traveltime_pairs"] = static_cast<qint64>(ttQPairs);
            record["first_quarter_traveltime_inside"] = ttQPairs ? static_cast<double>(ttQIn) / ttQPairs : 0.0;
            record["first_quarter_traveltime_mean_abs_diff_s"] = ttQPairs ? ttQAbs / ttQPairs : 0.0;
        }
    }
    emitRecord(record);
    return 0;
}

int main(int argc, char* argv[]) {
    // QCoreApplication, not QApplication: no display is required or requested.
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("tareek-bench");
    QCoreApplication::setApplicationVersion(TAREEK_VIS_VERSION);

    simvis::Logger::init();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Headless benchmark driver for TAREEK-Vis.\n\n"
        "Commands:\n"
        "  preprocess   Convert scenario XML to the binary cache, timed\n"
        "  stats        Report scenario size and index memory\n"
        "  density      Compute activity density maps, timed\n"
        "  metrics      Print the link color classes as a Markdown table\n"
        "  linkbins     Build the per-link time table, timed; check it against\n"
        "               the linkstats file of the run\n"
        "               (for help/link-colors.md; needs no scenario)\n\n"
        "Results are printed to stdout as one JSON object per line.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "preprocess | stats | density | metrics | linkbins");

    const QCommandLineOption scenarioOpt(
        {"s", "scenario"}, "Scenario directory holding the MATSim XML.", "dir");
    const QCommandLineOption forceOpt(
        "force", "Delete the cache first, so preprocessing is timed cold.");
    const QCommandLineOption typeOpt(
        "type", "Only this activity type (default: every type).", "name");
    const QCommandLineOption bandwidthOpt(
        "bandwidth", "Kernel bandwidth in meters (default 500).", "m", "500");
    const QCommandLineOption lixelOpt(
        "lixel", "Lixel length in meters (default 25).", "m", "25");
    const QCommandLineOption repeatOpt(
        "repeat", "Repeat each density run N times (default 1).", "n", "1");
    const QCommandLineOption gatherOpt(
        "gather", "Also run the standard lixel-driven baseline and compare. "
                  "Much slower: expect minutes per activity type.");

    const QCommandLineOption binOpt(
        "bin", "Bin length of the link table in seconds (default 900).", "s", "900");
    const QCommandLineOption threadsOpt(
        "threads", "Threads for the link table (default: all).", "n", "0");

    parser.addOptions({scenarioOpt, forceOpt, typeOpt, bandwidthOpt,
                       lixelOpt, repeatOpt, gatherOpt, binOpt, threadsOpt});
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        note("No command given.\n");
        parser.showHelp(1);
    }

    if (args.first() == "metrics") return cmdMetrics();

    const QString scenario = parser.value(scenarioOpt);
    if (scenario.isEmpty()) {
        note("--scenario is required.");
        return 2;
    }
    if (!QFileInfo(scenario).isDir()) {
        note("Not a directory: " + scenario);
        return 2;
    }

    const QString command = args.first();
    if (command == "preprocess")
        return cmdPreprocess(scenario, parser.isSet(forceOpt));
    if (command == "stats")
        return cmdStats(scenario);
    if (command == "linkbins")
        return cmdLinkbins(scenario, parser.value(binOpt).toInt(),
                           std::max(1, parser.value(repeatOpt).toInt()),
                           parser.value(threadsOpt).toInt());
    if (command == "density")
        return cmdDensity(scenario, parser.value(typeOpt),
                          parser.value(bandwidthOpt).toDouble(),
                          parser.value(lixelOpt).toInt(),
                          std::max(1, parser.value(repeatOpt).toInt()),
                          parser.isSet(gatherOpt));

    note("Unknown command: " + command);
    parser.showHelp(1);
}
