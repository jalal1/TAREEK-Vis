// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include "data/vehicle_index.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace simvis {

class NetworkIndex;

// Converts our directed MATSim network into the undirected edge graph a density
// computation works on, and places activities along its edges.
//
// Two conversions happen here, and each one can hide a mistake:
//
//  1. MATSim networks are directed and usually hold both A->B and B->A. Network
//     distance is symmetric, so reciprocal pairs merge into one undirected
//     edge. The link that is merged away is the "reversed" one, and any point
//     on it must have its offset flipped to (length - offset).
//
//  2. An activity has a true (x, y). Its offset along the edge comes from
//     projecting that point onto the link segment. We use the link named by the
//     event itself, so no nearest-link search is needed.
class NkdvNetwork {
public:
    // One piece of road carrying one density value, already in world
    // coordinates. Written to the heatmap cache as raw bytes, so the layout
    // must stay put: changing it means raising the cache file version.
    struct Lixel {
        float x, y;        // midpoint of the lixel, in the network CRS
        float value;       // density, in activities per metre
    };
    static_assert(sizeof(Lixel) == 12, "Lixel must be 12 bytes");

    // Build the undirected edge graph from a loaded network. Returns false and
    // fills `error` if the network is empty.
    bool build(const NetworkIndex& network, std::string& error);

    size_t nodeCount() const { return nodeCount_; }
    size_t edgeCount() const { return edges_.size(); }

    // A read-only view of one undirected edge, for a density engine that works
    // on the graph in memory rather than through a file.
    struct EdgeView {
        uint32_t n1, n2;
        float length;
        uint32_t linkId;
    };
    EdgeView edge(size_t index) const {
        const Edge& e = edges_[index];
        return EdgeView{e.n1, e.n2, e.length, e.linkId};
    }

    // Place each activity on its edge, as a distance from that edge's n1.
    // Returns one list per edge, indexed as edge() is. Activities whose link
    // the network does not know are counted in `skipped`.
    //
    // The offsets are what a density engine sums the kernel over.
    std::vector<std::vector<float>> placePoints(
        const std::vector<ActivityRecord>& points,
        const NetworkIndex& network,
        size_t& placed, size_t& skipped) const;

    // Total length of all undirected edges, in meters. The number of lixels
    // is about this divided by the lixel size, so it predicts the output size.
    double totalLength() const;

private:
    // One undirected edge, referencing the directed link it came from.
    struct Edge {
        uint32_t n1;         // dense node index
        uint32_t n2;
        float length;        // meters
        uint32_t linkId;     // the directed link supplying the geometry
    };

    // Where a directed link ended up after merging.
    struct LinkMapping {
        uint32_t edgeIndex;
        bool reversed;       // link runs n2 -> n1, so offsets must be flipped
    };

    // Project a point onto a link, returning the distance from the link's
    // from-node along the segment, clamped to the link's length.
    static double offsetAlongLink(const NetworkIndex& network,
                                  const LinkRecord& link,
                                  double x, double y);

    std::vector<Edge> edges_;
    std::unordered_map<uint32_t, LinkMapping> linkToEdge_;

    // Node ids are remapped to a dense 0..n-1 range, which is what the density
    // engine indexes its per-node arrays by.
    std::unordered_map<uint32_t, uint32_t> nodeToDenseIndex_;
    size_t nodeCount_ = 0;
};

} // namespace simvis
