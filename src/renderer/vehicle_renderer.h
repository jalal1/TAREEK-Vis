#pragma once

#include "gl_renderer.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"
#include <vector>
#include <unordered_map>

namespace simvis {

enum class VehicleColorMode : uint8_t {
    Speed = 0,      // Existing green-yellow-red gradient
    TransitMode     // Color by vehicle type (bus/tram/rail/car)
};

// Renders vehicles as moving points on the network
class VehicleRenderer : public GLRenderer {
public:
    VehicleRenderer();
    ~VehicleRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Set data sources
    void setNetworkIndex(NetworkIndex* index);
    void setVehicleIndex(VehicleIndex* index);

    // Update vehicle positions for current simulation time
    void updateTime(float simulationTime);

    // Render all active vehicles
    void render();

    // Get active vehicle count
    size_t activeVehicleCount() const { return activeVehicleCount_; }

    // Get vehicle state by ID (returns nullptr if not found or inactive)
    const VehicleState* getVehicleState(uint32_t vehicleId) const;

    // Find vehicle near a world position (returns 0 if none found)
    // hitRadius is in world coordinates
    uint32_t findVehicleAt(double worldX, double worldY, double hitRadius) const;

    // Rendering options
    void setVehicleSize(float size) { vehicleSize_ = size; }
    float getVehicleSize() const { return vehicleSize_; }

    // Multiplier for point sizes; >1 keeps vehicles proportional during
    // high-res off-screen export (point size is in pixels, not world units).
    void setPointScale(float scale) { pointScale_ = scale; }
    float pointScale() const { return pointScale_; }
    void setVehicleShape(int shape) { vehicleShape_ = shape; }  // -1=auto, 0=circle, 1=triangle, 2=rectangle, 3=diamond
    int getVehicleShape() const { return vehicleShape_; }
    // Changing the color mode recomputes vehicle colors immediately (colors
    // are assigned in updateTime(), which otherwise only runs as the sim clock
    // advances - so a paused sim would keep stale colors without this).
    void setColorMode(VehicleColorMode mode) {
        if (mode == colorMode_) return;
        colorMode_ = mode;
        updateTime(currentTime_);
    }
    VehicleColorMode colorMode() const { return colorMode_; }

    // Set tracked vehicle for highlight (0 = none)
    void setTrackedVehicle(uint32_t vehicleId) { trackedVehicleId_ = vehicleId; }
    uint32_t trackedVehicle() const { return trackedVehicleId_; }

    // Per-mode visibility (View > Vehicles on the ribbon). Hidden vehicles are
    // neither rendered nor clickable.
    void setShowCars(bool show)  { showCars_ = show; }
    void setShowBuses(bool show) { showBuses_ = show; }
    void setShowTrams(bool show) { showTrams_ = show; }
    void setShowRail(bool show)  { showRail_ = show; }

    // True if this vehicle's mode is currently visible
    bool isModeVisible(uint32_t vehicleId) const;

private:
    // Interpolate vehicle position along link
    Point2D interpolatePosition(uint32_t linkId, float progress) const;
    float getLinkAngle(uint32_t linkId) const;

    // Speed-based color: maps speedRatio (0=stopped, 1=freeflow) to RGB
    void getSpeedColor(float speedRatio, float& r, float& g, float& b) const;

    // Smooth transition helpers
    static float smoothstep(float t);
    static float lerpAngle(float from, float to, float t);

    NetworkIndex* networkIndex_ = nullptr;
    VehicleIndex* vehicleIndex_ = nullptr;

    // Current simulation time
    float currentTime_ = 0.0f;

    // Active vehicles: vehicleId -> state (rebuilt each frame from VehicleIndex)
    std::unordered_map<uint32_t, VehicleState> activeVehicles_;
    size_t activeVehicleCount_ = 0;

    // Smooth link transition parameters
    static constexpr float LINK_TRANSITION_DURATION = 0.5f;

    // Reusable buffer for getActiveVehicles results
    std::vector<VehicleId> activeVehicleIds_;

    // OpenGL resources
    GLuint vehicleVAO_ = 0;
    GLuint vehicleVBO_ = 0;
    GLuint vehicleProgram_ = 0;

    // Emoji icon atlas for "By Mode" display: one row of tiles
    // [car, bus, tram, rail], rendered once via QPainter and uploaded.
    GLuint iconAtlas_ = 0;
    static constexpr int kIconTileCount = 4;
    static constexpr int kIconTileSize = 64;  // px per tile
    void createIconAtlas();

    // Rendering options
    float vehicleSize_ = 8.0f;  // Default to large
    float pointScale_ = 1.0f;   // 1.0 on-screen; raised during high-res export
    int vehicleShape_ = -1;     // -1=auto, 0=circle, 1=triangle, 2=rectangle, 3=diamond
    VehicleColorMode colorMode_ = VehicleColorMode::Speed;
    uint32_t trackedVehicleId_ = 0;  // 0 = no vehicle tracked

    // Per-mode visibility
    bool showCars_ = true;
    bool showBuses_ = true;
    bool showTrams_ = true;
    bool showRail_ = true;
};

} // namespace simvis
