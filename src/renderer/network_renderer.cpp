#include "network_renderer.h"
#include <QDebug>

namespace simvis {

// Vertex: x, y, r, g, b
constexpr int VERTEX_SIZE = 5;

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
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;

uniform mat4 uMVP;

out vec3 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* linkFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 0.8);
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

    // Create VAOs and VBOs
    glGenVertexArrays(1, &nodeVAO_);
    glGenBuffers(1, &nodeVBO_);

    glGenVertexArrays(1, &linkVAO_);
    glGenBuffers(1, &linkVBO_);

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

    GLRenderer::cleanup();
}

void NetworkRenderer::setNetworkIndex(NetworkIndex* index) {
    networkIndex_ = index;
    buffersNeedUpdate_ = true;
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
                          (void*)(2 * sizeof(float)));
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
    vertices.reserve(links.size() * 2 * VERTEX_SIZE);  // 2 vertices per link

    for (const auto& link : links) {
        auto fromIt = nodeIdToIndex.find(link.fromNode);
        auto toIt = nodeIdToIndex.find(link.toNode);

        if (fromIt == nodeIdToIndex.end() || toIt == nodeIdToIndex.end()) {
            continue;
        }

        const auto& fromNode = nodes[fromIt->second];
        const auto& toNode = nodes[toIt->second];

        float r, g, b;
        getRoadTypeColor(static_cast<RoadType>(link.roadType), r, g, b);

        // From vertex
        vertices.push_back(fromNode.x);
        vertices.push_back(fromNode.y);
        vertices.push_back(r);
        vertices.push_back(g);
        vertices.push_back(b);

        // To vertex
        vertices.push_back(toNode.x);
        vertices.push_back(toNode.y);
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

    // Render links first (below nodes)
    if (showLinks_ && linkVertexCount_ > 0) {
        glUseProgram(linkProgram_);

        GLint mvpLoc = glGetUniformLocation(linkProgram_, "uMVP");
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

        glLineWidth(linkWidth_);
        glBindVertexArray(linkVAO_);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(linkVertexCount_));
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
