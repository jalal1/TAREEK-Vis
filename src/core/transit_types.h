// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace simvis {

// ============================================================================
// Transit mode classification
// ============================================================================

enum class TransitMode : uint8_t {
    Unknown = 0,
    Bus,
    Rail,
    Tram
};

const char* transitModeName(TransitMode mode);
TransitMode parseTransitMode(const std::string& mode);

// Mode colors (matching design doc)
struct TransitModeColor {
    float r, g, b;
};

inline TransitModeColor getTransitModeColor(TransitMode mode) {
    switch (mode) {
        case TransitMode::Bus:  return {0.129f, 0.588f, 0.953f}; // #2196F3
        case TransitMode::Tram: return {0.298f, 0.686f, 0.314f}; // #4CAF50
        case TransitMode::Rail: return {1.000f, 0.341f, 0.133f}; // #FF5722
        default:                return {0.620f, 0.620f, 0.620f}; // gray
    }
}

// Distinct color palette for per-route coloring (tram/rail routes).
// Chosen to be visually separable and to avoid the bus base color (#2196F3).
inline TransitModeColor getRoutePaletteColor(size_t index) {
    static const TransitModeColor kPalette[] = {
        {0.957f, 0.263f, 0.212f}, // #F44336 red
        {0.298f, 0.686f, 0.314f}, // #4CAF50 green
        {1.000f, 0.596f, 0.000f}, // #FF9800 orange
        {0.612f, 0.153f, 0.690f}, // #9C27B0 purple
        {0.000f, 0.737f, 0.831f}, // #00BCD4 cyan
        {1.000f, 0.922f, 0.231f}, // #FFEB3B yellow
        {0.913f, 0.118f, 0.388f}, // #E91E63 pink
        {0.404f, 0.227f, 0.718f}, // #673AB7 deep purple
        {0.545f, 0.765f, 0.290f}, // #8BC34A light green
        {0.475f, 0.333f, 0.282f}, // #795548 brown
        {0.000f, 0.588f, 0.533f}, // #009688 teal
        {1.000f, 0.341f, 0.133f}, // #FF5722 deep orange
        {0.247f, 0.318f, 0.710f}, // #3F51B5 indigo
        {0.804f, 0.863f, 0.224f}, // #CDDC39 lime
        {0.557f, 0.643f, 0.682f}, // #8D9BA9 blue gray
        {0.557f, 0.000f, 0.000f}, // #8E0000 dark red
    };
    constexpr size_t kCount = sizeof(kPalette) / sizeof(kPalette[0]);
    return kPalette[index % kCount];
}

// ============================================================================
// Transit stop (static, from transitSchedule.xml)
// ============================================================================

struct TransitStop {
    uint32_t id;            // Interned stop ID
    float x, y;             // Coordinates (same CRS as network)
    uint32_t linkRefId;     // Network link this stop is on (0 if unresolved)
    std::string name;       // Human-readable name
    std::string originalId; // Original XML ID for display
};

// ============================================================================
// Transit route (static, from transitSchedule.xml)
// ============================================================================

struct TransitRouteStop {
    uint32_t stopId;             // Reference to TransitStop
    uint32_t arrivalOffsetMs;    // Offset from departure in ms
    uint32_t departureOffsetMs;
};

struct TransitDeparture {
    uint32_t departureTimeMs;    // Time of day in ms
    std::string vehicleRefId;    // Vehicle string ID (e.g. "veh_152_bus")
};

struct TransitRoute {
    uint32_t id;                          // Interned route ID
    TransitMode mode;                     // bus / rail / tram
    std::vector<TransitRouteStop> stops;  // Ordered stop sequence
    std::vector<uint32_t> linkIds;        // Network link sequence for geometry
    std::vector<TransitDeparture> departures;
};

// ============================================================================
// Transit line (static, from transitSchedule.xml)
// ============================================================================

struct TransitLine {
    uint32_t id;                        // Interned line ID
    std::string name;                   // Display name ("10", "Blue Line", etc.)
    std::string originalId;             // Original XML ID
    TransitMode primaryMode;            // Determined from first route's mode
    std::vector<TransitRoute> routes;
};

} // namespace simvis
