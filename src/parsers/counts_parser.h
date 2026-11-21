// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace simvis {

// Data for one count location (one link)
struct CountData {
    std::string stationId;                        // cs_id from counts.xml
    std::array<uint32_t, 24> groundTruthVolumes;  // h=1..24 mapped to index 0..23
    std::array<uint32_t, 24> simulatedVolumes;    // computed from events
    uint32_t linkId;                              // internal link ID (StringInterner)
    uint32_t pairedLinkId;                        // reverse-direction link (0 if none)
    bool isPrimary;                               // true = blue, false = white
    float fromX, fromY, toX, toY;                // cached link geometry
};

// All counts data
struct CountsData {
    std::vector<CountData> counts;
    std::unordered_map<uint32_t, size_t> linkToCount;  // linkId -> index in counts
    std::vector<int32_t> linkIdLookup;                  // flat: linkId -> counts index (-1 if not)
};

class CountsParser {
public:
    // Parse counts.xml (plain or gzipped) and resolve against network.
    // Returns nullptr on failure.
    static std::unique_ptr<CountsData> parse(
        const std::string& xmlPath,
        const NetworkIndex& network);

    // Compute simulated hourly volumes by scanning all vehicle segments.
    // Must be called after parse() and after VehicleIndex is loaded.
    static void computeSimulatedVolumes(
        CountsData& counts,
        const VehicleIndex& vehicleIndex);

private:
    // Detect paired links (same nodes, reversed direction)
    static void detectPairedLinks(
        CountsData& counts,
        const NetworkIndex& network);

    // Cache link geometry for rendering and hit testing
    static void cacheGeometry(
        CountsData& counts,
        const NetworkIndex& network);

    // Build the flat linkId lookup array
    static void buildLinkIdLookup(CountsData& counts);
};

} // namespace simvis
