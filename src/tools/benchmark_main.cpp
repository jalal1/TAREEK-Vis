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
//                                          [--lixel M] [--repeat N]
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
#include <QRegularExpression>

#include <cstdio>
#include <map>
#include <memory>

#include "analysis/nkde_scatter.h"
#include "analysis/nkdv_network.h"
#include "core/logger.h"
#include "data/network_index.h"
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

// Uncompressed size of a gzip file, from the ISIZE field in its last 4 bytes.
//
// The compression claim the paper makes is against the XML the simulator
// produced, not against its gzipped form -- reporting the latter understates
// the index by an order of magnitude, because it credits gzip's work to us.
// ISIZE is modulo 2^32, so for a member over 4 GB it wraps; the caller is told
// (returns 0) rather than given a wrong number.
qint64 gzipUncompressedSize(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    if (f.size() < 18) return 0;                 // smaller than a valid member
    if (!f.seek(f.size() - 4)) return 0;
    const QByteArray tail = f.read(4);
    if (tail.size() != 4) return 0;
    const quint32 isize =
        (static_cast<quint8>(tail[0]))        |
        (static_cast<quint8>(tail[1]) << 8)   |
        (static_cast<quint8>(tail[2]) << 16)  |
        (static_cast<quint32>(static_cast<quint8>(tail[3])) << 24);
    // A file whose compressed size already exceeds ISIZE cannot be right: the
    // field has wrapped past 4 GB. Report nothing rather than a wrapped value.
    if (static_cast<qint64>(isize) < f.size()) return 0;
    return static_cast<qint64>(isize);
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
    for (size_t v = 0; v < vehicles->vehicleCount(); ++v)
        if (const auto* t = vehicles->trajectory(static_cast<uint32_t>(v)))
            segments += t->segments.size();

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

    const auto& bounds = network->bounds();
    emitRecord(QJsonObject{
        {"experiment", "stats"},
        {"scenario", QDir(scenarioDir).dirName()},
        {"nodes", qint64(network->nodeCount())},
        {"links", qint64(network->linkCount())},
        {"vehicles", qint64(vehicles->vehicleCount())},
        {"persons", qint64(vehicles->personCount())},
        {"segments", qint64(segments)},
        {"trips", qint64(trips)},
        {"activities", qint64(vehicles->activityCount())},
        {"activity_types", types},
        {"sim_start_s", vehicles->minTime() / 1000.0},
        {"sim_end_s", vehicles->maxTime() / 1000.0},
        {"extent_m_x", bounds.width()},
        {"extent_m_y", bounds.height()},
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
               double bandwidth, int lixelLength, int repeats) {
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
                {"points_placed", qint64(r.pointsPlaced)},
                {"points_skipped", qint64(r.pointsSkipped)},
                {"source_edges", qint64(r.sourceEdges)},
                {"considered_edges", qint64(r.consideredEdges)},
                {"total_edges", qint64(net.edgeCount())},
                {"lixels", qint64(r.lixels.size())},
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
    }
    return 0;
}

} // namespace

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
        "  density      Compute activity density maps, timed\n\n"
        "Results are printed to stdout as one JSON object per line.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "preprocess | stats | density");

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

    parser.addOptions({scenarioOpt, forceOpt, typeOpt, bandwidthOpt,
                       lixelOpt, repeatOpt});
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        note("No command given.\n");
        parser.showHelp(1);
    }

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
    if (command == "density")
        return cmdDensity(scenario, parser.value(typeOpt),
                          parser.value(bandwidthOpt).toDouble(),
                          parser.value(lixelOpt).toInt(),
                          std::max(1, parser.value(repeatOpt).toInt()));

    note("Unknown command: " + command);
    parser.showHelp(1);
}
