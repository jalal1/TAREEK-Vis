// SPDX-License-Identifier: GPL-3.0-or-later

#include "network_renderer.h"
#include "link_palette.h"
#include "core/logger.h"
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <limits>

namespace simvis {

// Node vertex: x, y, z, r, g, b. z is the node height (0 on the ground).
constexpr int VERTEX_SIZE = 6;
// Link vertex: x, y, z, road type. The color comes from uRoadColors, so the
// buffer does not change with the color mode.
constexpr int LINK_VERTEX_SIZE = 4;
constexpr int kRoadTypeCount = 16;   // size of uRoadColors; RoadType has 14
constexpr size_t kMaxClassLimits = 8;
constexpr int kGridCells = 128;      // cells per side of the culling grid
// Above this zoom the links of the value path are 1 px lines, not quads.
// At the Twin Cities overview (about 300 m/px) all 634,886 links are in view,
// the quads cost about 11 ms more per frame on an integrated GPU, and most
// links are only a few pixels long.
constexpr double kLineModeMetresPerPx = 60.0;

static const char* nodeVertexShader = R"(
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

static const char* nodeFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    // Circular point
    vec2 coord = gl_PointCoord - vec2(0.5);
    if (length(coord) > 0.5) discard;

    FragColor = vec4(vColor, 1.0);
}
)";

static const char* linkVertexShader = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in float aRoadType;

uniform mat4 uMVP;
uniform float uLiftPx;     // screen pixels per metre of height
uniform float uViewportH;
uniform vec3 uRoadColors[16];

out vec3 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos.xy, 0.0, 1.0);
    gl_Position.y += aPos.z * uLiftPx * 2.0 / uViewportH * gl_Position.w;
    vColor = uRoadColors[int(aRoadType + 0.5)];
}
)";

// Links colored by value: one instance per link, a quad of 4 vertices
// (triangle strip) made in screen space. The width and the offset to the
// right side are in pixels, so they do not change with the zoom.
static const char* valueVertexShader = R"(
#version 330 core
layout (location = 0) in uint aLink;

uniform samplerBuffer uGeom;     // x0, y0, x1, y1 per link
uniform samplerBuffer uValues;   // value per link
uniform mat4 uMVP;
uniform vec2 uViewport;          // pixels
uniform float uHalfWidths[9];    // half width of each class, pixels
uniform float uNoDataHalfPx;     // half width of a link with no data
uniform float uNoDataBelow;
uniform float uLimits[8];
uniform int uLimitCount;
uniform vec3 uColors[9];
uniform vec3 uNoDataColor;
uniform float uOffsetOn;         // 1: two-direction offset, 0: on the center line

out vec3 vColor;
out float vAcrossPx;            // signed distance from the center line, pixels
flat out float vHalfPx;
flat out float vHasData;

void main() {
    int link = int(aLink);
    vec4 g = texelFetch(uGeom, link);
    float v = texelFetch(uValues, link).r;
    bool hasData = v > uNoDataBelow;

    int cls = 0;
    for (int i = 0; i < uLimitCount; ++i) {
        if (v > uLimits[i]) cls++;
    }
    vColor = hasData ? uColors[cls] : uNoDataColor;
    vHasData = hasData ? 1.0 : 0.0;

    vec4 c0 = uMVP * vec4(g.xy, 0.0, 1.0);
    vec4 c1 = uMVP * vec4(g.zw, 0.0, 1.0);
    vec2 halfVp = 0.5 * uViewport;
    vec2 s0 = c0.xy / c0.w * halfVp;
    vec2 s1 = c1.xy / c1.w * halfVp;
    vec2 d = s1 - s0;
    float len = length(d);
    vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    // Right side of the direction of travel (y is up on the screen)
    vec2 n = vec2(dir.y, -dir.x);

    float halfPx = hasData ? uHalfWidths[cls] : uNoDataHalfPx;
    // The two directions of a road sit side by side: each one moves by its
    // half width to its right. A link with no data stays on the center line.
    float offset = hasData ? halfPx * uOffsetOn : 0.0;
    // Half a pixel more on each side for the antialiased edge
    float side = (gl_VertexID & 1) == 0 ? -1.0 : 1.0;
    float across = side * (halfPx + 0.5);
    vec2 p = ((gl_VertexID >> 1) == 0 ? s0 : s1) + n * (offset + across);

    gl_Position = vec4(p / halfVp, 0.0, 1.0);
    vAcrossPx = across;
    vHalfPx = halfPx;
}
)";

