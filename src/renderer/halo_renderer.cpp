// SPDX-License-Identifier: GPL-3.0-or-later

#include "halo_renderer.h"

namespace simvis {

static const char* haloVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;

uniform mat4 uMVP;
uniform float uDiameter;   // point size in pixels
uniform float uPointScale; // high-res export multiplier

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_PointSize = uDiameter * uPointScale;
}
)";

// Same soft fill + glowing rim as the tracked-vehicle halo.
static const char* haloFragmentShader = R"(
#version 330 core
out vec4 FragColor;

uniform vec3 uColor;

void main() {
    vec2 c = gl_PointCoord - vec2(0.5);
    float dist = length(c);
    if (dist > 0.5) discard;
    float ring = smoothstep(0.40, 0.46, dist) * (1.0 - smoothstep(0.46, 0.5, dist));
    float alpha = 0.18 * (1.0 - smoothstep(0.0, 0.45, dist)) + 0.65 * ring;
    FragColor = vec4(uColor, alpha);
}
)";

HaloRenderer::HaloRenderer() = default;

HaloRenderer::~HaloRenderer() {
    cleanup();
}

bool HaloRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    program_ = compileShader(haloVertexShader, haloFragmentShader);
    if (!program_) return false;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    return true;
}

void HaloRenderer::cleanup() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    GLRenderer::cleanup();
}

void HaloRenderer::setPositions(const std::vector<std::pair<float, float>>& positions) {
    positions_ = positions;
    buffersNeedUpdate_ = true;
}

void HaloRenderer::buildBuffers() {
    std::vector<float> vertices;
    vertices.reserve(positions_.size() * 2);
    for (const auto& p : positions_) {
        vertices.push_back(p.first);
        vertices.push_back(p.second);
    }
    count_ = positions_.size();

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.empty() ? nullptr : vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    buffersNeedUpdate_ = false;
}

void HaloRenderer::render() {
    if (!initialized_ || !visible_) return;
    if (buffersNeedUpdate_) buildBuffers();
    if (count_ == 0) return;

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    glUseProgram(program_);
    glUniformMatrix4fv(glGetUniformLocation(program_, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniform1f(glGetUniformLocation(program_, "uDiameter"), screenDiameter_);
    glUniform1f(glGetUniformLocation(program_, "uPointScale"), pointScale_);
    glUniform3f(glGetUniformLocation(program_, "uColor"), r_, g_, b_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(count_));
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace simvis
