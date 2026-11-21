// SPDX-License-Identifier: GPL-3.0-or-later

#include "nkdv_network.h"

#include "core/logger.h"
#include "data/network_index.h"

#include <QString>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace simvis {

namespace {

// Key for one undirected node pair, so A->B and B->A collide.
inline uint64_t undirectedKey(uint32_t a, uint32_t b) {
    uint32_t lo = std::min(a, b);
    uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

} // namespace

bool NkdvNetwork::build(const NetworkIndex& network, std::string& error) {
    edges_.clear();
    linkToEdge_.clear();
    nodeToDenseIndex_.clear();
    nodeCount_ = 0;

    const auto& links = network.allLinks();
    if (links.empty()) {
        error = "network has no links";
        return false;
    }

    // Merge reciprocal directed links into one undirected edge. The first link
    // seen for a node pair supplies the geometry and direction; its reciprocal
    // maps to the same edge, flagged reversed.
    std::unordered_map<uint64_t, uint32_t> pairToEdge;
    pairToEdge.reserve(links.size());
    edges_.reserve(links.size());
    linkToEdge_.reserve(links.size());

    auto denseIndexOf = [&](uint32_t nodeId) -> uint32_t {
        auto it = nodeToDenseIndex_.find(nodeId);
        if (it != nodeToDenseIndex_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(nodeToDenseIndex_.size());
        nodeToDenseIndex_.emplace(nodeId, idx);
        return idx;
    };

    size_t merged = 0;
    for (const auto& link : links) {
        // A self-loop has zero extent in the undirected graph and would make
        // the shortest-path search do pointless work.
        if (link.fromNode == link.toNode) continue;

        uint64_t key = undirectedKey(link.fromNode, link.toNode);
        auto found = pairToEdge.find(key);
        if (found != pairToEdge.end()) {
            // Reciprocal of an edge we already made. Whether this link is
            // reversed depends on which way the stored edge runs.
            const Edge& existing = edges_[found->second];
            bool reversed = denseIndexOf(link.fromNode) != existing.n1;
            linkToEdge_.emplace(link.id, LinkMapping{found->second, reversed});
            ++merged;
            continue;
        }

        Edge edge{};
        edge.n1 = denseIndexOf(link.fromNode);
        edge.n2 = denseIndexOf(link.toNode);
        edge.length = link.length;
        edge.linkId = link.id;

        // Placing lixels divides by edge length, so a zero-length edge would
        // divide by zero.
        if (!(edge.length > 0.0f)) continue;

        uint32_t edgeIndex = static_cast<uint32_t>(edges_.size());
        edges_.push_back(edge);
        pairToEdge.emplace(key, edgeIndex);
        linkToEdge_.emplace(link.id, LinkMapping{edgeIndex, false});
    }

    nodeCount_ = nodeToDenseIndex_.size();

    LOG_INFO(QString("NkdvNetwork: %1 directed links -> %2 undirected edges "
                     "(%3 reciprocal pairs merged), %4 nodes, %5 km")
        .arg(links.size()).arg(edges_.size()).arg(merged)
        .arg(nodeCount_).arg(totalLength() / 1000.0, 0, 'f', 1));

    if (edges_.empty()) {
        error = "no usable edges after merging";
        return false;
    }
    return true;
}

double NkdvNetwork::totalLength() const {
    double total = 0.0;
    for (const auto& e : edges_) total += e.length;
    return total;
}

double NkdvNetwork::offsetAlongLink(const NetworkIndex& network,
                                    const LinkRecord& link,
                                    double x, double y) {
    const auto* from = network.getNode(link.fromNode);
    const auto* to = network.getNode(link.toNode);
    if (!from || !to) return 0.0;

    double ax = from->x, ay = from->y;
    double bx = to->x,   by = to->y;
    double dx = bx - ax, dy = by - ay;
    double lenSq = dx * dx + dy * dy;
    if (lenSq <= 0.0) return 0.0;

    // Scalar projection of (point - a) onto (b - a), as a fraction of the
    // segment, clamped so a point beside the link lands on it.
    double t = ((x - ax) * dx + (y - ay) * dy) / lenSq;
    t = std::clamp(t, 0.0, 1.0);

    // Scale by the link's recorded length, not the straight-line node distance:
    // MATSim link lengths often exceed the node separation.
    return t * static_cast<double>(link.length);
}

std::vector<std::vector<float>> NkdvNetwork::placePoints(
    const std::vector<ActivityRecord>& points,
    const NetworkIndex& network,
    size_t& placed, size_t& skipped) const {

    placed = 0;
    skipped = 0;

    std::vector<std::vector<float>> pointsPerEdge(edges_.size());
    for (const auto& act : points) {
        if (act.linkId == TRIP_NO_LINK) { ++skipped; continue; }
        auto it = linkToEdge_.find(act.linkId);
        if (it == linkToEdge_.end()) { ++skipped; continue; }
        const auto* link = network.getLink(act.linkId);
        if (!link) { ++skipped; continue; }

        const Edge& edge = edges_[it->second.edgeIndex];
        double offset = offsetAlongLink(network, *link, act.x, act.y);

        // The offset is measured from the link's own from-node. When the link
        // was merged in as the reciprocal, that is the edge's n2, so flip it.
        if (it->second.reversed) offset = static_cast<double>(edge.length) - offset;
        offset = std::clamp(offset, 0.0, static_cast<double>(edge.length));

        pointsPerEdge[it->second.edgeIndex].push_back(static_cast<float>(offset));
        ++placed;
    }
    return pointsPerEdge;
}

} // namespace simvis