// Links colored by value at the overview zoom: plain 1 px lines from the
// static line buffer (not instanced; 2 vertices per link with instancing are
// slow on an integrated GPU). The width, the edge and the offset cannot be
// seen when a link is a few pixels long.
static const char* valueLineVertexShader = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in uint aLinkId;

uniform samplerBuffer uValues;
uniform mat4 uMVP;
uniform float uNoDataBelow;
uniform float uLimits[8];
uniform int uLimitCount;
uniform vec3 uColors[9];
uniform vec3 uNoDataColor;
uniform float uNoDataAlpha;

out vec4 vColor;

void main() {
    float v = texelFetch(uValues, int(aLinkId)).r;
    int cls = 0;
    for (int i = 0; i < uLimitCount; ++i) {
        if (v > uLimits[i]) cls++;
    }
    vColor = v > uNoDataBelow ? vec4(uColors[cls], 1.0) : vec4(uNoDataColor, uNoDataAlpha);
    gl_Position = uMVP * vec4(aPos.xy, 0.0, 1.0);
}
)";

static const char* valueLineFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

static const char* valueFragmentShader = R"(
#version 330 core
in vec3 vColor;
in float vAcrossPx;
flat in float vHalfPx;
flat in float vHasData;
out vec4 FragColor;

uniform float uEdgePx;
uniform vec3 uEdgeColor;
uniform float uEdgeAlpha;
uniform float uNoDataAlpha;

void main() {
    float d = abs(vAcrossPx);
    float coverage = clamp(vHalfPx - d + 0.5, 0.0, 1.0);
    if (vHasData < 0.5) {
        FragColor = vec4(vColor, uNoDataAlpha * coverage);
        return;
    }
    // A thin dark edge, so that the color reads on light and dark maps. On a
    // thin link the edge is thinner, or it hides the color.
    float edgePx = min(uEdgePx, 0.4 * vHalfPx);
    float edge = smoothstep(vHalfPx - edgePx - 0.5, vHalfPx - edgePx + 0.5, d);
    vec3 color = mix(vColor, uEdgeColor, edge * uEdgeAlpha);
    FragColor = vec4(color, coverage);
}
)";

static const char* linkFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

uniform float uAlpha;

void main() {
    FragColor = vec4(vColor, uAlpha);
}
)";

NetworkRenderer::NetworkRenderer() = default;

NetworkRenderer::~NetworkRenderer() {
    cleanup();
}

bool NetworkRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    // Compile shaders
    nodeProgram_ = compileShader(nodeVertexShader, nodeFragmentShader);
    if (!nodeProgram_) return false;

    linkProgram_ = compileShader(linkVertexShader, linkFragmentShader);
    if (!linkProgram_) return false;

    valueProgram_ = compileShader(valueVertexShader, valueFragmentShader);
    if (!valueProgram_) return false;

    valueLineProgram_ = compileShader(valueLineVertexShader, valueLineFragmentShader);
    if (!valueLineProgram_) return false;

    // Create VAOs and VBOs
    glGenVertexArrays(1, &nodeVAO_);
    glGenBuffers(1, &nodeVBO_);

    glGenVertexArrays(1, &linkVAO_);
    glGenBuffers(1, &linkVBO_);

    glGenVertexArrays(1, &valueVAO_);
    glGenBuffers(1, &orderVBO_);
    glGenBuffers(1, &geomBuffer_);
    glGenBuffers(1, &valueBuffer_);
    glGenTextures(1, &geomTexture_);
    glGenTextures(1, &valueTexture_);
    glGenBuffers(1, &linkIdVBO_);
    glGenBuffers(1, &lineEBO_);

    // Enable point sprites
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return true;
}

