// SPDX-License-Identifier: GPL-3.0-or-later

#include "transit_route_renderer.h"
#include <QDebug>
#include <unordered_map>
#include <algorithm>

namespace simvis {

// Stop vertex format: x, y, r, g, b
constexpr int VERTEX_SIZE = 5;

// Route quad vertex format: posX, posY, otherX, otherY, side, r, g, b
// Each line segment is expanded into a quad (2 triangles, 6 vertices) whose
// width is computed in screen space, so thickness works in the GL Core profile
// (glLineWidth is clamped to 1px on most drivers).
constexpr int ROUTE_VERTEX_SIZE = 8;

static const char* routeVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;      // this endpoint (world)
layout (location = 1) in vec2 aOther;    // opposite endpoint of the segment (world)
layout (location = 2) in float aSide;    // +1 / -1 : which side to offset
layout (location = 3) in vec3 aColor;

uniform mat4 uMVP;
uniform vec2 uViewport;   // viewport size in pixels
uniform float uHalfWidth; // half line width in pixels

out vec3 vColor;

void main() {
    vec4 clipPos   = uMVP * vec4(aPos,   0.0, 1.0);
    vec4 clipOther = uMVP * vec4(aOther, 0.0, 1.0);

    // Convert both endpoints to screen (pixel) space
    vec2 screenPos   = clipPos.xy   / clipPos.w   * 0.5 * uViewport;
    vec2 screenOther = clipOther.xy / clipOther.w * 0.5 * uViewport;

    vec2 dir = screenOther - screenPos;
    float len = length(dir);
    vec2 normal = (len > 0.0) ? vec2(-dir.y, dir.x) / len : vec2(0.0);

    // Offset in pixels, convert back to NDC and apply (accounting for w)
    vec2 offsetPx = normal * aSide * uHalfWidth;
    vec2 offsetNdc = offsetPx / (0.5 * uViewport);

    gl_Position = clipPos;
    gl_Position.xy += offsetNdc * clipPos.w;
    vColor = aColor;
}
)";

static const char* routeFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 0.85);
}
)";

static const char* stopVertexShader = R"(
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

static const char* stopFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    // Circular point with white outline
    vec2 coord = gl_PointCoord - vec2(0.5);
    float dist = length(coord);
    if (dist > 0.5) discard;

    // White outline ring
    if (dist > 0.38) {
        FragColor = vec4(1.0, 1.0, 1.0, 1.0);
    } else {
        FragColor = vec4(vColor, 1.0);
    }
}
)";

TransitRouteRenderer::TransitRouteRenderer() = default;

TransitRouteRenderer::~TransitRouteRenderer() {
    cleanup();
}

bool TransitRouteRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    routeProgram_ = compileShader(routeVertexShader, routeFragmentShader);
    if (!routeProgram_) return false;

    stopProgram_ = compileShader(stopVertexShader, stopFragmentShader);
    if (!stopProgram_) return false;

    // Create VAOs and VBOs for routes (one per mode)
    glGenVertexArrays(1, &busRouteVAO_);
    glGenBuffers(1, &busRouteVBO_);
    glGenVertexArrays(1, &tramRouteVAO_);
    glGenBuffers(1, &tramRouteVBO_);
    glGenVertexArrays(1, &railRouteVAO_);
    glGenBuffers(1, &railRouteVBO_);

    // Create VAO and VBO for stops
    glGenVertexArrays(1, &stopVAO_);
    glGenBuffers(1, &stopVBO_);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return true;
}

void TransitRouteRenderer::cleanup() {
    auto deleteVAOVBO = [this](GLuint& vao, GLuint& vbo) {
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    };

    deleteVAOVBO(busRouteVAO_, busRouteVBO_);
    deleteVAOVBO(tramRouteVAO_, tramRouteVBO_);
    deleteVAOVBO(railRouteVAO_, railRouteVBO_);
    deleteVAOVBO(stopVAO_, stopVBO_);

    if (routeProgram_) { glDeleteProgram(routeProgram_); routeProgram_ = 0; }
    if (stopProgram_) { glDeleteProgram(stopProgram_); stopProgram_ = 0; }

    GLRenderer::cleanup();
}

