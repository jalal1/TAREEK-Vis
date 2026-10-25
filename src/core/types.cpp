#include "types.h"
#include <stdexcept>
#include <ostream>
#include <istream>

namespace simvis {

RoadType parseRoadType(const std::string& type) {
    if (type == "motorway" || type == "motorway_link") return RoadType::Motorway;
    if (type == "primary" || type == "primary_link") return RoadType::Primary;
    if (type == "secondary" || type == "secondary_link") return RoadType::Secondary;
    if (type == "tertiary" || type == "tertiary_link") return RoadType::Tertiary;
    if (type == "residential") return RoadType::Residential;
    if (type == "service") return RoadType::Service;
    if (type.empty()) return RoadType::Unknown;
    if (type.rfind("air_", 0) == 0) {
        if (type == "air_vertical") return RoadType::AirVertical;
        if (type == "air_climb") return RoadType::AirClimb;
        if (type == "air_cruise") return RoadType::AirCruise;
        if (type == "air_descent") return RoadType::AirDescent;
        if (type == "air_hover") return RoadType::AirHover;
        return RoadType::AirOther;
    }
    return RoadType::Other;
}

EventType parseEventType(const std::string& type) {
    return parseEventType(std::string_view(type));
}

EventType parseEventType(std::string_view type) {
    if (type == "vehicle enters traffic") return EventType::VehicleEntersTraffic;
    if (type == "vehicle leaves traffic") return EventType::VehicleLeavesTraffic;
    if (type == "entered link") return EventType::EnteredLink;
    if (type == "left link") return EventType::LeftLink;
    if (type == "PersonEntersVehicle") return EventType::PersonEntersVehicle;
    if (type == "PersonLeavesVehicle") return EventType::PersonLeavesVehicle;
    if (type == "actstart") return EventType::ActivityStart;
    if (type == "actend") return EventType::ActivityEnd;
    if (type == "departure") return EventType::Departure;
    if (type == "arrival") return EventType::Arrival;
    if (type == "TransitDriverStarts") return EventType::TransitDriverStarts;
    if (type.size() > 4 && type.substr(0, 4) == "uam_") {
        if (type == "uam_los_start") return EventType::UamLosStart;
        if (type == "uam_los_end") return EventType::UamLosEnd;
        if (type == "uam_ground_hold") return EventType::UamGroundHold;
        if (type == "uam_hold_start") return EventType::UamHoldStart;
        if (type == "uam_hold_end") return EventType::UamHoldEnd;
    }
    return EventType::Unknown;
}

const char* eventTypeName(EventType type) {
    switch (type) {
        case EventType::VehicleEntersTraffic: return "vehicle enters traffic";
        case EventType::VehicleLeavesTraffic: return "vehicle leaves traffic";
        case EventType::EnteredLink: return "entered link";
        case EventType::LeftLink: return "left link";
        case EventType::PersonEntersVehicle: return "PersonEntersVehicle";
        case EventType::PersonLeavesVehicle: return "PersonLeavesVehicle";
        case EventType::ActivityStart: return "actstart";
        case EventType::ActivityEnd: return "actend";
        case EventType::Departure: return "departure";
        case EventType::Arrival: return "arrival";
        case EventType::TransitDriverStarts: return "TransitDriverStarts";
        case EventType::UamLosStart: return "uam_los_start";
        case EventType::UamLosEnd: return "uam_los_end";
        case EventType::UamGroundHold: return "uam_ground_hold";
        case EventType::UamHoldStart: return "uam_hold_start";
        case EventType::UamHoldEnd: return "uam_hold_end";
        default: return "unknown";
    }
}

// StringInterner implementation

uint32_t StringInterner::intern(const std::string& str) {
    auto it = stringToId_.find(str);
    if (it != stringToId_.end()) {
        return it->second;
    }
    uint32_t id = static_cast<uint32_t>(strings_.size());
    strings_.push_back(str);
    stringToId_[str] = id;
    return id;
}

const std::string& StringInterner::getString(uint32_t id) const {
    if (id >= strings_.size()) {
        throw std::out_of_range("Invalid string ID");
    }
    return strings_[id];
}

uint32_t StringInterner::getId(const std::string& str) const {
    auto it = stringToId_.find(str);
    if (it == stringToId_.end()) {
        throw std::out_of_range("String not found: " + str);
    }
    return it->second;
}

uint32_t StringInterner::intern(std::string_view sv) {
    // Construct a temporary string for the lookup.
    // This still saves allocations overall because getAttrView() no longer
    // allocates per call — only the interner lookup does, and most IDs are
    // already interned (millions of events but only thousands of unique IDs).
    std::string key(sv);
    auto it = stringToId_.find(key);
    if (it != stringToId_.end()) {
        return it->second;
    }
    uint32_t id = static_cast<uint32_t>(strings_.size());
    strings_.push_back(std::move(key));
    stringToId_[strings_.back()] = id;
    return id;
}

uint32_t StringInterner::getId(std::string_view sv) const {
    std::string key(sv);
    auto it = stringToId_.find(key);
    if (it == stringToId_.end()) {
        throw std::out_of_range("String not found: " + key);
    }
    return it->second;
}

bool StringInterner::hasString(const std::string& str) const {
    return stringToId_.find(str) != stringToId_.end();
}

bool StringInterner::hasString(std::string_view sv) const {
    return stringToId_.find(std::string(sv)) != stringToId_.end();
}

void StringInterner::reserve(size_t capacity) {
    strings_.reserve(capacity);
    stringToId_.reserve(capacity);
}

void StringInterner::clear() {
    strings_.clear();
    stringToId_.clear();
}

void StringInterner::writeTo(std::ostream& out) const {
    uint32_t count = static_cast<uint32_t>(strings_.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& str : strings_) {
        uint32_t len = static_cast<uint32_t>(str.length());
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        out.write(str.data(), len);
    }
}

void StringInterner::readFrom(std::istream& in) {
    clear();

    uint32_t count;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));

    strings_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len;
        in.read(reinterpret_cast<char*>(&len), sizeof(len));

        std::string str(len, '\0');
        in.read(str.data(), len);

        strings_.push_back(str);
        stringToId_[str] = i;
    }
}

} // namespace simvis
