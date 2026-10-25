#pragma once

#include "gl_renderer.h"
#include "data/network_index.h"
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
    // Multiplier for the lift in a high-resolution export
    void setPointScale(float scale) { pointScale_ = scale; }

private:
    void buildNodeBuffer();
    void buildLinkBuffer();

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