void NetworkRenderer::cleanup() {
    if (nodeVAO_) {
        glDeleteVertexArrays(1, &nodeVAO_);
        nodeVAO_ = 0;
    }
    if (nodeVBO_) {
        glDeleteBuffers(1, &nodeVBO_);
        nodeVBO_ = 0;
    }
    if (nodeProgram_) {
        glDeleteProgram(nodeProgram_);
        nodeProgram_ = 0;
    }

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

    if (valueProgram_) { glDeleteProgram(valueProgram_); valueProgram_ = 0; }
    if (valueLineProgram_) { glDeleteProgram(valueLineProgram_); valueLineProgram_ = 0; }
    if (valueVAO_) { glDeleteVertexArrays(1, &valueVAO_); valueVAO_ = 0; }
    for (GLuint* b : {&orderVBO_, &geomBuffer_, &valueBuffer_, &linkIdVBO_, &lineEBO_}) {
        if (*b) { glDeleteBuffers(1, b); *b = 0; }
    }
    for (GLuint* t : {&geomTexture_, &valueTexture_}) {
        if (*t) { glDeleteTextures(1, t); *t = 0; }
    }

    GLRenderer::cleanup();
}

void NetworkRenderer::setNetworkIndex(NetworkIndex* index) {
    networkIndex_ = index;
    buffersNeedUpdate_ = true;
    // Values of another network mean nothing here
    clearLinkValues();
}

void NetworkRenderer::setLinkValues(const std::vector<float>& values, const LinkClasses& classes) {
    linkValues_ = values;
    linkClasses_ = classes;
    if (linkClasses_.limits.size() > kMaxClassLimits) linkClasses_.limits.resize(kMaxClassLimits);
    linkClasses_.colors.resize(linkClasses_.limits.size() + 1, {0.6f, 0.6f, 0.6f});
    linkClasses_.halfWidthsPx.resize(linkClasses_.limits.size() + 1, 1.75f);
    colorByValue_ = true;
    valuesNeedUpload_ = true;
}

void NetworkRenderer::clearLinkValues() {
    colorByValue_ = false;
    valuesNeedUpload_ = false;
    linkValues_.clear();
    linkValues_.shrink_to_fit();
}

void NetworkRenderer::getRoadTypeColor(RoadType type, float& r, float& g, float& b) {
    switch (type) {
        case RoadType::Motorway:
            r = 0.9f; g = 0.3f; b = 0.3f;  // Red
            break;
        case RoadType::Primary:
            r = 0.9f; g = 0.6f; b = 0.2f;  // Orange
            break;
        case RoadType::Secondary:
            r = 0.9f; g = 0.9f; b = 0.3f;  // Yellow
            break;
        case RoadType::Tertiary:
            r = 0.7f; g = 0.7f; b = 0.7f;  // Light gray
            break;
        case RoadType::Residential:
            r = 0.5f; g = 0.5f; b = 0.5f;  // Gray
            break;
        case RoadType::Service:
            r = 0.4f; g = 0.4f; b = 0.4f;  // Dark gray
            break;
        // Air links by flight phase
        case RoadType::AirVertical:
            r = 0.85f; g = 0.85f; b = 0.95f;  // White
            break;
        case RoadType::AirClimb:
            r = 0.40f; g = 0.80f; b = 0.45f;  // Green
            break;
        case RoadType::AirCruise:
            r = 0.35f; g = 0.60f; b = 0.95f;  // Blue
            break;
        case RoadType::AirDescent:
            r = 0.95f; g = 0.60f; b = 0.30f;  // Orange
            break;
        case RoadType::AirHover:
            r = 0.95f; g = 0.90f; b = 0.35f;  // Yellow
            break;
        default:
            r = 0.6f; g = 0.6f; b = 0.6f;  // Default gray
            break;
    }
}

