#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace simvis {

// ============================================================================
// Traffic modes
//
// One row per kind of vehicle: how it looks and how it is filtered. The row id
// is the mode byte that the .vidx file keeps for each vehicle
// (VehicleIndex::vehicleModes_). A new mode (bike, e-scooter, ...) is a new
// row in modes.cpp, not a new code path. See docs/air-and-modes-plan.md.
// ============================================================================

enum class ModeClass : uint8_t {
    Private,   // car
    Transit,   // bus, rail, tram (from the transit schedule)
    Shared,    // taxi, ride-hailing
    Micro,     // bike, e-scooter
    Air        // eVTOL, drone
};

// Glyphs of the vehicle fragment shader
enum class ModeGlyph : uint8_t {
    Circle = 0,
    Triangle = 1,
    Rectangle = 2,
    Diamond = 3,
    Aircraft = 4
};

struct TrafficMode {
    uint8_t id;            // stored per vehicle; ids 0-3 equal TransitMode
    const char* key;       // stable name, for example "air"
    const char* name;      // shown in the UI
    ModeClass modeClass;
    float r, g, b;         // colour for Vehicle Color > By Mode
    ModeGlyph glyph;       // shape for Vehicle Shape > Auto
    float glyphScale;      // size of the glyph against the vehicle size
    const char* icon;      // emoji (UTF-8) for Vehicle Color > By Mode
    bool is3D;             // has a height: drawn with lift, shadow and trail
    const char* legModes;  // MATSim leg modes of the driver, comma-separated
};

namespace modes {

constexpr uint8_t kCar  = 0;
constexpr uint8_t kBus  = 1;
constexpr uint8_t kRail = 2;
constexpr uint8_t kTram = 3;
constexpr uint8_t kAir  = 4;
constexpr uint8_t kNone = 0xFF;  // fromLegMode: no row lists the leg mode

// All rows, in id order (row i has id i)
const std::vector<TrafficMode>& all();

// The row of a mode id; the car row for an unknown id
const TrafficMode& get(uint8_t id);

// The first row that lists this leg mode, or kNone
uint8_t fromLegMode(std::string_view legMode);

} // namespace modes

} // namespace simvis
