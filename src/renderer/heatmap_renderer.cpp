// SPDX-License-Identifier: GPL-3.0-or-later

#include "heatmap_renderer.h"

#include "heatmap_palette.h"
#include <algorithm>

namespace simvis {

// Vertex: x, y, r, g, b
constexpr int VERTEX_SIZE = 5;

// Point sprites, not quads. A density map holds hundreds of thousands of
// lixels, so one vertex each instead of six keeps the buffer small: 356 K
// lixels is 7 MB this way against 43 MB as triangles.
static const char* heatmapVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;

uniform mat4 uMVP;
uniform float uPointSize;

out vec3 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_PointSize = uPointSize;
    vColor = aColor;
}
)";

// Round the sprite and fade its edge, so neighbouring lixels blend into a
// continuous band instead of showing a row of squares.
static const char* heatmapFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    vec2 offset = gl_PointCoord - vec2(0.5);
    float dist = length(offset) * 2.0;
    if (dist > 1.0) discard;
    float alpha = smoothstep(1.0, 0.55, dist);
    FragColor = vec4(vColor, alpha * 0.85);
}
)";

HeatmapRenderer::HeatmapRenderer() = default;

HeatmapRenderer::~HeatmapRenderer() {
    cleanup();
}

bool HeatmapRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    program_ = compileShader(heatmapVertexShader, heatmapFragmentShader);
    if (!program_) return false;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    return true;
}

void HeatmapRenderer::cleanup() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    GLRenderer::cleanup();
}

void HeatmapRenderer::setLixels(const std::vector<NkdvNetwork::Lixel>& lixels) {
    lixels_ = lixels;

    maxValue_ = 0.0f;
    for (const auto& lx : lixels_) maxValue_ = std::max(maxValue_, lx.value);

    // Anchor the colors near the top of the distribution rather than on the
    // single highest lixel. Density along a road is very uneven: a work map
    // can put thousands of activities on the few segments beside an office
    // tower, so its maximum is many times the value of almost every other
    // lixel. Anchoring on that maximum pushes the whole real distribution into
    // the darkest step and the map reads as empty.
    //
    // Measured on Twin Cities: for Work the maximum is 16.9x the 99th
    // percentile and only 750 of 1.39 million lixels reach half of it, so the
    // median lixel lands at 0.06 on the ramp. Against the 99th percentile it
    // lands at 0.34, in the readable middle. A spread-out map like Home barely
    // moves, because its maximum is only 2.3x its 99th percentile.
    //
    // Values above the anchor still paint the brightest step; they are clamped,
    // not lost.
    anchorValue_ = maxValue_;
    if (!lixels_.empty()) {
        std::vector<float> positive;
        positive.reserve(lixels_.size());
        for (const auto& lx : lixels_)
            if (lx.value > 0.0f) positive.push_back(lx.value);

        if (!positive.empty()) {
            const size_t index =
                static_cast<size_t>(static_cast<double>(positive.size()) * kAnchorPercentile);
            const size_t clamped = std::min(index, positive.size() - 1);
            std::nth_element(positive.begin(), positive.begin() + clamped,
                             positive.end());
            const float percentile = positive[clamped];
            if (percentile > 0.0f) anchorValue_ = percentile;
        }
    }

    buffersNeedUpdate_ = true;
}

void HeatmapRenderer::buildBuffers() {
    std::vector<float> vertices;
    vertices.reserve(lixels_.size() * VERTEX_SIZE);

    // Anchor of the ramp: this map's own high percentile, or a value shared
    // with other maps so their colors mean the same thing.
    const float anchor = scaleMax_ > 0.0f ? scaleMax_ : anchorValue_;

    for (const auto& lx : lixels_) {
        // Lixels with no density would paint the whole network with the
        // ramp's darkest step, hiding the base map for no information.
        if (!(lx.value > 0.0f)) continue;

        const float t = heatmap::normalize(lx.value, anchor);
        const heatmap::Rgb c = heatmap::sample(t);

        vertices.push_back(lx.x);
        vertices.push_back(lx.y);
        vertices.push_back(c.r);
        vertices.push_back(c.g);
        vertices.push_back(c.b);
    }

    vertexCount_ = vertices.size() / VERTEX_SIZE;

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.empty() ? nullptr : vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    buffersNeedUpdate_ = false;
}

void HeatmapRenderer::render() {
    if (!initialized_ || !visible_) return;

    if (buffersNeedUpdate_) buildBuffers();
    if (vertexCount_ == 0) return;

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    glUseProgram(program_);
    GLint mvpLoc = glGetUniformLocation(program_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

    // Size the sprite so lixels spaced `lixelSize_` apart in the world just
    // touch. zoom_ is pixels per world meter, as MapWidget tracks it.
    float pointSize = static_cast<float>(lixelSize_ * zoom_) * pointScale_;
    // Below ~2 px the marks fall between pixels and the map looks sparse;
    // above ~64 px they smear over the road they describe.
    pointSize = std::clamp(pointSize, 2.0f * pointScale_, 64.0f * pointScale_);
    glUniform1f(glGetUniformLocation(program_, "uPointSize"), pointSize);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    // Additive blending would let overlapping marks saturate to white and
    // invent density that is not there, so use ordinary alpha blending.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vertexCount_));
    glBindVertexArray(0);
    glUseProgram(0);

    // GL_PROGRAM_POINT_SIZE stays enabled on purpose. The layers drawn after
    // this one - vehicles, transit stops, halos - all size their points from
    // gl_PointSize, and disabling it here would shrink every one of them to a
    // single pixel. The other renderers treat it as shared state and only ever
    // enable it, so this one must not switch it off.
}

} // namespace simvis
