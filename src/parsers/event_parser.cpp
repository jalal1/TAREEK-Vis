#include "event_parser.h"
#include "core/logger.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <charconv>
#include <string_view>
#include <zlib.h>

namespace simvis {

namespace {

// Optimized gzip reader with larger buffer for better performance
class GzipReader {
public:
    static constexpr size_t BUFFER_SIZE = 65536;  // 64KB buffer

    explicit GzipReader(const std::string& path) {
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) {
            throw std::runtime_error("Failed to open gzip file: " + path);
        }
        // Set internal zlib buffer size for better decompression performance
        gzbuffer(file_, 131072);  // 128KB internal buffer
    }

    ~GzipReader() {
        if (file_) gzclose(file_);
    }

    // Read line into internal buffer, return view. Zero allocations.
    // The returned view is valid until the next call to readLineView().
    std::string_view readLineView() {
        if (gzgets(file_, buffer_, BUFFER_SIZE)) {
            size_t len = strlen(buffer_);
            bytesRead_ += len;
            return {buffer_, len};
        }
        return {};
    }

    bool eof() const { return gzeof(file_); }
    size_t bytesRead() const { return bytesRead_; }

    // Get compressed file size for progress estimation
    static size_t getFileSize(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        return f.tellg();
    }

private:
    gzFile file_ = nullptr;
    size_t bytesRead_ = 0;
    char buffer_[BUFFER_SIZE];
};

// Zero-allocation attribute extraction: returns a view into the line buffer.
// SAFETY: The returned view is only valid as long as the line pointer is valid.
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

// Fast float parse from string_view using std::from_chars (C++17, zero-allocation).
inline float fastParseFloat(std::string_view sv) {
    float val = 0.0f;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc()) return val;
    // Fallback for edge cases (should be rare)
    return std::stof(std::string(sv));
}

// Sentinel for PersonState::openActivity — no activity currently open.
constexpr uint32_t kNoOpenActivity = 0xFFFFFFFF;

} // anonymous namespace

