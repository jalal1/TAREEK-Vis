#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <QString>
#include "core/types.h"          // StringInterner
#include "core/transit_types.h"

namespace simvis {

using VehicleId = uint32_t;
using LinkId    = uint32_t;
using TimeMs    = uint32_t;

// A single segment of a vehicle's trip on one link
struct VehicleSegment {
    TimeMs enterTime;   // inclusive (milliseconds from sim start)
    TimeMs leaveTime;   // exclusive
    LinkId linkId;
};
static_assert(sizeof(VehicleSegment) == 12, "VehicleSegment must be 12 bytes");

// Full trajectory for one vehicle: an ordered sequence of segments
struct VehicleTrajectory {
    std::vector<VehicleSegment> segments;
};

// One MATSim leg of a person's day (departure -> arrival)
struct PersonTrip {
    TimeMs departTimeMs;
    TimeMs arriveTimeMs;      // UINT32_MAX = never arrived (stuck agent)
    // v5: when the person entered the leg's vehicle. Later than the departure
    // on a pt leg, by the wait at the stop. UINT32_MAX = no vehicle.
    TimeMs boardTimeMs;
    LinkId fromLinkId;
    LinkId toLinkId;          // UINT32_MAX = unknown
    uint32_t vehicleId;       // internal VehicleId + 1; 0 = teleported / no vehicle
    uint16_t legModeId;       // index into legModeStrings_
    uint16_t fromActTypeId;   // index into actTypeStrings_; UINT16_MAX = none (mid-journey leg)
    uint16_t toActTypeId;     // UINT16_MAX = none
    uint16_t flags;           // bit0: teleported, bit1: transit rider; rest reserved
};
static_assert(sizeof(PersonTrip) == 32, "PersonTrip must be 32 bytes");

constexpr uint16_t TRIP_NO_ACT   = 0xFFFF;
constexpr TimeMs   TRIP_NO_TIME  = 0xFFFFFFFF;
constexpr LinkId   TRIP_NO_LINK  = 0xFFFFFFFF;
constexpr uint16_t TRIP_FLAG_TELEPORTED    = 1 << 0;
constexpr uint16_t TRIP_FLAG_TRANSIT_RIDER = 1 << 1;

// One MATSim activity occurrence, at its true location.
//
// MATSim actstart/actend events carry x/y attributes giving the activity's
// actual coordinates, which are far more accurate than the link geometry we
// would otherwise fall back to (measured against a Twin Cities run: using the
// endpoint link's to-node is a median 125 m off). Activities are also recorded
// independently of trips so that a person's first activity of the day — which
// has no preceding leg — is still captured.
struct ActivityRecord {
    float    x;              // projected CRS, same as the network
    float    y;
    TimeMs   startTimeMs;    // ACT_NO_TIME = in progress at start of day
    TimeMs   endTimeMs;      // ACT_NO_TIME = still in progress at end of day
    LinkId   linkId;         // link the activity is attached to (TRIP_NO_LINK if absent)
    uint32_t personId;
    uint16_t actTypeId;      // index into actTypeStrings_
    uint16_t flags;          // bit0: coordinates were derived, not from the event
};
static_assert(sizeof(ActivityRecord) == 28, "ActivityRecord must be 28 bytes");

constexpr TimeMs   ACT_NO_TIME = 0xFFFFFFFF;
// Set when x/y were absent from the event and we fell back to link geometry.
constexpr uint16_t ACT_FLAG_DERIVED_COORD = 1 << 0;

// An interval from the tareek-air custom events, for the air overlays
// (docs/air-and-modes-plan.md, VIS-4): a loss of separation between two
// aircraft, or a hold at a vertiport.
enum class OverlayKind : uint8_t {
    Loss = 1,        // uam_los_start .. uam_los_end
    GroundHold = 2,  // uam_ground_hold: on the ground before departure
    AirHold = 3      // uam_hold_start .. uam_hold_end: airborne, before landing
};

struct OverlayEvent {
    TimeMs   startMs;
    TimeMs   endMs;       // TRIP_NO_TIME: the end event is missing
    uint32_t vehicleA;
    uint32_t vehicleB;    // second aircraft of a loss; kNoVehicle for a hold
    float    x;           // hold: the vertiport (from the flight's first or
    float    y;           //   last node); loss: 0
    float    value;       // loss: smallest horizontal distance, m; hold: s
    uint8_t  kind;        // OverlayKind
    uint8_t  flags;       // loss: OVERLAY_FLAG_TERMINAL
    uint16_t reserved;
};
static_assert(sizeof(OverlayEvent) == 32, "OverlayEvent must be 32 bytes");

constexpr uint8_t OVERLAY_FLAG_TERMINAL = 1 << 0;  // loss in a terminal zone
constexpr uint8_t OVERLAY_FLAG_PLACED   = 1 << 1;  // hold: x and y are set

// Binary file header for .vidx v6 files (120 bytes: 6xu32 + 4xu64 + 2xu32 + 4xu64
// + 1xu32 + 1xu32 + 2xu64, naturally packed — u64 fields land on 8-byte
// boundaries with no padding)
struct VehicleIndexHeader {
    uint32_t magic;                  // VIDX_FILE_MAGIC
    uint32_t version;                // VIDX_FILE_VERSION
    uint32_t vehicleCount;
    uint32_t totalSegments;          // sum of all segments across all vehicles
    uint32_t minTimeMs;
    uint32_t maxTimeMs;
    uint64_t trajectoryTableOffset;  // per-vehicle (offset, count) table
    uint64_t segmentDataOffset;      // packed VehicleSegment data
    uint64_t modeTableOffset;        // per-vehicle mode bytes (vehicleCount bytes)
    uint64_t tripTableOffset;        // per-person (tripOffset, tripCount) table
    uint32_t personCount;
    uint32_t totalTrips;             // sum of all trips across all persons
    uint64_t tripDataOffset;         // packed PersonTrip data
    uint64_t vehicleDriverOffset;    // per-vehicle uint32 (personId+1, 0 = none)
    uint64_t transitServiceOffset;   // uint32 count + TransitService records
    uint64_t stringTablesOffset;     // 6 sequential StringInterner blobs
    uint32_t activityCount;          // v4: total ActivityRecords
    uint32_t overlayCount;           // v6: total OverlayEvents (was padding)
    uint64_t activityDataOffset;     // v4: packed ActivityRecord data
    uint64_t overlayDataOffset;      // v6: packed OverlayEvent data
};
static_assert(sizeof(VehicleIndexHeader) == 120, "VehicleIndexHeader must be 120 bytes");

constexpr uint32_t VIDX_FILE_MAGIC   = 0x56494458;  // "VIDX"
// v4 adds the activity table (ActivityRecord); v5 adds PersonTrip::boardTimeMs;
// v6 keeps the segments of the first vehicle and the first link (id-0 fix),
// stores the vehicle mode as a registry id, and adds the overlay events.
// A version bump forces a one-time automatic re-preprocess, which is how older
// caches are retired.
constexpr uint32_t VIDX_FILE_VERSION = 6;

// Pre-computed per-vehicle index for fast active-vehicle queries
class VehicleIndex {
public:
    VehicleIndex() = default;