void NetworkRenderer::buildNodeBuffer() {
    if (!networkIndex_) return;

    const auto& nodes = networkIndex_->allNodes();
    if (nodes.empty()) return;

    std::vector<float> vertices;
    vertices.reserve(nodes.size() * VERTEX_SIZE);

    for (const auto& node : nodes) {
        vertices.push_back(node.x);
        vertices.push_back(node.y);
        vertices.push_back(node.z);
        // Node color: blue
        vertices.push_back(0.2f);
        vertices.push_back(0.4f);
        vertices.push_back(0.8f);
    }

    nodeVertexCount_ = nodes.size();

    glBindVertexArray(nodeVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, nodeVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void NetworkRenderer::buildLinkBuffer() {
    if (!networkIndex_) return;

    const auto& links = networkIndex_->allLinks();
    const auto& nodes = networkIndex_->allNodes();

    if (links.empty() || nodes.empty()) return;

    // Build node ID to index map for quick lookup
    std::unordered_map<uint32_t, size_t> nodeIdToIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIdToIndex[nodes[i].id] = i;
    }

    std::vector<float> vertices;
    vertices.reserve(links.size() * 2 * LINK_VERTEX_SIZE);  // 2 vertices per link
    // End points of each road link, for the value path (index = link index)
    std::vector<float> geometry(links.size() * 4, 0.0f);
    roadLinkDrawable_.assign(links.size(), 0);
    linkFirstVertex_.assign(links.size(), UINT32_MAX);
    std::vector<uint32_t> vertexLinkIds;
    vertexLinkIds.reserve(links.size() * 2);

    // Road links first, then air links, so each kind is one draw range
    for (const bool air : {false, true}) {
        if (air) airVertexStart_ = vertices.size() / LINK_VERTEX_SIZE;
        for (size_t i = 0; i < links.size(); ++i) {
            const auto& link = links[i];
            if (isAirRoadType(static_cast<RoadType>(link.roadType)) != air) continue;
            auto fromIt = nodeIdToIndex.find(link.fromNode);
            auto toIt = nodeIdToIndex.find(link.toNode);

            if (fromIt == nodeIdToIndex.end() || toIt == nodeIdToIndex.end()) {
                continue;
            }

            const auto& fromNode = nodes[fromIt->second];
            const auto& toNode = nodes[toIt->second];
            const float type = static_cast<float>(link.roadType);

            if (!air) linkFirstVertex_[i] = static_cast<uint32_t>(vertices.size() / LINK_VERTEX_SIZE);
            vertices.insert(vertices.end(), {fromNode.x, fromNode.y, fromNode.z, type});
            vertices.insert(vertices.end(), {toNode.x, toNode.y, toNode.z, type});
            vertexLinkIds.insert(vertexLinkIds.end(), {static_cast<uint32_t>(i), static_cast<uint32_t>(i)});

            if (!air) {
                geometry[i * 4 + 0] = fromNode.x;
                geometry[i * 4 + 1] = fromNode.y;
                geometry[i * 4 + 2] = toNode.x;
                geometry[i * 4 + 3] = toNode.y;
                roadLinkDrawable_[i] = 1;
            }
        }
    }

    linkVertexCount_ = vertices.size() / LINK_VERTEX_SIZE;

    glBindVertexArray(linkVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, linkVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, LINK_VERTEX_SIZE * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);

    // Road type attribute (index into uRoadColors)
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, LINK_VERTEX_SIZE * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Link id of each vertex, for the value lines at the overview zoom (the
    // road type program does not read it)
    glBindBuffer(GL_ARRAY_BUFFER, linkIdVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertexLinkIds.size() * sizeof(uint32_t), vertexLinkIds.data(),
                 GL_STATIC_DRAW);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    // Static geometry of the value path
    glBindBuffer(GL_TEXTURE_BUFFER, geomBuffer_);
    glBufferData(GL_TEXTURE_BUFFER, geometry.size() * sizeof(float), geometry.data(),
                 GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, geomTexture_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, geomBuffer_);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    glBindVertexArray(valueVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, orderVBO_);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    buildLinkGrid(geometry);

    // The draw order depends on which links can be drawn
    valuesNeedUpload_ = colorByValue_;
}

void NetworkRenderer::buildLinkGrid(const std::vector<float>& geometry) {
    LinkGrid g;
    const size_t n = roadLinkDrawable_.size();
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (size_t i = 0; i < n; ++i) {
        if (!roadLinkDrawable_[i]) continue;
        for (int e = 0; e < 2; ++e) {
            minX = std::min<double>(minX, geometry[i * 4 + e * 2]);
            maxX = std::max<double>(maxX, geometry[i * 4 + e * 2]);
            minY = std::min<double>(minY, geometry[i * 4 + e * 2 + 1]);
            maxY = std::max<double>(maxY, geometry[i * 4 + e * 2 + 1]);
        }
    }
    visibleCells_.clear();
    orderNeedsUpdate_ = true;
    if (minX > maxX) { linkGrid_ = g; return; }
    g.nx = g.ny = kGridCells;
    g.minX = minX;
    g.minY = minY;
    g.cellW = std::max(1.0, (maxX - minX) / g.nx);
    g.cellH = std::max(1.0, (maxY - minY) / g.ny);
    const size_t cells = static_cast<size_t>(g.nx) * g.ny;
    g.cellStart.assign(cells + 1, 0);
    g.cellReach.assign(cells, 0.0f);
    // Each link goes into the cell of its midpoint. The reach of a cell is the
    // longest half length of its links, so a cell is visible when its box,
    // made larger by the reach, touches the view.
    std::vector<uint32_t> cellOf(n, UINT32_MAX);
    for (size_t i = 0; i < n; ++i) {
        if (!roadLinkDrawable_[i]) continue;
        const float* q = &geometry[i * 4];
        const double mx = 0.5 * (q[0] + q[2]), my = 0.5 * (q[1] + q[3]);
        const int cx = std::clamp(static_cast<int>((mx - g.minX) / g.cellW), 0, g.nx - 1);
        const int cy = std::clamp(static_cast<int>((my - g.minY) / g.cellH), 0, g.ny - 1);
        const uint32_t c = static_cast<uint32_t>(cy * g.nx + cx);
        cellOf[i] = c;
        ++g.cellStart[c + 1];
        const float half = 0.5f * std::hypot(q[2] - q[0], q[3] - q[1]);
        g.cellReach[c] = std::max(g.cellReach[c], half);
    }
    for (size_t c = 0; c < cells; ++c) g.cellStart[c + 1] += g.cellStart[c];
    g.links.resize(g.cellStart[cells]);
    std::vector<uint32_t> cursor(g.cellStart.begin(), g.cellStart.end() - 1);
    for (size_t i = 0; i < n; ++i)
        if (cellOf[i] != UINT32_MAX) g.links[cursor[cellOf[i]]++] = static_cast<uint32_t>(i);
    g.maxReach = *std::max_element(g.cellReach.begin(), g.cellReach.end());
    linkGrid_ = std::move(g);
}

void NetworkRenderer::uploadLinkValues() {
    QElapsedTimer timer;
    timer.start();
    valuesNeedUpload_ = false;
    const size_t n = roadLinkDrawable_.size();
    if (n == 0) { instanceCount_ = 0; return; }

    std::vector<float> values(n, std::numeric_limits<float>::lowest());
    std::copy_n(linkValues_.begin(), std::min(n, linkValues_.size()), values.begin());

    glBindBuffer(GL_TEXTURE_BUFFER, valueBuffer_);
    glBufferData(GL_TEXTURE_BUFFER, n * sizeof(float), values.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, valueTexture_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, valueBuffer_);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    // Draw bucket of each link: links with no data first (0), then the classes
    // from the least to the most important, so the worst links are on top
    const int classCount = static_cast<int>(linkClasses_.limits.size()) + 1;
    bucketCount_ = classCount + 1;
    linkBucket_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = values[i];
        int b = 0;
        if (v > linkClasses_.noDataBelow) {
            int c = 0;
            for (float limit : linkClasses_.limits)
                if (v > limit) ++c;
            b = 1 + (linkClasses_.lowOnTop ? classCount - 1 - c : c);
        }
        linkBucket_[i] = static_cast<uint8_t>(b);
    }
    orderNeedsUpdate_ = true;
    LOG_INFO(QString("Link colors: upload and classes in %1 ms").arg(timer.nsecsElapsed() / 1e6, 0, 'f', 2));
}

void NetworkRenderer::updateDrawOrder(bool lines) {
    // The cells that the view touches. The grid is coarse (kGridCells x
    // kGridCells), so this is cheap, and the order is made again only when
    // this list changes.
    const LinkGrid& g = linkGrid_;
    std::vector<uint32_t> cells;
    size_t visibleLinks = 0;
    if (!g.cellStart.empty()) {
        const BoundingBox view = getViewportBounds();
        // A few pixels more for the width and the offset of a link
        const double margin = 8.0 * (view.maxX - view.minX) / std::max(1, viewportWidth_);
        const double reach = g.maxReach + margin;
        auto cellX = [&](double x) {
            return std::clamp(static_cast<int>(std::floor((x - g.minX) / g.cellW)), 0, g.nx - 1);
        };
        auto cellY = [&](double y) {
            return std::clamp(static_cast<int>(std::floor((y - g.minY) / g.cellH)), 0, g.ny - 1);
        };
        const int x0 = cellX(view.minX - reach), x1 = cellX(view.maxX + reach);
        const int y0 = cellY(view.minY - reach), y1 = cellY(view.maxY + reach);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                const uint32_t c = static_cast<uint32_t>(cy * g.nx + cx);
                if (g.cellStart[c + 1] == g.cellStart[c]) continue;
                const double r = g.cellReach[c] + margin;
                const double bx0 = g.minX + cx * g.cellW - r;
                const double bx1 = g.minX + (cx + 1) * g.cellW + r;
                const double by0 = g.minY + cy * g.cellH - r;
                const double by1 = g.minY + (cy + 1) * g.cellH + r;
                if (bx1 < view.minX || bx0 > view.maxX || by1 < view.minY || by0 > view.maxY) continue;
                cells.push_back(c);
                visibleLinks += g.cellStart[c + 1] - g.cellStart[c];
            }
        }
    }
    // Most of the network in view: draw all links, and keep that list while
    // the view moves, so that a pan at the overview zoom costs nothing
    const bool all = visibleLinks * 10 > g.links.size() * 6;
    if (all) cells.assign(1, UINT32_MAX);
    // The list is for quads or for lines: a change of the kind needs a new one
    cells.push_back(lines ? 1u : 0u);
    if (!orderNeedsUpdate_ && cells == visibleCells_) return;
    QElapsedTimer timer;
    timer.start();
    orderNeedsUpdate_ = false;
    visibleCells_ = cells;

    // Counting sort of the visible links by draw bucket
    std::vector<uint32_t> start(static_cast<size_t>(bucketCount_) + 1, 0);
    auto forEachLink = [&](auto&& f) {
        if (all) {
            for (uint32_t i : g.links) f(i);
        } else {
            for (size_t j = 0; j + 1 < cells.size(); ++j)
                for (uint32_t k = g.cellStart[cells[j]]; k < g.cellStart[cells[j] + 1]; ++k) f(g.links[k]);
        }
    };
    forEachLink([&](uint32_t i) { ++start[linkBucket_[i] + 1u]; });
    for (int b = 0; b < bucketCount_; ++b) start[b + 1] += start[b];
    std::vector<uint32_t> order(start[static_cast<size_t>(bucketCount_)]);
    forEachLink([&](uint32_t i) { order[start[linkBucket_[i]]++] = i; });

    if (lines) {
        // Two vertex indices per link into the static line buffer
        std::vector<uint32_t> indices;
        indices.reserve(order.size() * 2);
        for (uint32_t i : order) {
            indices.push_back(linkFirstVertex_[i]);
            indices.push_back(linkFirstVertex_[i] + 1);
        }
        glBindVertexArray(linkVAO_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineEBO_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(),
                     GL_DYNAMIC_DRAW);
        glBindVertexArray(0);
        lineIndexCount_ = static_cast<GLsizei>(indices.size());
        instanceCount_ = 0;
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, orderVBO_);
        glBufferData(GL_ARRAY_BUFFER, order.size() * sizeof(uint32_t), order.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        instanceCount_ = static_cast<GLsizei>(order.size());
        lineIndexCount_ = 0;
    }
    LOG_INFO(QString("Link colors: draw order of %1 links (%2) in %3 ms")
                 .arg(order.size()).arg(lines ? "lines" : "quads")
                 .arg(timer.nsecsElapsed() / 1e6, 0, 'f', 2));
}

