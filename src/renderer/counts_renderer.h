// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gl_renderer.h"
#include "parsers/counts_parser.h"
#include <vector>

namespace simvis {

// Renders highlighted count links as thick colored GL_LINES overlay
class CountsRenderer : public GLRenderer {
public:
    CountsRenderer();
    ~CountsRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Set counts data and rebuild buffers
    void setCountsData(const CountsData* data);

    // Render the highlighted links
    void render();

    // Visibility
    void setShowCounts(bool show) { showCounts_ = show; }
    bool showCounts() const { return showCounts_; }

    // Hit testing: find count link near world coordinates.
    // Returns linkId of the nearest count link, or 0 if none within radius.
    uint32_t findCountLinkAt(double worldX, double worldY, double radius) const;

private:
    void buildBuffers();

    const CountsData* countsData_ = nullptr;

    GLuint linkVAO_ = 0;
    GLuint linkVBO_ = 0;
    GLuint linkProgram_ = 0;
    size_t linkVertexCount_ = 0;

    bool showCounts_ = false;
    bool buffersNeedUpdate_ = true;
    float linkWidth_ = 7.5f;  // 5x default network link width (1.5)
};

} // namespace simvis
