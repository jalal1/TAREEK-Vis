#pragma once

#include <QOpenGLWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QSize>
#include <memory>

class QPainter;

#include "renderer/network_renderer.h"
#include "renderer/vehicle_renderer.h"
#include "renderer/transit_route_renderer.h"
#include "renderer/counts_renderer.h"
#include "renderer/person_route_renderer.h"
#include "renderer/halo_renderer.h"
#include "renderer/tile_renderer.h"
#include "renderer/heatmap_renderer.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"

class QPainter;

namespace simvis {

class VideoRecorder;  // Forward declaration

// Main OpenGL widget for rendering the map visualization
class MapWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit MapWidget(QWidget* parent = nullptr);
    ~MapWidget() override;

    // Set data sources
    void setNetworkIndex(NetworkIndex* index);
    void setVehicleIndex(VehicleIndex* index);
    void setTransitData(const TransitScheduleParser::Result* data);

    // Camera control
    void fitToNetwork();
    void zoomIn();
    void zoomOut();
    void resetView();

    // Simulation time
    void setSimulationTime(float time);
    float simulationTime() const { return simulationTime_; }

    // Playback control
    void setPlaying(bool playing);
    bool isPlaying() const { return isPlaying_; }
    void setPlaybackSpeed(double speed);
    double playbackSpeed() const { return playbackSpeed_; }

    // Rendering options
    void setShowNodes(bool show);
    void setShowLinks(bool show);
    void setShowVehicles(bool show);
    void setShowCars(bool show);
    void setShowBuses(bool show);
    void setShowTrams(bool show);
    void setShowRailVehicles(bool show);

    // Vehicle rendering options
    void setVehicleSize(float size);
    float getVehicleSize() const;
    void setVehicleShape(int shape);  // -1=auto, 0=circle, 1=triangle, 2=rect, 3=diamond
    int getVehicleShape() const;
    void setVehicleColorMode(VehicleColorMode mode);
    VehicleColorMode getVehicleColorMode() const;

    // Background map options
    void setShowBackgroundMap(bool show);
    bool showBackgroundMap() const;
    void setTileSource(TileSource source);
    TileSource tileSource() const;
    void setCRS(const CRSInfo& crs);

    // Transit rendering options
    void setShowBusRoutes(bool show);
    void setShowBusStops(bool show);
    void setShowTramRoutes(bool show);
    void setShowRailRoutes(bool show);
    void setRouteLineWidth(float width);
    void setTransitHighlightedLine(uint32_t lineId);

    // Counts rendering options
    void setCountsData(const CountsData* data);
    void setShowCounts(bool show);

    // Vehicle tracking
    void setTrackedVehicle(uint32_t vehicleId);  // 0 to stop tracking
    uint32_t trackedVehicle() const { return trackedVehicleId_; }
    bool isTrackingVehicle() const { return trackedVehicleId_ != 0; }
    void stopTracking();
    // Live state of a vehicle this frame (nullptr if not currently active)
    const VehicleState* vehicleState(uint32_t vehicleId) const;

    // Person route overlay (full-day yellow route). Empty segments hide it.
    void setPersonRoute(const std::vector<PersonRouteRenderer::Segment>& segments);
    void setPersonRouteVisible(bool visible);

    // Activity markers drawn on top of the route (emoji icon + label at a
    // world position). Shown/hidden together with the person route overlay.
    struct ActivityMarker {
        float x, y;      // world coordinates
        QString icon;    // emoji
        QString label;   // activity name (e.g. "Home")
    };
    void setActivityMarkers(const std::vector<ActivityMarker>& markers);

    // Density heatmap overlay. An empty vector clears it. `lixelSize` is the
    // lixel length used for the computation, in meters, which sets the mark
    // size. `title` names the map in the legend, e.g. "Home".
    void setHeatmap(const std::vector<NkdvNetwork::Lixel>& lixels,
                    float lixelSize, const QString& title);
    void setHeatmapVisible(bool visible);
    bool heatmapVisible() const;
    void clearHeatmap();

    // Anchor the top of the color ramp at a shared value, so maps of different
    // activity types can be compared. Pass 0 to scale each map by its own peak.
    void setHeatmapScaleMax(float value);
    // Peak density of the map currently shown, or 0 when there is none.
    float heatmapPeak() const;

