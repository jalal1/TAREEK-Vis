#pragma once

#include "core/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace simvis {

class VehicleIndex;

// ============================================================================
// Per-link, per-time-bin sums of the vehicle segments (roadmap item 9).
//
// The table is made one time from the vehicle index and kept in the cache.
// Every link metric (flow, speed, delay, density, v/c, queue) and the hourly
// volume chart come from these sums. See docs/link-colors.md for the layout
// decision: one sorted list of links per bin ("bin-major"), because the map
// needs all links of one bin each time the time moves into a new bin.
//
// No Qt GUI and no OpenGL: tareek-bench and tests use it too.
// ============================================================================

// The sums of one link in one bin. A vehicle "enters" in the bin of its
// enter time; its travel time counts in that bin too, as in the MATSim
// linkstats. The vehicle time and the queue time are split over the bins
// that the vehicle is on the link.
struct LinkBinEntry {
    uint32_t link;       // link id (index into NetworkIndex::allLinks())
    uint32_t enters;     // vehicles that entered the link in this bin
    float travelS;       // sum of the link travel times of those vehicles, s
    float vehicleS;      // vehicle-seconds on the link in this bin
    float queueS;        // vehicle-seconds after the free-flow time, in this bin
};
static_assert(sizeof(LinkBinEntry) == 20, "LinkBinEntry must be 20 bytes");

// Sums over several bins, for a time window or a chart
struct LinkSums {
    uint32_t enters = 0;
    double travelS = 0.0;
    double vehicleS = 0.0;
    double queueS = 0.0;

    void add(const LinkBinEntry& e) {
        enters += e.enters;
        travelS += e.travelS;
        vehicleS += e.vehicleS;
        queueS += e.queueS;
    }
    bool empty() const { return enters == 0 && vehicleS <= 0.0; }
};

class LinkTimeTable {
public:
    // What the cache file must match: the .vidx it was made from and the
    // network (the free-flow times decide the queue time)
    struct Source {
        uint64_t vidxBytes = 0;
        int64_t vidxModifiedMs = 0;
        uint32_t vidxVersion = 0;
        uint64_t networkHash = 0;   // networkHash() of the links
    };
    static uint64_t networkHash(const std::vector<LinkRecord>& links);

    // Makes the table. Uses all hardware threads (threads = 0) or the given
    // number. binSeconds: 900 (decision 1).
    static LinkTimeTable build(const VehicleIndex& vehicles,
                               const std::vector<LinkRecord>& links,
                               uint32_t binSeconds, int threads = 0);

    bool save(const std::string& path, const Source& source) const;
    // False when the file is missing, of another version or of another source
    bool load(const std::string& path, const Source& source);

    bool empty() const { return bins_.empty(); }
    uint32_t binSeconds() const { return binSeconds_; }
    uint32_t binCount() const { return static_cast<uint32_t>(bins_.size()); }
    uint32_t linkCount() const { return linkCount_; }
    // The bin that holds a time of the simulation (clamped to the table)
    uint32_t binOf(double seconds) const;

    // The entries of one bin, sorted by link
    const std::vector<LinkBinEntry>& bin(uint32_t b) const { return bins_[b]; }
    // The entry of one link in one bin, or nullptr
    const LinkBinEntry* find(uint32_t link, uint32_t b) const;
    // The sums of one link in each bin (binCount() entries)
    std::vector<LinkSums> linkSeries(uint32_t link) const;

    size_t entryCount() const;
    size_t memoryBytes() const;
    double buildSeconds() const { return buildSeconds_; }

private:
    uint32_t binSeconds_ = 0;
    uint32_t linkCount_ = 0;
    std::vector<std::vector<LinkBinEntry>> bins_;
    double buildSeconds_ = 0.0;
};

} // namespace simvis
