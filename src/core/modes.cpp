#include "modes.h"

namespace simvis::modes {

namespace {

// Rows 0-3 keep the look that cars and transit vehicles had before the
// registry, so a ground-only run looks the same. Colours of rows 1-3 equal
// getTransitModeColor (transit_types.h).
const std::vector<TrafficMode> kModes = {
    {kCar,  "car",  "Car", ModeClass::Private, 0.620f, 0.620f, 0.620f,
     ModeGlyph::Triangle, 1.00f, "\xF0\x9F\x9A\x97", false, "car"},              // U+1F697
    {kBus,  "bus",  "Bus",  ModeClass::Transit, 0.129f, 0.588f, 0.953f,
     ModeGlyph::Rectangle, 1.00f, "\xF0\x9F\x9A\x8C", false, ""},                // U+1F68C
    {kRail, "rail", "Rail", ModeClass::Transit, 1.000f, 0.341f, 0.133f,
     ModeGlyph::Rectangle, 1.75f, "\xF0\x9F\x9A\x86", false, ""},                // U+1F686
    {kTram, "tram", "Tram", ModeClass::Transit, 0.298f, 0.686f, 0.314f,
     ModeGlyph::Diamond, 1.25f, "\xF0\x9F\x9A\x8B", false, ""},                  // U+1F68B
    {kAir,  "air",  "Aircraft", ModeClass::Air, 0.878f, 0.251f, 0.984f,
     ModeGlyph::Aircraft, 2.00f, "\xF0\x9F\x9A\x81", true, "uam,evtol,drone"},   // U+1F681
    {5, "taxi", "Taxi", ModeClass::Shared, 1.000f, 0.757f, 0.027f,
     ModeGlyph::Triangle, 1.00f, "\xF0\x9F\x9A\x95", false, "taxi,drt,av"},      // U+1F695
    {6, "bike", "Bike", ModeClass::Micro, 0.000f, 0.737f, 0.831f,
     ModeGlyph::Circle, 0.80f, "\xF0\x9F\x9A\xB2", false, "bike,bicycle"},       // U+1F6B2
    {7, "escooter", "E-scooter", ModeClass::Micro, 0.545f, 0.765f, 0.290f,
     ModeGlyph::Circle, 0.80f, "\xF0\x9F\x9B\xB4", false, "escooter,e_scooter,scooter"},  // U+1F6F4
};

} // anonymous namespace

const std::vector<TrafficMode>& all() {
    return kModes;
}

const TrafficMode& get(uint8_t id) {
    return id < kModes.size() ? kModes[id] : kModes[kCar];
}

uint8_t fromLegMode(std::string_view legMode) {
    if (legMode.empty()) return kNone;
    for (const TrafficMode& mode : kModes) {
        std::string_view list(mode.legModes);
        while (!list.empty()) {
            const size_t comma = list.find(',');
            if (list.substr(0, comma) == legMode) return mode.id;
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
    }
    return kNone;
}

} // namespace simvis::modes
