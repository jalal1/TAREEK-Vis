#pragma once

#include "gl_renderer.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"
#include <vector>

namespace simvis {

class VehicleRenderer;

// Air view overlays that are not vehicles (docs/air-and-modes-plan.md):
//
// - Vertiports: the ground ends of the vertical air links (RoadType::
//   AirVertical). The pads of one vertiport are within a few metres of each
//   other, so ends in one cell of kClusterM are one vertiport.
// - Losses of separation: a red line between the two aircraft while the loss
//   lasts, at the positions where their symbols are drawn.
// - Holds: a ring at the vertiport while a hold lasts, amber on the ground
//   and red-orange in the air. More holds at once give a larger ring.
class AirOverlayRenderer : public GLRenderer {
public:
    AirOverlayRenderer() = default;
    ~AirOverlayRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Finds the vertiports of the network. No air links: no vertiports.
    void setNetworkIndex(const NetworkIndex* index);
    const std::vector<Point2D>& vertiports() const { return vertiports_; }

    // The events, and where the aircraft are drawn (not owned)
    void setVehicleIndex(const VehicleIndex* index);
    void setVehicleRenderer(const VehicleRenderer* renderer) { vehicles_ = renderer; }

    void setShowVertiports(bool show) { showVertiports_ = show; }
    void setShowLosses(bool show) { showLosses_ = show; }
    void setShowHolds(bool show) { showHolds_ = show; }
    // Multiplier for point sizes in a high-resolution export
    void setPointScale(float scale) { pointScale_ = scale; }

    // Finds the events that are active at this time
    void updateTime(float simulationTime);
    size_t activeLossCount() const { return activeLosses_; }
    size_t activeHoldCount() const { return activeHolds_; }

    // Vertiports and hold rings: under the vehicles
    void render();
    // Loss lines and rings: over the vehicles, so that a loss between two
    // aircraft at one place (a pad) still shows
    void renderLosses();

private:
    static constexpr double kClusterM = 30.0;

    void drawEventMarks();

    std::vector<Point2D> vertiports_;
    bool showVertiports_ = true;
    bool showLosses_ = true;
    bool showHolds_ = true;
    float pointScale_ = 1.0f;

    const VehicleIndex* index_ = nullptr;
    const VehicleRenderer* vehicles_ = nullptr;
    TimeMs maxDurationMs_ = 0;          // longest event, for the time search
    std::vector<uint32_t> active_;      // indices into overlayEvents()
    size_t activeLosses_ = 0;
    size_t activeHolds_ = 0;

    // Vertiport marks
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    bool bufferDirty_ = true;

    // Event lines and rings: x, y, r, g, b, a, size
    GLuint eventProgram_ = 0;
    GLuint eventVao_ = 0;
    GLuint eventVbo_ = 0;
    std::vector<float> lines_;
    std::vector<float> rings_;
};

} // namespace simvis
