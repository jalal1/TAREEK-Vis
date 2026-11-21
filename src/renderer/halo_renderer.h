// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gl_renderer.h"
#include <vector>

namespace simvis {

// Reusable "selection halo": a soft transparent-fill circle with a brighter
// glowing rim (the cyan magnifier look used for the tracked vehicle). Draw it
// at any set of world positions to mark locations - selected vehicle, count
// stations, future clickable points, etc.
//
// The halo keeps a constant screen radius regardless of zoom (radius is in
// pixels), so call setScreenRadius() whenever zoom changes if you want a fixed
// world size instead; by default it is screen-constant via point sprites.
class HaloRenderer : public GLRenderer {
public:
    HaloRenderer();
    ~HaloRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // World positions to mark. Empty clears the overlay.
    void setPositions(const std::vector<std::pair<float, float>>& positions);
    void clear() { setPositions({}); }
    bool empty() const { return positions_.empty(); }

    // On-screen diameter of each halo, in pixels (default 44).
    void setScreenDiameter(float pixels) { screenDiameter_ = pixels; }

    // RGB of the halo (default cyan 0.30, 1.0, 1.0).
    void setColor(float r, float g, float b) { r_ = r; g_ = g; b_ = b; }

    // Multiplier for high-res export (point size is in pixels).
    void setPointScale(float scale) { pointScale_ = scale; }

    void setVisible(bool visible) { visible_ = visible; }
    bool visible() const { return visible_; }

    void render();

private:
    void buildBuffers();

    std::vector<std::pair<float, float>> positions_;

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint program_ = 0;
    size_t count_ = 0;

    bool visible_ = false;
    bool buffersNeedUpdate_ = false;
    float screenDiameter_ = 44.0f;
    float pointScale_ = 1.0f;
    float r_ = 0.30f, g_ = 1.0f, b_ = 1.0f;
};

} // namespace simvis