void TransitRouteRenderer::setTransitData(const TransitScheduleParser::Result* data) {
    transitData_ = data;
    buffersBuilt_ = false;
}

void TransitRouteRenderer::setNetworkIndex(NetworkIndex* index) {
    networkIndex_ = index;
    buffersBuilt_ = false;
}

const TransitRoute* TransitRouteRenderer::pickRepresentativeRoute(const TransitLine& line) const {
    if (line.routes.empty()) return nullptr;

    // Pick route with most links (best geometry coverage)
    const TransitRoute* best = &line.routes[0];
    for (const auto& route : line.routes) {
        if (route.linkIds.size() > best->linkIds.size()) {
            best = &route;
        }
    }
    return best;
}

void TransitRouteRenderer::buildBuffers() {
    if (!transitData_ || !networkIndex_) return;

    buildRouteBuffers();
    buildStopBuffer();
    buffersBuilt_ = true;
}

void TransitRouteRenderer::buildRouteBuffers() {
    // Build node ID -> index map for coordinate lookup
    const auto& nodes = networkIndex_->allNodes();
    const auto& links = networkIndex_->allLinks();

    std::unordered_map<uint32_t, size_t> nodeIdToIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIdToIndex[nodes[i].id] = i;
    }

    std::unordered_map<uint32_t, size_t> linkIdToIndex;
    for (size_t i = 0; i < links.size(); ++i) {
        linkIdToIndex[links[i].id] = i;
    }

    // Collect vertices per mode (quad geometry: 6 verts per segment)
    std::vector<float> busVertices, tramVertices, railVertices;
    busDrawCmds_.clear();
    tramDrawCmds_.clear();
    railDrawCmds_.clear();
    stopColors_.clear();

    // Shared palette counter for tram/rail routes: each gets a distinct color,
    // none reused across tram or rail. Bus routes keep the single shared color.
    size_t paletteIndex = 0;

    // Emit one segment (from->to) as a quad of 6 vertices into the buffer.
    // Vertex layout: posX, posY, otherX, otherY, side, r, g, b
    auto addSegment = [](std::vector<float>& v, float ax, float ay, float bx, float by,
                         const TransitModeColor& c) {
        auto vert = [&](float px, float py, float ox, float oy, float side) {
            v.push_back(px); v.push_back(py);
            v.push_back(ox); v.push_back(oy);
            v.push_back(side);
            v.push_back(c.r); v.push_back(c.g); v.push_back(c.b);
        };
        // Each vertex offsets perpendicular to the segment in screen space.
        // The shader's normal flips sign depending on which endpoint is "pos",
        // so to stay on the same physical side, vertex b uses the opposite sign
        // of vertex a. Quad corners around the perimeter:
        //   C1 = a(+1)  C2 = b(-1)   [one long edge]
        //   C3 = b(+1)  C4 = a(-1)   [other long edge]
        // Fan from C1: (C1,C2,C3), (C1,C3,C4).
        vert(ax, ay, bx, by, +1.0f);   // C1
        vert(bx, by, ax, ay, -1.0f);   // C2
        vert(bx, by, ax, ay, +1.0f);   // C3
        vert(ax, ay, bx, by, +1.0f);   // C1
        vert(bx, by, ax, ay, +1.0f);   // C3
        vert(ax, ay, bx, by, -1.0f);   // C4
    };

    for (const auto& line : transitData_->lines) {
        const TransitRoute* route = pickRepresentativeRoute(line);
        if (!route || route->linkIds.empty()) continue;

        // Bus: uniform mode color. Tram/Rail: distinct color per route from palette.
        TransitModeColor color;
        if (route->mode == TransitMode::Tram || route->mode == TransitMode::Rail) {
            color = getRoutePaletteColor(paletteIndex++);
        } else {
            color = getTransitModeColor(route->mode);
        }

        // Record this color for every stop served by the representative route, so
        // stop markers match their route color. First writer wins (keeps stops
        // colored by the first route drawn over them).
        for (const auto& rs : route->stops) {
            stopColors_.emplace(rs.stopId, color);
        }

        // Select target buffers
        std::vector<float>* vertices = nullptr;
        std::vector<RouteDrawCmd>* drawCmds = nullptr;

        switch (route->mode) {
            case TransitMode::Bus:
                vertices = &busVertices;
                drawCmds = &busDrawCmds_;
                break;
            case TransitMode::Tram:
                vertices = &tramVertices;
                drawCmds = &tramDrawCmds_;
                break;
            case TransitMode::Rail:
                vertices = &railVertices;
                drawCmds = &railDrawCmds_;
                break;
            default:
                vertices = &busVertices;
                drawCmds = &busDrawCmds_;
                break;
        }

        GLint firstVertex = static_cast<GLint>(vertices->size() / ROUTE_VERTEX_SIZE);

        // Walk the link sequence, emitting a quad per drawn segment.
        for (uint32_t linkId : route->linkIds) {
            auto linkIt = linkIdToIndex.find(linkId);
            if (linkIt == linkIdToIndex.end()) continue;

            const auto& link = links[linkIt->second];

            auto fromIt = nodeIdToIndex.find(link.fromNode);
            auto toIt = nodeIdToIndex.find(link.toNode);
            if (fromIt == nodeIdToIndex.end() || toIt == nodeIdToIndex.end()) continue;

            const auto& fromNode = nodes[fromIt->second];
            const auto& toNode = nodes[toIt->second];

            // Skip zero-length self-loops (pt_ artificial links)
            if (fromNode.x == toNode.x && fromNode.y == toNode.y) continue;

            addSegment(*vertices, fromNode.x, fromNode.y, toNode.x, toNode.y, color);
        }

        GLsizei vertexCount =
            static_cast<GLsizei>(vertices->size() / ROUTE_VERTEX_SIZE) - firstVertex;
        if (vertexCount > 0) {
            drawCmds->push_back({firstVertex, vertexCount, line.id});
        }
    }

    // Upload to GPU
    auto uploadBuffer = [this](GLuint vao, GLuint vbo, const std::vector<float>& vertices) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                     vertices.data(), GL_STATIC_DRAW);

        const GLsizei stride = ROUTE_VERTEX_SIZE * sizeof(float);
        // aPos
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        // aOther
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // aSide
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
        glEnableVertexAttribArray(2);
        // aColor
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);
    };

    uploadBuffer(busRouteVAO_, busRouteVBO_, busVertices);
    uploadBuffer(tramRouteVAO_, tramRouteVBO_, tramVertices);
    uploadBuffer(railRouteVAO_, railRouteVBO_, railVertices);

    busVertexCount_ = busVertices.size() / ROUTE_VERTEX_SIZE;
    tramVertexCount_ = tramVertices.size() / ROUTE_VERTEX_SIZE;
    railVertexCount_ = railVertices.size() / ROUTE_VERTEX_SIZE;
}

