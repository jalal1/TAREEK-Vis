#include "network_index.h"
#include <QMutexLocker>
#include <fstream>
#include <cmath>
#include <unordered_set>

namespace simvis {

NetworkIndex::NetworkIndex(QObject* parent)
    : QObject(parent)
{
}

NetworkIndex::~NetworkIndex() {
    unload();
}

bool NetworkIndex::loadFile(const QString& path) {
    QMutexLocker locker(&mutex_);

    filePath_ = path;

    std::ifstream in(path.toStdString(), std::ios::binary);
    if (!in) {
        emit loadError("Failed to open network file");
        return false;
    }

    // Read header
    in.read(reinterpret_cast<char*>(&header_), sizeof(header_));

    if (header_.magic != NETWORK_FILE_MAGIC || header_.version != FILE_VERSION) {
        emit loadError("Invalid network file format");
        return false;
    }

    bounds_ = header_.bounds;

    // Read nodes
    nodes_.resize(header_.nodeCount);
    in.seekg(header_.nodeOffset);
    in.read(reinterpret_cast<char*>(nodes_.data()),
            header_.nodeCount * sizeof(NodeRecord));

    // Read links
    links_.resize(header_.linkCount);
    in.seekg(header_.linkOffset);
    in.read(reinterpret_cast<char*>(links_.data()),
            header_.linkCount * sizeof(LinkRecord));

    // Read string tables
    in.seekg(header_.stringTableOffset);
    nodeIds_.readFrom(in);
    linkIds_.readFrom(in);

    // Build index maps
    nodeIdToIndex_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) {
        nodeIdToIndex_[nodes_[i].id] = i;
    }

    linkIdToIndex_.clear();
    for (size_t i = 0; i < links_.size(); ++i) {
        linkIdToIndex_[links_[i].id] = i;
    }

    linksByNodePair_.clear();
    for (const auto& link : links_) {
        const uint64_t key = (static_cast<uint64_t>(link.fromNode) << 32) | link.toNode;
        linksByNodePair_[key].push_back(link.id);
    }

    // Build spatial index
    buildSpatialIndex();

    emit loaded(true);
    return true;
}

void NetworkIndex::unload() {
    QMutexLocker locker(&mutex_);

    filePath_.clear();
    header_ = {};
    bounds_ = {};
    nodes_.clear();
    links_.clear();
    nodeIdToIndex_.clear();
    linkIdToIndex_.clear();
    linksByNodePair_.clear();
    nodeIds_.clear();
    linkIds_.clear();
    spatialGrid_.clear();
}

const NodeRecord* NetworkIndex::getNode(uint32_t id) const {
    QMutexLocker locker(&mutex_);

    auto it = nodeIdToIndex_.find(id);
    if (it == nodeIdToIndex_.end()) return nullptr;
    return &nodes_[it->second];
}

const LinkRecord* NetworkIndex::getLink(uint32_t id) const {
    QMutexLocker locker(&mutex_);

    auto it = linkIdToIndex_.find(id);
    if (it == linkIdToIndex_.end()) return nullptr;
    return &links_[it->second];
}

std::vector<uint32_t> NetworkIndex::linksBetween(uint32_t from, uint32_t to) const {
    QMutexLocker locker(&mutex_);

    auto it = linksByNodePair_.find((static_cast<uint64_t>(from) << 32) | to);
    if (it == linksByNodePair_.end()) return {};
    return it->second;
}

uint32_t NetworkIndex::findLinkByOriginalId(const std::string& originalId) const {
    QMutexLocker locker(&mutex_);

    if (!linkIds_.hasString(originalId)) return UINT32_MAX;
    const uint32_t id = linkIds_.getId(originalId);
    // The string table also names ids that may have no record; check.
    return linkIdToIndex_.count(id) ? id : UINT32_MAX;
}

