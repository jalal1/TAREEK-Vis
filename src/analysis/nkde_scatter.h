#pragma once

#include "analysis/nkdv_network.h"

#include <QString>
#include <atomic>
#include <string>
#include <vector>

namespace simvis {

class NetworkIndex;

// Network kernel density, computed by scattering from the activities outwards.
//
// The textbook formulation asks, for every lixel, "which activities can reach
// me?" That suits data where activities outnumber road segments. MATSim output
// is the other way round: across four metropolitan scenarios, 64.6-99.6 % of
// edges carry no activity of a given type at all, yet a per-lixel method
// searches from every one of them.
//
// This asks the reverse question. For each edge that HAS activities, one
// bounded search finds the lixels within reach, and the kernel is added to
// each. Network distance is symmetric on an undirected graph, so every lixel
// ends up with the same sum; only the loop order changes. On Twin Cities Other
// that is 26,954 searches instead of 692,704.
//
// NkdeGather implements the other direction, for measurement. The two agree
// exactly, and the inversion is worth 1.9-14.1x -- far less than the ratio of
// search counts, because the summing term is common to both.
//
// The kernel is summed directly, with no approximation: density at a lixel is
// sum over activities of exp(-d^2 / b^2), where d is the shortest path distance
// along the network. Past `zeroCutoff` the term is small enough to treat as
// zero, which is what bounds each search.
class NkdeScatter {
public:
    struct Params {
        // Length of one lixel, in meters. Shorter gives a finer map and more
        // output; it does not change the density values themselves.
        int lixelLength = 25;

        // Gaussian bandwidth, in meters. Sets how far one activity's influence
        // reaches, and with it the search cutoff.
        double bandwidth = 500.0;
    };

    struct Result {
        bool success = false;
        QString errorMessage;

        // Lixels with a non-zero density. Lixels the kernel never reached are
        // dropped, so this is smaller than the network's total lixel count --
        // see `lixelsTotal`, which is the figure a cost argument wants.
        std::vector<NkdvNetwork::Lixel> lixels;

        // Lixels the network divides into, whatever their density. Depends only
        // on network length and lixel length, so it is the same for every
        // activity type in a scenario.
        size_t lixelsTotal = 0;

        // Activity records supplied, and how many of them were repeat visits
        // by one person to one place, merged away before placement.
        size_t pointsInput = 0;
        size_t pointsMerged = 0;

        // Activities placed on the network, and those whose link was unknown.
        size_t pointsPlaced = 0;
        size_t pointsSkipped = 0;

        // Edges that carried at least one activity, and edges considered after
        // pruning. The ratio is why this method is fast on our data.
        size_t sourceEdges = 0;
        size_t consideredEdges = 0;

        double seconds = 0.0;
    };

    // Collapse repeated visits by one person to one place into a single point.
    //
    // A density map answers "where does this activity happen?", so each place
    // a person uses must contribute once. MATSim splits a day into episodes, so
    // an agent who leaves home in the morning and returns in the evening writes
    // two Home records at the identical coordinate. If both are counted, that
    // home is weighted twice. On Twin Cities this applies to 49.2% of Home
    // records and 8.3% of Work records.
    //
    // Only exact repeats by the same person at the same place are merged. A
    // person who shops at two different shops keeps both points, because those
    // are two places and the map must show both.
    static std::vector<ActivityRecord> uniquePlaces(
        const std::vector<ActivityRecord>& points);

    // Compute one density map. `cancelled` is polled between searches, so a
    // long run can be abandoned when the scenario changes or the window closes.
    // Passing nullptr uses this class's own flag, below.
    static Result run(const NkdvNetwork& net,
                      const NetworkIndex& network,
                      const std::vector<ActivityRecord>& points,
                      const Params& params,
                      const std::atomic<bool>* cancelled = nullptr);

    // Ask every running and queued computation to stop. A run notices between
    // source edges, so it stops in milliseconds rather than at the end. Call it
    // when closing the window or changing scenario.
    static void cancelAll();

    // True once cancelAll() has been called. Callers use it to drop a result
    // instead of acting on it.
    static bool cancelled();

    // Clear the cancelled state, so later runs can start.
    static void resetCancel();

    // The flag itself, for run() to poll.
    static const std::atomic<bool>* cancelFlag();

    // Distance past which the Gaussian is treated as zero. Beyond 3 bandwidths
    // the kernel is below 1.3e-4 of its peak, which cannot move a color step.
    static double zeroCutoff(double bandwidth) { return 3.0 * bandwidth; }
};

} // namespace simvis
