#include "nkde_gather.h"

#include "analysis/nkde_scatter.h"
#include "core/logger.h"
#include "data/network_index.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace simvis {

namespace {

struct LixelSlot {
    uint32_t edgeIndex;
    float    distFromN1;
};

constexpr double kInfinity = std::numeric_limits<double>::max();

} // namespace

NkdeGather::Result NkdeGather::run(const NkdvNetwork& net,
                                   const NetworkIndex& network,
                                   const std::vector<ActivityRecord>& points,
                                   const Params& params,
                                   const std::atomic<bool>* cancelled) {
    Result result;
    QElapsedTimer timer;
    timer.start();

    if (cancelled == nullptr) cancelled = NkdeScatter::cancelFlag();

    const size_t edgeCount = net.edgeCount();
    const size_t nodeCount = net.nodeCount();
    if (edgeCount == 0 || nodeCount == 0) {
        result.errorMessage = QObject::tr("The network has no edges to compute on.");
        return result;
    }

    // Identical input preparation to the scatter form, so the two differ only
    // in the order they visit things.
    const std::vector<ActivityRecord> places = NkdeScatter::uniquePlaces(points);
    result.pointsInput = points.size();
    result.pointsMerged = points.size() - places.size();

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

    std::vector<std::vector<std::pair<uint32_t, float>>> adjacency(nodeCount);
    std::vector<std::vector<uint32_t>> nodeEdges(nodeCount);
    for (size_t e = 0; e < edgeCount; ++e) {
        const auto edge = net.edge(e);
        adjacency[edge.n1].push_back({edge.n2, edge.length});
        adjacency[edge.n2].push_back({edge.n1, edge.length});
        nodeEdges[edge.n1].push_back(static_cast<uint32_t>(e));
        nodeEdges[edge.n2].push_back(static_cast<uint32_t>(e));
    }

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

    // Same stamped scratch arrays as the scatter form. Giving the baseline this
    // optimization matters: without it the comparison would measure array
    // clearing rather than direction.
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

    std::vector<uint32_t> reached;
    std::vector<uint32_t> reachedBoth;
    std::vector<uint32_t> candidates;

    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;

    // The gather loop: every edge that holds lixels is searched from, whether
    // or not anything is near it. That is the cost the inversion removes -- on
    // this data most of these searches return nothing usable.
    for (size_t targetEdge = 0; targetEdge < edgeCount; ++targetEdge) {
        if (firstLixelOfEdge[targetEdge] == firstLixelOfEdge[targetEdge + 1])
            continue;
        ++result.searchedEdges;

        if (cancelled && cancelled->load()) {
            result.errorMessage = QObject::tr("Cancelled");
            return result;
        }

        const auto target = net.edge(targetEdge);
        const double targetLength = target.length;
        const uint32_t endpoints[2] = {target.n1, target.n2};

        reachedBoth.clear();

        for (int side = 0; side < 2; ++side) {
            ++stamp;
            ++result.searches;
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

        const uint32_t stampN2 = stamp;
        const uint32_t stampN1 = stamp - 1;

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

        const size_t begin = firstLixelOfEdge[targetEdge];
        const size_t end = firstLixelOfEdge[targetEdge + 1];

        // Now pull: for each edge in reach, add every point it carries to every
        // lixel of this edge.
        for (uint32_t sourceEdge : candidates) {
            const std::vector<float>& sourcePoints = pointsPerEdge[sourceEdge];
            if (sourcePoints.empty()) continue;

            const auto source = net.edge(sourceEdge);
            const double sourceLength = source.length;

            // Endpoint-to-endpoint distances, hoisted out of the lixel loop
            // because they do not depend on which lixel is being filled. This
            // mirrors the hoist the scatter form performs.
            const double t1ToS1 = distanceFrom(0, source.n1);
            const double t2ToS1 = distanceFrom(1, source.n1);
            const double t1ToS2 = distanceFrom(0, source.n2);
            const double t2ToS2 = distanceFrom(1, source.n2);
            if (t1ToS1 >= kInfinity && t2ToS1 >= kInfinity &&
                t1ToS2 >= kInfinity && t2ToS2 >= kInfinity) {
                continue;
            }

            for (size_t li = begin; li < end; ++li) {
                const double alongTarget = lixelSlots[li].distFromN1;
                const double toTargetEnd = targetLength - alongTarget;

                // Distance from this lixel out to each end of the source edge,
                // leaving the target edge by whichever end is nearer.
                const double viaN1 = std::min(t1ToS1 + alongTarget,
                                              t2ToS1 + toTargetEnd);
                const double viaN2 = std::min(t1ToS2 + alongTarget,
                                              t2ToS2 + toTargetEnd);

                double sum = 0.0;
                for (float offset : sourcePoints) {
                    double distance = std::min(offset + viaN1,
                                               (sourceLength - offset) + viaN2);
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

    LOG_INFO(QString("NkdeGather: %1 lixels in %2 s (%3 searches over %4 edges, "
                     "%5 activities)")
        .arg(result.lixels.size())
        .arg(result.seconds, 0, 'f', 2)
        .arg(result.searches)
        .arg(result.searchedEdges)
        .arg(result.pointsPlaced));

    return result;
}

NkdeGather::Agreement NkdeGather::verify(
        const std::vector<NkdvNetwork::Lixel>& a,
        const std::vector<NkdvNetwork::Lixel>& b) {
    Agreement out;
    if (a.size() != b.size()) return out;
    out.comparable = true;
    out.compared = a.size();
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i].value;
        const double y = b[i].value;
        const double diff = std::fabs(x - y);
        out.maxAbsolute = std::max(out.maxAbsolute, diff);
        const double scale = std::max(std::fabs(x), std::fabs(y));
        if (scale > 0.0) out.maxRelative = std::max(out.maxRelative, diff / scale);
    }
    return out;
}

} // namespace simvis
