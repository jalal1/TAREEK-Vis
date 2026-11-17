#pragma once

#include "gl_renderer.h"
#include "data/network_index.h"
#include <array>
#include <vector>

namespace simvis {

// Renders network nodes and links
class NetworkRenderer : public GLRenderer {
public:
    NetworkRenderer();
    ~NetworkRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Set network data source
    void setNetworkIndex(NetworkIndex* index);

    // Render the network
    void render();

    // Update buffers when viewport changes significantly
    void updateBuffers();

    // Rendering options
    void setShowNodes(bool show) { showNodes_ = show; }
    void setShowLinks(bool show) { showLinks_ = show; }
    void setNodeSize(float size) { nodeSize_ = size; }
    void setLinkWidth(float width) { linkWidth_ = width; }

    // Air links (RoadType::Air*) are drawn after the road links, in their own
    // range of the buffer, so this toggle costs nothing. Off by default.
    void setShowAirLinks(bool show) { showAirLinks_ = show; }
    bool hasAirLinks() const { return linkVertexCount_ > airVertexStart_; }
    // Height lift of the air links, as for the aircraft (VehicleRenderer)
    void setLiftPxPerMetre(float pxPerMetre) { liftPxPerM_ = pxPerMetre; }
    // Multiplier for the lift and the colored link width in a
    // high-resolution export
    void setPointScale(float scale) { pointScale_ = scale; }

    // Colors of the road links from one value per link (roadmap item 9).
    // The renderer knows nothing of the metric: it gets the values, the class
    // limits and one color per class.
    struct LinkClasses {
        // Class i holds the values v with limits[i-1] < v <= limits[i]
        std::vector<float> limits;                  // at most 8
        std::vector<std::array<float, 3>> colors;   // limits.size() + 1
        // Half width of each class in screen pixels (before the export
        // scale). Empty: 1.75 px for every class.
        std::vector<float> halfWidthsPx;
        // Values at or below this are "no data": thin, dim and neutral
        float noDataBelow = -1.0e38f;
        // True: class 0 is drawn last (on top), for metrics where low is worst
        bool lowOnTop = false;
        // Each link moves by its half width to its right side, so the two
        // directions of a road are both visible. Off for Road Type.
        bool offsetDirections = true;
        // A thin dark edge on the links with data. Off for Road Type.
        bool edge = true;
    };
    // values: one per link, in the order of NetworkIndex::allLinks(). The
    // geometry does not change; only the values and the draw order go to the
    // GPU. Each link is offset to its right side in screen space, so the two
    // directions of a road are both visible (LinkClasses::offsetDirections).
    void setLinkValues(const std::vector<float>& values, const LinkClasses& classes);
    // Back to the road type colors
    void clearLinkValues();
    bool hasLinkValues() const { return colorByValue_; }

private:
    void buildNodeBuffer();
    void buildLinkBuffer();
    void uploadLinkValues();
    // Makes the instance list of the value path again when the set of grid
    // cells in view (or the values) changed
    void updateDrawOrder(bool lines);
    void renderValueLinks(const QMatrix4x4& mvp);
    void buildLinkGrid(const std::vector<float>& geometry);

    // Get color for road type
    static void getRoadTypeColor(RoadType type, float& r, float& g, float& b);

    NetworkIndex* networkIndex_ = nullptr;

    // Node rendering
    GLuint nodeVAO_ = 0;
    GLuint nodeVBO_ = 0;
    GLuint nodeProgram_ = 0;
    size_t nodeVertexCount_ = 0;

    // Link rendering
    GLuint linkVAO_ = 0;
    GLuint linkVBO_ = 0;
    GLuint linkProgram_ = 0;
    size_t linkVertexCount_ = 0;
    size_t airVertexStart_ = 0;   // first vertex of the air links

    // Links colored by value: instanced quads. One instance is one link id
    // (orderVBO_, the draw order); the shader reads the link's end points from
    // a buffer texture (static) and its value from a second one.
    GLuint valueProgram_ = 0;
    GLuint valueLineProgram_ = 0; // overview zoom: 1 px lines from linkVBO_
    GLuint linkIdVBO_ = 0;        // link id of each vertex of linkVBO_
    GLuint lineEBO_ = 0;          // vertex indices of the value lines, in draw order
    GLsizei lineIndexCount_ = 0;
    std::vector<uint32_t> linkFirstVertex_;  // per link: its first vertex in linkVBO_
    GLuint valueVAO_ = 0;
    GLuint orderVBO_ = 0;
    GLuint geomBuffer_ = 0;       // per link: x0, y0, x1, y1 (RGBA32F)
    GLuint geomTexture_ = 0;
    GLuint valueBuffer_ = 0;      // per link: the value (R32F)
    GLuint valueTexture_ = 0;
    std::vector<uint8_t> roadLinkDrawable_;  // per link: 1 = a road link with both nodes
    bool colorByValue_ = false;
    bool valuesNeedUpload_ = false;
    std::vector<float> linkValues_;
    LinkClasses linkClasses_;
    GLsizei instanceCount_ = 0;
    std::vector<uint8_t> linkBucket_;   // draw bucket per link (0 = no data)
    int bucketCount_ = 0;
    bool orderNeedsUpdate_ = true;

    // Coarse grid of the road links by midpoint, to draw only the links near
    // the view. On an integrated GPU the vertex work of all links costs about
    // 8 ms per frame (Twin Cities, 634,886 links), also when few are in view.
    struct LinkGrid {
        double minX = 0, minY = 0, cellW = 1, cellH = 1;
        int nx = 0, ny = 0;
        std::vector<uint32_t> cellStart;   // CSR offsets, nx * ny + 1
        std::vector<uint32_t> links;       // link indices by cell
        std::vector<float> cellReach;      // longest half link length per cell, m
        float maxReach = 0.0f;
    };
    LinkGrid linkGrid_;
    std::vector<uint32_t> visibleCells_;  // cells of the current instance list

    // Options
    bool showNodes_ = true;
    bool showLinks_ = true;
    float nodeSize_ = 3.0f;
    float linkWidth_ = 1.5f;
    bool showAirLinks_ = false;
    float liftPxPerM_ = 0.0f;
    float pointScale_ = 1.0f;

    // Cached viewport bounds for buffer updates
    BoundingBox lastViewportBounds_;
    bool buffersNeedUpdate_ = true;
};

} // namespace simvis
