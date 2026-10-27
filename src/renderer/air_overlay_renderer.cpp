#include "air_overlay_renderer.h"
#include "vehicle_renderer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace simvis {

static const char* vertiportVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;

uniform mat4 uMVP;
uniform float uPointSize;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_PointSize = uPointSize;
}
)";

// A helipad mark: a dark disc with a light ring and an "H"
static const char* vertiportFragmentShader = R"(
#version 330 core
out vec4 FragColor;

void main() {
    vec2 c = gl_PointCoord - vec2(0.5);
    float r = length(c);
    if (r > 0.5) discard;
    float edge = 1.0 - smoothstep(0.46, 0.5, r);
    float ring = 1.0 - smoothstep(0.03, 0.06, abs(r - 0.40));
    float h = 0.0;
    if (abs(c.y) < 0.22 && abs(abs(c.x) - 0.14) < 0.04) h = 1.0;   // two posts
    if (abs(c.y) < 0.035 && abs(c.x) < 0.15) h = 1.0;              // bar
    vec3 light = vec3(0.92, 0.93, 0.97);
    vec3 dark = vec3(0.12, 0.13, 0.17);
    float mark = max(ring, h);
    FragColor = vec4(mix(dark, light, mark), edge * mix(0.75, 1.0, mark));
}
)";

// Event marks: a line (uMode 0) or a ring (2)
static const char* eventVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in float aSize;

uniform mat4 uMVP;
uniform float uPointScale;

out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    gl_PointSize = aSize * uPointScale;
    vColor = aColor;
}
)";

static const char* eventFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

uniform int uMode;

void main() {
    if (uMode == 0) { FragColor = vColor; return; }
    float r = length(gl_PointCoord - vec2(0.5));
    if (r > 0.5) discard;
    float a = 1.0 - smoothstep(0.03, 0.07, abs(r - 0.42));
    if (a <= 0.0) discard;
    FragColor = vec4(vColor.rgb, vColor.a * a);
}
)";

AirOverlayRenderer::~AirOverlayRenderer() {
    cleanup();
}

bool AirOverlayRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;
    program_ = compileShader(vertiportVertexShader, vertiportFragmentShader);
    if (!program_) return false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    eventProgram_ = compileShader(eventVertexShader, eventFragmentShader);
    if (!eventProgram_) return false;
    glGenVertexArrays(1, &eventVao_);
    glGenBuffers(1, &eventVbo_);
    glBindVertexArray(eventVao_);
    glBindBuffer(GL_ARRAY_BUFFER, eventVbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return true;
}

void AirOverlayRenderer::cleanup() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    if (eventVao_) { glDeleteVertexArrays(1, &eventVao_); eventVao_ = 0; }
    if (eventVbo_) { glDeleteBuffers(1, &eventVbo_); eventVbo_ = 0; }
    if (eventProgram_) { glDeleteProgram(eventProgram_); eventProgram_ = 0; }
    GLRenderer::cleanup();
}

