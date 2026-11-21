// SPDX-License-Identifier: GPL-3.0-or-later

#include "nkde_scatter.h"

#include "core/logger.h"
#include "data/network_index.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_set>

namespace simvis {

namespace {

// A lixel, before it is given world coordinates: which edge it sits on and how
// far along that edge its midpoint is.
struct LixelSlot {
    uint32_t edgeIndex;
    float    distFromN1;
};

constexpr double kInfinity = std::numeric_limits<double>::max();

// Set when the application is closing or the scenario changes, so a run in
// progress gives up instead of holding the process open.
std::atomic<bool> g_cancelled{false};

} // namespace

std::vector<ActivityRecord> NkdeScatter::uniquePlaces(
        const std::vector<ActivityRecord>& points) {
    // Key on the person and the exact stored coordinate. Coordinates come
    // from the event attributes unchanged, so the two episodes of one home
    // carry bit-identical floats and compare equal without a tolerance.
    // Comparing bit patterns also keeps this exact rather than distance-based:
    // two nearby but distinct places are never merged.
    struct Key {
        uint32_t person;
        uint32_t x;
        uint32_t y;
        bool operator==(const Key& o) const {
            return person == o.person && x == o.x && y == o.y;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = k.person;
            h = h * 31 + k.x;
            h = h * 31 + k.y;
            return h;
        }
    };

    std::vector<ActivityRecord> unique;
    unique.reserve(points.size());
    std::unordered_set<Key, KeyHash> seen;
    seen.reserve(points.size() * 2);

    for (const auto& act : points) {
        Key k{act.personId, 0, 0};
        std::memcpy(&k.x, &act.x, sizeof(k.x));
        std::memcpy(&k.y, &act.y, sizeof(k.y));
        if (seen.insert(k).second) unique.push_back(act);
    }
    return unique;
}

void NkdeScatter::cancelAll() { g_cancelled.store(true); }
bool NkdeScatter::cancelled() { return g_cancelled.load(); }
void NkdeScatter::resetCancel() { g_cancelled.store(false); }
const std::atomic<bool>* NkdeScatter::cancelFlag() { return &g_cancelled; }

NkdeScatter::Result NkdeScatter::run(const NkdvNetwork& net,
                                     const NetworkIndex& network,
                                     const std::vector<ActivityRecord>& points,
                                     const Params& params,
                                     const std::atomic<bool>* cancelled) {
    Result result;
    QElapsedTimer timer;
    timer.start();

    if (cancelled == nullptr) cancelled = cancelFlag();

    const size_t edgeCount = net.edgeCount();
    const size_t nodeCount = net.nodeCount();
    if (edgeCount == 0 || nodeCount == 0) {
        result.errorMessage = QObject::tr("The network has no edges to compute on.");
        return result;
    }

    // One place per person, not one per episode. An agent that returns home in
    // the evening writes a second Home record at the same coordinate, and
    // counting both would weight that home twice. See uniquePlaces().
    const std::vector<ActivityRecord> places = uniquePlaces(points);
    result.pointsInput = points.size();
    result.pointsMerged = points.size() - places.size();

    // Place the activities. This is the same placement the file-based path
    // uses, so both engines see identical input.
    const std::vector<std::vector<float>> pointsPerEdge =
        net.placePoints(places, network, result.pointsPlaced, result.pointsSkipped);
    if (result.pointsPlaced == 0) {
        result.errorMessage =
            QObject::tr("No activities could be placed on the network, so "
                        "there is nothing to compute.");
        return result;
    }

    const double cutoff = zeroCutoff(params.bandwidth);
    const double lixelLength = static_cast<double>(params.lixelLength);
    const double invBandwidthSq = 1.0 / (params.bandwidth * params.bandwidth);

    // Adjacency, and the edges meeting at each node. The second is how a search
    // finds the edges it reached without scanning the whole network.
    std::vector<std::vector<std::pair<uint32_t, float>>> adjacency(nodeCount);
    std::vector<std::vector<uint32_t>> nodeEdges(nodeCount);
    for (size_t e = 0; e < edgeCount; ++e) {
        const auto edge = net.edge(e);
        adjacency[edge.n1].push_back({edge.n2, edge.length});
        adjacency[edge.n2].push_back({edge.n1, edge.length});
        nodeEdges[edge.n1].push_back(static_cast<uint32_t>(e));
        nodeEdges[edge.n2].push_back(static_cast<uint32_t>(e));
    }

    // Lay out the lixels: each edge is cut into pieces of `lixelLength`, with a
    // shorter remainder at the end, and each piece contributes its midpoint.
    std::vector<LixelSlot> lixelSlots;
    std::vector<size_t> firstLixelOfEdge(edgeCount + 1, 0);
    lixelSlots.reserve(static_cast<size_t>(net.totalLength() / lixelLength) + edgeCount);
    for (size_t e = 0; e < edgeCount; ++e) {
        firstLixelOfEdge[e] = lixelSlots.size();
        const double length = net.edge(e).length;
        double cursor = 0.0;
        while (cursor < length) {
            double next = cursor + lixelLength;
            if (next > length) next = length;
            lixelSlots.push_back(LixelSlot{static_cast<uint32_t>(e),
                                      static_cast<float>((cursor + next) * 0.5)});
            cursor += lixelLength;
        }
    }
    firstLixelOfEdge[edgeCount] = lixelSlots.size();
    result.lixelsTotal = lixelSlots.size();

    std::vector<double> density(lixelSlots.size(), 0.0);

    // Search state. These persist across searches and are reset by a stamp
    // rather than by rewriting them: a search reaches a few hundred nodes out
    // of hundreds of thousands, so clearing the whole array would cost far more
    // than the search itself.
    std::vector<double>   nodeDist(nodeCount, 0.0);
    std::vector<uint32_t> nodeStamp(nodeCount, 0);
    std::vector<double>   endpointDist[2] = {std::vector<double>(nodeCount, 0.0),
                                             std::vector<double>(nodeCount, 0.0)};
    std::vector<uint32_t> endpointStamp[2] = {std::vector<uint32_t>(nodeCount, 0),
                                              std::vector<uint32_t>(nodeCount, 0)};
    std::vector<uint32_t> edgeStampSeen(edgeCount, 0);
    std::vector<uint32_t> nodeStampSeen(nodeCount, 0);
    uint32_t stamp = 0;
    uint32_t candidateStamp = 0;

    std::vector<uint32_t> reached;      // nodes one search settled
    std::vector<uint32_t> reachedBoth;  // nodes either search settled
    std::vector<uint32_t> candidates;   // edges either search reached

    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;

    for (size_t sourceEdge = 0; sourceEdge < edgeCount; ++sourceEdge) {
        const std::vector<float>& sourcePoints = pointsPerEdge[sourceEdge];
        if (sourcePoints.empty()) continue;
        ++result.sourceEdges;

        if (cancelled && cancelled->load()) {
            result.errorMessage = QObject::tr("Cancelled");
            return result;
        }

        const auto source = net.edge(sourceEdge);
        const double sourceLength = source.length;
        const uint32_t endpoints[2] = {source.n1, source.n2};

        reachedBoth.clear();

        // One bounded search from each end of the source edge. A point sitting
        // `offset` along the edge is (offset) from n1 and (length - offset)
        // from n2, so these two searches describe every point on it.
        for (int side = 0; side < 2; ++side) {
            ++stamp;
            reached.clear();

            const uint32_t start = endpoints[side];
            nodeStamp[start] = stamp;
            nodeDist[start] = 0.0;
            reached.push_back(start);
            queue.push({0.0, start});

            while (!queue.empty()) {
                const auto [distance, node] = queue.top();
                queue.pop();
                if (nodeStamp[node] != stamp || distance > nodeDist[node]) continue;

                for (const auto& [neighbor, weight] : adjacency[node]) {
                    const double next = distance + weight;
                    // Past the cutoff the kernel cannot move a color step, so
                    // the search stops rather than crossing the whole network.
                    if (next > cutoff) continue;
                    if (nodeStamp[neighbor] != stamp) {
                        nodeStamp[neighbor] = stamp;
                        nodeDist[neighbor] = next;
                        reached.push_back(neighbor);
                        queue.push({next, neighbor});
                    } else if (next < nodeDist[neighbor]) {
                        nodeDist[neighbor] = next;
                        queue.push({next, neighbor});
                    }
                }
            }

            for (uint32_t node : reached) {
                endpointDist[side][node] = nodeDist[node];
                endpointStamp[side][node] = stamp;
                reachedBoth.push_back(node);
            }
        }

        const uint32_t stampN2 = stamp;        // the second search's stamp
        const uint32_t stampN1 = stamp - 1;    // the first search's stamp

        // Every edge meeting a settled node is a candidate to receive density.
        ++candidateStamp;
        candidates.clear();
        for (uint32_t node : reachedBoth) {
            if (nodeStampSeen[node] == candidateStamp) continue;
            nodeStampSeen[node] = candidateStamp;
            for (uint32_t e : nodeEdges[node]) {
                if (edgeStampSeen[e] == candidateStamp) continue;
                edgeStampSeen[e] = candidateStamp;
                candidates.push_back(e);
            }
        }
        result.consideredEdges += candidates.size();

        const auto distanceFrom = [&](int side, uint32_t node) -> double {
            const uint32_t want = (side == 0) ? stampN1 : stampN2;
            return endpointStamp[side][node] == want ? endpointDist[side][node]
                                                     : kInfinity;
        };

        for (uint32_t targetEdge : candidates) {
            const auto target = net.edge(targetEdge);
            const double fromN1ToT1 = distanceFrom(0, target.n1);
            const double fromN1ToT2 = distanceFrom(0, target.n2);
            const double fromN2ToT1 = distanceFrom(1, target.n1);
            const double fromN2ToT2 = distanceFrom(1, target.n2);
            if (fromN1ToT1 >= kInfinity && fromN1ToT2 >= kInfinity &&
                fromN2ToT1 >= kInfinity && fromN2ToT2 >= kInfinity) {
                continue;
            }

            const double targetLength = target.length;
            const size_t begin = firstLixelOfEdge[targetEdge];
            const size_t end = firstLixelOfEdge[targetEdge + 1];

            for (size_t li = begin; li < end; ++li) {
                const double alongTarget = lixelSlots[li].distFromN1;
                const double toTargetEnd = targetLength - alongTarget;

                // Reaching this lixel means leaving the source edge by one end
                // and entering the target edge by one end. Hoisted out of the
                // point loop, because it does not depend on the point.
                const double viaSourceN1 =
                    std::min(fromN1ToT1 + alongTarget, fromN1ToT2 + toTargetEnd);
                const double viaSourceN2 =
                    std::min(fromN2ToT1 + alongTarget, fromN2ToT2 + toTargetEnd);

                double sum = 0.0;
                for (float offset : sourcePoints) {
                    double distance = std::min(offset + viaSourceN1,
                                               (sourceLength - offset) + viaSourceN2);
                    // On the source edge itself the two may share a stretch of
                    // road, so the direct distance along the edge can be
                    // shorter than any path through a node.
                    if (targetEdge == sourceEdge) {
                        distance = std::min(distance, std::fabs(offset - alongTarget));
                    }
                    if (distance > cutoff) continue;
                    sum += std::exp(-distance * distance * invBandwidthSq);
                }
                density[li] += sum;
            }
        }
    }

    // Give every lixel its world position, the same way the file-based path
    // does, so a map means the same thing whichever engine produced it.
    result.lixels.reserve(lixelSlots.size());
    for (size_t i = 0; i < lixelSlots.size(); ++i) {
        if (!(density[i] > 0.0)) continue;

        const auto edge = net.edge(lixelSlots[i].edgeIndex);
        const auto* link = network.getLink(edge.linkId);
        if (!link) continue;
        const auto* from = network.getNode(link->fromNode);
        const auto* to = network.getNode(link->toNode);
        if (!from || !to) continue;

        const double t = (edge.length > 0.0f)
            ? std::clamp(static_cast<double>(lixelSlots[i].distFromN1) /
                             static_cast<double>(edge.length), 0.0, 1.0)
            : 0.0;

        NkdvNetwork::Lixel lixel{};
        lixel.x = static_cast<float>(from->x + (to->x - from->x) * t);
        lixel.y = static_cast<float>(from->y + (to->y - from->y) * t);
        lixel.value = static_cast<float>(density[i]);
        result.lixels.push_back(lixel);
    }

    result.seconds = timer.elapsed() / 1000.0;
    result.success = true;

    LOG_INFO(QString("NkdeScatter: %1 lixels in %2 s (%3 source edges of %4, "
                     "%5 activities)")
        .arg(result.lixels.size())
        .arg(result.seconds, 0, 'f', 2)
        .arg(result.sourceEdges)
        .arg(edgeCount)
        .arg(result.pointsPlaced));

    return result;
}

} // namespace simvis