EventParser::StreamResult EventParser::streamProcessEvents(
    const std::string& xmlPath,
    const StringInterner& linkIds,
    ProgressCallback progress)
{
    StreamResult result;

    try {
        // Get compressed file size for progress estimation
        size_t compressedSize = GzipReader::getFileSize(xmlPath);
        size_t estimatedTotal = compressedSize * 8;  // ~8x compression ratio for XML

        GzipReader reader(xmlPath);

        if (progress) {
            progress(0, estimatedTotal, "Opening events file...");
        }

        // Pre-reserve interners to reduce rehashing
        size_t estimatedVehicles = std::min(compressedSize / 500, size_t(500000));
        result.vehicleIds.reserve(estimatedVehicles);
        result.personIds.reserve(estimatedVehicles * 2);

        // Mutable copy of link IDs (some events may reference links not in network)
        StringInterner mutableLinkIds = linkIds;

        // Vehicle index building state
        struct OpenSegment {
            uint32_t linkId = 0;
            TimeMs enterTime = 0;
            bool valid = false;
        };
        std::vector<VehicleTrajectory> vehicles;
        std::vector<OpenSegment> openSegments;

        // Per-person leg-building state (indexed by interned personId).
        // One PersonTrip is emitted per MATSim leg (departure -> arrival).
        struct PersonState {
            uint16_t lastActEndType = TRIP_NO_ACT; // set by actend, consumed by next departure
            TimeMs departTimeMs = 0;
            LinkId departLinkId = 0;
            uint16_t legModeId = 0;
            uint32_t currentVehicle = 0;           // vehicleId+1 while in a vehicle
            TimeMs boardTimeMs = TRIP_NO_TIME;     // when it entered currentVehicle
            bool legOpen = false;
            bool awaitingActStart = false;         // last emitted trip wants toActTypeId

            // Activity currently in progress, opened by actstart and closed by
            // the matching actend. index = position in vidx.activities_.
            uint32_t openActivity = kNoOpenActivity;
        };
        std::vector<PersonState> personStates;
        auto& vidx = result.vehicleIndex;          // trip data goes straight into the index

        auto growPersonArrays = [&]() {
            size_t n = result.personIds.size();
            if (n > personStates.size()) {
                personStates.resize(n);
                vidx.personTrips_.resize(n);
            }
        };
        auto growDriverArray = [&](size_t vehicleCount) {
            if (vehicleCount > vidx.driverOfVehicle_.size()) {
                vidx.driverOfVehicle_.resize(vehicleCount, 0);
            }
        };

        // Build an ActivityRecord from an actstart/actend line. MATSim writes the
        // activity's true x/y on these events; when they are absent (the schema
        // makes them optional) the coordinate is left at the origin and flagged,
        // so callers can fall back to link geometry.
        auto makeActivity = [&](const char* data, size_t len, uint32_t personId,
                                uint16_t actTypeId, TimeMs startMs,
                                TimeMs endMs) -> ActivityRecord {
            ActivityRecord act{};
            act.personId = personId;
            act.actTypeId = actTypeId;
            act.startTimeMs = startMs;
            act.endTimeMs = endMs;
            act.flags = 0;

            // Leading space is significant: a bare "x" would also match the tail
            // of a longer attribute name ending in x (e.g. maxSpeed="…").
            std::string_view xStr = getAttrView(data, len, " x", 2);
            std::string_view yStr = getAttrView(data, len, " y", 2);
            if (!xStr.empty() && !yStr.empty()) {
                act.x = fastParseFloat(xStr);
                act.y = fastParseFloat(yStr);
            } else {
                act.x = 0.0f;
                act.y = 0.0f;
                act.flags |= ACT_FLAG_DERIVED_COORD;
            }

            std::string_view linkStr = getAttrView(data, len, "link", 4);
            act.linkId = TRIP_NO_LINK;
            if (!linkStr.empty()) {
                act.linkId = mutableLinkIds.hasString(linkStr)
                    ? mutableLinkIds.getId(linkStr)
                    : mutableLinkIds.intern(linkStr);
            }
            return act;
        };

        size_t eventCount = 0;
        size_t lastProgress = 0;

        while (!reader.eof()) {
            std::string_view line = reader.readLineView();
            if (line.empty()) continue;

            // Progress update every ~2MB of decompressed data
            size_t currentBytes = reader.bytesRead();
            if (progress && currentBytes - lastProgress > 2000000) {
                lastProgress = currentBytes;
                size_t displayBytes = std::min(currentBytes, estimatedTotal - 1);
                progress(displayBytes, estimatedTotal,
                    "Parsing events: " + std::to_string(eventCount) + " events");
            }

            // Quick reject: minimum event line is "<event />" = 9 chars
            if (line.size() < 9) continue;

            // Fast scan for "<event "
            if (line.find("<event ") == std::string_view::npos) continue;

            const char* data = line.data();
            size_t len = line.size();

            std::string_view timeStr = getAttrView(data, len, "time", 4);
            std::string_view typeStr = getAttrView(data, len, "type", 4);
            if (timeStr.empty()) continue;

            float time = fastParseFloat(timeStr);
            EventType type = parseEventType(typeStr);

            // Skip events we don't care about
            if (type == EventType::Unknown) continue;

            // Track time range
            if (eventCount == 0) {
                result.minTime = time;
            }
            result.maxTime = time;
            eventCount++;

            TimeMs eventTimeMs = VehicleIndex::toTimeMs(time);

            // --- Person leg / trip events (departure, arrival, act, boarding) ---
            switch (type) {
                case EventType::ActivityEnd: {
                    std::string_view personStr = getAttrView(data, len, "person", 6);
                    std::string_view actStr = getAttrView(data, len, "actType", 7);
                    if (personStr.empty()) continue;
                    uint32_t personId = result.personIds.intern(personStr);
                    growPersonArrays();
                    auto& ps = personStates[personId];
                    uint16_t actTypeId = actStr.empty()
                        ? TRIP_NO_ACT
                        : static_cast<uint16_t>(vidx.actTypeStrings_.intern(actStr));
                    ps.lastActEndType = actTypeId;

                    // Close the activity opened by the matching actstart. If none
                    // is open this is the person's first activity of the day —
                    // it began before the simulation, so record it here with no
                    // start time. Those would otherwise be lost entirely.
                    if (ps.openActivity != kNoOpenActivity) {
                        vidx.activities_[ps.openActivity].endTimeMs = eventTimeMs;
                        ps.openActivity = kNoOpenActivity;
                    } else if (actTypeId != TRIP_NO_ACT) {
                        vidx.activities_.push_back(
                            makeActivity(data, len, personId, actTypeId,
                                         ACT_NO_TIME, eventTimeMs));
                    }
                    continue;
                }

                case EventType::Departure: {
                    std::string_view personStr = getAttrView(data, len, "person", 6);
                    std::string_view linkStr = getAttrView(data, len, "link", 4);
                    std::string_view modeStr = getAttrView(data, len, "legMode", 7);
                    if (personStr.empty()) continue;
                    uint32_t personId = result.personIds.intern(personStr);
                    growPersonArrays();
                    auto& ps = personStates[personId];
                    ps.departTimeMs = eventTimeMs;
                    ps.departLinkId = 0;
                    if (!linkStr.empty()) {
                        if (mutableLinkIds.hasString(linkStr))
                            ps.departLinkId = mutableLinkIds.getId(linkStr);
                        else
                            ps.departLinkId = mutableLinkIds.intern(linkStr);
                    }
                    ps.legModeId = modeStr.empty()
                        ? 0
                        : static_cast<uint16_t>(vidx.legModeStrings_.intern(modeStr));
                    ps.currentVehicle = 0;
                    ps.boardTimeMs = TRIP_NO_TIME;
                    ps.legOpen = true;
                    ps.awaitingActStart = false;
                    continue;
                }

                case EventType::Arrival: {
                    std::string_view personStr = getAttrView(data, len, "person", 6);
                    std::string_view linkStr = getAttrView(data, len, "link", 4);
                    if (personStr.empty()) continue;
                    uint32_t personId = result.personIds.intern(personStr);
                    growPersonArrays();
                    auto& ps = personStates[personId];
                    if (!ps.legOpen) continue;

                    PersonTrip trip{};
                    trip.departTimeMs = ps.departTimeMs;
                    trip.arriveTimeMs = eventTimeMs;
                    trip.boardTimeMs = ps.boardTimeMs;
                    trip.fromLinkId = ps.departLinkId;
                    trip.toLinkId = TRIP_NO_LINK;
                    if (!linkStr.empty()) {
                        if (mutableLinkIds.hasString(linkStr))
                            trip.toLinkId = mutableLinkIds.getId(linkStr);
                        else
                            trip.toLinkId = mutableLinkIds.intern(linkStr);
                    }
                    trip.vehicleId = ps.currentVehicle;
                    trip.legModeId = ps.legModeId;
                    trip.fromActTypeId = ps.lastActEndType;
                    trip.toActTypeId = TRIP_NO_ACT;   // patched by next actstart
                    trip.flags = 0;
                    if (ps.currentVehicle == 0) {
                        trip.flags |= TRIP_FLAG_TELEPORTED;
                    } else {
                        // Rider in someone else's vehicle (driver set earlier by
                        // TransitDriverStarts or the first-boarding person) = pt passenger
                        uint32_t vehId = ps.currentVehicle - 1;
                        if (vehId < vidx.driverOfVehicle_.size() &&
                            vidx.driverOfVehicle_[vehId] != 0 &&
                            vidx.driverOfVehicle_[vehId] != personId + 1) {
                            trip.flags |= TRIP_FLAG_TRANSIT_RIDER;
                        }
                    }
                    vidx.personTrips_[personId].push_back(trip);

                    ps.lastActEndType = TRIP_NO_ACT;  // consumed by this leg
                    ps.legOpen = false;
                    ps.awaitingActStart = true;
                    continue;
                }

                case EventType::ActivityStart: {
                    std::string_view personStr = getAttrView(data, len, "person", 6);
                    std::string_view actStr = getAttrView(data, len, "actType", 7);
                    if (personStr.empty()) continue;
                    uint32_t personId = result.personIds.intern(personStr);
                    growPersonArrays();
                    auto& ps = personStates[personId];
                    uint16_t actTypeId = actStr.empty()
                        ? TRIP_NO_ACT
                        : static_cast<uint16_t>(vidx.actTypeStrings_.intern(actStr));
                    if (ps.awaitingActStart && !vidx.personTrips_[personId].empty() &&
                        actTypeId != TRIP_NO_ACT) {
                        vidx.personTrips_[personId].back().toActTypeId = actTypeId;
                    }
                    ps.awaitingActStart = false;

                    // Open an activity; the matching actend closes it. A second
                    // actstart without an intervening actend would abandon the
                    // first, so close it defensively at this event's time.
                    if (actTypeId != TRIP_NO_ACT) {
                        if (ps.openActivity != kNoOpenActivity) {
                            vidx.activities_[ps.openActivity].endTimeMs = eventTimeMs;
                        }
                        ps.openActivity = static_cast<uint32_t>(vidx.activities_.size());
                        vidx.activities_.push_back(
                            makeActivity(data, len, personId, actTypeId,
                                         eventTimeMs, ACT_NO_TIME));
                    }
                    continue;
                }

                case EventType::PersonEntersVehicle: {
                    std::string_view personStr = getAttrView(data, len, "person", 6);
                    std::string_view vehStr = getAttrView(data, len, "vehicle", 7);
                    if (personStr.empty() || vehStr.empty()) continue;
                    uint32_t personId = result.personIds.intern(personStr);
                    growPersonArrays();
                    // Same interner as movement events: keeps VehicleId spaces identical
                    uint32_t vehId = result.vehicleIds.intern(vehStr);
                    growDriverArray(result.vehicleIds.size());
                    auto& ps = personStates[personId];
                    if (ps.legOpen) {
                        ps.currentVehicle = vehId + 1;
                        ps.boardTimeMs = eventTimeMs;
                    }
                    // First person to enter is the driver (transit drivers were
                    // already set by TransitDriverStarts; don't overwrite)
                    if (vidx.driverOfVehicle_[vehId] == 0) {
                        vidx.driverOfVehicle_[vehId] = personId + 1;
                    }
                    continue;
                }

                case EventType::PersonLeavesVehicle:
                    // Occupancy intervals are derived from trips at load time;
                    // nothing to record here.
                    continue;

                case EventType::TransitDriverStarts: {
                    std::string_view driverStr = getAttrView(data, len, "driverId", 8);
                    std::string_view vehStr = getAttrView(data, len, "vehicleId", 9);
                    std::string_view lineStr = getAttrView(data, len, "transitLineId", 13);
                    std::string_view routeStr = getAttrView(data, len, "transitRouteId", 14);
                    if (vehStr.empty()) continue;
                    uint32_t vehId = result.vehicleIds.intern(vehStr);
                    growDriverArray(result.vehicleIds.size());
                    if (!driverStr.empty()) {
                        uint32_t driverId = result.personIds.intern(driverStr);
                        growPersonArrays();
                        vidx.driverOfVehicle_[vehId] = driverId + 1;
                    }
                    VehicleIndex::TransitService svc{};
                    svc.vehicleId = vehId;
                    // UINT32_MAX = missing (string lookups return "" for out-of-range)
                    svc.lineStrId = lineStr.empty()
                        ? 0xFFFFFFFFu : vidx.transitLineStrings_.intern(lineStr);
                    svc.routeStrId = routeStr.empty()
                        ? 0xFFFFFFFFu : vidx.transitRouteStrings_.intern(routeStr);
                    vidx.transitServices_.push_back(svc);
                    continue;
                }

                default:
                    break;  // fall through to vehicle movement handling
            }

            // Only vehicle movement events matter for the vehicle index
            if (type != EventType::VehicleEntersTraffic &&
                type != EventType::VehicleLeavesTraffic &&
                type != EventType::EnteredLink &&
                type != EventType::LeftLink) {
                continue;
            }

            // Extract vehicle and link attributes
            std::string_view vehicleStr = getAttrView(data, len, "vehicle", 7);
            std::string_view linkStr = getAttrView(data, len, "link", 4);

            uint32_t vehicleId = 0;
            uint32_t linkId = 0;

            if (!vehicleStr.empty()) {
                vehicleId = result.vehicleIds.intern(vehicleStr);
            }
            if (!linkStr.empty()) {
                if (mutableLinkIds.hasString(linkStr)) {
                    linkId = mutableLinkIds.getId(linkStr);
                } else {
                    linkId = mutableLinkIds.intern(linkStr);
                }
            }

            // Ensure vehicle arrays are large enough
            uint32_t vehicleCount = static_cast<uint32_t>(result.vehicleIds.size());
            if (vehicleCount > vehicles.size()) {
                vehicles.resize(vehicleCount);
                openSegments.resize(vehicleCount);
            }

            if (vehicleId == 0 && type != EventType::VehicleEntersTraffic)
                continue;
            if (vehicleId >= vehicleCount) continue;

            TimeMs timeMs = eventTimeMs;

            // State machine: build vehicle segments inline
            switch (type) {
                case EventType::VehicleEntersTraffic: {
                    auto& open = openSegments[vehicleId];
                    open.linkId = linkId;
                    open.enterTime = timeMs;
                    open.valid = true;
                    break;
                }

                case EventType::EnteredLink: {
                    auto& open = openSegments[vehicleId];
                    if (open.valid && open.linkId != 0) {
                        VehicleSegment seg;
                        seg.enterTime = open.enterTime;
                        seg.leaveTime = timeMs;
                        seg.linkId = open.linkId;
                        if (seg.leaveTime > seg.enterTime) {
                            vehicles[vehicleId].segments.push_back(seg);
                        }
                    }
                    open.linkId = linkId;
                    open.enterTime = timeMs;
                    open.valid = true;
                    break;
                }

                case EventType::LeftLink: {
                    auto& open = openSegments[vehicleId];
                    if (open.valid && open.linkId == linkId) {
                        VehicleSegment seg;
                        seg.enterTime = open.enterTime;
                        seg.leaveTime = timeMs;
                        seg.linkId = open.linkId;
                        if (seg.leaveTime > seg.enterTime) {
                            vehicles[vehicleId].segments.push_back(seg);
                        }
                        open.valid = false;
                    }
                    break;
                }

                case EventType::VehicleLeavesTraffic: {
                    auto& open = openSegments[vehicleId];
                    if (open.valid && open.linkId != 0) {
                        VehicleSegment seg;
                        seg.enterTime = open.enterTime;
                        seg.leaveTime = timeMs;
                        seg.linkId = open.linkId;
                        if (seg.leaveTime > seg.enterTime) {
                            vehicles[vehicleId].segments.push_back(seg);
                        }
                    }
                    open.valid = false;
                    break;
                }

                default:
                    break;
            }
        }

        if (progress) {
            progress(estimatedTotal, estimatedTotal,
                "Completed: " + std::to_string(eventCount) + " events");
        }

        // Close any legs still open at end of file (stuck agents)
        for (uint32_t personId = 0; personId < personStates.size(); ++personId) {
            auto& ps = personStates[personId];
            if (!ps.legOpen) continue;
            PersonTrip trip{};
            trip.departTimeMs = ps.departTimeMs;
            trip.arriveTimeMs = TRIP_NO_TIME;   // never arrived
            trip.boardTimeMs = ps.boardTimeMs;
            trip.fromLinkId = ps.departLinkId;
            trip.toLinkId = TRIP_NO_LINK;
            trip.vehicleId = ps.currentVehicle;
            trip.legModeId = ps.legModeId;
            trip.fromActTypeId = ps.lastActEndType;
            trip.toActTypeId = TRIP_NO_ACT;
            trip.flags = (ps.currentVehicle == 0) ? TRIP_FLAG_TELEPORTED : 0;
            vidx.personTrips_[personId].push_back(trip);
        }

        // Activities still open at end of file are the day's final activity
        // (the agent is at home and never leaves again). Their endTimeMs stays
        // ACT_NO_TIME, which reads as "still in progress".

        // Finalize vehicle index. The vehicles_ array must cover every interned
        // vehicle id (PersonEntersVehicle/TransitDriverStarts can intern vehicles
        // that produced no movement events; their trajectories stay empty).
        vehicles.resize(result.vehicleIds.size());
        result.vehicleIndex.vehicles_ = std::move(vehicles);
        result.vehicleIndex.buildLookupArrays();
        result.vehicleIndex.driverOfVehicle_.resize(result.vehicleIds.size(), 0);
        result.eventCount = eventCount;
        result.success = true;

        {
            size_t derived = 0;
            std::vector<size_t> perType(vidx.actTypeStrings_.size(), 0);
            for (const auto& a : vidx.activities_) {
                if (a.flags & ACT_FLAG_DERIVED_COORD) ++derived;
                if (a.actTypeId < perType.size()) ++perType[a.actTypeId];
            }
            LOG_INFO(QString("EventParser: %1 activities (%2 without event coordinates)")
                .arg(vidx.activities_.size()).arg(derived));

            // Per-type counts. A heatmap is built per activity type, so these
            // numbers say which types hold enough data to be worth plotting.
            for (size_t t = 0; t < perType.size(); ++t) {
                if (perType[t] == 0) continue;
                LOG_INFO(QString("  activity type %1: %2")
                    .arg(QString::fromStdString(vidx.actTypeStrings_.getString(
                        static_cast<uint32_t>(t))))
                    .arg(perType[t]));
            }
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }

    return result;
}

} // namespace simvis