    // Highlight a single network link (yellow band, reuses the route overlay).
    void highlightLink(uint32_t linkId);
    // Clear every transient highlight/overlay (route, link, vehicle track,
    // transit line, activity markers). Used when a new selection is made.
    void clearAllHighlights();

    // Pan to world coordinates
    void panTo(float worldX, float worldY);

    // Video recording support
    void setVideoRecorder(VideoRecorder* recorder);
    VideoRecorder* videoRecorder() const { return videoRecorder_; }
    QImage captureFrame();  // Capture current frame as QImage

    // Render the current view off-screen at scaleFactor x the current pixel
    // size (1=native). Used for high-resolution PNG/PDF export. Returns a null
    // QImage on failure. The visible widget is left unchanged.
    QImage renderToImage(int scaleFactor);

    // Draw the heatmap key onto any paint device. The legend is a QPainter
    // overlay rather than GL, so both the live widget and the off-screen
    // export have to ask for it. `scale` multiplies every size so the key
    // keeps its on-screen proportions in a high-resolution export.
    void drawHeatmapLegend(QPainter& painter, const QSize& deviceSize, qreal scale);

signals:
    void simulationTimeChanged(float time);
    void viewChanged();
    void activeVehicleCountChanged(size_t count);
    void vehicleTrackingChanged(uint32_t vehicleId);  // 0 when tracking stopped
    void transitStopClicked(int stopIndex);            // stop index in transit data
    void transitRouteClicked(uint32_t lineId);         // transit line ID
    void countLinkClicked(uint32_t linkId);            // count link ID
    void networkLinkClicked(uint32_t linkId);          // network link internal ID

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private slots:
    void onPlaybackTick();

private:
    void updateProjection();
    void updateView();
    void updateVehicleHalo();  // sync tracked-vehicle halo position/visibility

    // Shown until a scenario is loaded, so the first window is not a blank
    // rectangle with no indication of what to do next
    void drawEmptyState(QPainter& painter);

    // Nearest network link to a world point within radius (UINT32_MAX = none)
    uint32_t findNetworkLinkAt(double worldX, double worldY, double radius) const;
    QPointF screenToWorld(const QPoint& screenPos) const;

    // Renderers
    std::unique_ptr<TileRenderer> tileRenderer_;
    std::unique_ptr<NetworkRenderer> networkRenderer_;
    std::unique_ptr<VehicleRenderer> vehicleRenderer_;
    std::unique_ptr<TransitRouteRenderer> transitRouteRenderer_;
    std::unique_ptr<CountsRenderer> countsRenderer_;
    std::unique_ptr<PersonRouteRenderer> personRouteRenderer_;
    std::unique_ptr<HeatmapRenderer> heatmapRenderer_;
    std::unique_ptr<HaloRenderer> vehicleHaloRenderer_;  // cyan ring on tracked vehicle
    std::unique_ptr<HaloRenderer> countsHaloRenderer_;   // cyan rings on count stations

    // Activity markers painted via QPainter on top of the GL frame
    std::vector<ActivityMarker> activityMarkers_;
    bool activityMarkersVisible_ = false;

    // Legend text for the density heatmap, e.g. "Home".
    QString heatmapTitle_;

    // Data sources (not owned)
    NetworkIndex* networkIndex_ = nullptr;
    VehicleIndex* vehicleIndex_ = nullptr;
    const TransitScheduleParser::Result* transitData_ = nullptr;

    // Video recording (not owned)
    VideoRecorder* videoRecorder_ = nullptr;

    // Camera state
    double centerX_ = 0;
    double centerY_ = 0;
    double zoom_ = 1.0;
    double minZoom_ = 0.001;
    double maxZoom_ = 100.0;

    // Mouse interaction
    bool isDragging_ = false;
    QPoint lastMousePos_;

    // Simulation state
    float simulationTime_ = 0.0f;
    float minTime_ = 0.0f;
    float maxTime_ = 0.0f;

    // Playback state
    bool isPlaying_ = false;
    double playbackSpeed_ = 1.0;
    QTimer playbackTimer_;
    QElapsedTimer playbackClock_;

    // Rendering options
    bool showNodes_ = false;  // Off by default (View > Show Nodes)
    bool showLinks_ = true;
    bool showVehicles_ = true;

    // Vehicle tracking
    uint32_t trackedVehicleId_ = 0;  // 0 = not tracking any vehicle

    // Matrices
    QMatrix4x4 projectionMatrix_;
    QMatrix4x4 viewMatrix_;
};

} // namespace simvis