void AirOverlayRenderer::setNetworkIndex(const NetworkIndex* index) {
    vertiports_.clear();
    bufferDirty_ = true;
    if (!index) return;

    // Ground end (lower node) of each vertical link, one per grid cell of
    // kClusterM. The first end found in a cell stands for the vertiport.
    std::unordered_map<uint64_t, size_t> cells;
    for (const LinkRecord& link : index->allLinks()) {
        if (static_cast<RoadType>(link.roadType) != RoadType::AirVertical) continue;
        const NodeRecord* a = index->getNode(link.fromNode);
        const NodeRecord* b = index->getNode(link.toNode);
        if (!a || !b) continue;
        const NodeRecord* ground = a->z <= b->z ? a : b;
        const int64_t cx = static_cast<int64_t>(std::floor(ground->x / kClusterM));
        const int64_t cy = static_cast<int64_t>(std::floor(ground->y / kClusterM));
        const uint64_t key = (static_cast<uint64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
        if (cells.emplace(key, vertiports_.size()).second)
            vertiports_.emplace_back(ground->x, ground->y);
    }
}

// An event without its end event lasts this long
static constexpr TimeMs kOpenEventMs = 1000;

static TimeMs eventEnd(const OverlayEvent& ev) {
    return ev.endMs == TRIP_NO_TIME ? ev.startMs + kOpenEventMs : ev.endMs;
}

void AirOverlayRenderer::setVehicleIndex(const VehicleIndex* index) {
    index_ = index;
    active_.clear();
    activeLosses_ = activeHolds_ = 0;
    maxDurationMs_ = 0;
    if (!index) return;
    for (const OverlayEvent& ev : index->overlayEvents())
        maxDurationMs_ = std::max(maxDurationMs_, eventEnd(ev) - ev.startMs);
}

void AirOverlayRenderer::updateTime(float simulationTime) {
    active_.clear();
    activeLosses_ = activeHolds_ = 0;
    if (!index_ || index_->overlayEvents().empty()) return;

    // The events are sorted by start. An event active now started at most
    // maxDurationMs_ ago, so only that window is scanned.
    const auto& events = index_->overlayEvents();
    const TimeMs now = VehicleIndex::toTimeMs(simulationTime);
    const TimeMs from = now > maxDurationMs_ ? now - maxDurationMs_ : 0;
    auto it = std::lower_bound(events.begin(), events.end(), from,
        [](const OverlayEvent& ev, TimeMs t) { return ev.startMs < t; });
    for (; it != events.end() && it->startMs <= now; ++it) {
        if (now >= eventEnd(*it)) continue;
        active_.push_back(static_cast<uint32_t>(it - events.begin()));
        if (it->kind == static_cast<uint8_t>(OverlayKind::Loss)) ++activeLosses_;
        else ++activeHolds_;
    }
}

static void pushMark(std::vector<float>& out, double x, double y,
                     float r, float g, float b, float a, float size) {
    out.insert(out.end(), {static_cast<float>(x), static_cast<float>(y), r, g, b, a, size});
}

void AirOverlayRenderer::renderLosses() {
    lines_.clear();
    rings_.clear();
    if (!initialized_ || !index_ || !vehicles_ || !showLosses_ || activeLosses_ == 0) return;

    // A red line between the two aircraft, where their symbols are, and a
    // red ring around each
    const auto& events = index_->overlayEvents();
    for (uint32_t i : active_) {
        const OverlayEvent& ev = events[i];
        if (ev.kind != static_cast<uint8_t>(OverlayKind::Loss)) continue;
        if (!vehicles_->isShown(ev.vehicleA) || !vehicles_->isShown(ev.vehicleB)) continue;
        const Point2D a = vehicles_->displayPosition(*vehicles_->getVehicleState(ev.vehicleA));
        const Point2D b = vehicles_->displayPosition(*vehicles_->getVehicleState(ev.vehicleB));
        pushMark(lines_, a.x, a.y, 1.0f, 0.18f, 0.18f, 0.95f, 0.0f);
        pushMark(lines_, b.x, b.y, 1.0f, 0.18f, 0.18f, 0.95f, 0.0f);
        pushMark(rings_, a.x, a.y, 1.0f, 0.18f, 0.18f, 0.95f, 24.0f);
        pushMark(rings_, b.x, b.y, 1.0f, 0.18f, 0.18f, 0.95f, 24.0f);
    }
    drawEventMarks();
}

void AirOverlayRenderer::drawEventMarks() {
    if (lines_.empty() && rings_.empty()) return;

    glUseProgram(eventProgram_);
    const QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;
    glUniformMatrix4fv(glGetUniformLocation(eventProgram_, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniform1f(glGetUniformLocation(eventProgram_, "uPointScale"), pointScale_);
    glBindVertexArray(eventVao_);
    glBindBuffer(GL_ARRAY_BUFFER, eventVbo_);
    const GLint modeLoc = glGetUniformLocation(eventProgram_, "uMode");
    auto draw = [&](const std::vector<float>& data, int mode, GLenum primitive) {
        if (data.empty()) return;
        glUniform1i(modeLoc, mode);
        glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(primitive, 0, static_cast<GLsizei>(data.size() / 7));
    };
    draw(rings_, 2, GL_POINTS);
    glLineWidth(2.0f * pointScale_);
    draw(lines_, 0, GL_LINES);
    glBindVertexArray(0);
    glUseProgram(0);
}

void AirOverlayRenderer::render() {
    if (!initialized_) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (showVertiports_ && !vertiports_.empty()) {
        if (bufferDirty_) {
            std::vector<float> data;
            data.reserve(vertiports_.size() * 2);
            for (const Point2D& p : vertiports_) {
                data.push_back(static_cast<float>(p.x));
                data.push_back(static_cast<float>(p.y));
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
            bufferDirty_ = false;
        }

        glUseProgram(program_);
        const QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;
        glUniformMatrix4fv(glGetUniformLocation(program_, "uMVP"), 1, GL_FALSE, mvp.constData());
        glUniform1f(glGetUniformLocation(program_, "uPointSize"), 18.0f * pointScale_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vertiports_.size()));
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // Holds per vertiport and kind: one ring, larger for more holds
    lines_.clear();
    rings_.clear();
    if (!index_ || !showHolds_ || activeHolds_ == 0) return;
    std::map<std::pair<std::pair<float, float>, uint8_t>, int> holds;
    const auto& events = index_->overlayEvents();
    for (uint32_t i : active_) {
        const OverlayEvent& ev = events[i];
        if (ev.kind == static_cast<uint8_t>(OverlayKind::Loss)) continue;
        if (ev.flags & OVERLAY_FLAG_PLACED) ++holds[{{ev.x, ev.y}, ev.kind}];
    }
    for (const auto& [key, count] : holds) {
        const bool ground = key.second == static_cast<uint8_t>(OverlayKind::GroundHold);
        // An airborne ring is drawn outside a ground ring at the same place
        const float size = (ground ? 26.0f : 34.0f) + 4.0f * static_cast<float>(std::min(count - 1, 6));
        if (ground) pushMark(rings_, key.first.first, key.first.second, 1.0f, 0.76f, 0.10f, 0.95f, size);
        else pushMark(rings_, key.first.first, key.first.second, 1.0f, 0.36f, 0.20f, 0.95f, size);
    }
    drawEventMarks();
}

} // namespace simvis
