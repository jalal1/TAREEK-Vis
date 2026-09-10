#pragma once

#include "analysis/nkdv_network.h"

#include <QString>
#include <atomic>
#include <vector>

namespace simvis {

class NetworkIndex;

// Network kernel density in the standard, lixel-driven ("gather") direction.
//
// This exists to be measured, not to be shipped: NkdeScatter computes the same
// map and is what the application uses. The textbook formulation asks, for each
// lixel, "which activities can reach me?", which needs a bounded search from
// every lixel. That is the cost NkdeScatter's inversion is meant to avoid, and
// a claim about avoiding it is worth more measured than argued.
//
// Everything except the loop direction is deliberately shared with the scatter
// form: the same uniquePlaces() merge, the same placement onto edges, the same
// Gaussian, the same 3-bandwidth cutoff, the same lixel layout. Both therefore
// produce the same density values up to floating-point summation order, which
// verify() checks. Any runtime difference is attributable to direction alone.
//
// One search per lixel would repeat the same work for every lixel on an edge,
// which no sensible implementation of this form does. We search once per edge
// endpoint and reuse it across that edge's lixels -- the standard optimization,
// and the same accounting the scatter side gets. The comparison is therefore
// against a fair baseline rather than a straw man.
class NkdeGather {
public:
    struct Params {
        int lixelLength = 25;
        double bandwidth = 500.0;
    };

    struct Result {
        bool success = false;
        QString errorMessage;

        std::vector<NkdvNetwork::Lixel> lixels;
        size_t lixelsTotal = 0;

        size_t pointsInput = 0;
        size_t pointsMerged = 0;
        size_t pointsPlaced = 0;
        size_t pointsSkipped = 0;

        // Edges searched from (every edge holding at least one lixel, i.e. all
        // of them) and edge-visits made while collecting reachable points. The
        // scatter form's counterparts are sourceEdges and consideredEdges.
        size_t searchedEdges = 0;
        size_t consideredEdges = 0;

        // Bounded Dijkstra runs. This is the quantity the inversion changes:
        // two per edge here, two per *occupied* edge in the scatter form.
        size_t searches = 0;

        double seconds = 0.0;
    };

    static Result run(const NkdvNetwork& net,
                      const NetworkIndex& network,
                      const std::vector<ActivityRecord>& points,
                      const Params& params,
                      const std::atomic<bool>* cancelled = nullptr);

    // Largest absolute and relative disagreement between two maps of the same
    // scenario, for confirming the two directions agree. Lixels are compared in
    // layout order, which both forms produce identically.
    struct Agreement {
        bool comparable = false;
        size_t compared = 0;
        double maxAbsolute = 0.0;
        double maxRelative = 0.0;
    };
    static Agreement verify(const std::vector<NkdvNetwork::Lixel>& a,
                            const std::vector<NkdvNetwork::Lixel>& b);

    static double zeroCutoff(double bandwidth) { return 3.0 * bandwidth; }
};

} // namespace simvis
