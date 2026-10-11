#include "person_route_renderer.h"
#include <cmath>

namespace simvis {

// Vertex: x, y, alpha (color is a uniform)
constexpr int VERTEX_SIZE = 3;

static const char* routeVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in float aAlpha;

uniform mat4 uMVP;

out float vAlpha;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    vAlpha = aAlpha;
}
)";

static const char* routeFragmentShader = R"(
#version 330 core
in float vAlpha;
out vec4 FragColor;

uniform vec3 uColor;

void main() {
    FragColor = vec4(uColor, vAlpha);
}
)";

PersonRouteRenderer::PersonRouteRenderer() = default;

PersonRouteRenderer::~PersonRouteRenderer() {
    cleanup();
}

bool PersonRouteRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    program_ = compileShader(routeVertexShader, routeFragmentShader);
    if (!program_) return false;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    return true;
}

void PersonRouteRenderer::cleanup() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    GLRenderer::cleanup();
}

void PersonRouteRenderer::setRoute(const std::vector<Segment>& segments) {
    segments_ = segments;
    buffersNeedUpdate_ = true;
}

void PersonRouteRenderer::buildBuffers() {
    std::vector<float> vertices;
    vertices.reserve(segments_.size() * 6 * VERTEX_SIZE);

    for (const auto& seg : segments_) {
        float dx = seg.x2 - seg.x1;
        float dy = seg.y2 - seg.y1;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;

        // Perpendicular offset for quad thickness
        float nx = -dy / len * halfWidth_;
        float ny =  dx / len * halfWidth_;

        // Teleported (walk/bike) connectors drawn more transparent
        float alpha = seg.teleported ? 0.35f : 0.75f;

        // Two triangles: (a-,a+,b-) and (a+,b+,b-)
        float ax1 = seg.x1 - nx, ay1 = seg.y1 - ny;
        float ax2 = seg.x1 + nx, ay2 = seg.y1 + ny;
        float bx1 = seg.x2 - nx, by1 = seg.y2 - ny;
        float bx2 = seg.x2 + nx, by2 = seg.y2 + ny;

        const float quad[6][2] = {
            {ax1, ay1}, {ax2, ay2}, {bx1, by1},
            {ax2, ay2}, {bx2, by2}, {bx1, by1},
        };
        for (const auto& v : quad) {
            vertices.push_back(v[0]);
            vertices.push_back(v[1]);
            vertices.push_back(alpha);
        }
    }

    vertexCount_ = vertices.size() / VERTEX_SIZE;

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.empty() ? nullptr : vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    buffersNeedUpdate_ = false;
}

void PersonRouteRenderer::render() {
    if (!initialized_ || !visible_) return;

    if (buffersNeedUpdate_) {
        buildBuffers();
    }
    if (vertexCount_ == 0) return;

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    glUseProgram(program_);
    GLint mvpLoc = glGetUniformLocation(program_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());
    glUniform3f(glGetUniformLocation(program_, "uColor"), colorR_, colorG_, colorB_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertexCount_));
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace simvis
