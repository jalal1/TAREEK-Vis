#include "vehicle_index.h"
#include "core/logger.h"
#include "core/modes.h"
#include <fstream>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <filesystem>

namespace simvis {

// ============================================================================
// Lookup array construction
// ============================================================================

TransitMode VehicleIndex::getVehicleMode(VehicleId id) const {
    return isTransitVehicle(id) ? static_cast<TransitMode>(vehicleModes_[id])
                                : TransitMode::Unknown;
}

bool VehicleIndex::isTransitVehicle(VehicleId id) const {
    if (id >= vehicleModes_.size()) return false;
    return modes::get(vehicleModes_[id]).modeClass == ModeClass::Transit;
}

void VehicleIndex::buildLookupArrays() {
    size_t n = vehicles_.size();
    firstEnterTime_.resize(n);
    lastLeaveTime_.resize(n);
    vehicleModes_.resize(n, 0);  // default: all cars

    minTime_ = std::numeric_limits<TimeMs>::max();
    maxTime_ = 0;

    for (size_t i = 0; i < n; ++i) {
        const auto& traj = vehicles_[i];
        if (traj.segments.empty()) {
            firstEnterTime_[i] = std::numeric_limits<TimeMs>::max();
            lastLeaveTime_[i] = 0;
        } else {
            firstEnterTime_[i] = traj.segments.front().enterTime;
            lastLeaveTime_[i]  = traj.segments.back().leaveTime;

            minTime_ = std::min(minTime_, firstEnterTime_[i]);
            maxTime_ = std::max(maxTime_, lastLeaveTime_[i]);
        }
    }

    if (minTime_ == std::numeric_limits<TimeMs>::max()) {
        minTime_ = 0;
    }
}

// ============================================================================
// Queries
// ============================================================================

const VehicleSegment* VehicleIndex::segmentAt(VehicleId id, TimeMs time) const {
    if (id >= vehicles_.size()) return nullptr;

    const auto& segs = vehicles_[id].segments;
    if (segs.empty()) return nullptr;

    // Quick reject using precomputed bounds
    if (time < firstEnterTime_[id] || time >= lastLeaveTime_[id]) return nullptr;

    // Binary search: find the last segment with enterTime <= time
    // Segments are sorted by enterTime
    auto it = std::upper_bound(segs.begin(), segs.end(), time,
        [](TimeMs t, const VehicleSegment& seg) {
            return t < seg.enterTime;
        });

    if (it == segs.begin()) return nullptr;
    --it;

    // Check that time is within [enterTime, leaveTime)
    if (time >= it->enterTime && time < it->leaveTime) {
        return &(*it);
    }

    return nullptr;
}

void VehicleIndex::getActiveVehicles(TimeMs time, std::vector<VehicleId>& out) const {
    out.clear();

    size_t n = vehicles_.size();
    for (size_t i = 0; i < n; ++i) {
        if (time >= firstEnterTime_[i] && time < lastLeaveTime_[i]) {
            out.push_back(static_cast<VehicleId>(i));
        }
    }
}

const VehicleTrajectory* VehicleIndex::trajectory(VehicleId id) const {
    if (id >= vehicles_.size()) return nullptr;
    return &vehicles_[id];
}

// ============================================================================
// Trip / person queries (v3)
// ============================================================================

void VehicleIndex::buildTripLookups() {
    transitServiceLookup_.clear();
    for (size_t i = 0; i < transitServices_.size(); ++i) {
        // First service wins (a vehicle can run multiple departures on the
        // same line/route; we only need one association for display)
        transitServiceLookup_.emplace(transitServices_[i].vehicleId, i);
    }

    // Occupancy from vehicle trips: person is aboard [boardTimeMs, arriveTimeMs).
    // Not from the departure: on a pt leg that would count the wait at the stop.
    occupancy_.clear();
    for (uint32_t personId = 0; personId < personTrips_.size(); ++personId) {
        for (const auto& trip : personTrips_[personId]) {
            if (trip.vehicleId == 0) continue;  // teleported
            if (trip.arriveTimeMs == TRIP_NO_TIME) continue;  // stuck
            const TimeMs board = trip.boardTimeMs != TRIP_NO_TIME
                ? trip.boardTimeMs : trip.departTimeMs;
            occupancy_[trip.vehicleId - 1].push_back(
                {board, trip.arriveTimeMs, personId});
        }
    }
    for (auto& [vehicleId, list] : occupancy_) {
        std::sort(list.begin(), list.end(),
            [](const Occupancy& a, const Occupancy& b) { return a.enterMs < b.enterMs; });
    }

    // Per-person activity index. activities_ is in event order, so appending as
    // we scan keeps each person's list chronological without a sort.
    activitiesByPerson_.clear();
    for (uint32_t i = 0; i < activities_.size(); ++i) {
        activitiesByPerson_[activities_[i].personId].push_back(i);
    }
}

const std::vector<PersonTrip>* VehicleIndex::personTrips(uint32_t personId) const {
    if (personId >= personTrips_.size()) return nullptr;
    return &personTrips_[personId];
}

const std::vector<uint32_t>* VehicleIndex::activitiesForPerson(uint32_t personId) const {
    auto it = activitiesByPerson_.find(personId);
    if (it == activitiesByPerson_.end()) return nullptr;
    return &it->second;
}

uint32_t VehicleIndex::vehicleDriver(VehicleId id) const {
    if (id >= driverOfVehicle_.size()) return 0;
    return driverOfVehicle_[id];
}

const VehicleIndex::TransitService* VehicleIndex::transitServiceInfo(VehicleId id) const {
    auto it = transitServiceLookup_.find(id);
    if (it == transitServiceLookup_.end()) return nullptr;
    return &transitServices_[it->second];
}

std::vector<uint32_t> VehicleIndex::personsAboard(VehicleId id, TimeMs time) const {
    std::vector<uint32_t> persons;
    auto it = occupancy_.find(id);
    if (it == occupancy_.end()) return persons;
    for (const auto& occ : it->second) {
        if (time >= occ.enterMs && time < occ.leaveMs) persons.push_back(occ.personId);
    }
    return persons;
}

// --- String lookups ---

static QString internedOrEmpty(const StringInterner& interner, uint32_t id) {
    if (id >= interner.size()) return QString();
    return QString::fromStdString(interner.strings()[id]);
}

uint32_t VehicleIndex::findVehicleByOriginalId(const std::string& id) const {
    if (!vehicleIdStrings_.hasString(id)) return UINT32_MAX;
    const uint32_t vid = vehicleIdStrings_.getId(id);
    return vid < vehicles_.size() ? vid : UINT32_MAX;
}

uint32_t VehicleIndex::findPersonByOriginalId(const std::string& id) const {
    if (!personIdStrings_.hasString(id)) return UINT32_MAX;
    const uint32_t pid = personIdStrings_.getId(id);
    return pid < personTrips_.size() ? pid : UINT32_MAX;
}

QString VehicleIndex::vehicleIdString(VehicleId id) const {
    return internedOrEmpty(vehicleIdStrings_, id);
}
QString VehicleIndex::personIdString(uint32_t personId) const {
    return internedOrEmpty(personIdStrings_, personId);
}
QString VehicleIndex::actTypeString(uint16_t id) const {
    if (id == TRIP_NO_ACT) return QString();
    return internedOrEmpty(actTypeStrings_, id);
}
QString VehicleIndex::legModeString(uint16_t id) const {
    return internedOrEmpty(legModeStrings_, id);
}
QString VehicleIndex::transitLineString(uint32_t id) const {
    return internedOrEmpty(transitLineStrings_, id);
}
QString VehicleIndex::transitRouteString(uint32_t id) const {
    return internedOrEmpty(transitRouteStrings_, id);
}

// ============================================================================
// Binary I/O
// ============================================================================

// On-disk per-vehicle entry: offset into packed segment array + segment count
struct TrajectoryEntry {
    uint32_t offset;     // index into packed segments array
    uint32_t count;      // number of segments
};

bool VehicleIndex::writeFile(const QString& path, const VehicleIndex& index) {
    LOG_INFO(QString("VehicleIndex::writeFile: %1 (%2 vehicles)")
        .arg(path).arg(index.vehicles_.size()));

    try {
        std::ofstream out(path.toStdString(), std::ios::binary);
        if (!out) {
            LOG_ERROR(QString("VehicleIndex::writeFile: cannot open %1").arg(path));
            return false;
        }

        uint32_t vehicleCount = static_cast<uint32_t>(index.vehicles_.size());
        uint32_t personCount  = static_cast<uint32_t>(index.personTrips_.size());

        // Count total segments and trips
        uint32_t totalSegments = 0;
        for (const auto& traj : index.vehicles_) {
            totalSegments += static_cast<uint32_t>(traj.segments.size());
        }
        uint32_t totalTrips = 0;
        for (const auto& trips : index.personTrips_) {
            totalTrips += static_cast<uint32_t>(trips.size());
        }
        uint32_t serviceCount = static_cast<uint32_t>(index.transitServices_.size());

        // Calculate section offsets (layout order = write order)
        size_t headerSize = sizeof(VehicleIndexHeader);
        VehicleIndexHeader header{};
        header.magic = VIDX_FILE_MAGIC;
        header.version = VIDX_FILE_VERSION;
        header.vehicleCount = vehicleCount;
        header.totalSegments = totalSegments;
        header.minTimeMs = index.minTime_;
        header.maxTimeMs = index.maxTime_;
        header.personCount = personCount;
        header.totalTrips = totalTrips;
        header.trajectoryTableOffset = headerSize;
        header.segmentDataOffset  = header.trajectoryTableOffset + vehicleCount * sizeof(TrajectoryEntry);
        header.modeTableOffset    = header.segmentDataOffset + totalSegments * sizeof(VehicleSegment);
        header.tripTableOffset    = header.modeTableOffset + vehicleCount;
        header.tripDataOffset     = header.tripTableOffset + personCount * sizeof(TrajectoryEntry);
        header.vehicleDriverOffset = header.tripDataOffset + static_cast<uint64_t>(totalTrips) * sizeof(PersonTrip);
        header.transitServiceOffset = header.vehicleDriverOffset + vehicleCount * sizeof(uint32_t);
        header.stringTablesOffset = header.transitServiceOffset + sizeof(uint32_t)
                                    + serviceCount * sizeof(TransitService);
        // Activities are written last: the string tables are variable-length and
        // read sequentially, so their end offset is not known until they are
        // written. loadFile() seeks here using the count, after the tables.
        header.activityCount = static_cast<uint32_t>(index.activities_.size());
        header.overlayCount = static_cast<uint32_t>(index.overlayEvents_.size());
        header.activityDataOffset = 0;  // patched below, once the tables are out
        header.overlayDataOffset = 0;

        // Write header
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        // Write trajectory table
        uint32_t segOffset = 0;
        for (const auto& traj : index.vehicles_) {
            TrajectoryEntry entry;
            entry.offset = segOffset;
            entry.count = static_cast<uint32_t>(traj.segments.size());
            out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
            segOffset += entry.count;
        }

        // Write packed segment data
        for (const auto& traj : index.vehicles_) {
            if (!traj.segments.empty()) {
                out.write(reinterpret_cast<const char*>(traj.segments.data()),
                          traj.segments.size() * sizeof(VehicleSegment));
            }
        }

        // Write vehicle mode array: one byte per vehicle
        if (!index.vehicleModes_.empty()) {
            out.write(reinterpret_cast<const char*>(index.vehicleModes_.data()),
                      vehicleCount);
        } else {
            std::vector<uint8_t> zeros(vehicleCount, 0);
            out.write(reinterpret_cast<const char*>(zeros.data()), vehicleCount);
        }

        // Write trip table (per person: offset, count into packed trips)
        uint32_t tripOffset = 0;
        for (const auto& trips : index.personTrips_) {
            TrajectoryEntry entry;
            entry.offset = tripOffset;
            entry.count = static_cast<uint32_t>(trips.size());
            out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
            tripOffset += entry.count;
        }

        // Write packed trip data
        for (const auto& trips : index.personTrips_) {
            if (!trips.empty()) {
                out.write(reinterpret_cast<const char*>(trips.data()),
                          trips.size() * sizeof(PersonTrip));
            }
        }

        // Write driver-of-vehicle array (personId+1, 0 = none)
        if (index.driverOfVehicle_.size() == vehicleCount && vehicleCount > 0) {
            out.write(reinterpret_cast<const char*>(index.driverOfVehicle_.data()),
                      vehicleCount * sizeof(uint32_t));
        } else {
            std::vector<uint32_t> zeros(vehicleCount, 0);
            if (vehicleCount > 0) {
                // Copy what we have (array may be shorter if no person events seen)
                std::copy(index.driverOfVehicle_.begin(), index.driverOfVehicle_.end(),
                          zeros.begin());
                out.write(reinterpret_cast<const char*>(zeros.data()),
                          vehicleCount * sizeof(uint32_t));
            }
        }

        // Write transit services (count-prefixed)
        out.write(reinterpret_cast<const char*>(&serviceCount), sizeof(serviceCount));
        if (serviceCount > 0) {
            out.write(reinterpret_cast<const char*>(index.transitServices_.data()),
                      serviceCount * sizeof(TransitService));
        }

        // Write the six string tables in fixed order
        {
            std::ostream& os = out;
            index.vehicleIdStrings_.writeTo(os);
            index.personIdStrings_.writeTo(os);
            index.actTypeStrings_.writeTo(os);
            index.legModeStrings_.writeTo(os);
            index.transitLineStrings_.writeTo(os);
            index.transitRouteStrings_.writeTo(os);
        }

        // Write packed activity data and patch its offset into the header
        uint64_t activityOffset = static_cast<uint64_t>(out.tellp());
        if (!index.activities_.empty()) {
            out.write(reinterpret_cast<const char*>(index.activities_.data()),
                      static_cast<std::streamsize>(
                          index.activities_.size() * sizeof(ActivityRecord)));
        }
        // Overlay events after the activities
        uint64_t overlayOffset = static_cast<uint64_t>(out.tellp());
        if (!index.overlayEvents_.empty()) {
            out.write(reinterpret_cast<const char*>(index.overlayEvents_.data()),
                      static_cast<std::streamsize>(
                          index.overlayEvents_.size() * sizeof(OverlayEvent)));
        }
        out.seekp(offsetof(VehicleIndexHeader, activityDataOffset));
        out.write(reinterpret_cast<const char*>(&activityOffset), sizeof(activityOffset));
        out.seekp(offsetof(VehicleIndexHeader, overlayDataOffset));
        out.write(reinterpret_cast<const char*>(&overlayOffset), sizeof(overlayOffset));

        out.flush();
        bool ok = out.good();
        if (!ok)
            LOG_ERROR(QString("VehicleIndex::writeFile: stream error on %1").arg(path));
        else
            LOG_INFO(QString("VehicleIndex::writeFile: complete (%1 vehicles, %2 segments, "
                             "%3 persons, %4 trips, %5 transit services, %6 activities)")
                .arg(vehicleCount).arg(totalSegments)
                .arg(personCount).arg(totalTrips).arg(serviceCount)
                .arg(index.activities_.size()));
        return ok;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("VehicleIndex::writeFile exception: %1").arg(e.what()));
        return false;
    }
}

