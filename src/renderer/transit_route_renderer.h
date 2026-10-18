#pragma once

#include "gl_renderer.h"
#include "core/transit_types.h"
#include "data/network_index.h"
#include "parsers/transit_schedule_parser.h"
#include <vector>
#include <unordered_map>

namespace simvis {

// Renders transit route polylines and stop circles
class TransitRouteRenderer : public GLRenderer {
public:
    TransitRouteRenderer();
    ~TransitRouteRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Set data sources
    void setTransitData(const TransitScheduleParser::Result* data);
    void setNetworkIndex(NetworkIndex* index);

    // Build GPU buffers from transit data (call after both data sources are set)
    void buildBuffers();

    // Render
    void render();

    // Visibility per mode
    void setShowBusRoutes(bool show) { showBus_ = show; }
    void setShowRailRoutes(bool show) { showRail_ = show; }
    void setShowTramRoutes(bool show) { showTram_ = show; }

    bool showBusRoutes() const { return showBus_; }
    bool showRailRoutes() const { return showRail_; }
    bool showTramRoutes() const { return showTram_; }

    // Bus stop circles can be hidden independently of bus routes
    void setShowBusStops(bool show) { showBusStops_ = show; }
    bool showBusStops() const { return showBusStops_; }

    // Route line width
    void setRouteLineWidth(float width) { routeLineWidth_ = width; }
    float routeLineWidth() const { return routeLineWidth_; }

    // Multiplier for stop point sizes; >1 keeps stops proportional during
    // high-res off-screen export (point size is in pixels, not world units).
    void setPointScale(float scale) { pointScale_ = scale; }
    float pointScale() const { return pointScale_; }

    // Highlight a specific line (0 = none)
    void setHighlightedLine(uint32_t lineId) { highlightedLineId_ = lineId; }
    uint32_t highlightedLine() const { return highlightedLineId_; }

    // Hit testing: find stop near world coordinates (returns stop index, or -1)
    int findStopAt(double worldX, double worldY, double radius) const;

    // Hit testing: find route line near world coordinates (returns lineId, or 0)
    uint32_t findRouteAt(double worldX, double worldY, double radius) const;

    // Stats
    size_t busRouteVertexCount() const { return busVertexCount_; }
    size_t tramRouteVertexCount() const { return tramVertexCount_; }
    size_t railRouteVertexCount() const { return railVertexCount_; }
    size_t stopVertexCount() const { return stopVertexCount_; }

private:
    void buildRouteBuffers();
    void buildStopBuffer();

    // Color assigned to each stop, matching the color of a route serving it.
    // Populated during buildRouteBuffers(), consumed by buildStopBuffer().
    std::unordered_map<uint32_t, TransitModeColor> stopColors_;

    // Pick the representative route for a line (most links)
    const TransitRoute* pickRepresentativeRoute(const TransitLine& line) const;

    // Data sources (not owned)
    const TransitScheduleParser::Result* transitData_ = nullptr;
    NetworkIndex* networkIndex_ = nullptr;

    // Shader programs
    GLuint routeProgram_ = 0;
    GLuint stopProgram_ = 0;

    // Route VBOs per mode
    GLuint busRouteVAO_ = 0, busRouteVBO_ = 0;
    GLuint tramRouteVAO_ = 0, tramRouteVBO_ = 0;
    GLuint railRouteVAO_ = 0, railRouteVBO_ = 0;
    size_t busVertexCount_ = 0;
    size_t tramVertexCount_ = 0;
    size_t railVertexCount_ = 0;

    // Each route is rendered as a triangle list of quads (6 verts per segment);
    // we store start index and count (in vertices) so thickness is geometry-based
    // rather than relying on glLineWidth (clamped to 1px in GL Core profile).
    struct RouteDrawCmd {
        GLint first;
        GLsizei count;
        uint32_t lineId; // for highlight detection
    };
    std::vector<RouteDrawCmd> busDrawCmds_;
    std::vector<RouteDrawCmd> tramDrawCmds_;
    std::vector<RouteDrawCmd> railDrawCmds_;

    // Stop rendering
    GLuint stopVAO_ = 0, stopVBO_ = 0;
    size_t stopVertexCount_ = 0;

    // Per-mode stop ranges in the VBO for visibility filtering
    struct ModeStopRange {
        GLint first;
        GLsizei count;
    };
    ModeStopRange busStopRange_{0, 0};
    ModeStopRange tramStopRange_{0, 0};
    ModeStopRange railStopRange_{0, 0};

    // Options
    // All transit layers hidden by default (matches the Transit tab)
    bool showBus_ = false;
    bool showRail_ = false;
    bool showTram_ = false;
    bool showBusStops_ = false;
    float routeLineWidth_ = 2.0f;
    float pointScale_ = 1.0f;  // 1.0 on-screen; raised during high-res export
    uint32_t highlightedLineId_ = 0;

    bool buffersBuilt_ = false;
};

} // namespace simvis
