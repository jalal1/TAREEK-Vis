#include "vehicle_renderer.h"
#include "core/transit_types.h"
#include "core/modes.h"
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
layout (location = 6) in float aLift;   // screen pixels up (3D modes; 0 on the ground)

uniform mat4 uMVP;
uniform float uPointScale; // multiplier for high-res export (1.0 on screen)
uniform float uViewportH;  // viewport height in pixels

out vec3 vColor;
out float vAngle;
out float vSelected;
out float vShape;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_Position.y += aLift * uPointScale * 2.0 / uViewportH * gl_Position.w;
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
    vec3 color = vColor;
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
    } else if (shape == 4) {
        // Aircraft seen from above, nose at +x: fuselage, swept wings, tail.
        // Each part is the smallest distance to its edges (> 0 inside); the
        // glyph is their union.
        vec2 p = rotated;
        float ay = abs(p.y);
        float body = min(min(0.065 - ay, p.x + 0.42), 0.40 - p.x);
        float nose = 0.065 - length(p - vec2(0.40, 0.0));
        float wing = min(min(0.46 - ay, 0.14 - 0.45 * ay - p.x), p.x + 0.10 + 0.45 * ay);
        float tail = min(min(0.18 - ay, -0.30 - 0.30 * ay - p.x), p.x + 0.42);
        float d = max(max(body, nose), max(wing, tail));
        if (d >= 0.0) alpha = smoothstep(0.0, 0.025, d);
        // A dark edge keeps the symbol readable on a trail or a light map
        color = mix(vec3(0.08), vColor, smoothstep(0.012, 0.035, d));
    } else {
        // Fallback circle
        float dist = length(coord);
        if (dist <= 0.5) alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    }

    if (alpha < 0.0) discard;
    FragColor = vec4(color, alpha);
}
)";

// Trails, stems and ground marks of the air view. A point is a round dot.
static const char* airVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in float aLift;
layout (location = 2) in vec4 aColor;

uniform mat4 uMVP;
uniform float uPointScale;
uniform float uViewportH;
uniform float uPointSize;

out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_Position.y += aLift * uPointScale * 2.0 / uViewportH * gl_Position.w;
    gl_PointSize = uPointSize * uPointScale;
    vColor = aColor;
}
)";

static const char* airFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

uniform bool uRound;

void main() {
    if (uRound) {
        float d = length(gl_PointCoord - vec2(0.5));
        if (d > 0.5) discard;
        FragColor = vec4(vColor.rgb, vColor.a * (1.0 - smoothstep(0.35, 0.5, d)));
        return;
    }
    FragColor = vColor;
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

    // Set up VAO: pos(2) + color(3) + angle(1) + selected(1) + shape(1) + size(1)
    // + lift(1) = 10 floats
    constexpr int VERTEX_SIZE = 10;

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

    // Lift attribute (location 6): 1 float
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE * sizeof(float),
                          (void*)(9 * sizeof(float)));
    glEnableVertexAttribArray(6);

    glBindVertexArray(0);

    // Air view: x, y, lift, r, g, b, a = 7 floats
    airProgram_ = compileShader(airVertexShader, airFragmentShader);
    if (!airProgram_) return false;
    glGenVertexArrays(1, &airVAO_);
    glGenBuffers(1, &airVBO_);
    glBindVertexArray(airVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, airVBO_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    createIconAtlas();

    return true;
}

