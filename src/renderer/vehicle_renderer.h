#pragma once

#include "gl_renderer.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"
#include <vector>
#include <unordered_map>
#include <algorithm>

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

    // Find vehicle near a world position (returns kNoVehicle if none found)
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

    // Set tracked vehicle for highlight (kNoVehicle = none)
    void setTrackedVehicle(uint32_t vehicleId) { trackedVehicleId_ = vehicleId; }
    uint32_t trackedVehicle() const { return trackedVehicleId_; }

    // Per-mode visibility (View > Vehicles on the ribbon), by mode id
    // (core/modes.h). Hidden vehicles are neither rendered nor clickable.
    void setModeVisible(uint8_t modeId, bool show) {
        if (modeId >= 64) return;
        if (show) hiddenModes_ &= ~(uint64_t(1) << modeId);
        else      hiddenModes_ |= uint64_t(1) << modeId;
    }

    // True if this vehicle's mode is currently visible
    bool isModeVisible(uint32_t vehicleId) const;

    // --- Air view: vehicles of a 3D mode (core/modes.h) ---
    // Height lift: the symbol is drawn this many screen pixels per metre of
    // height above its ground position, with a stem down to a ground mark.
    // 0 draws it at its ground position.
    void setLiftPxPerMetre(float pxPerMetre) { liftPxPerM_ = std::max(pxPerMetre, 0.0f); }
    float liftPxPerMetre() const { return liftPxPerM_; }
    // Short fading trail behind each 3D vehicle (the last kTrailSamples x
    // kTrailStepS seconds), lifted like the symbol
    void setShowTrails(bool show) { showTrails_ = show; }
    // Only the 3D vehicles with a height in [minZ, maxZ] show and can be
    // clicked. Ground vehicles are not filtered.
    void setAltitudeBand(float minZ, float maxZ) { bandMin_ = minZ; bandMax_ = maxZ; }
    // Height at the top of the altitude colour ramp, m
    void setAltitudeTop(float z) { altitudeTop_ = std::max(z, 1.0f); }
    float altitudeTop() const { return altitudeTop_; }
    // The vehicle is on the map now: active, its mode visible, in the band
    bool isShown(uint32_t vehicleId) const {
        const VehicleState* state = getVehicleState(vehicleId);
        return state && isModeVisible(vehicleId) && isInBand(*state);
    }
    // Number of active vehicles of a 3D mode (aircraft aloft or taxiing)
    size_t activeAirCount() const { return activeAirCount_; }
    // Where the symbol of this vehicle is drawn, in world coordinates: the
    // ground position, moved up the screen by the lift for a 3D vehicle
    Point2D displayPosition(const VehicleState& state) const;
    // Altitude colour ramp (violet low to yellow high), t in [0, 1]
    static void altitudeColor(float t, float& r, float& g, float& b);

private:
    // Heading of a 3D vehicle on this segment. On a link with no horizontal
    // length (vertical, hover) it is the heading of the nearest horizontal
    // link of the same trajectory, so the symbol does not turn.
    float headingOnSegment(VehicleId vid, const VehicleSegment* seg) const;
    // Ground position and height of a vehicle at a time (false if not active)
    bool positionAt(VehicleId vid, TimeMs time, float& x, float& y, float& z) const;
    bool isInBand(const VehicleState& state) const {
        return !state.is3D || (state.z >= bandMin_ && state.z <= bandMax_);
    }
    // World units per screen pixel of the current projection
    float worldPerPixel() const;
    // Trails, stems and ground marks of the 3D vehicles (before the symbols)
    void renderAirCues();

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
    size_t activeAirCount_ = 0;

    // Smooth link transition parameters
    static constexpr float LINK_TRANSITION_DURATION = 0.5f;

    // Reusable buffer for getActiveVehicles results
    std::vector<VehicleId> activeVehicleIds_;

    // OpenGL resources
    GLuint vehicleVAO_ = 0;
    GLuint vehicleVBO_ = 0;
    GLuint vehicleProgram_ = 0;
    // Lines and points of the air view: x, y, lift px, r, g, b, a
    GLuint airVAO_ = 0;
    GLuint airVBO_ = 0;
    GLuint airProgram_ = 0;
    std::vector<float> airLines_;
    std::vector<float> airPoints_;

    // Emoji icon atlas for "By Mode" display: one row of tiles, one per mode
    // in mode-id order, rendered once via QPainter and uploaded.
    GLuint iconAtlas_ = 0;
    int iconTileCount_ = 0;
    static constexpr int kIconTileSize = 64;  // px per tile
    void createIconAtlas();

    // Rendering options
    float vehicleSize_ = 8.0f;  // Default to large
    float pointScale_ = 1.0f;   // 1.0 on-screen; raised during high-res export
    int vehicleShape_ = -1;     // -1=auto, 0=circle, 1=triangle, 2=rectangle, 3=diamond
    VehicleColorMode colorMode_ = VehicleColorMode::Speed;
    uint32_t trackedVehicleId_ = kNoVehicle;

    // Per-mode visibility: bit i set = mode i hidden
    uint64_t hiddenModes_ = 0;

    // Air view
    static constexpr int kTrailSamples = 10;
    static constexpr float kTrailStepS = 3.0f;
    float liftPxPerM_ = 0.04f;
    bool showTrails_ = true;
    float bandMin_ = -1e9f;
    float bandMax_ = 1e9f;
    float altitudeTop_ = 1000.0f;
};

} // namespace simvis
