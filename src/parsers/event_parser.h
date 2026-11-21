// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include "data/vehicle_index.h"
#include "network_parser.h"  // For ProgressCallback
#include <string>

namespace simvis {

class EventParser {
public:
    // Result of single-pass streaming event processing.
    // Contains a fully built VehicleIndex and string interners,
    // but does NOT store individual events in memory.
    struct StreamResult {
        VehicleIndex vehicleIndex;
        StringInterner vehicleIds;
        StringInterner personIds;
        float minTime = 0.0f;
        float maxTime = 0.0f;
        size_t eventCount = 0;
        bool success = false;
        std::string errorMessage;
    };

    // Single-pass streaming processor: parses gzipped XML events and builds
    // the VehicleIndex in one pass. No events are stored in memory.
    // linkIds is passed from network parsing to ensure consistent ID mapping.
    static StreamResult streamProcessEvents(
        const std::string& xmlPath,
        const StringInterner& linkIds,
        ProgressCallback progress = nullptr
    );
};

} // namespace simvis
