#include "vehicle_renderer.h"
#include "core/transit_types.h"
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <cmath>
#include <algorithm>

namespace simvis {

static const char* vehicleVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;
layout (location = 2) in float aAngle;
layout (location = 3) in float aSelected;
layout (location = 4) in float aShape;
layout (location = 5) in float aSize;

uniform mat4 uMVP;
uniform float uPointScale; // multiplier for high-res export (1.0 on screen)

out vec3 vColor;
out float vAngle;
out float vSelected;
out float vShape;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_PointSize = aSize * uPointScale;
    vColor = aColor;
    vAngle = aAngle;
    vSelected = aSelected;
    vShape = aShape;
}
)";

static const char* vehicleFragmentShader = R"(
#version 330 core
in vec3 vColor;
in float vAngle;
in float vSelected;
in float vShape;
out vec4 FragColor;

uniform bool uIconMode;         // draw emoji icons from the atlas
uniform sampler2D uIconAtlas;   // 1 row of kIconTileCount tiles
uniform float uIconTileCount;

void main() {
    vec2 coord = vec2(gl_PointCoord.x - 0.5, 0.5 - gl_PointCoord.y);

    // The tracked-vehicle halo is drawn separately by HaloRenderer so the
    // selection ring looks identical everywhere (vehicles, count stations, ...).

    // Icon mode (View > Color > By Mode): sample the emoji atlas,
    // oriented to the travel direction WITHOUT ever turning upside-down.
    // The source emoji face LEFT (west). For west-ish headings we use them
    // as-is and tilt by the residual angle; for east-ish headings we mirror
    // horizontally first. Wheels stay down at every heading - the same trick
    // map apps use, so one icon per mode suffices.
    if (uIconMode) {
        float c = cos(vAngle);
        float s = sin(vAngle);
        bool eastish = c >= 0.0;
        // Tilt from the horizontal axis (+-90 deg). East-ish: nose ends at
        // tilt after mirroring, so tilt = vAngle. West-ish (no mirror): nose
        // ends at pi + tilt, so tilt = vAngle - pi = atan(-s, -c).
        float tilt = eastish ? atan(s, c) : atan(-s, -c);
        float cosI = cos(tilt);
        float sinI = sin(tilt);
        // Inverse-rotate the fragment position into icon space
        vec2 rot = vec2(coord.x * cosI + coord.y * sinI,
                        -coord.x * sinI + coord.y * cosI);
        // Inscribe: shrink so the rotated icon always fits the sprite square
        rot *= 1.41421356;  // sqrt(2)
        // East-ish: mirror the left-facing source so the nose points forward
        if (eastish) rot.x = -rot.x;
        vec2 local = rot + 0.5;               // 0..1 inside the icon tile
        if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0)
            discard;
        float tile = vShape;
        vec2 uv = vec2((tile + local.x) / uIconTileCount, 1.0 - local.y);
        vec4 texel = texture(uIconAtlas, uv);
        if (texel.a < 0.02) discard;
        FragColor = texel;
        return;
    }

    float cosA = cos(vAngle);
    float sinA = sin(vAngle);
    vec2 rotated = vec2(
        coord.x * cosA + coord.y * sinA,
        -coord.x * sinA + coord.y * cosA
    );

    // Glyph alpha; < 0 means the fragment is outside the glyph
    float alpha = -1.0;
    int shape = int(vShape + 0.5);

    if (shape == 0) {
        // Circle
        float dist = length(coord);
        if (dist <= 0.5) alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    } else if (shape == 1) {
        // Triangle/arrow
        vec2 v0 = vec2(-0.4, 0.35);
        vec2 v1 = vec2(-0.4, -0.35);
        vec2 v2 = vec2(0.45, 0.0);

        vec2 v0v1 = v1 - v0;
        vec2 v0v2 = v2 - v0;
        vec2 v0p = rotated - v0;

        float dot00 = dot(v0v1, v0v1);
        float dot01 = dot(v0v1, v0v2);
        float dot02 = dot(v0v1, v0p);
        float dot11 = dot(v0v2, v0v2);
        float dot12 = dot(v0v2, v0p);

        float invDenom = 1.0 / (dot00 * dot11 - dot01 * dot01);
        float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
        float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

        if (u >= 0.0 && v >= 0.0 && (u + v) <= 1.0) {
            float edgeDist = min(min(u, v), 1.0 - u - v);
            alpha = smoothstep(0.0, 0.08, edgeDist);
        }
    } else if (shape == 2) {
        // Rectangle (bus / rail)
        if (abs(rotated.x) <= 0.45 && abs(rotated.y) <= 0.3) {
            float edgeX = 0.45 - abs(rotated.x);
            float edgeY = 0.3 - abs(rotated.y);
            alpha = smoothstep(0.0, 0.06, min(edgeX, edgeY));
        }
    } else if (shape == 3) {
        // Diamond (tram)
        float d = abs(rotated.x) + abs(rotated.y);
        if (d <= 0.5) alpha = smoothstep(0.0, 0.08, 0.5 - d);
    } else {
        // Fallback circle
        float dist = length(coord);
        if (dist <= 0.5) alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    }

    if (alpha < 0.0) discard;
    FragColor = vec4(vColor, alpha);
}
)";