    // Load from binary .vidx file (accepts current version only)
    bool loadFile(const QString& path);

    // Write to binary .vidx file
    static bool writeFile(const QString& path, const VehicleIndex& index);

    // Read just the version field from a .vidx file (0 on failure).
    // Used by the cache-validity check to force re-preprocessing of old formats.
    static uint32_t fileVersion(const QString& path);

    // --- Queries ---

    // Find the segment a vehicle is on at the given time.
    // Returns nullptr if vehicle is not active at that time.
    const VehicleSegment* segmentAt(VehicleId id, TimeMs time) const;

    // Collect all vehicle IDs active at the given time into `out`.
    // Clears `out` before filling.
    void getActiveVehicles(TimeMs time, std::vector<VehicleId>& out) const;

    // Get the full trajectory for a vehicle (empty if invalid id)
    const VehicleTrajectory* trajectory(VehicleId id) const;

    // --- Metadata ---
    TimeMs minTime() const { return minTime_; }
    TimeMs maxTime() const { return maxTime_; }
    size_t vehicleCount() const { return vehicles_.size(); }
    size_t personCount() const { return personTrips_.size(); }

    // Convert seconds (float) to TimeMs
    static TimeMs toTimeMs(float seconds) {
        return static_cast<TimeMs>(seconds * 1000.0f + 0.5f);
    }

    // Convert TimeMs to seconds (float)
    static float toSeconds(TimeMs ms) {
        return static_cast<float>(ms) / 1000.0f;
    }

    // --- Mode classification ---
    // Mode id of a vehicle (core/modes.h); modes::kCar for an invalid id
    uint8_t modeId(VehicleId id) const {
        return id < vehicleModes_.size() ? vehicleModes_[id] : 0;
    }
    // Transit mode, or TransitMode::Unknown for a vehicle that is not transit
    TransitMode getVehicleMode(VehicleId id) const;
    bool isTransitVehicle(VehicleId id) const;

