// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/transit_types.h"
#include "core/types.h"
#include "network_parser.h"  // for ProgressCallback
#include <string>
#include <unordered_map>

namespace simvis {

class TransitScheduleParser {
public:
    struct Result {
        std::vector<TransitStop> stops;
        std::vector<TransitLine> lines;
        StringInterner stopIds;
        StringInterner lineIds;
        StringInterner routeIds;

        // Vehicle mode map built from departure -> route -> transportMode
        // Key: vehicle string ID (e.g. "veh_152_bus"), Value: TransitMode
        std::unordered_map<std::string, TransitMode> vehicleToMode;

        // Stop lookup by interned ID
        std::unordered_map<uint32_t, size_t> stopIdToIndex;

        bool success = false;
        std::string errorMessage;
    };

    // Parse from plain XML file
    static Result parse(const std::string& xmlPath,
                        const StringInterner& linkIds,
                        ProgressCallback progress = nullptr);

    // Parse from gzipped XML file
    static Result parseGzipFile(const std::string& gzipPath,
                                const StringInterner& linkIds,
                                ProgressCallback progress = nullptr);
};

} // namespace simvis