void VehicleRenderer::createIconAtlas() {
    // Render the mode emoji into a 1-row atlas via QPainter, upload once.
    // Tile i is the icon of mode id i, which render() uses as the shape index.
    const auto& allModes = modes::all();
    iconTileCount_ = static_cast<int>(allModes.size());

    QImage atlas(kIconTileSize * iconTileCount_, kIconTileSize,
                 QImage::Format_RGBA8888);
    atlas.fill(Qt::transparent);
    {
        QPainter painter(&atlas);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font;
        font.setPointSize(kIconTileSize * 3 / 5);
        painter.setFont(font);
        for (int i = 0; i < iconTileCount_; ++i) {
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
                            Qt::AlignCenter, QString::fromUtf8(allModes[i].icon));
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
    if (airVAO_) {
        glDeleteVertexArrays(1, &airVAO_);
        airVAO_ = 0;
    }
    if (airVBO_) {
        glDeleteBuffers(1, &airVBO_);
        airVBO_ = 0;
    }
    if (airProgram_) {
        glDeleteProgram(airProgram_);
        airProgram_ = 0;
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
    trackedVehicleId_ = kNoVehicle;
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
    activeAirCount_ = 0;

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
        const bool is3D = modes::get(vehicleIndex_->modeId(vid)).is3D;

        // Detect link change for smooth transition
        bool linkChanged = (state.active || state.vehicleId == vid) &&
                           state.currentLink != kNoLinkId &&
                           state.currentLink != seg->linkId;

        if (linkChanged) {
            state.previousLink = state.currentLink;
            state.transitionStartTime = VehicleIndex::toSeconds(seg->enterTime);
            // A 3D vehicle turns from the heading it shows: on a vertical
            // link the link's own angle means nothing
            state.previousLinkAngle = is3D ? state.angle : getLinkAngle(state.currentLink);
            state.inTransition = true;
        }

        state.vehicleId = vid;
        state.currentLink = seg->linkId;
        state.linkProgress = progress;
        state.entryTime = VehicleIndex::toSeconds(seg->enterTime);
        state.exitTime = VehicleIndex::toSeconds(seg->leaveTime);
        state.active = true;
        state.speedRatio = speedRatio;
        state.is3D = is3D;

        if (is3D) {
            // A segment is a straight move at constant speed between two
            // nodes, so linear interpolation in x, y and z is exact. The links
            // of a flight connect, so only the heading is blended.
            float x = 0.0f, y = 0.0f, z = 0.0f;
            positionAt(vid, timeMs, x, y, z);
            state.x = x;
            state.y = y;
            state.z = z;
            state.verticalRate = 0.0f;
            if (link && duration > 0) {
                const NodeRecord* from = networkIndex_->getNode(link->fromNode);
                const NodeRecord* to = networkIndex_->getNode(link->toNode);
                if (from && to) state.verticalRate = (to->z - from->z) / (duration / 1000.0f);
            }
            const float heading = headingOnSegment(vid, seg);
            if (state.inTransition) {
                const float t = (simulationTime - state.transitionStartTime) / LINK_TRANSITION_DURATION;
                if (t >= 1.0f) {
                    state.inTransition = false;
                    state.previousLink = kNoLinkId;
                    state.angle = heading;
                } else {
                    state.angle = lerpAngle(state.previousLinkAngle, heading, smoothstep(t));
                }
            } else {
                state.angle = heading;
            }
            // Speed is always the planned speed on an air link, so colour
            // by height in place of speed
            if (colorMode_ == VehicleColorMode::TransitMode) {
                const TrafficMode& mode = modes::get(vehicleIndex_->modeId(vid));
                state.r = mode.r; state.g = mode.g; state.b = mode.b;
            } else {
                altitudeColor(std::max(state.z, 0.0f) / altitudeTop_, state.r, state.g, state.b);
            }
            ++activeVehicleCount_;
            ++activeAirCount_;
            continue;
        }

        // Color based on color mode
        if (colorMode_ == VehicleColorMode::TransitMode) {
            const TrafficMode& mode = modes::get(vehicleIndex_->modeId(vid));
            state.r = mode.r; state.g = mode.g; state.b = mode.b;
        } else {
            getSpeedColor(speedRatio, state.r, state.g, state.b);
        }

        // Position and angle
        float newLinkAngle = getLinkAngle(seg->linkId);
        Point2D newLinkPos = interpolatePosition(seg->linkId, progress);

        if (state.inTransition && state.previousLink != kNoLinkId) {
            float transitionElapsed = simulationTime - state.transitionStartTime;
            float transitionT = transitionElapsed / LINK_TRANSITION_DURATION;

            if (transitionT >= 1.0f) {
                // Transition complete
                state.inTransition = false;
                state.previousLink = kNoLinkId;
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

    // Trails, stems and ground marks go under the symbols
    renderAirCues();

    // Vertex format: [X, Y, R, G, B, Angle, Selected, Shape, Size, Lift] = 10 floats
    constexpr int VERTEX_SIZE = 10;

    std::vector<float> vertices;
    vertices.reserve(activeVehicles_.size() * VERTEX_SIZE);

    // "By Mode" shows emoji icons instead of colored glyphs
    const bool iconMode = colorMode_ == VehicleColorMode::TransitMode;

    for (const auto& [vehicleId, state] : activeVehicles_) {
        if (!state.active) continue;
        if (!isModeVisible(vehicleId)) continue;
        if (!isInBand(state)) continue;

        // Determine per-vertex shape and size from the mode registry
        const TrafficMode& mode =
            modes::get(vehicleIndex_ ? vehicleIndex_->modeId(vehicleId) : modes::kCar);
        float shape, size;
        if (iconMode) {
            // shape carries the atlas tile index, which is the mode id.
            // Icons need a larger sprite than glyphs to stay legible; the
            // Vehicle size choice (vehicleSize_) still scales them.
            shape = static_cast<float>(mode.id);
            // 2.5x for legibility, plus sqrt(2) headroom because the rotated
            // icon is inscribed in the sprite square (see fragment shader)
            size = vehicleSize_ * 3.5f;
        } else if (vehicleShape_ == -1) {
            // Auto by mode
            shape = static_cast<float>(mode.glyph);
            size = vehicleSize_ * mode.glyphScale;
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
        vertices.push_back(state.is3D ? std::max(state.z, 0.0f) * liftPxPerM_ : 0.0f);
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
    glUniform1f(glGetUniformLocation(vehicleProgram_, "uViewportH"),
                static_cast<float>(viewportHeight_));

    glUniform1i(glGetUniformLocation(vehicleProgram_, "uIconMode"), iconMode ? 1 : 0);
    glUniform1f(glGetUniformLocation(vehicleProgram_, "uIconTileCount"),
                static_cast<float>(iconTileCount_));
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
    const uint8_t id = vehicleIndex_ ? vehicleIndex_->modeId(vehicleId) : modes::kCar;
    return id >= 64 || !(hiddenModes_ & (uint64_t(1) << id));
}

const VehicleState* VehicleRenderer::getVehicleState(uint32_t vehicleId) const {
    auto it = activeVehicles_.find(vehicleId);
    if (it != activeVehicles_.end() && it->second.active) {
        return &it->second;
    }
    return nullptr;
}

uint32_t VehicleRenderer::findVehicleAt(double worldX, double worldY, double hitRadius) const {
    uint32_t closestVehicle = kNoVehicle;
    double closestDistSq = hitRadius * hitRadius;

    for (const auto& [vehicleId, state] : activeVehicles_) {
        if (!state.active) continue;
        if (!isModeVisible(vehicleId)) continue;  // hidden modes not clickable
        if (!isInBand(state)) continue;

        // A lifted symbol is clicked where it is drawn
        const Point2D shown = displayPosition(state);
        double dx = shown.x - worldX;
        double dy = shown.y - worldY;
        double distSq = dx * dx + dy * dy;

        if (distSq < closestDistSq) {
            closestDistSq = distSq;
            closestVehicle = vehicleId;
        }
    }

    return closestVehicle;
}

// ============================================================================
// Air view
// ============================================================================

float VehicleRenderer::worldPerPixel() const {
    const float sy = projectionMatrix_(1, 1);
    return (sy > 0.0f && viewportHeight_ > 0) ? 2.0f / (sy * viewportHeight_) : 0.0f;
}

Point2D VehicleRenderer::displayPosition(const VehicleState& state) const {
    if (!state.is3D) return Point2D(state.x, state.y);
    return Point2D(state.x, state.y + std::max(state.z, 0.0f) * liftPxPerM_ * worldPerPixel());
}

void VehicleRenderer::altitudeColor(float t, float& r, float& g, float& b) {
    // Violet, pink, orange, yellow: the lightness grows with the height, so
    // the order reads without the hue, and every stop shows on a dark map.
    static const float kStops[4][3] = {
        {0.56f, 0.36f, 0.98f},
        {0.93f, 0.33f, 0.62f},
        {1.00f, 0.60f, 0.20f},
        {0.98f, 0.95f, 0.35f},
    };
    t = std::clamp(t, 0.0f, 1.0f) * 3.0f;
    const int i = std::min(static_cast<int>(t), 2);
    const float f = t - static_cast<float>(i);
    r = kStops[i][0] + f * (kStops[i + 1][0] - kStops[i][0]);
    g = kStops[i][1] + f * (kStops[i + 1][1] - kStops[i][1]);
    b = kStops[i][2] + f * (kStops[i + 1][2] - kStops[i][2]);
}

bool VehicleRenderer::positionAt(VehicleId vid, TimeMs time, float& x, float& y, float& z) const {
    const VehicleSegment* seg = vehicleIndex_->segmentAt(vid, time);
    if (!seg) return false;
    const LinkRecord* link = networkIndex_->getLink(seg->linkId);
    if (!link) return false;
    const NodeRecord* from = networkIndex_->getNode(link->fromNode);
    const NodeRecord* to = networkIndex_->getNode(link->toNode);
    if (!from || !to) return false;
    const float duration = static_cast<float>(seg->leaveTime - seg->enterTime);
    const float p = duration > 0
        ? std::clamp(static_cast<float>(time - seg->enterTime) / duration, 0.0f, 1.0f) : 1.0f;
    x = from->x + p * (to->x - from->x);
    y = from->y + p * (to->y - from->y);
    z = from->z + p * (to->z - from->z);
    return true;
}

float VehicleRenderer::headingOnSegment(VehicleId vid, const VehicleSegment* seg) const {
    // Angle of a link with at least 0.5 m of horizontal length
    auto horizontalAngle = [this](uint32_t linkId, float& angle) {
        const LinkRecord* link = networkIndex_->getLink(linkId);
        if (!link) return false;
        const NodeRecord* from = networkIndex_->getNode(link->fromNode);
        const NodeRecord* to = networkIndex_->getNode(link->toNode);
        if (!from || !to) return false;
        const float dx = to->x - from->x;
        const float dy = to->y - from->y;
        if (dx * dx + dy * dy < 0.25f) return false;
        angle = std::atan2(dy, dx);
        return true;
    };
    float angle = 0.0f;
    if (horizontalAngle(seg->linkId, angle)) return angle;

    // Vertical or hover: at the first segment (take-off) the heading of the
    // departure; else the heading the vehicle had (landing, hold)
    const VehicleTrajectory* traj = vehicleIndex_->trajectory(vid);
    if (!traj || traj->segments.empty()) return 0.0f;
    const auto& segs = traj->segments;
    const size_t i = static_cast<size_t>(seg - segs.data());
    if (i >= segs.size()) return 0.0f;
    if (i > 0) {
        for (size_t k = i; k-- > 0;)
            if (horizontalAngle(segs[k].linkId, angle)) return angle;
    }
    for (size_t k = i + 1; k < segs.size(); ++k)
        if (horizontalAngle(segs[k].linkId, angle)) return angle;
    return 0.0f;
}

void VehicleRenderer::renderAirCues() {
    airLines_.clear();
    airPoints_.clear();
    const TimeMs now = VehicleIndex::toTimeMs(currentTime_);

    auto push = [](std::vector<float>& out, float x, float y, float lift,
                   float r, float g, float b, float a) {
        out.insert(out.end(), {x, y, lift, r, g, b, a});
    };

    for (const auto& [vehicleId, state] : activeVehicles_) {
        if (!state.active || !state.is3D) continue;
        if (!isModeVisible(vehicleId) || !isInBand(state)) continue;
        const float lift = std::max(state.z, 0.0f) * liftPxPerM_;

        // Ground mark and stem: where the vehicle is over the ground, and how
        // high. The stem is left out when it is shorter than a pixel.
        push(airPoints_, state.x, state.y, 0.0f, state.r * 0.6f, state.g * 0.6f, state.b * 0.6f, 0.9f);
        if (lift >= 1.0f) {
            push(airLines_, state.x, state.y, 0.0f, state.r, state.g, state.b, 0.45f);
            push(airLines_, state.x, state.y, lift, state.r, state.g, state.b, 0.45f);
        }

        // Trail: the recent path, sampled back in time and fading out
        if (!showTrails_) continue;
        float px = state.x, py = state.y, pz = std::max(state.z, 0.0f);
        for (int k = 1; k <= kTrailSamples; ++k) {
            const float back = k * kTrailStepS * 1000.0f;
            if (back > static_cast<float>(now)) break;
            float x, y, z;
            if (!positionAt(vehicleId, now - static_cast<TimeMs>(back), x, y, z)) break;
            z = std::max(z, 0.0f);
            float r = state.r, g = state.g, b = state.b;
            if (colorMode_ != VehicleColorMode::TransitMode) altitudeColor(z / altitudeTop_, r, g, b);
            const float a0 = 0.7f * (1.0f - static_cast<float>(k - 1) / kTrailSamples);
            const float a1 = 0.7f * (1.0f - static_cast<float>(k) / kTrailSamples);
            push(airLines_, px, py, pz * liftPxPerM_, r, g, b, a0);
            push(airLines_, x, y, z * liftPxPerM_, r, g, b, a1);
            px = x; py = y; pz = z;
        }
    }
    if (airLines_.empty() && airPoints_.empty()) return;

    glUseProgram(airProgram_);
    const QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;
    glUniformMatrix4fv(glGetUniformLocation(airProgram_, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniform1f(glGetUniformLocation(airProgram_, "uPointScale"), pointScale_);
    glUniform1f(glGetUniformLocation(airProgram_, "uViewportH"), static_cast<float>(viewportHeight_));
    glBindVertexArray(airVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, airVBO_);

    if (!airLines_.empty()) {
        glUniform1i(glGetUniformLocation(airProgram_, "uRound"), 0);
        glBufferData(GL_ARRAY_BUFFER, airLines_.size() * sizeof(float), airLines_.data(), GL_DYNAMIC_DRAW);
        glLineWidth(1.5f * pointScale_);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(airLines_.size() / 7));
    }
    if (!airPoints_.empty()) {
        glUniform1i(glGetUniformLocation(airProgram_, "uRound"), 1);
        glUniform1f(glGetUniformLocation(airProgram_, "uPointSize"), 5.0f);
        glBufferData(GL_ARRAY_BUFFER, airPoints_.size() * sizeof(float), airPoints_.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(airPoints_.size() / 7));
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace simvis