void TransitRouteRenderer::buildStopBuffer() {
    if (!transitData_) return;

    // Collect unique stops per mode for visibility toggling.
    // A stop may serve multiple modes; assign it to the "highest priority" mode.
    // Priority: Rail > Tram > Bus (rail stops are rarest, most important to show)
    std::unordered_map<uint32_t, TransitMode> stopModeMap;

    for (const auto& line : transitData_->lines) {
        for (const auto& route : line.routes) {
            for (const auto& rs : route.stops) {
                auto it = stopModeMap.find(rs.stopId);
                if (it == stopModeMap.end()) {
                    stopModeMap[rs.stopId] = route.mode;
                } else {
                    // Upgrade priority: Rail > Tram > Bus
                    if (route.mode == TransitMode::Rail ||
                        (route.mode == TransitMode::Tram && it->second == TransitMode::Bus)) {
                        it->second = route.mode;
                    }
                }
            }
        }
    }

    // Sort stops by mode so we can render ranges
    struct StopEntry {
        TransitMode mode;
        float x, y;
        TransitModeColor color;
    };
    std::vector<StopEntry> busStops, tramStops, railStops;

    for (const auto& [stopId, mode] : stopModeMap) {
        auto it = transitData_->stopIdToIndex.find(stopId);
        if (it == transitData_->stopIdToIndex.end()) continue;

        const auto& stop = transitData_->stops[it->second];

        // Use the per-route color recorded during route building so stops match
        // their route. Fall back to the mode color if no route covered this stop.
        TransitModeColor color;
        auto cit = stopColors_.find(stopId);
        if (cit != stopColors_.end()) {
            color = cit->second;
        } else {
            color = getTransitModeColor(mode);
        }

        StopEntry entry{mode, stop.x, stop.y, color};

        switch (mode) {
            case TransitMode::Bus:  busStops.push_back(entry); break;
            case TransitMode::Tram: tramStops.push_back(entry); break;
            case TransitMode::Rail: railStops.push_back(entry); break;
            default: busStops.push_back(entry); break;
        }
    }

    // Build vertex buffer: bus stops, then tram stops, then rail stops
    std::vector<float> vertices;
    vertices.reserve((busStops.size() + tramStops.size() + railStops.size()) * VERTEX_SIZE);

    auto addStops = [&vertices](const std::vector<StopEntry>& stops) {
        for (const auto& s : stops) {
            vertices.push_back(s.x);
            vertices.push_back(s.y);
            vertices.push_back(s.color.r);
            vertices.push_back(s.color.g);
            vertices.push_back(s.color.b);
        }
    };

    GLint offset = 0;
    busStopRange_.first = offset;
    busStopRange_.count = static_cast<GLsizei>(busStops.size());
    addStops(busStops);
    offset += busStopRange_.count;

    tramStopRange_.first = offset;
    tramStopRange_.count = static_cast<GLsizei>(tramStops.size());
    addStops(tramStops);
    offset += tramStopRange_.count;

    railStopRange_.first = offset;
    railStopRange_.count = static_cast<GLsizei>(railStops.size());
    addStops(railStops);

    stopVertexCount_ = vertices.size() / VERTEX_SIZE;

    // Upload
    glBindVertexArray(stopVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, stopVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void TransitRouteRenderer::render() {
    if (!initialized_ || !transitData_ || !networkIndex_) return;

    if (!buffersBuilt_) {
        buildBuffers();
    }

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    // =====================================================================
    // Render route polylines (as screen-space quads — glLineWidth is clamped
    // to 1px in the GL Core profile, so width is computed in the shader)
    // =====================================================================
    glUseProgram(routeProgram_);
    GLint mvpLoc = glGetUniformLocation(routeProgram_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

    GLint vpLoc = glGetUniformLocation(routeProgram_, "uViewport");
    glUniform2f(vpLoc, static_cast<float>(viewportWidth_),
                static_cast<float>(viewportHeight_));

    GLint hwLoc = glGetUniformLocation(routeProgram_, "uHalfWidth");

    auto renderRoutes = [&](GLuint vao, const std::vector<RouteDrawCmd>& cmds,
                            float lineWidth) {
        if (cmds.empty()) return;
        glBindVertexArray(vao);

        for (const auto& cmd : cmds) {
            bool isHighlighted = (highlightedLineId_ != 0 && cmd.lineId == highlightedLineId_);
            float w = isHighlighted ? lineWidth * 2.0f : lineWidth;
            glUniform1f(hwLoc, w * 0.5f);
            glDrawArrays(GL_TRIANGLES, cmd.first, cmd.count);
        }
    };

    if (showBus_) {
        renderRoutes(busRouteVAO_, busDrawCmds_, routeLineWidth_);
    }
    if (showTram_) {
        renderRoutes(tramRouteVAO_, tramDrawCmds_, routeLineWidth_);
    }
    if (showRail_) {
        renderRoutes(railRouteVAO_, railDrawCmds_, routeLineWidth_);
    }

    // =====================================================================
    // Render stop circles
    // =====================================================================
    glUseProgram(stopProgram_);
    mvpLoc = glGetUniformLocation(stopProgram_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

    GLint sizeLoc = glGetUniformLocation(stopProgram_, "uPointSize");
    glUniform1f(sizeLoc, 5.0f * pointScale_);

    glBindVertexArray(stopVAO_);

    if (showBus_ && showBusStops_ && busStopRange_.count > 0) {
        glDrawArrays(GL_POINTS, busStopRange_.first, busStopRange_.count);
    }
    if (showTram_ && tramStopRange_.count > 0) {
        glDrawArrays(GL_POINTS, tramStopRange_.first, tramStopRange_.count);
    }
    if (showRail_ && railStopRange_.count > 0) {
        glDrawArrays(GL_POINTS, railStopRange_.first, railStopRange_.count);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

int TransitRouteRenderer::findStopAt(double worldX, double worldY, double radius) const {
    if (!transitData_) return -1;

    double bestDist = radius * radius;
    int bestIndex = -1;

    for (size_t i = 0; i < transitData_->stops.size(); ++i) {
        const auto& stop = transitData_->stops[i];
        double dx = stop.x - worldX;
        double dy = stop.y - worldY;
        double distSq = dx * dx + dy * dy;
        if (distSq < bestDist) {
            bestDist = distSq;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

uint32_t TransitRouteRenderer::findRouteAt(double worldX, double worldY, double radius) const {
    if (!transitData_ || !networkIndex_) return 0;

    // Build link lookup (could cache, but transit lines are few)
    const auto& nodes = networkIndex_->allNodes();
    const auto& links = networkIndex_->allLinks();

    std::unordered_map<uint32_t, size_t> nodeIdToIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIdToIndex[nodes[i].id] = i;
    }
    std::unordered_map<uint32_t, size_t> linkIdToIndex;
    for (size_t i = 0; i < links.size(); ++i) {
        linkIdToIndex[links[i].id] = i;
    }

    double bestDist = radius;
    uint32_t bestLineId = 0;

    for (const auto& line : transitData_->lines) {
        // Check visibility
        bool visible = false;
        switch (line.primaryMode) {
            case TransitMode::Bus:  visible = showBus_; break;
            case TransitMode::Tram: visible = showTram_; break;
            case TransitMode::Rail: visible = showRail_; break;
            default: visible = showBus_; break;
        }
        if (!visible) continue;

        // Check representative route's link segments
        const TransitRoute* route = pickRepresentativeRoute(line);
        if (!route) continue;

        for (uint32_t linkId : route->linkIds) {
            auto linkIt = linkIdToIndex.find(linkId);
            if (linkIt == linkIdToIndex.end()) continue;

            const auto& link = links[linkIt->second];
            auto fromIt = nodeIdToIndex.find(link.fromNode);
            auto toIt = nodeIdToIndex.find(link.toNode);
            if (fromIt == nodeIdToIndex.end() || toIt == nodeIdToIndex.end()) continue;

            const auto& fromNode = nodes[fromIt->second];
            const auto& toNode = nodes[toIt->second];

            // Point-to-segment distance
            double ax = fromNode.x, ay = fromNode.y;
            double bx = toNode.x, by = toNode.y;
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
                bestLineId = line.id;
            }
        }
    }

    return bestLineId;
}

} // namespace simvis