void NetworkRenderer::renderValueLinks(const QMatrix4x4& mvp) {
    if (valuesNeedUpload_) uploadLinkValues();
    if (linkBucket_.size() != roadLinkDrawable_.size()) return;

    // Metres per pixel of the view, at the scale of the live view (an export
    // at 2x has half the metres per pixel and twice the point scale)
    const BoundingBox view = getViewportBounds();
    const double metresPerPx =
        (view.maxX - view.minX) / std::max(1, viewportWidth_) * pointScale_;
    const bool lines = metresPerPx > kLineModeMetresPerPx;
    updateDrawOrder(lines);
    if ((lines ? lineIndexCount_ : instanceCount_) == 0) return;

    const GLuint program = lines ? valueLineProgram_ : valueProgram_;
    glUseProgram(program);
    auto loc = [this, program](const char* name) { return glGetUniformLocation(program, name); };
    glUniformMatrix4fv(loc("uMVP"), 1, GL_FALSE, mvp.constData());
    glUniform1f(loc("uNoDataBelow"), linkClasses_.noDataBelow);
    const int limitCount = static_cast<int>(linkClasses_.limits.size());
    glUniform1i(loc("uLimitCount"), limitCount);
    if (limitCount > 0) glUniform1fv(loc("uLimits"), limitCount, linkClasses_.limits.data());
    std::vector<float> colors;
    for (const auto& c : linkClasses_.colors) colors.insert(colors.end(), c.begin(), c.end());
    glUniform3fv(loc("uColors"), static_cast<GLsizei>(linkClasses_.colors.size()), colors.data());
    const auto& nd = linkpalette::kNoData;
    glUniform3f(loc("uNoDataColor"), nd.r, nd.g, nd.b);
    glUniform1f(loc("uNoDataAlpha"), linkpalette::kNoDataAlpha);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, valueTexture_);
    glUniform1i(loc("uValues"), 1);

    if (lines) {
        glLineWidth(1.0f);
        glBindVertexArray(linkVAO_);
        glDrawElements(GL_LINES, lineIndexCount_, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        glLineWidth(linkWidth_);
    } else {
        glUniform2f(loc("uViewport"), static_cast<float>(viewportWidth_),
                    static_cast<float>(viewportHeight_));
        // Widths per class; a link with no data is 1.2 px wide. The edge is 0.6 px.
        std::vector<float> halfWidths;
        for (float w : linkClasses_.halfWidthsPx) halfWidths.push_back(w * pointScale_);
        glUniform1fv(loc("uHalfWidths"), static_cast<GLsizei>(halfWidths.size()), halfWidths.data());
        glUniform1f(loc("uNoDataHalfPx"), 0.6f * pointScale_);
        glUniform1f(loc("uEdgePx"), 0.6f * pointScale_);
        const auto& edge = linkpalette::kEdge;
        glUniform3f(loc("uEdgeColor"), edge.r, edge.g, edge.b);
        glUniform1f(loc("uEdgeAlpha"), linkClasses_.edge ? linkpalette::kEdgeAlpha : 0.0f);
        glUniform1f(loc("uOffsetOn"), linkClasses_.offsetDirections ? 1.0f : 0.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, geomTexture_);
        glUniform1i(loc("uGeom"), 0);

        glBindVertexArray(valueVAO_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instanceCount_);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
}

void NetworkRenderer::updateBuffers() {
    buildNodeBuffer();
    buildLinkBuffer();
    buffersNeedUpdate_ = false;
}

void NetworkRenderer::render() {
    if (!initialized_ || !networkIndex_) return;

    // Set here, not only in initialize(): the state is shared, and what drew
    // before (QPainter overlays, the tile layer) leaves it changed
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (buffersNeedUpdate_) {
        updateBuffers();
    }

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    // Render links first (below nodes): road links, then the air links
    const bool drawRoads = showLinks_ && airVertexStart_ > 0;
    const bool drawAir = showLinks_ && showAirLinks_ && linkVertexCount_ > airVertexStart_;
    if (drawRoads || drawAir) {
        glUseProgram(linkProgram_);

        GLint mvpLoc = glGetUniformLocation(linkProgram_, "uMVP");
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());
        glUniform1f(glGetUniformLocation(linkProgram_, "uViewportH"),
                    static_cast<float>(viewportHeight_));

        // The road type colors, in the order of RoadType
        static const std::vector<float> roadColors = [] {
            std::vector<float> c;
            for (int t = 0; t < kRoadTypeCount; ++t) {
                float r, g, b;
                getRoadTypeColor(static_cast<RoadType>(t), r, g, b);
                c.insert(c.end(), {r, g, b});
            }
            return c;
        }();
        glUniform3fv(glGetUniformLocation(linkProgram_, "uRoadColors"), kRoadTypeCount,
                     roadColors.data());

        glLineWidth(linkWidth_);
        glBindVertexArray(linkVAO_);
        if (drawRoads && colorByValue_) {
            renderValueLinks(mvp);
            // Back to the line program for the air links
            glUseProgram(linkProgram_);
            glBindVertexArray(linkVAO_);
        } else if (drawRoads) {
            glUniform1f(glGetUniformLocation(linkProgram_, "uLiftPx"), 0.0f);
            glUniform1f(glGetUniformLocation(linkProgram_, "uAlpha"), 0.8f);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(airVertexStart_));
        }
        if (drawAir) {
            // Lifted like the aircraft, so a symbol sits on its corridor.
            // Fainter than roads: there are many, and they cross.
            glUniform1f(glGetUniformLocation(linkProgram_, "uLiftPx"), liftPxPerM_ * pointScale_);
            glUniform1f(glGetUniformLocation(linkProgram_, "uAlpha"), 0.35f);
            glDrawArrays(GL_LINES, static_cast<GLint>(airVertexStart_),
                         static_cast<GLsizei>(linkVertexCount_ - airVertexStart_));
        }
    }

    // Render nodes
    if (showNodes_ && nodeVertexCount_ > 0) {
        glUseProgram(nodeProgram_);

        GLint mvpLoc = glGetUniformLocation(nodeProgram_, "uMVP");
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

        GLint sizeLoc = glGetUniformLocation(nodeProgram_, "uPointSize");
        glUniform1f(sizeLoc, nodeSize_);

        glBindVertexArray(nodeVAO_);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(nodeVertexCount_));
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace simvis
