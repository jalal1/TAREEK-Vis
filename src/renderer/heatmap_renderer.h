// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "analysis/nkdv_network.h"
#include "gl_renderer.h"
#include <vector>

namespace simvis {

// Draws a network kernel density map as colored points along the road network.
//
// The density engine returns one value per lixel, a short piece of an edge.
// Lixels are drawn as camera-facing squares rather than line segments:
// consecutive lixels lie
// end to end along an edge, so squares sized to the lixel spacing join into a
// continuous colored road, and no adjacency information is needed.
//
// The geometry only changes when a new density map arrives, so the buffer is
// rebuilt then and simply redrawn every frame.
class HeatmapRenderer : public GLRenderer {
public:
    HeatmapRenderer();
    ~HeatmapRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Replace the density map. An empty vector clears it.
    void setLixels(const std::vector<NkdvNetwork::Lixel>& lixels);
    void clear() { setLixels({}); }
    bool hasData() const { return !lixels_.empty(); }

    void setVisible(bool visible) { visible_ = visible; }
    bool visible() const { return visible_; }

    // World-space size of one lixel mark, in meters. Set this to the lixel
    // length used for the computation so the marks meet without overlapping.
    void setLixelSize(float meters) { lixelSize_ = meters; }

    // Current zoom, in pixels per world meter, as MapWidget tracks it. The
    // mark is sized from this so it covers the same ground at every zoom.
    void setZoom(double zoom) { zoom_ = zoom; }

    // Extra multiplier for high-resolution export, matching the other
    // renderers' pointScale convention.
    void setPointScale(float scale) { pointScale_ = scale; }

    // Highest density in the current map.
    float maxValue() const { return maxValue_; }

    // Value that anchors the top of the color ramp.
    //
    // With a per-map scale, every map's brightest road is full yellow, so a
    // type with few activities looks as dense as one with many. That is useful
    // for reading the shape of one map, and misleading for comparing two.
    //
    // Pass a shared value to make colors mean the same on every map; pass 0 to
    // go back to scaling each map by its own maximum.
    // Rebuilding the vertex colors costs a pass over every lixel, so do nothing
    // when the anchor did not move. This is called after every background map
    // finishes, and most of those leave the anchor where it was.
    void setScaleMax(float value) {
        if (value == scaleMax_) return;
        scaleMax_ = value;
        buffersNeedUpdate_ = true;
    }
    float scaleMax() const { return scaleMax_ > 0.0f ? scaleMax_ : anchorValue_; }
    // The value this map's own colors are anchored to: a high percentile, not
    // the single highest lixel. See setLixels for why.
    float anchorValue() const { return anchorValue_; }
    // The stored anchor, before the fall back to this map's own peak. Use this
    // to tell whether a new anchor is a change; use scaleMax() to read the
    // value the colors actually use.
    float rawScaleMax() const { return scaleMax_; }
    bool usingSharedScale() const { return scaleMax_ > 0.0f; }

    void render();

private:
    void buildBuffers();

    std::vector<NkdvNetwork::Lixel> lixels_;
    // Where the top of the color ramp sits for this map on its own. A high
    // percentile rather than maxValue_, so a few extreme lixels cannot push
    // the rest of the map into the darkest step.
    static constexpr double kAnchorPercentile = 0.99;

    float maxValue_ = 0.0f;
    float anchorValue_ = 0.0f;
    float scaleMax_ = 0.0f;   // 0 = scale each map by its own maximum

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint program_ = 0;
    size_t vertexCount_ = 0;

    bool visible_ = false;
    bool buffersNeedUpdate_ = false;
    float lixelSize_ = 25.0f;
    double zoom_ = 1.0;
    float pointScale_ = 1.0f;
};

} // namespace simvis
