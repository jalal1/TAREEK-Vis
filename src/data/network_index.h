#pragma once

#include "core/types.h"
#include <QObject>
#include <QString>
#include <QMutex>
#include <vector>
#include <unordered_map>
#include <memory>

namespace simvis {

// Manages network data with support for viewport-based loading
class NetworkIndex : public QObject {
    Q_OBJECT

public:
    explicit NetworkIndex(QObject* parent = nullptr);
    ~NetworkIndex() override;

    // Load binary network file
    bool loadFile(const QString& path);
    void unload();

    // Get network bounds
    const BoundingBox& bounds() const { return bounds_; }

    // Get all nodes (for smaller networks that fit in memory)
    const std::vector<NodeRecord>& allNodes() const { return nodes_; }
    const std::vector<LinkRecord>& allLinks() const { return links_; }

    // Get node by internal ID
    const NodeRecord* getNode(uint32_t id) const;

    // Get link by internal ID
    const LinkRecord* getLink(uint32_t id) const;

    // Get nodes/links in viewport (for large networks)
    std::vector<const NodeRecord*> getNodesInBounds(const BoundingBox& bounds) const;
    std::vector<const LinkRecord*> getLinksInBounds(const BoundingBox& bounds) const;

    // String ID lookup
    const StringInterner& nodeIds() const { return nodeIds_; }
    const StringInterner& linkIds() const { return linkIds_; }

    // Every link that runs from node `from` to node `to` (internal ids). More
    // than one when the network has parallel links. Empty when there is none.
    std::vector<uint32_t> linksBetween(uint32_t from, uint32_t to) const;

    // Internal id of the link with this MATSim id string, or UINT32_MAX.
    uint32_t findLinkByOriginalId(const std::string& originalId) const;

    // Stats
    size_t nodeCount() const { return nodes_.size(); }
    size_t linkCount() const { return links_.size(); }

signals:
    void loaded(bool success);
    void loadError(const QString& message);

private:
    // Build spatial index for viewport queries
    void buildSpatialIndex();

    QString filePath_;
    NetworkFileHeader header_;
    BoundingBox bounds_;

    std::vector<NodeRecord> nodes_;
    std::vector<LinkRecord> links_;

    // ID -> index maps for fast lookup
    std::unordered_map<uint32_t, size_t> nodeIdToIndex_;
    std::unordered_map<uint32_t, size_t> linkIdToIndex_;

    StringInterner nodeIds_;
    StringInterner linkIds_;

    // (fromNode << 32 | toNode) -> link ids. The two directions of a road are
    // drawn on top of each other, so a click cannot tell them apart; this is
    // how the other direction is found.
    std::unordered_map<uint64_t, std::vector<uint32_t>> linksByNodePair_;

    // Simple grid-based spatial index
    struct SpatialCell {
        std::vector<uint32_t> nodeIndices;
        std::vector<uint32_t> linkIndices;
    };
    std::vector<std::vector<SpatialCell>> spatialGrid_;
    int gridWidth_ = 0;
    int gridHeight_ = 0;
    double cellWidth_ = 0;
    double cellHeight_ = 0;

    mutable QMutex mutex_;
};

} // namespace simvis