VehicleRenderer::VehicleRenderer() = default;

VehicleRenderer::~VehicleRenderer() {
    cleanup();
}

bool VehicleRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    vehicleProgram_ = compileShader(vehicleVertexShader, vehicleFragmentShader);
    if (!vehicleProgram_) return false;

    glGenVertexArrays(1, &vehicleVAO_);
    glGenBuffers(1, &vehicleVBO_);

    // Set up VAO: pos(2) + color(3) + angle(1) + selected(1) + shape(1) + size(1) = 9 floats
    constexpr int VERTEX_SIZE = 9;

    glBindVertexArray(vehicleVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, vehicleVBO_);

    // Position attribute (location 0): 2 floats
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute (location 1): 3 floats
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Angle attribute (location 2): 1 float
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Selected attribute (location 3): 1 float
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    // Shape attribute (location 4): 1 float
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(4);

    // Size attribute (location 5): 1 float
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(5);

    glBindVertexArray(0);

    createIconAtlas();

    return true;
}

void VehicleRenderer::createIconAtlas() {
    // Render the mode emoji into a 1-row atlas via QPainter, upload once.
    // Tile order matches the shape index used in render(): car, bus, tram, rail.
    static const char* kIcons[kIconTileCount] = {
        "\xF0\x9F\x9A\x97",  // car     U+1F697
        "\xF0\x9F\x9A\x8C",  // bus     U+1F68C
        "\xF0\x9F\x9A\x8B",  // tram    U+1F68B
        "\xF0\x9F\x9A\x86",  // rail    U+1F686
    };

    QImage atlas(kIconTileSize * kIconTileCount, kIconTileSize,
                 QImage::Format_RGBA8888);
    atlas.fill(Qt::transparent);
    {
        QPainter painter(&atlas);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font;
        font.setPointSize(kIconTileSize * 3 / 5);
        painter.setFont(font);
        for (int i = 0; i < kIconTileCount; ++i) {
            // Draw each emoji into its own scratch tile, find the ink
            // bounding box, and blit it exactly centered into the atlas tile.
            // AlignCenter alone centers the em box, not the visible glyph -
            // emoji baseline padding otherwise shifts the icon off the link.
            QImage scratch(kIconTileSize, kIconTileSize, QImage::Format_RGBA8888);
            scratch.fill(Qt::transparent);
            {
                QPainter sp(&scratch);
                sp.setRenderHint(QPainter::Antialiasing);
                sp.setFont(font);
                sp.drawText(QRect(0, 0, kIconTileSize, kIconTileSize),
                            Qt::AlignCenter, QString::fromUtf8(kIcons[i]));
            }
            // Ink bounding box by alpha scan
            int minX = kIconTileSize, minY = kIconTileSize, maxX = -1, maxY = -1;
            for (int y = 0; y < kIconTileSize; ++y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(scratch.constScanLine(y));
                for (int x = 0; x < kIconTileSize; ++x) {
                    if (qAlpha(row[x]) > 8) {
                        minX = std::min(minX, x); maxX = std::max(maxX, x);
                        minY = std::min(minY, y); maxY = std::max(maxY, y);
                    }
                }
            }
            if (maxX < 0) continue;  // nothing rendered (missing font glyph)
            const int inkW = maxX - minX + 1;
            const int inkH = maxY - minY + 1;
            const int dstX = i * kIconTileSize + (kIconTileSize - inkW) / 2;
            const int dstY = (kIconTileSize - inkH) / 2;
            painter.drawImage(QPoint(dstX, dstY),
                              scratch.copy(minX, minY, inkW, inkH));
        }
    }

    glGenTextures(1, &iconAtlas_);
    glBindTexture(GL_TEXTURE_2D, iconAtlas_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas.width(), atlas.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, atlas.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VehicleRenderer::cleanup() {
    if (vehicleVAO_) {
        glDeleteVertexArrays(1, &vehicleVAO_);
        vehicleVAO_ = 0;
    }
    if (vehicleVBO_) {
        glDeleteBuffers(1, &vehicleVBO_);
        vehicleVBO_ = 0;
    }
    if (vehicleProgram_) {
        glDeleteProgram(vehicleProgram_);
        vehicleProgram_ = 0;
    }
    if (iconAtlas_) {
        glDeleteTextures(1, &iconAtlas_);
        iconAtlas_ = 0;
    }

    activeVehicles_.clear();

    GLRenderer::cleanup();
}

void VehicleRenderer::setNetworkIndex(NetworkIndex* index) {
    networkIndex_ = index;
}

void VehicleRenderer::setVehicleIndex(VehicleIndex* index) {
    vehicleIndex_ = index;
    // The states are keyed by vehicle id, and an id means nothing in another
    // scenario. Drop them all, or the old run's vehicles stay on the map until
    // the time changes.
    activeVehicles_.clear();
    activeVehicleIds_.clear();
    activeVehicleCount_ = 0;
    trackedVehicleId_ = 0;
}

// ============================================================================
// Speed-based color helper function
// ============================================================================

void VehicleRenderer::getSpeedColor(float speedRatio, float& r, float& g, float& b) const {
    // speedRatio: 0.0 = stopped, 1.0 = full speed (freeflow)
    speedRatio = std::clamp(speedRatio, 0.0f, 1.0f);

    if (speedRatio >= 0.8f) {
        // Fast (80-100% of freeflow): Green
        r = 0.2f; g = 0.8f; b = 0.2f;
    } else if (speedRatio >= 0.4f) {
        // Medium (40-80% of freeflow): Green -> Yellow
        float t = (speedRatio - 0.4f) / 0.4f;  // 0 to 1
        r = 1.0f - t * 0.8f;   // 1.0 -> 0.2
        g = 0.8f;              // stays high
        b = 0.0f + t * 0.2f;   // 0.0 -> 0.2
    } else {
        // Slow (0-40% of freeflow): Yellow -> Red
        float t = speedRatio / 0.4f;  // 0 to 1
        r = 0.9f + t * 0.1f;   // 0.9 -> 1.0
        g = t * 0.8f;          // 0.0 -> 0.8
        b = 0.0f;              // stays 0
    }
}


// ============================================================================
// Smooth transition helper functions
// ============================================================================

float VehicleRenderer::smoothstep(float t) {
    // Clamp t to [0, 1]
    t = std::clamp(t, 0.0f, 1.0f);
    // Smoothstep: 3t^2 - 2t^3 (ease-in-ease-out)
    return t * t * (3.0f - 2.0f * t);
}

float VehicleRenderer::lerpAngle(float from, float to, float t) {
    // Normalize angles to [-pi, pi]
    constexpr float PI = 3.14159265358979323846f;
    constexpr float TWO_PI = 2.0f * PI;

    // Calculate the shortest angular distance
    float diff = to - from;

    // Wrap difference to [-pi, pi]
    while (diff > PI) diff -= TWO_PI;
    while (diff < -PI) diff += TWO_PI;

    return from + diff * t;
}


// ============================================================================
// Main update: query VehicleIndex for all active vehicles at current time
// ============================================================================

void VehicleRenderer::updateTime(float simulationTime) {
    if (!vehicleIndex_ || !networkIndex_) return;

    currentTime_ = simulationTime;
    TimeMs timeMs = VehicleIndex::toTimeMs(simulationTime);

    // Get all active vehicle IDs at this time
    vehicleIndex_->getActiveVehicles(timeMs, activeVehicleIds_);

    // Mark all existing states as inactive; we'll reactivate the ones still present
    for (auto& [id, state] : activeVehicles_) {
        state.active = false;
    }

    activeVehicleCount_ = 0;

    for (VehicleId vid : activeVehicleIds_) {
        const VehicleSegment* seg = vehicleIndex_->segmentAt(vid, timeMs);
        if (!seg) continue;

        // Calculate progress along link
        float duration = static_cast<float>(seg->leaveTime - seg->enterTime);
        float elapsed  = static_cast<float>(timeMs - seg->enterTime);
        float progress = (duration > 0) ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;

        // Calculate speed ratio
        float speedRatio = 1.0f;
        const LinkRecord* link = networkIndex_->getLink(seg->linkId);
        if (link && link->freespeed > 0 && duration > 0) {
            float freeflowTimeMs = (link->length / link->freespeed) * 1000.0f;
            speedRatio = std::clamp(freeflowTimeMs / duration, 0.0f, 1.0f);
        }

        // Get or create vehicle state
        auto& state = activeVehicles_[vid];

        // Detect link change for smooth transition
        bool linkChanged = (state.active || state.vehicleId == vid) &&
                           state.currentLink != 0 &&
                           state.currentLink != seg->linkId;

        if (linkChanged) {
            state.previousLink = state.currentLink;
            state.transitionStartTime = VehicleIndex::toSeconds(seg->enterTime);
            state.previousLinkAngle = getLinkAngle(state.currentLink);
            state.inTransition = true;
        }

        state.vehicleId = vid;
        state.currentLink = seg->linkId;
        state.linkProgress = progress;
        state.entryTime = VehicleIndex::toSeconds(seg->enterTime);
        state.exitTime = VehicleIndex::toSeconds(seg->leaveTime);
        state.active = true;
        state.speedRatio = speedRatio;

        // Color based on color mode
        if (colorMode_ == VehicleColorMode::TransitMode) {
            TransitMode mode = vehicleIndex_->getVehicleMode(vid);
            if (mode != TransitMode::Unknown) {
                auto c = getTransitModeColor(mode);
                state.r = c.r; state.g = c.g; state.b = c.b;
            } else {
                // Private car: gray
                state.r = 0.62f; state.g = 0.62f; state.b = 0.62f;
            }
        } else {
            getSpeedColor(speedRatio, state.r, state.g, state.b);
        }

        // Position and angle
        float newLinkAngle = getLinkAngle(seg->linkId);
        Point2D newLinkPos = interpolatePosition(seg->linkId, progress);

        if (state.inTransition && state.previousLink != 0) {
            float transitionElapsed = simulationTime - state.transitionStartTime;
            float transitionT = transitionElapsed / LINK_TRANSITION_DURATION;

            if (transitionT >= 1.0f) {
                // Transition complete
                state.inTransition = false;
                state.previousLink = 0;
                state.x = static_cast<float>(newLinkPos.x);
                state.y = static_cast<float>(newLinkPos.y);
                state.angle = newLinkAngle;
            } else {
                // Blend position and angle
                float t = smoothstep(transitionT);
                Point2D oldLinkEndPos = interpolatePosition(state.previousLink, 1.0f);

                state.x = static_cast<float>(oldLinkEndPos.x + t * (newLinkPos.x - oldLinkEndPos.x));
                state.y = static_cast<float>(oldLinkEndPos.y + t * (newLinkPos.y - oldLinkEndPos.y));
                state.angle = lerpAngle(state.previousLinkAngle, newLinkAngle, t);
            }
        } else {
            state.x = static_cast<float>(newLinkPos.x);
            state.y = static_cast<float>(newLinkPos.y);
            state.angle = newLinkAngle;
        }

        ++activeVehicleCount_;
    }

    // Remove vehicles that are no longer active
    for (auto it = activeVehicles_.begin(); it != activeVehicles_.end(); ) {
        if (!it->second.active) {
            it = activeVehicles_.erase(it);
        } else {
            ++it;
        }
    }
}

Point2D VehicleRenderer::interpolatePosition(uint32_t linkId, float progress) const {
    if (!networkIndex_) return Point2D(0, 0);

    const LinkRecord* link = networkIndex_->getLink(linkId);
    if (!link) return Point2D(0, 0);

    const NodeRecord* fromNode = networkIndex_->getNode(link->fromNode);
    const NodeRecord* toNode = networkIndex_->getNode(link->toNode);

    if (!fromNode || !toNode) return Point2D(0, 0);

    // Linear interpolation
    double x = fromNode->x + progress * (toNode->x - fromNode->x);
    double y = fromNode->y + progress * (toNode->y - fromNode->y);

    return Point2D(x, y);
}

float VehicleRenderer::getLinkAngle(uint32_t linkId) const {
    if (!networkIndex_) return 0.0f;

    const LinkRecord* link = networkIndex_->getLink(linkId);
    if (!link) return 0.0f;

    const NodeRecord* fromNode = networkIndex_->getNode(link->fromNode);
    const NodeRecord* toNode = networkIndex_->getNode(link->toNode);

    if (!fromNode || !toNode) return 0.0f;

    // Calculate angle from fromNode to toNode
    double dx = toNode->x - fromNode->x;
    double dy = toNode->y - fromNode->y;

    return static_cast<float>(std::atan2(dy, dx));
}

void VehicleRenderer::render() {
    if (!initialized_ || activeVehicles_.empty()) return;

    // Set here, not only in initialize(): the state is shared, and what drew
    // before (QPainter overlays, the tile layer) leaves it changed
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Vertex format: [X, Y, R, G, B, Angle, Selected, Shape, Size] = 9 floats per vertex
    constexpr int VERTEX_SIZE = 9;

    std::vector<float> vertices;
    vertices.reserve(activeVehicles_.size() * VERTEX_SIZE);

    // "By Mode" shows emoji icons instead of colored glyphs
    const bool iconMode = colorMode_ == VehicleColorMode::TransitMode;

    for (const auto& [vehicleId, state] : activeVehicles_) {
        if (!state.active) continue;
        if (!isModeVisible(vehicleId)) continue;

        // Determine per-vertex shape and size
        float shape, size;
        if (iconMode) {
            // shape carries the atlas tile index: 0=car, 1=bus, 2=tram, 3=rail.
            // Icons need a larger sprite than glyphs to stay legible; the
            // Vehicle size choice (vehicleSize_) still scales them.
            TransitMode mode = vehicleIndex_ ? vehicleIndex_->getVehicleMode(vehicleId)
                                             : TransitMode::Unknown;
            switch (mode) {
                case TransitMode::Bus:  shape = 1.0f; break;
                case TransitMode::Tram: shape = 2.0f; break;
                case TransitMode::Rail: shape = 3.0f; break;
                default:                shape = 0.0f; break;  // car
            }
            // 2.5x for legibility, plus sqrt(2) headroom because the rotated
            // icon is inscribed in the sprite square (see fragment shader)
            size = vehicleSize_ * 3.5f;
        } else if (vehicleShape_ == -1) {
            // Auto by mode
            TransitMode mode = vehicleIndex_ ? vehicleIndex_->getVehicleMode(vehicleId)
                                             : TransitMode::Unknown;
            switch (mode) {
                case TransitMode::Bus:
                    shape = 2.0f; size = vehicleSize_;        break;  // rectangle
                case TransitMode::Tram:
                    shape = 3.0f; size = vehicleSize_ * 1.25f; break; // diamond
                case TransitMode::Rail:
                    shape = 2.0f; size = vehicleSize_ * 1.75f; break; // large rectangle
                default:
                    shape = 1.0f; size = vehicleSize_;        break;  // triangle (car)
            }
        } else {
            shape = static_cast<float>(vehicleShape_);
            size = vehicleSize_;
        }

        vertices.push_back(state.x);
        vertices.push_back(state.y);
        vertices.push_back(state.r);
        vertices.push_back(state.g);
        vertices.push_back(state.b);
        vertices.push_back(state.angle);
        vertices.push_back(vehicleId == trackedVehicleId_ ? 1.0f : 0.0f);
        vertices.push_back(shape);
        vertices.push_back(size);
    }

    if (vertices.empty()) return;

    // Upload to GPU
    glBindBuffer(GL_ARRAY_BUFFER, vehicleVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_DYNAMIC_DRAW);

    // Render
    glUseProgram(vehicleProgram_);

    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;
    GLint mvpLoc = glGetUniformLocation(vehicleProgram_, "uMVP");
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.constData());

    GLint scaleLoc = glGetUniformLocation(vehicleProgram_, "uPointScale");
    glUniform1f(scaleLoc, pointScale_);

    glUniform1i(glGetUniformLocation(vehicleProgram_, "uIconMode"), iconMode ? 1 : 0);
    glUniform1f(glGetUniformLocation(vehicleProgram_, "uIconTileCount"),
                static_cast<float>(kIconTileCount));
    if (iconMode && iconAtlas_) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, iconAtlas_);
        glUniform1i(glGetUniformLocation(vehicleProgram_, "uIconAtlas"), 0);
    }

    glBindVertexArray(vehicleVAO_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vertices.size() / VERTEX_SIZE));

    glBindVertexArray(0);
    if (iconMode) glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