bool VehicleIndex::loadFile(const QString& path) {
    try {
        // Log file size for diagnostics
        std::error_code ec;
        auto fsize = std::filesystem::file_size(path.toStdString(), ec);
        if (!ec)
            LOG_INFO(QString("VehicleIndex::loadFile: %1 (%2 bytes)").arg(path).arg(fsize));
        else
            LOG_INFO(QString("VehicleIndex::loadFile: %1").arg(path));

        std::ifstream in(path.toStdString(), std::ios::binary);
        if (!in) {
            LOG_ERROR(QString("VehicleIndex::loadFile: cannot open %1").arg(path));
            return false;
        }

        // Read header
        VehicleIndexHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));

        if (header.magic != VIDX_FILE_MAGIC || header.version != VIDX_FILE_VERSION) {
            LOG_ERROR(QString("VehicleIndex::loadFile: invalid magic/version in %1 "
                              "(magic=0x%2, version=%3, expected version=%4)")
                .arg(path)
                .arg(header.magic, 8, 16, QChar('0'))
                .arg(header.version).arg(VIDX_FILE_VERSION));
            return false;
        }

        // Sanity check before large allocations
        constexpr uint32_t kMaxVehicles  = 10'000'000;
        constexpr uint32_t kMaxSegments  = 500'000'000;
        constexpr uint32_t kMaxPersons   = 50'000'000;
        constexpr uint32_t kMaxTrips     = 500'000'000;
        if (header.vehicleCount > kMaxVehicles || header.totalSegments > kMaxSegments ||
            header.personCount > kMaxPersons || header.totalTrips > kMaxTrips) {
            LOG_ERROR(QString("VehicleIndex::loadFile: counts too large "
                              "(vehicles=%1, segments=%2, persons=%3, trips=%4) in %5")
                .arg(header.vehicleCount).arg(header.totalSegments)
                .arg(header.personCount).arg(header.totalTrips).arg(path));
            return false;
        }

        // Read trajectory table
        in.seekg(header.trajectoryTableOffset);
        std::vector<TrajectoryEntry> entries(header.vehicleCount);
        in.read(reinterpret_cast<char*>(entries.data()),
                header.vehicleCount * sizeof(TrajectoryEntry));

        // Read all segments — chunked to avoid >2GB single-read failures on Windows
        in.seekg(header.segmentDataOffset);
        std::vector<VehicleSegment> allSegments(header.totalSegments);
        {
            constexpr size_t kChunkBytes = 512 * 1024 * 1024; // 512 MB
            size_t totalBytes = static_cast<size_t>(header.totalSegments) * sizeof(VehicleSegment);
            char* dst = reinterpret_cast<char*>(allSegments.data());
            size_t remaining = totalBytes;
            while (remaining > 0 && in.good()) {
                size_t toRead = std::min(remaining, kChunkBytes);
                in.read(dst, static_cast<std::streamsize>(toRead));
                dst += toRead;
                remaining -= toRead;
            }
        }

        if (!in.good()) {
            LOG_ERROR(QString("VehicleIndex::loadFile: read error after segments in %1").arg(path));
            return false;
        }

        // Distribute segments into per-vehicle trajectories
        vehicles_.resize(header.vehicleCount);
        for (uint32_t i = 0; i < header.vehicleCount; ++i) {
            const auto& e = entries[i];
            // Bounds check
            if (e.offset + e.count > header.totalSegments) {
                LOG_ERROR(QString("VehicleIndex::loadFile: corrupt entry[%1] "
                                  "offset=%2 count=%3 exceeds totalSegments=%4 in %5")
                    .arg(i).arg(e.offset).arg(e.count).arg(header.totalSegments).arg(path));
                return false;
            }
            auto& traj = vehicles_[i];
            if (e.count > 0) {
                traj.segments.assign(
                    allSegments.begin() + e.offset,
                    allSegments.begin() + e.offset + e.count);
            }
        }

        // Build lookup arrays from loaded data
        buildLookupArrays();

        // Read vehicle mode array
        in.seekg(header.modeTableOffset);
        vehicleModes_.resize(header.vehicleCount, 0);
        in.read(reinterpret_cast<char*>(vehicleModes_.data()), header.vehicleCount);

        // Read trip table + packed trips
        in.seekg(header.tripTableOffset);
        std::vector<TrajectoryEntry> tripEntries(header.personCount);
        if (header.personCount > 0) {
            in.read(reinterpret_cast<char*>(tripEntries.data()),
                    header.personCount * sizeof(TrajectoryEntry));
        }

        in.seekg(header.tripDataOffset);
        std::vector<PersonTrip> allTrips(header.totalTrips);
        if (header.totalTrips > 0) {
            in.read(reinterpret_cast<char*>(allTrips.data()),
                    static_cast<std::streamsize>(
                        static_cast<size_t>(header.totalTrips) * sizeof(PersonTrip)));
        }

        personTrips_.resize(header.personCount);
        for (uint32_t i = 0; i < header.personCount; ++i) {
            const auto& e = tripEntries[i];
            if (static_cast<uint64_t>(e.offset) + e.count > header.totalTrips) {
                LOG_ERROR(QString("VehicleIndex::loadFile: corrupt trip entry[%1] in %2")
                    .arg(i).arg(path));
                return false;
            }
            if (e.count > 0) {
                personTrips_[i].assign(allTrips.begin() + e.offset,
                                       allTrips.begin() + e.offset + e.count);
            }
        }

        // Read driver-of-vehicle array
        in.seekg(header.vehicleDriverOffset);
        driverOfVehicle_.resize(header.vehicleCount, 0);
        if (header.vehicleCount > 0) {
            in.read(reinterpret_cast<char*>(driverOfVehicle_.data()),
                    header.vehicleCount * sizeof(uint32_t));
        }

        // Read transit services (count-prefixed)
        in.seekg(header.transitServiceOffset);
        uint32_t serviceCount = 0;
        in.read(reinterpret_cast<char*>(&serviceCount), sizeof(serviceCount));
        constexpr uint32_t kMaxServices = 10'000'000;
        if (serviceCount > kMaxServices) {
            LOG_ERROR(QString("VehicleIndex::loadFile: service count too large (%1) in %2")
                .arg(serviceCount).arg(path));
            return false;
        }
        transitServices_.resize(serviceCount);
        if (serviceCount > 0) {
            in.read(reinterpret_cast<char*>(transitServices_.data()),
                    serviceCount * sizeof(TransitService));
        }

        // Read the six string tables (sequential from stringTablesOffset)
        if (!in.good()) {
            LOG_ERROR(QString("VehicleIndex::loadFile: read error before string tables in %1")
                .arg(path));
            return false;
        }
        in.seekg(header.stringTablesOffset);
        vehicleIdStrings_.readFrom(in);
        personIdStrings_.readFrom(in);
        actTypeStrings_.readFrom(in);
        legModeStrings_.readFrom(in);
        transitLineStrings_.readFrom(in);
        transitRouteStrings_.readFrom(in);

        if (!in.good() && !in.eof()) {
            LOG_ERROR(QString("VehicleIndex::loadFile: read error in string tables in %1")
                .arg(path));
            return false;
        }

        // Read packed activities (v4). Written after the string tables, so the
        // stream may have hit EOF above — clear it before seeking.
        if (header.activityCount > 0) {
            constexpr uint32_t kMaxActivities = 500'000'000;
            if (header.activityCount > kMaxActivities || header.activityDataOffset == 0) {
                LOG_ERROR(QString("VehicleIndex::loadFile: bad activity table "
                                  "(count=%1, offset=%2) in %3")
                    .arg(header.activityCount).arg(header.activityDataOffset).arg(path));
                return false;
            }
            in.clear();
            in.seekg(static_cast<std::streamoff>(header.activityDataOffset));
            activities_.resize(header.activityCount);
            in.read(reinterpret_cast<char*>(activities_.data()),
                    static_cast<std::streamsize>(
                        static_cast<size_t>(header.activityCount) * sizeof(ActivityRecord)));
            if (in.gcount() != static_cast<std::streamsize>(
                    static_cast<size_t>(header.activityCount) * sizeof(ActivityRecord))) {
                LOG_ERROR(QString("VehicleIndex::loadFile: short read on activities in %1")
                    .arg(path));
                return false;
            }
        }

        // Read packed overlay events (v6), after the activities
        overlayEvents_.clear();
        if (header.overlayCount > 0) {
            constexpr uint32_t kMaxOverlays = 100'000'000;
            if (header.overlayCount > kMaxOverlays || header.overlayDataOffset == 0) {
                LOG_ERROR(QString("VehicleIndex::loadFile: bad overlay table "
                                  "(count=%1, offset=%2) in %3")
                    .arg(header.overlayCount).arg(header.overlayDataOffset).arg(path));
                return false;
            }
            in.clear();
            in.seekg(static_cast<std::streamoff>(header.overlayDataOffset));
            overlayEvents_.resize(header.overlayCount);
            const auto bytes = static_cast<std::streamsize>(
                static_cast<size_t>(header.overlayCount) * sizeof(OverlayEvent));
            in.read(reinterpret_cast<char*>(overlayEvents_.data()), bytes);
            if (in.gcount() != bytes) {
                LOG_ERROR(QString("VehicleIndex::loadFile: short read on overlays in %1")
                    .arg(path));
                return false;
            }
        }

        // Build lookup maps for transit services and occupancy
        buildTripLookups();

        LOG_INFO(QString("VehicleIndex loaded: %1 vehicles, %2 segments, %3 persons, "
                         "%4 trips, %5 transit services, %6 activities, t=[%7s,%8s]")
            .arg(header.vehicleCount)
            .arg(header.totalSegments)
            .arg(header.personCount)
            .arg(header.totalTrips)
            .arg(serviceCount)
            .arg(header.activityCount)
            .arg(toSeconds(header.minTimeMs), 0, 'f', 1)
            .arg(toSeconds(header.maxTimeMs), 0, 'f', 1));
        return true;

    } catch (const std::bad_alloc& e) {
        LOG_ERROR(QString("VehicleIndex::loadFile out-of-memory: %1").arg(e.what()));
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR(QString("VehicleIndex::loadFile exception: %1").arg(e.what()));
        return false;
    }
}

uint32_t VehicleIndex::fileVersion(const QString& path) {
    std::ifstream in(path.toStdString(), std::ios::binary);
    if (!in) return 0;
    uint32_t magic = 0, version = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in.good() || magic != VIDX_FILE_MAGIC) return 0;
    return version;
}

} // namespace simvis
