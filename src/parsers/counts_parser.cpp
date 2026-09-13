#include "counts_parser.h"
#include "core/logger.h"
#include <fstream>
#include <cstring>
#include <charconv>
#include <string_view>
#include <zlib.h>

namespace simvis {

namespace {

// Simple line reader that handles both plain and gzipped files
class FileReader {
public:
    static constexpr size_t BUFFER_SIZE = 65536;

    explicit FileReader(const std::string& path) {
        // Try gzopen (works for both plain and gzipped)
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) {
            throw std::runtime_error("Failed to open file: " + path);
        }
        gzbuffer(file_, 131072);
    }

    ~FileReader() {
        if (file_) gzclose(file_);
    }

    std::string_view readLineView() {
        if (gzgets(file_, buffer_, BUFFER_SIZE)) {
            size_t len = strlen(buffer_);
            return {buffer_, len};
        }
        return {};
    }

    bool eof() const { return gzeof(file_); }

private:
    gzFile file_ = nullptr;
    char buffer_[BUFFER_SIZE];
};

inline std::string_view getAttrView(const char* line, size_t lineLen,
                                     const char* name, size_t nameLen) {
    const char* pos = line;
    const char* end = line + lineLen;

    while (pos < end) {
        pos = static_cast<const char*>(memchr(pos, name[0], end - pos));
        if (!pos) return {};

        if (pos + nameLen + 2 <= end &&
            memcmp(pos, name, nameLen) == 0 &&
            pos[nameLen] == '=' && pos[nameLen + 1] == '"') {

            const char* valueStart = pos + nameLen + 2;
            const char* valueEnd = static_cast<const char*>(
                memchr(valueStart, '"', end - valueStart));
            if (valueEnd) {
                return {valueStart, static_cast<size_t>(valueEnd - valueStart)};
            }
            return {};
        }
        pos++;
    }
    return {};
}

inline uint32_t fastParseUint(std::string_view sv) {
    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

} // anonymous namespace

std::unique_ptr<CountsData> CountsParser::parse(
    const std::string& xmlPath,
    const NetworkIndex& network)
{
    auto result = std::make_unique<CountsData>();

    try {
        FileReader reader(xmlPath);

        // Current count being parsed
        CountData current{};
        bool inCount = false;
        size_t countsParsed = 0;
        size_t countsResolved = 0;

        const auto& linkIds = network.linkIds();

        while (!reader.eof()) {
            std::string_view line = reader.readLineView();
            if (line.empty()) continue;

            const char* data = line.data();
            size_t len = line.size();

            if (line.find("<count ") != std::string_view::npos) {
                // Start a new count location
                current = CountData{};
                current.groundTruthVolumes.fill(0);
                current.simulatedVolumes.fill(0);
                current.isPrimary = true;
                current.pairedLinkId = 0;

                std::string_view locId = getAttrView(data, len, "loc_id", 6);
                std::string_view csId = getAttrView(data, len, "cs_id", 5);

                if (!locId.empty()) {
                    current.stationId = std::string(csId);

                    // Resolve link ID via StringInterner
                    if (linkIds.hasString(locId)) {
                        current.linkId = linkIds.getId(locId);
                        inCount = true;
                    } else {
                        // Link not in network - skip this count
                        inCount = false;
                        LOG_WARN(QString("Counts: link '%1' not found in network, skipping")
                            .arg(QString::fromUtf8(locId.data(), locId.size())));
                    }
                }
                countsParsed++;
            }
            else if (inCount && line.find("<volume ") != std::string_view::npos) {
                std::string_view hStr = getAttrView(data, len, "h", 1);
                std::string_view valStr = getAttrView(data, len, "val", 3);

                if (!hStr.empty() && !valStr.empty()) {
                    uint32_t h = fastParseUint(hStr);
                    uint32_t val = fastParseUint(valStr);
                    // h=1..24 -> index 0..23
                    if (h >= 1 && h <= 24) {
                        current.groundTruthVolumes[h - 1] = val;
                    }
                }
            }
            else if (inCount && (line.find("</count>") != std::string_view::npos ||
                                  line.find("<count ") != std::string_view::npos)) {
                // Finalize current count (the self-closing case is handled by the
                // next <count> or </count> tag)
            }

            // Detect end of count block: </count> or next <count> or />
            if (inCount && (line.find("</count>") != std::string_view::npos)) {
                result->counts.push_back(current);
                result->linkToCount[current.linkId] = result->counts.size() - 1;
                countsResolved++;
                inCount = false;
            }
            // Handle self-closing <count ... /> with volumes as child elements
            // The typical format has </count>, so this handles the main case
        }

        // If we were still in a count at EOF (unlikely but safe)
        if (inCount && current.linkId != 0) {
            result->counts.push_back(current);
            result->linkToCount[current.linkId] = result->counts.size() - 1;
            countsResolved++;
        }

        LOG_INFO(QString("Counts parsed: %1 locations (%2 resolved to network links)")
            .arg(countsParsed).arg(countsResolved));

        // Post-processing
        cacheGeometry(*result, network);
        detectPairedLinks(*result, network);
        buildLinkIdLookup(*result);

    } catch (const std::exception& e) {
        LOG_ERROR(QString("Counts parse error: %1").arg(e.what()));
        return nullptr;
    }

    return result;
}

void CountsParser::computeSimulatedVolumes(
    CountsData& counts,
    const VehicleIndex& vehicleIndex)
{
    // Reset simulated volumes
    for (auto& c : counts.counts) {
        c.simulatedVolumes.fill(0);
    }

    if (counts.linkIdLookup.empty()) return;

    const size_t lookupSize = counts.linkIdLookup.size();
    const int32_t* lookup = counts.linkIdLookup.data();

    size_t vehicleCount = vehicleIndex.vehicleCount();
    size_t segmentsScanned = 0;

    for (size_t v = 0; v < vehicleCount; ++v) {
        const VehicleTrajectory* traj = vehicleIndex.trajectory(static_cast<VehicleId>(v));
        if (!traj || traj->segments.empty()) continue;

        for (const auto& seg : traj->segments) {
            segmentsScanned++;

            // Fast flat-array lookup
            if (seg.linkId >= lookupSize) continue;
            int32_t countIdx = lookup[seg.linkId];
            if (countIdx < 0) continue;

            // Determine hour from leave time (ms -> seconds -> hour)
            // MATSim's VolumesAnalyzer uses LinkLeaveEvent for counting.
            // Runs routinely pass 24:00, so fold back to hour of day: clamping
            // to 23 piled every post-midnight trip onto the last hour and
            // overstated it against the ground truth.
            uint32_t hour = (seg.leaveTime / 3600000u) % 24u;

            counts.counts[countIdx].simulatedVolumes[hour]++;
        }
    }

    LOG_INFO(QString("Simulated volumes computed: scanned %1 segments across %2 vehicles")
        .arg(segmentsScanned).arg(vehicleCount));
}

void CountsParser::detectPairedLinks(
    CountsData& counts,
    const NetworkIndex& network)
{
    // Build fromNode->toNode lookup for counted links
    struct LinkNodes {
        uint32_t fromNode;
        uint32_t toNode;
        size_t countIndex;
    };
    std::vector<LinkNodes> linkNodes;
    linkNodes.reserve(counts.counts.size());

    for (size_t i = 0; i < counts.counts.size(); ++i) {
        const LinkRecord* link = network.getLink(counts.counts[i].linkId);
        if (link) {
            linkNodes.push_back({link->fromNode, link->toNode, i});
        }
    }

    // For each counted link, look for reverse direction among other counted links
    for (size_t i = 0; i < linkNodes.size(); ++i) {
        if (counts.counts[linkNodes[i].countIndex].pairedLinkId != 0) continue;

        for (size_t j = i + 1; j < linkNodes.size(); ++j) {
            if (counts.counts[linkNodes[j].countIndex].pairedLinkId != 0) continue;

            if (linkNodes[i].fromNode == linkNodes[j].toNode &&
                linkNodes[i].toNode == linkNodes[j].fromNode) {
                // Found a pair
                auto& a = counts.counts[linkNodes[i].countIndex];
                auto& b = counts.counts[linkNodes[j].countIndex];
                a.pairedLinkId = b.linkId;
                b.pairedLinkId = a.linkId;
                a.isPrimary = true;
                b.isPrimary = false;
                break;
            }
        }
    }

    size_t pairedCount = 0;
    for (const auto& c : counts.counts) {
        if (c.pairedLinkId != 0) pairedCount++;
    }
    LOG_INFO(QString("Counts: %1 paired links detected (%2 pairs)")
        .arg(pairedCount).arg(pairedCount / 2));
}

void CountsParser::cacheGeometry(
    CountsData& counts,
    const NetworkIndex& network)
{
    const auto& nodes = network.allNodes();
    std::unordered_map<uint32_t, size_t> nodeIdToIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIdToIndex[nodes[i].id] = i;
    }

    for (auto& c : counts.counts) {
        const LinkRecord* link = network.getLink(c.linkId);
        if (!link) continue;

        auto fromIt = nodeIdToIndex.find(link->fromNode);
        auto toIt = nodeIdToIndex.find(link->toNode);
        if (fromIt == nodeIdToIndex.end() || toIt == nodeIdToIndex.end()) continue;

        c.fromX = nodes[fromIt->second].x;
        c.fromY = nodes[fromIt->second].y;
        c.toX = nodes[toIt->second].x;
        c.toY = nodes[toIt->second].y;
    }
}

void CountsParser::buildLinkIdLookup(CountsData& counts) {
    if (counts.counts.empty()) return;

    // Find max linkId
    uint32_t maxId = 0;
    for (const auto& c : counts.counts) {
        if (c.linkId > maxId) maxId = c.linkId;
    }

    counts.linkIdLookup.assign(maxId + 1, -1);
    for (size_t i = 0; i < counts.counts.size(); ++i) {
        counts.linkIdLookup[counts.counts[i].linkId] = static_cast<int32_t>(i);
    }
}

} // namespace simvis