bool VehicleRenderer::isModeVisible(uint32_t vehicleId) const {
    TransitMode mode = vehicleIndex_ ? vehicleIndex_->getVehicleMode(vehicleId)
                                     : TransitMode::Unknown;
    switch (mode) {
        case TransitMode::Bus:  return showBuses_;
        case TransitMode::Tram: return showTrams_;
        case TransitMode::Rail: return showRail_;
        default:                return showCars_;
    }
}

const VehicleState* VehicleRenderer::getVehicleState(uint32_t vehicleId) const {
    auto it = activeVehicles_.find(vehicleId);
    if (it != activeVehicles_.end() && it->second.active) {
        return &it->second;
    }
    return nullptr;
}

uint32_t VehicleRenderer::findVehicleAt(double worldX, double worldY, double hitRadius) const {
    uint32_t closestVehicle = 0;
    double closestDistSq = hitRadius * hitRadius;

    for (const auto& [vehicleId, state] : activeVehicles_) {
        if (!state.active) continue;
        if (!isModeVisible(vehicleId)) continue;  // hidden modes not clickable

        double dx = state.x - worldX;
        double dy = state.y - worldY;
        double distSq = dx * dx + dy * dy;

        if (distSq < closestDistSq) {
            closestDistSq = distSq;
            closestVehicle = vehicleId;
        }
    }

    return closestVehicle;
}

} // namespace simvis