void NetworkIndex::buildSpatialIndex() {
    if (nodes_.empty()) return;

    // Target ~100x100 grid
    const int TARGET_CELLS = 100;

    double width = bounds_.width();
    double height = bounds_.height();

    if (width <= 0 || height <= 0) return;

    // Calculate cell size
    double maxDim = std::max(width, height);
    double cellSize = maxDim / TARGET_CELLS;

    gridWidth_ = static_cast<int>(std::ceil(width / cellSize));
    gridHeight_ = static_cast<int>(std::ceil(height / cellSize));
    cellWidth_ = cellSize;
    cellHeight_ = cellSize;

    // Initialize grid
    spatialGrid_.resize(gridHeight_);
    for (auto& row : spatialGrid_) {
        row.resize(gridWidth_);
    }

    // Index nodes
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const auto& node = nodes_[i];
        int cellX = static_cast<int>((node.x - bounds_.minX) / cellWidth_);
        int cellY = static_cast<int>((node.y - bounds_.minY) / cellHeight_);

        cellX = std::clamp(cellX, 0, gridWidth_ - 1);
        cellY = std::clamp(cellY, 0, gridHeight_ - 1);

        spatialGrid_[cellY][cellX].nodeIndices.push_back(static_cast<uint32_t>(i));
    }

    // Index links (add to all cells they might intersect)
    for (size_t i = 0; i < links_.size(); ++i) {
        const auto& link = links_[i];

        // Get from/to node positions
        auto fromIt = nodeIdToIndex_.find(link.fromNode);
        auto toIt = nodeIdToIndex_.find(link.toNode);

        if (fromIt == nodeIdToIndex_.end() || toIt == nodeIdToIndex_.end()) {
            continue;
        }

        const auto& fromNode = nodes_[fromIt->second];
        const auto& toNode = nodes_[toIt->second];

        // Calculate bounding box of link
        float minX = std::min(fromNode.x, toNode.x);
        float maxX = std::max(fromNode.x, toNode.x);
        float minY = std::min(fromNode.y, toNode.y);
        float maxY = std::max(fromNode.y, toNode.y);

        int minCellX = static_cast<int>((minX - bounds_.minX) / cellWidth_);
        int maxCellX = static_cast<int>((maxX - bounds_.minX) / cellWidth_);
        int minCellY = static_cast<int>((minY - bounds_.minY) / cellHeight_);
        int maxCellY = static_cast<int>((maxY - bounds_.minY) / cellHeight_);

        minCellX = std::clamp(minCellX, 0, gridWidth_ - 1);
        maxCellX = std::clamp(maxCellX, 0, gridWidth_ - 1);
        minCellY = std::clamp(minCellY, 0, gridHeight_ - 1);
        maxCellY = std::clamp(maxCellY, 0, gridHeight_ - 1);

        // Add link to all cells it intersects
        for (int cy = minCellY; cy <= maxCellY; ++cy) {
            for (int cx = minCellX; cx <= maxCellX; ++cx) {
                spatialGrid_[cy][cx].linkIndices.push_back(static_cast<uint32_t>(i));
            }
        }
    }
}

std::vector<const NodeRecord*> NetworkIndex::getNodesInBounds(const BoundingBox& queryBounds) const {
    QMutexLocker locker(&mutex_);

    std::vector<const NodeRecord*> result;

    if (spatialGrid_.empty()) {
        // No spatial index, return all nodes that intersect
        for (const auto& node : nodes_) {
            if (queryBounds.contains(node.x, node.y)) {
                result.push_back(&node);
            }
        }
        return result;
    }

    // Calculate cell range
    int minCellX = static_cast<int>((queryBounds.minX - bounds_.minX) / cellWidth_);
    int maxCellX = static_cast<int>((queryBounds.maxX - bounds_.minX) / cellWidth_);
    int minCellY = static_cast<int>((queryBounds.minY - bounds_.minY) / cellHeight_);
    int maxCellY = static_cast<int>((queryBounds.maxY - bounds_.minY) / cellHeight_);

    minCellX = std::clamp(minCellX, 0, gridWidth_ - 1);
    maxCellX = std::clamp(maxCellX, 0, gridWidth_ - 1);
    minCellY = std::clamp(minCellY, 0, gridHeight_ - 1);
    maxCellY = std::clamp(maxCellY, 0, gridHeight_ - 1);

    // Collect unique nodes from cells
    std::unordered_set<uint32_t> seen;
    for (int cy = minCellY; cy <= maxCellY; ++cy) {
        for (int cx = minCellX; cx <= maxCellX; ++cx) {
            for (uint32_t idx : spatialGrid_[cy][cx].nodeIndices) {
                if (seen.insert(idx).second) {
                    const auto& node = nodes_[idx];
                    if (queryBounds.contains(node.x, node.y)) {
                        result.push_back(&node);
                    }
                }
            }
        }
    }

    return result;
}

std::vector<const LinkRecord*> NetworkIndex::getLinksInBounds(const BoundingBox& queryBounds) const {
    QMutexLocker locker(&mutex_);

    std::vector<const LinkRecord*> result;

    if (spatialGrid_.empty()) {
        // No spatial index, return all links
        for (const auto& link : links_) {
            result.push_back(&link);
        }
        return result;
    }

    // Calculate cell range
    int minCellX = static_cast<int>((queryBounds.minX - bounds_.minX) / cellWidth_);
    int maxCellX = static_cast<int>((queryBounds.maxX - bounds_.minX) / cellWidth_);
    int minCellY = static_cast<int>((queryBounds.minY - bounds_.minY) / cellHeight_);
    int maxCellY = static_cast<int>((queryBounds.maxY - bounds_.minY) / cellHeight_);

    minCellX = std::clamp(minCellX, 0, gridWidth_ - 1);
    maxCellX = std::clamp(maxCellX, 0, gridWidth_ - 1);
    minCellY = std::clamp(minCellY, 0, gridHeight_ - 1);
    maxCellY = std::clamp(maxCellY, 0, gridHeight_ - 1);

    // Collect unique links from cells
    std::unordered_set<uint32_t> seen;
    for (int cy = minCellY; cy <= maxCellY; ++cy) {
        for (int cx = minCellX; cx <= maxCellX; ++cx) {
            for (uint32_t idx : spatialGrid_[cy][cx].linkIndices) {
                if (seen.insert(idx).second) {
                    result.push_back(&links_[idx]);
                }
            }
        }
    }

    return result;
}

} // namespace simvis