    // --- Trip / person queries (v3) ---

    // All legs of one person's day, in departure order (nullptr if invalid id)
    const std::vector<PersonTrip>* personTrips(uint32_t personId) const;

    // Driver of a vehicle: personId + 1, or 0 if unknown
    uint32_t vehicleDriver(VehicleId id) const;

    // Internal id of a vehicle or a person from its MATSim id, or UINT32_MAX
    // when the run has no such id (used by the search box)
    uint32_t findVehicleByOriginalId(const std::string& id) const;
    uint32_t findPersonByOriginalId(const std::string& id) const;

    // Transit service info for a transit vehicle (nullptr if not a transit vehicle)
    struct TransitService {
        uint32_t vehicleId;
        uint32_t lineStrId;   // index into transitLineStrings_
        uint32_t routeStrId;  // index into transitRouteStrings_
    };
    const TransitService* transitServiceInfo(VehicleId id) const;

    // Persons aboard a vehicle at a given time, in boarding order. The driver
    // is one of them; the caller leaves them out when it wants passengers.
    std::vector<uint32_t> personsAboard(VehicleId id, TimeMs time) const;

    // --- Air overlay events (v6), sorted by startMs ---
    const std::vector<OverlayEvent>& overlayEvents() const { return overlayEvents_; }

    // --- Activity queries (v4) ---

    // Every activity in the scenario, in event order (therefore time-ordered).
    const std::vector<ActivityRecord>& activities() const { return activities_; }
    size_t activityCount() const { return activities_.size(); }

    // Indices into activities() for one person, in chronological order.
    // Returns nullptr if the person has no recorded activities.
    const std::vector<uint32_t>* activitiesForPerson(uint32_t personId) const;

    // --- String lookups (empty string on invalid/sentinel id) ---
    QString vehicleIdString(VehicleId id) const;
    QString personIdString(uint32_t personId) const;
    QString actTypeString(uint16_t id) const;
    QString legModeString(uint16_t id) const;
    QString transitLineString(uint32_t id) const;
    QString transitRouteString(uint32_t id) const;

    // --- Build interface (used by preprocessor / event parser) ---
    std::vector<VehicleTrajectory> vehicles_;
    std::vector<TimeMs> firstEnterTime_;  // per vehicle
    std::vector<TimeMs> lastLeaveTime_;   // per vehicle
    std::vector<uint8_t> vehicleModes_;   // per vehicle: mode id (core/modes.h); 1-3 = TransitMode

    std::vector<std::vector<PersonTrip>> personTrips_;  // per person
    std::vector<uint32_t> driverOfVehicle_;             // per vehicle: personId+1, 0 = none
    std::vector<TransitService> transitServices_;

    // All activities, flat and in event order. Indexed per person by
    // activitiesByPerson_, which buildTripLookups() derives.
    std::vector<ActivityRecord> activities_;

    // Air overlay events, sorted by startMs
    std::vector<OverlayEvent> overlayEvents_;

    // Occupancy: who is in which vehicle when (built by parser, rebuilt at load
    // from serialized trips is NOT possible for pt riders' exact board/alight
    // times, so it is derived from trips at load: teleported trips are skipped,
    // vehicle trips use [departTimeMs, arriveTimeMs]).
    StringInterner vehicleIdStrings_;
    StringInterner personIdStrings_;
    StringInterner actTypeStrings_;
    StringInterner legModeStrings_;
    StringInterner transitLineStrings_;
    StringInterner transitRouteStrings_;

    // Call after populating vehicles_ to compute firstEnterTime_/lastLeaveTime_
    void buildLookupArrays();

    // Call after personTrips_/transitServices_ are final (load or preprocess)
    // to build the transit-service and occupancy lookup maps.
    void buildTripLookups();

private:
    TimeMs minTime_ = 0;
    TimeMs maxTime_ = 0;

    // vehicleId -> index into transitServices_
    std::unordered_map<uint32_t, size_t> transitServiceLookup_;

    // vehicleId -> intervals a person is aboard [board, arrive), from vehicle trips
    struct Occupancy { TimeMs enterMs; TimeMs leaveMs; uint32_t personId; };
    std::unordered_map<uint32_t, std::vector<Occupancy>> occupancy_;

    // personId -> indices into activities_, chronological
    std::unordered_map<uint32_t, std::vector<uint32_t>> activitiesByPerson_;
};

} // namespace simvis
