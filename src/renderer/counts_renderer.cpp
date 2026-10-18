#include "counts_renderer.h"
#include <QDebug>
#include <cmath>
#include <algorithm>

namespace simvis {

// Vertex: x, y, r, g, b
constexpr int VERTEX_SIZE = 5;

static const char* countsVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;

uniform mat4 uMVP;

out vec3 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* countsFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 0.9);
}
)";

CountsRenderer::CountsRenderer() = default;

CountsRenderer::~CountsRenderer() {
    cleanup();
}

bool CountsRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    linkProgram_ = compileShader(countsVertexShader, countsFragmentShader);
    if (!linkProgram_) return false;

    glGenVertexArrays(1, &linkVAO_);
    glGenBuffers(1, &linkVBO_);

    return true;
}

void CountsRenderer::cleanup() {
    if (linkVAO_) {
        glDeleteVertexArrays(1, &linkVAO_);
        linkVAO_ = 0;
    }
    if (linkVBO_) {
        glDeleteBuffers(1, &linkVBO_);
        linkVBO_ = 0;
    }
    if (linkProgram_) {
        glDeleteProgram(linkProgram_);
        linkProgram_ = 0;
    }

    GLRenderer::cleanup();
}

void CountsRenderer::setCountsData(const CountsData* data) {
    countsData_ = data;
    buffersNeedUpdate_ = true;
}

void CountsRenderer::buildBuffers() {
    if (!countsData_ || countsData_->counts.empty()) {
        linkVertexCount_ = 0;
        buffersNeedUpdate_ = false;
        return;
    }

    std::vector<float> vertices;
    vertices.reserve(countsData_->counts.size() * 2 * VERTEX_SIZE);

    for (const auto& c : countsData_->counts) {
        // Skip links with no geometry
        if (c.fromX == 0 && c.fromY == 0 && c.toX == 0 && c.toY == 0) continue;

        // Same yellow as the person-route overlay so highlights read as one
        // consistent visual language across the app.
        const float r = 1.0f, g = 0.85f, b = 0.1f;

        // From vertex
        vertices.push_back(c.fromX);
        vertices.push_back(c.fromY);
        vertices.push_back(r);
        vertices.push_back(g);
        vertices.push_back(b);

        // To vertex
        vertices.push_back(c.toX);
        vertices.push_back(c.toY);
        vertices.push_back(r);
        vertices.push_back(g);
        vertices.push_back(b);
    }

    linkVertexCount_ = vertices.size() / VERTEX_SIZE;

    glBindVertexArray(linkVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, linkVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    buffersNeedUpdate_ = false;
}

void CountsRenderer::render() {
    if (!initialized_ || !showCounts_ || !countsData_) return;

    // Set here, not only in initialize(): the state is shared, and what drew
    // before (QPainter overlays, the tile layer) leaves it changed
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (buffersNeedUpdate_) {
        buildBuffers();
    }

    if (linkVertexCount_ == 0) return;

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    glUseProgram(linkProgram_);

    GLint mvpLoc = glGetUniformLocation(linkProgram_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

    glLineWidth(linkWidth_);
    glBindVertexArray(linkVAO_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(linkVertexCount_));

    glBindVertexArray(0);
    glUseProgram(0);
}

uint32_t CountsRenderer::findCountLinkAt(double worldX, double worldY, double radius) const {
    if (!countsData_ || !showCounts_) return 0;

    double bestDist = radius;
    uint32_t bestLinkId = 0;

    for (const auto& c : countsData_->counts) {
        // Skip links with no geometry
        if (c.fromX == 0 && c.fromY == 0 && c.toX == 0 && c.toY == 0) continue;

        // Point-to-segment distance
        double ax = c.fromX, ay = c.fromY;
        double bx = c.toX, by = c.toY;
        double abx = bx - ax, aby = by - ay;
        double apx = worldX - ax, apy = worldY - ay;
        double abLenSq = abx * abx + aby * aby;

        double dist;
        if (abLenSq < 1e-10) {
            dist = std::sqrt(apx * apx + apy * apy);
        } else {
            double t = std::clamp((apx * abx + apy * aby) / abLenSq, 0.0, 1.0);
            double projX = ax + t * abx;
            double projY = ay + t * aby;
            double dx = worldX - projX;
            double dy = worldY - projY;
            dist = std::sqrt(dx * dx + dy * dy);
        }

        if (dist < bestDist) {
            bestDist = dist;
            bestLinkId = c.linkId;
        }
    }

    return bestLinkId;
}

} // namespace simvis
