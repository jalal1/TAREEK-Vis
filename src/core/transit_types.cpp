// SPDX-License-Identifier: GPL-3.0-or-later

#include "transit_types.h"
#include <algorithm>

namespace simvis {

const char* transitModeName(TransitMode mode) {
    switch (mode) {
        case TransitMode::Bus:  return "Bus";
        case TransitMode::Rail: return "Rail";
        case TransitMode::Tram: return "Tram";
        default:                return "Unknown";
    }
}

TransitMode parseTransitMode(const std::string& mode) {
    // Case-insensitive comparison
    std::string lower = mode;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "bus") return TransitMode::Bus;
    if (lower == "rail") return TransitMode::Rail;
    if (lower == "tram") return TransitMode::Tram;
    return TransitMode::Unknown;
}

} // namespace simvis
