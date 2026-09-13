#include "map_widget.h"
#include "core/video_recorder.h"
#include "core/logger.h"
#include "renderer/heatmap_palette.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace simvis {

MapWidget::MapWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Set up playback timer (60 FPS)
    playbackTimer_.setInterval(16);  // ~60 FPS
    connect(&playbackTimer_, &QTimer::timeout, this, &MapWidget::onPlaybackTick);
}

MapWidget::~MapWidget() {
    makeCurrent();
    countsRenderer_.reset();
    transitRouteRenderer_.reset();
    networkRenderer_.reset();
    vehicleRenderer_.reset();
    tileRenderer_.reset();
    doneCurrent();
}

void MapWidget::setNetworkIndex(NetworkIndex* index) {
    networkIndex_ = index;
    if (networkRenderer_) {
        networkRenderer_->setNetworkIndex(index);
    }
    if (vehicleRenderer_) {
        vehicleRenderer_->setNetworkIndex(index);
    }
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setNetworkIndex(index);
    }
    fitToNetwork();
    update();
}

void MapWidget::setVehicleIndex(VehicleIndex* index) {
    vehicleIndex_ = index;
    if (vehicleRenderer_) {
        vehicleRenderer_->setVehicleIndex(index);
    }
    if (index) {
        minTime_ = VehicleIndex::toSeconds(index->minTime());
        maxTime_ = VehicleIndex::toSeconds(index->maxTime());
        simulationTime_ = minTime_;
    }

    update();
}

void MapWidget::fitToNetwork() {
    if (!networkIndex_) return;

    const auto& bounds = networkIndex_->bounds();
    if (bounds.width() <= 0 || bounds.height() <= 0) return;

    centerX_ = bounds.center().x;
    centerY_ = bounds.center().y;

    // Calculate zoom to fit network with padding
    double aspectRatio = static_cast<double>(width()) / height();
    double boundsAspect = bounds.width() / bounds.height();

    if (boundsAspect > aspectRatio) {
        // Width-limited
        zoom_ = (width() * 0.9) / bounds.width();
    } else {
        // Height-limited
        zoom_ = (height() * 0.9) / bounds.height();
    }

    updateView();
    emit viewChanged();
    update();
}

void MapWidget::zoomIn() {
    zoom_ *= 1.5;
    zoom_ = std::min(zoom_, maxZoom_);
    updateView();
    emit viewChanged();
    update();
}

void MapWidget::zoomOut() {
    zoom_ /= 1.5;
    zoom_ = std::max(zoom_, minZoom_);
    updateView();
    emit viewChanged();
    update();
}

void MapWidget::resetView() {
    fitToNetwork();
}

void MapWidget::setSimulationTime(float time) {
    simulationTime_ = std::clamp(time, minTime_, maxTime_);

    if (vehicleRenderer_) {
        vehicleRenderer_->updateTime(simulationTime_);
        emit activeVehicleCountChanged(vehicleRenderer_->activeVehicleCount());

        // Update camera to follow tracked vehicle
        if (trackedVehicleId_ != 0) {
            const auto* state = vehicleRenderer_->getVehicleState(trackedVehicleId_);
            if (state) {
                centerX_ = state->x;
                centerY_ = state->y;
                updateView();
            } else {
                // Vehicle no longer exists - stop tracking
                stopTracking();
            }
        }
        // Keep the selection halo glued to the tracked vehicle as it moves
        updateVehicleHalo();
    }

    emit simulationTimeChanged(simulationTime_);
    update();
}

void MapWidget::setPlaying(bool playing) {
    isPlaying_ = playing;

    if (playing) {
        playbackClock_.start();
        playbackTimer_.start();
    } else {
        playbackTimer_.stop();
    }
}

void MapWidget::setPlaybackSpeed(double speed) {
    playbackSpeed_ = speed;
}

void MapWidget::setShowNodes(bool show) {
    showNodes_ = show;
    if (networkRenderer_) {
        networkRenderer_->setShowNodes(show);
    }
    update();
}

void MapWidget::setShowLinks(bool show) {
    showLinks_ = show;
    if (networkRenderer_) {
        networkRenderer_->setShowLinks(show);
    }
    update();
}

void MapWidget::setShowVehicles(bool show) {
    showVehicles_ = show;
    update();
}

void MapWidget::setShowCars(bool show) {
    if (vehicleRenderer_) vehicleRenderer_->setShowCars(show);
    update();
}

void MapWidget::setShowBuses(bool show) {
    if (vehicleRenderer_) vehicleRenderer_->setShowBuses(show);
    update();
}

void MapWidget::setShowTrams(bool show) {
    if (vehicleRenderer_) vehicleRenderer_->setShowTrams(show);
    update();
}

void MapWidget::setShowRailVehicles(bool show) {
    if (vehicleRenderer_) vehicleRenderer_->setShowRail(show);
    update();
}

void MapWidget::setShowBackgroundMap(bool show) {
    if (tileRenderer_) {
        tileRenderer_->setEnabled(show);
    }
    update();
}

bool MapWidget::showBackgroundMap() const {
    return tileRenderer_ && tileRenderer_->isEnabled();
}

void MapWidget::setTileSource(TileSource source) {
    if (tileRenderer_) {
        makeCurrent();
        tileRenderer_->setTileSource(source);
        doneCurrent();
    }
    update();
}

TileSource MapWidget::tileSource() const {
    return tileRenderer_ ? tileRenderer_->tileSource() : TileSource::OpenStreetMap;
}

void MapWidget::setCRS(const CRSInfo& crs) {
    if (tileRenderer_) {
        tileRenderer_->setCRS(crs);
    }
}

void MapWidget::setVehicleSize(float size) {
    if (vehicleRenderer_) {
        vehicleRenderer_->setVehicleSize(size);
    }
    update();
}

float MapWidget::getVehicleSize() const {
    if (vehicleRenderer_) {
        return vehicleRenderer_->getVehicleSize();
    }
    return 5.0f;
}

void MapWidget::setVehicleShape(int shape) {
    if (vehicleRenderer_) {
        vehicleRenderer_->setVehicleShape(shape);
    }
    update();
}

int MapWidget::getVehicleShape() const {
    if (vehicleRenderer_) {
        return vehicleRenderer_->getVehicleShape();
    }
    return 1;
}

void MapWidget::setVehicleColorMode(VehicleColorMode mode) {
    if (vehicleRenderer_) {
        vehicleRenderer_->setColorMode(mode);
    }
    update();
}

VehicleColorMode MapWidget::getVehicleColorMode() const {
    if (vehicleRenderer_) {
        return vehicleRenderer_->colorMode();
    }
    return VehicleColorMode::Speed;
}

void MapWidget::setTransitData(const TransitScheduleParser::Result* data) {
    transitData_ = data;
    if (transitRouteRenderer_) {
        makeCurrent();
        transitRouteRenderer_->setTransitData(data);
        if (networkIndex_) {
            transitRouteRenderer_->setNetworkIndex(networkIndex_);
        }
        transitRouteRenderer_->buildBuffers();
        doneCurrent();
    }
    update();
}

void MapWidget::setShowBusRoutes(bool show) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setShowBusRoutes(show);
    }
    update();
}

void MapWidget::setShowTramRoutes(bool show) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setShowTramRoutes(show);
    }
    update();
}

void MapWidget::setShowRailRoutes(bool show) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setShowRailRoutes(show);
    }
    update();
}

void MapWidget::setShowBusStops(bool show) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setShowBusStops(show);
    }
    update();
}

void MapWidget::setRouteLineWidth(float width) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setRouteLineWidth(width);
    }
    update();
}

void MapWidget::setTransitHighlightedLine(uint32_t lineId) {
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setHighlightedLine(lineId);
    }
    update();
}

void MapWidget::setTrackedVehicle(uint32_t vehicleId) {
    if (trackedVehicleId_ != vehicleId) {
        trackedVehicleId_ = vehicleId;

        // Update renderer's tracked vehicle for highlighting
        if (vehicleRenderer_) {
            vehicleRenderer_->setTrackedVehicle(vehicleId);
        }

        emit vehicleTrackingChanged(vehicleId);

        // Immediately center on vehicle if tracking started
        if (vehicleId != 0 && vehicleRenderer_) {
            const auto* state = vehicleRenderer_->getVehicleState(vehicleId);
            if (state) {
                centerX_ = state->x;
                centerY_ = state->y;
                updateView();
            }
        }
        updateVehicleHalo();
        update();
    }
}

void MapWidget::updateVehicleHalo() {
    if (!vehicleHaloRenderer_) return;
    if (trackedVehicleId_ != 0 && vehicleRenderer_) {
        const auto* state = vehicleRenderer_->getVehicleState(trackedVehicleId_);
        if (state) {
            vehicleHaloRenderer_->setPositions({{state->x, state->y}});
            vehicleHaloRenderer_->setVisible(true);
            return;
        }
    }
    vehicleHaloRenderer_->setVisible(false);
}

void MapWidget::stopTracking() {
    setTrackedVehicle(0);
}

const VehicleState* MapWidget::vehicleState(uint32_t vehicleId) const {
    return vehicleRenderer_ ? vehicleRenderer_->getVehicleState(vehicleId) : nullptr;
}

void MapWidget::setPersonRoute(const std::vector<PersonRouteRenderer::Segment>& segments) {
    if (personRouteRenderer_) {
        personRouteRenderer_->setRoute(segments);
        // Ensure current matrices are applied even if the renderer was created
        // after the last updateView() (e.g. before any pan/zoom happened)
        personRouteRenderer_->setViewMatrix(viewMatrix_);
        personRouteRenderer_->setProjectionMatrix(projectionMatrix_);
        personRouteRenderer_->setHalfWidth(
            static_cast<float>(5.0 / std::max(zoom_, 1e-9)));
    }
    update();
}

void MapWidget::setPersonRouteVisible(bool visible) {
    if (personRouteRenderer_) {
        personRouteRenderer_->setVisible(visible);
        personRouteRenderer_->setViewMatrix(viewMatrix_);
        personRouteRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    // Activity markers show/hide together with the route overlay
    activityMarkersVisible_ = visible;
    update();
}

void MapWidget::setActivityMarkers(const std::vector<ActivityMarker>& markers) {
    activityMarkers_ = markers;
    update();
}

void MapWidget::setHeatmap(const std::vector<NkdvNetwork::Lixel>& lixels,
                           float lixelSize, const QString& title) {
    if (!heatmapRenderer_) return;
    heatmapRenderer_->setLixels(lixels);
    heatmapRenderer_->setLixelSize(lixelSize);
    heatmapTitle_ = title;
    update();
}

void MapWidget::setHeatmapVisible(bool visible) {
    if (!heatmapRenderer_) return;
    heatmapRenderer_->setVisible(visible);
    update();
}

bool MapWidget::heatmapVisible() const {
    return heatmapRenderer_ && heatmapRenderer_->visible();
}

void MapWidget::setHeatmapScaleMax(float value) {
    if (!heatmapRenderer_) return;
    if (value == heatmapRenderer_->rawScaleMax()) return;  // nothing to redraw
    heatmapRenderer_->setScaleMax(value);
    update();
}

float MapWidget::heatmapPeak() const {
    // The anchor, not the single highest lixel: this feeds the shared scale,
    // which must not be set by a few extreme segments.
    return heatmapRenderer_ ? heatmapRenderer_->anchorValue() : 0.0f;
}

void MapWidget::clearHeatmap() {
    if (!heatmapRenderer_) return;
    heatmapRenderer_->clear();
    heatmapRenderer_->setVisible(false);
    heatmapTitle_.clear();
    update();
}

uint32_t MapWidget::findNetworkLinkAt(double worldX, double worldY, double radius) const {
    if (!networkIndex_) return 0xFFFFFFFFu;

    // Search only links near the click via the spatial index
    BoundingBox box;
    box.minX = worldX - radius; box.maxX = worldX + radius;
    box.minY = worldY - radius; box.maxY = worldY + radius;
    auto candidates = networkIndex_->getLinksInBounds(box);

    double bestDist = radius;
    uint32_t bestLink = 0xFFFFFFFFu;
    for (const LinkRecord* link : candidates) {
        const NodeRecord* a = networkIndex_->getNode(link->fromNode);
        const NodeRecord* b = networkIndex_->getNode(link->toNode);
        if (!a || !b) continue;

        // Point-to-segment distance
        double ax = a->x, ay = a->y, bx = b->x, by = b->y;
        double abx = bx - ax, aby = by - ay;
        double apx = worldX - ax, apy = worldY - ay;
        double abLenSq = abx * abx + aby * aby;
        double dist;
        if (abLenSq < 1e-10) {
            dist = std::sqrt(apx * apx + apy * apy);
        } else {
            double t = std::clamp((apx * abx + apy * aby) / abLenSq, 0.0, 1.0);
            double dx = worldX - (ax + t * abx);
            double dy = worldY - (ay + t * aby);
            dist = std::sqrt(dx * dx + dy * dy);
        }
        if (dist < bestDist) {
            bestDist = dist;
            bestLink = link->id;
        }
    }
    return bestLink;
}

void MapWidget::highlightLink(uint32_t linkId) {
    if (!networkIndex_ || !personRouteRenderer_) return;
    const LinkRecord* link = networkIndex_->getLink(linkId);
    if (!link) { setPersonRouteVisible(false); return; }
    const NodeRecord* a = networkIndex_->getNode(link->fromNode);
    const NodeRecord* b = networkIndex_->getNode(link->toNode);
    if (!a || !b) { setPersonRouteVisible(false); return; }

    // Reuse the yellow route overlay to draw the single highlighted link
    std::vector<PersonRouteRenderer::Segment> seg{
        {a->x, a->y, b->x, b->y, false}};
    setPersonRoute(seg);
    setActivityMarkers({});
    setPersonRouteVisible(true);
}

void MapWidget::clearAllHighlights() {
    // Route / link overlay + activity markers
    setPersonRouteVisible(false);
    setPersonRoute({});
    setActivityMarkers({});
    // Transit line highlight
    if (transitRouteRenderer_) transitRouteRenderer_->setHighlightedLine(0);
    // Vehicle tracking + its halo
    if (trackedVehicleId_ != 0) stopTracking();
    update();
}

void MapWidget::panTo(float worldX, float worldY) {
    centerX_ = worldX;
    centerY_ = worldY;
    updateView();
    emit viewChanged();
    update();
}

void MapWidget::initializeGL() {
    // Initialize renderers
    tileRenderer_ = std::make_unique<TileRenderer>(this);
    networkRenderer_ = std::make_unique<NetworkRenderer>();
    vehicleRenderer_ = std::make_unique<VehicleRenderer>();
    transitRouteRenderer_ = std::make_unique<TransitRouteRenderer>();

    if (!tileRenderer_->initialize()) {
        LOG_WARN("Failed to initialize tile renderer");
    }
    connect(tileRenderer_.get(), &TileRenderer::tileLoaded,
            this, QOverload<>::of(&QOpenGLWidget::update));

    if (!networkRenderer_->initialize()) {
        LOG_ERROR("Failed to initialize network renderer");
    }
    networkRenderer_->setShowNodes(showNodes_);  // widget default (off)

    if (!vehicleRenderer_->initialize()) {
        LOG_ERROR("Failed to initialize vehicle renderer");
    }

    if (!transitRouteRenderer_->initialize()) {
        LOG_WARN("Failed to initialize transit route renderer");
    }

    countsRenderer_ = std::make_unique<CountsRenderer>();
    personRouteRenderer_ = std::make_unique<PersonRouteRenderer>();
    heatmapRenderer_ = std::make_unique<HeatmapRenderer>();
    vehicleHaloRenderer_ = std::make_unique<HaloRenderer>();
    countsHaloRenderer_ = std::make_unique<HaloRenderer>();
    if (!countsRenderer_->initialize()) {
        LOG_WARN("Failed to initialize counts renderer");
    }
    if (!heatmapRenderer_->initialize()) {
        LOG_WARN("Failed to initialize heatmap renderer");
    }
    if (!personRouteRenderer_->initialize()) {
        LOG_WARN("Failed to initialize person route renderer");
    }
    if (!vehicleHaloRenderer_->initialize()) {
        LOG_WARN("Failed to initialize vehicle halo renderer");
    }
    if (!countsHaloRenderer_->initialize()) {
        LOG_WARN("Failed to initialize counts halo renderer");
    }
    // Count-station halos are a touch larger than the vehicle halo
    countsHaloRenderer_->setScreenDiameter(52.0f);

    // Set data sources if available
    if (networkIndex_) {
        networkRenderer_->setNetworkIndex(networkIndex_);
        vehicleRenderer_->setNetworkIndex(networkIndex_);
        transitRouteRenderer_->setNetworkIndex(networkIndex_);
    }

    if (vehicleIndex_) {
        vehicleRenderer_->setVehicleIndex(vehicleIndex_);
    }

    if (transitData_) {
        transitRouteRenderer_->setTransitData(transitData_);
        transitRouteRenderer_->buildBuffers();
    }

    // Set clear color (dark background)
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);

    LOG_INFO("MapWidget OpenGL initialization complete");
}

void MapWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    updateProjection();

    if (tileRenderer_) {
        tileRenderer_->setViewportSize(w, h);
    }
    if (networkRenderer_) {
        networkRenderer_->setViewportSize(w, h);
    }
    if (vehicleRenderer_) {
        vehicleRenderer_->setViewportSize(w, h);
    }
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setViewportSize(w, h);
    }
    if (countsRenderer_) {
        countsRenderer_->setViewportSize(w, h);
    }
    if (personRouteRenderer_) {
        personRouteRenderer_->setViewportSize(w, h);
    }
}

void MapWidget::paintGL() {
    try {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render background map tiles (bottom-most layer)
        if (tileRenderer_) {
            tileRenderer_->render();
        }

        // Render network
        if (networkRenderer_) {
            networkRenderer_->render();
        }
        // Density heatmap sits directly on the roads it describes, so it is
        // drawn just above the network and below every selection overlay.
        if (heatmapRenderer_) {
            heatmapRenderer_->render();
        }

        // Render count links overlay (above network, below transit)
        if (countsRenderer_) {
            countsRenderer_->render();
        }
        // Cyan halos marking count-station locations (above the yellow links)
        if (countsHaloRenderer_) {
            countsHaloRenderer_->render();
        }

        // Render transit routes and stops (middle layer, above network)
        if (transitRouteRenderer_) {
            transitRouteRenderer_->render();
        }

        // Person full-day route overlay (above routes, below vehicles)
        if (personRouteRenderer_) {
            personRouteRenderer_->render();
        }

        // Tracked-vehicle selection halo (just under the vehicle glyph)
        if (vehicleHaloRenderer_) {
            vehicleHaloRenderer_->render();
        }

        // Render vehicles (top layer)
        if (showVehicles_ && vehicleRenderer_) {
            vehicleRenderer_->render();
        }

        // Activity markers (QPainter over the GL frame): emoji + label pinned
        // to world positions, shown together with the person route overlay
        if (activityMarkersVisible_ && !activityMarkers_.empty()) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QFont iconFont = painter.font();
            iconFont.setPointSize(16);
            QFont labelFont = painter.font();
            labelFont.setPointSize(9);
            labelFont.setBold(true);

            for (const auto& m : activityMarkers_) {
                // World -> screen (inverse of screenToWorld)
                double halfW = width() / (2.0 * zoom_);
                double halfH = height() / (2.0 * zoom_);
                double sx = (m.x - centerX_ + halfW) / (2.0 * halfW) * width();
                double sy = (1.0 - (m.y - centerY_ + halfH) / (2.0 * halfH)) * height();
                if (sx < -50 || sx > width() + 50 || sy < -50 || sy > height() + 50)
                    continue;

                // Label pill under the icon for readability on any background
                painter.setFont(labelFont);
                QString text = m.label;
                QRectF textRect = painter.fontMetrics().boundingRect(text);
                QRectF pill(sx - textRect.width() / 2.0 - 6, sy + 4,
                            textRect.width() + 12, textRect.height() + 4);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 160));
                painter.drawRoundedRect(pill, 6, 6);
                painter.setPen(Qt::white);
                painter.drawText(pill, Qt::AlignCenter, text);

                // Emoji icon above the label
                painter.setFont(iconFont);
                QRectF iconRect(sx - 16, sy - 30, 32, 32);
                painter.drawText(iconRect, Qt::AlignCenter, m.icon);
            }
        }

        // Heatmap legend. A magnitude encoding is unreadable without a key, so
        // the ramp is drawn with its value range whenever the map is shown.
        {
            QPainter painter(this);
            drawHeatmapLegend(painter, size(), 1.0);
        }

        // Nothing loaded yet: say how to load something
        if (!networkIndex_ || networkIndex_->nodeCount() == 0) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            drawEmptyState(painter);
        }

        // Submit frame to video recorder if recording
        if (videoRecorder_ && videoRecorder_->isRecording()) {
            try {
                QImage frame = captureFrame();
                videoRecorder_->submitFrame(frame, simulationTime_);
            } catch (const std::exception& e) {
                LOG_ERROR(QString("Frame capture exception: %1").arg(e.what()));
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("paintGL exception: %1").arg(e.what()));
    }
}

void MapWidget::drawEmptyState(QPainter& painter) {
    QFont titleFont = painter.font();
    titleFont.setPointSize(15);
    QFont hintFont = painter.font();
    hintFont.setPointSize(10);

    const QRect area = rect();

    painter.setFont(titleFont);
    painter.setPen(QColor(190, 190, 200));
    painter.drawText(QRect(area.left(), area.center().y() - 34, area.width(), 30),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("Drop a MATSim output folder here"));

    painter.setFont(hintFont);
    painter.setPen(QColor(130, 130, 140));
    painter.drawText(QRect(area.left(), area.center().y() + 2, area.width(), 22),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("or use File > Open Folder"));
    painter.drawText(QRect(area.left(), area.center().y() + 26, area.width(), 22),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("The network, events and transit files are found for you."));
}

void MapWidget::updateProjection() {
    projectionMatrix_.setToIdentity();

    double halfWidth = width() / (2.0 * zoom_);
    double halfHeight = height() / (2.0 * zoom_);

    projectionMatrix_.ortho(
        -halfWidth, halfWidth,
        -halfHeight, halfHeight,
        -1.0f, 1.0f
    );

    if (tileRenderer_) {
        tileRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    if (networkRenderer_) {
        networkRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    if (vehicleRenderer_) {
        vehicleRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    if (countsRenderer_) {
        countsRenderer_->setProjectionMatrix(projectionMatrix_);
    }
    if (personRouteRenderer_) {
        personRouteRenderer_->setProjectionMatrix(projectionMatrix_);
        // Keep the route band a constant ~5px on screen regardless of zoom so
        // it stays clearly visible above other layers at any zoom level
        personRouteRenderer_->setHalfWidth(
            static_cast<float>(5.0 / std::max(zoom_, 1e-9)));
    }
    if (heatmapRenderer_) {
        heatmapRenderer_->setProjectionMatrix(projectionMatrix_);
        // The mark covers one lixel of ground, so it grows with zoom.
        heatmapRenderer_->setZoom(zoom_);
    }
    if (vehicleHaloRenderer_) vehicleHaloRenderer_->setProjectionMatrix(projectionMatrix_);
    if (countsHaloRenderer_)  countsHaloRenderer_->setProjectionMatrix(projectionMatrix_);
}

void MapWidget::updateView() {
    viewMatrix_.setToIdentity();
    viewMatrix_.translate(-centerX_, -centerY_, 0);

    if (tileRenderer_) {
        tileRenderer_->setViewMatrix(viewMatrix_);
    }
    if (networkRenderer_) {
        networkRenderer_->setViewMatrix(viewMatrix_);
    }
    if (vehicleRenderer_) {
        vehicleRenderer_->setViewMatrix(viewMatrix_);
    }
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setViewMatrix(viewMatrix_);
    }
    if (countsRenderer_) {
        countsRenderer_->setViewMatrix(viewMatrix_);
    }
    if (personRouteRenderer_) {
        personRouteRenderer_->setViewMatrix(viewMatrix_);
    }
    if (heatmapRenderer_) {
        heatmapRenderer_->setViewMatrix(viewMatrix_);
    }
    if (vehicleHaloRenderer_) vehicleHaloRenderer_->setViewMatrix(viewMatrix_);
    if (countsHaloRenderer_)  countsHaloRenderer_->setViewMatrix(viewMatrix_);

    updateProjection();
}

QPointF MapWidget::screenToWorld(const QPoint& screenPos) const {
    // Convert screen coordinates to normalized device coordinates
    double ndcX = (2.0 * screenPos.x() / width()) - 1.0;
    double ndcY = 1.0 - (2.0 * screenPos.y() / height());

    // Convert to world coordinates
    double halfWidth = width() / (2.0 * zoom_);
    double halfHeight = height() / (2.0 * zoom_);

    double worldX = centerX_ + ndcX * halfWidth;
    double worldY = centerY_ + ndcY * halfHeight;

    return QPointF(worldX, worldY);
}

void MapWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QPointF worldPos = screenToWorld(event->pos());
        double hitRadius = 20.0 / zoom_;

        // Check if clicking on a vehicle first
        if (showVehicles_ && vehicleRenderer_) {
            uint32_t clickedVehicle = vehicleRenderer_->findVehicleAt(
                worldPos.x(), worldPos.y(), hitRadius);

            if (clickedVehicle != 0) {
                setTrackedVehicle(clickedVehicle);
                return;
            }
        }

        // Check if clicking on a transit stop
        if (transitRouteRenderer_ && transitData_) {
            int stopIndex = transitRouteRenderer_->findStopAt(
                worldPos.x(), worldPos.y(), hitRadius);

            if (stopIndex >= 0) {
                emit transitStopClicked(stopIndex);
                return;
            }
        }

        // Check if clicking on a transit route
        if (transitRouteRenderer_ && transitData_) {
            uint32_t lineId = transitRouteRenderer_->findRouteAt(
                worldPos.x(), worldPos.y(), hitRadius);

            if (lineId != 0) {
                transitRouteRenderer_->setHighlightedLine(lineId);
                emit transitRouteClicked(lineId);
                update();
                return;
            }
        }

        // Check if clicking on a count link
        if (countsRenderer_) {
            uint32_t countLinkId = countsRenderer_->findCountLinkAt(
                worldPos.x(), worldPos.y(), hitRadius);

            if (countLinkId != 0) {
                emit countLinkClicked(countLinkId);
                return;
            }
        }

        // Check if clicking on a network link (lowest priority interactive
        // layer). Link id 0 is a valid internal id, so UINT32_MAX = miss.
        if (networkIndex_) {
            uint32_t linkId = findNetworkLinkAt(worldPos.x(), worldPos.y(), hitRadius);
            if (linkId != 0xFFFFFFFFu) {
                emit networkLinkClicked(linkId);
                return;
            }
        }

        // No interactive element clicked - start panning (and stop tracking)
        if (trackedVehicleId_ != 0) {
            stopTracking();
        }
        // Clear route highlight when clicking empty space
        if (transitRouteRenderer_) {
            transitRouteRenderer_->setHighlightedLine(0);
            update();
        }
        isDragging_ = true;
        lastMousePos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void MapWidget::mouseMoveEvent(QMouseEvent* event) {
    if (isDragging_) {
        QPoint delta = event->pos() - lastMousePos_;
        lastMousePos_ = event->pos();

        // Convert pixel delta to world delta
        double worldDeltaX = -delta.x() / zoom_;
        double worldDeltaY = delta.y() / zoom_;

        centerX_ += worldDeltaX;
        centerY_ += worldDeltaY;

        updateView();
        emit viewChanged();
        update();
    }
}

void MapWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        isDragging_ = false;
        setCursor(Qt::ArrowCursor);
    }
}

void MapWidget::wheelEvent(QWheelEvent* event) {
    // Get mouse position in world coordinates before zoom
    QPointF worldPosBefore = screenToWorld(event->position().toPoint());

    // Zoom
    double zoomFactor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
    zoom_ *= zoomFactor;
    zoom_ = std::clamp(zoom_, minZoom_, maxZoom_);

    // Get mouse position in world coordinates after zoom
    updateProjection();
    QPointF worldPosAfter = screenToWorld(event->position().toPoint());

    // Adjust center to keep mouse position fixed
    centerX_ += worldPosBefore.x() - worldPosAfter.x();
    centerY_ += worldPosBefore.y() - worldPosAfter.y();

    updateView();
    emit viewChanged();
    update();
}

void MapWidget::onPlaybackTick() {
    if (!isPlaying_) return;

    // Calculate elapsed time since last tick
    qint64 elapsedMs = playbackClock_.restart();

    // Cap elapsed time to avoid large jumps after pause/resume or heavy frames.
    // The timer fires every 16ms; allow up to 50ms to tolerate occasional slow frames
    // but reject anything larger (e.g. first tick after resuming playback).
    constexpr qint64 maxElapsedMs = 50;
    if (elapsedMs > maxElapsedMs) {
        elapsedMs = maxElapsedMs;
    }

    double elapsedSeconds = elapsedMs / 1000.0;

    // Advance simulation time
    float newTime = simulationTime_ + static_cast<float>(elapsedSeconds * playbackSpeed_);

    if (newTime > maxTime_) {
        newTime = maxTime_;
        setPlaying(false);  // Stop at end
    }

    setSimulationTime(newTime);
}

void MapWidget::setCountsData(const CountsData* data) {
    if (countsRenderer_) {
        makeCurrent();
        countsRenderer_->setCountsData(data);
        doneCurrent();
    }
    // Place a cyan halo at the midpoint of each count station's link so users
    // can spot station locations at a glance
    if (countsHaloRenderer_) {
        std::vector<std::pair<float, float>> centers;
        if (data) {
            centers.reserve(data->counts.size());
            for (const auto& c : data->counts) {
                if (c.fromX == 0 && c.fromY == 0 && c.toX == 0 && c.toY == 0) continue;
                // Only the primary direction of a paired link, to avoid two
                // overlapping halos on the same road
                if (c.pairedLinkId != 0 && !c.isPrimary) continue;
                centers.emplace_back((c.fromX + c.toX) * 0.5f,
                                     (c.fromY + c.toY) * 0.5f);
            }
        }
        countsHaloRenderer_->setPositions(centers);
    }
    update();
}

void MapWidget::setShowCounts(bool show) {
    if (countsRenderer_) {
        countsRenderer_->setShowCounts(show);
    }
    if (countsHaloRenderer_) {
        countsHaloRenderer_->setVisible(show);
    }
    update();
}

void MapWidget::setVideoRecorder(VideoRecorder* recorder) {
    videoRecorder_ = recorder;
}

QImage MapWidget::captureFrame() {
    // Capture the current OpenGL framebuffer
    // Note: This method should be called after paintGL() has rendered
    return grabFramebuffer();
}

QImage MapWidget::renderToImage(int scaleFactor) {
    if (scaleFactor < 1) scaleFactor = 1;

    const int baseW = width();
    const int baseH = height();
    if (baseW <= 0 || baseH <= 0) return QImage();

    // Target pixel dimensions. Multiply by devicePixelRatio so a 1x export on a
    // HiDPI display still matches what the user sees on screen.
    const qreal dpr = devicePixelRatioF();
    const int outW = static_cast<int>(baseW * dpr) * scaleFactor;
    const int outH = static_cast<int>(baseH * dpr) * scaleFactor;
    if (outW <= 0 || outH <= 0) return QImage();

    makeCurrent();

    // Off-screen framebuffer at the target resolution (independent of the
    // on-screen widget, so no black-border resize artifacts).
    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    fboFormat.setInternalTextureFormat(GL_RGBA8);
    QOpenGLFramebufferObject fbo(outW, outH, fboFormat);
    if (!fbo.isValid()) {
        LOG_ERROR("renderToImage: failed to create off-screen framebuffer");
        doneCurrent();
        return QImage();
    }

    // Build a projection that shows the SAME world area as the live view but at
    // the higher pixel count. The live projection half-extent is
    // width()/(2*zoom_); to keep it identical at outW pixels we scale zoom_ by
    // the same factor as the viewport (outW / baseW).
    const double pxScaleX = static_cast<double>(outW) / baseW;
    const double pxScaleY = static_cast<double>(outH) / baseH;
    const double exportZoomX = zoom_ * pxScaleX;
    const double exportZoomY = zoom_ * pxScaleY;

    QMatrix4x4 exportProj;
    exportProj.setToIdentity();
    {
        double halfWidth = outW / (2.0 * exportZoomX);
        double halfHeight = outH / (2.0 * exportZoomY);
        exportProj.ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, -1.0f, 1.0f);
    }

    // Push high-res projection + viewport size into every renderer. Renderers
    // use viewport size for pixel-space math (route quad width, point sizes),
    // so they must see the FBO dimensions while we render into it.
    auto applyExportState = [&](const QMatrix4x4& proj, int vpW, int vpH) {
        if (tileRenderer_)         { tileRenderer_->setProjectionMatrix(proj);         tileRenderer_->setViewportSize(vpW, vpH); }
        if (networkRenderer_)      { networkRenderer_->setProjectionMatrix(proj);      networkRenderer_->setViewportSize(vpW, vpH); }
        if (vehicleRenderer_)      { vehicleRenderer_->setProjectionMatrix(proj);      vehicleRenderer_->setViewportSize(vpW, vpH); }
        if (transitRouteRenderer_) { transitRouteRenderer_->setProjectionMatrix(proj); transitRouteRenderer_->setViewportSize(vpW, vpH); }
        if (countsRenderer_)       { countsRenderer_->setProjectionMatrix(proj);       countsRenderer_->setViewportSize(vpW, vpH); }
        if (personRouteRenderer_)  { personRouteRenderer_->setProjectionMatrix(proj);  personRouteRenderer_->setViewportSize(vpW, vpH); }
        if (vehicleHaloRenderer_)  { vehicleHaloRenderer_->setProjectionMatrix(proj);  vehicleHaloRenderer_->setViewportSize(vpW, vpH); }
        if (countsHaloRenderer_)   { countsHaloRenderer_->setProjectionMatrix(proj);   countsHaloRenderer_->setViewportSize(vpW, vpH); }
        if (heatmapRenderer_)      { heatmapRenderer_->setProjectionMatrix(proj);      heatmapRenderer_->setViewportSize(vpW, vpH); }
    };

    applyExportState(exportProj, outW, outH);

    // Point sizes (vehicles, transit stops) and route line width are specified in
    // pixels, so they don't grow with the viewport like world-space geometry does.
    // Scale them by the resolution factor so they stay proportional in the export.
    const float liveRouteWidth = transitRouteRenderer_ ? transitRouteRenderer_->routeLineWidth() : 0.0f;
    if (vehicleRenderer_)      vehicleRenderer_->setPointScale(static_cast<float>(scaleFactor));
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setPointScale(static_cast<float>(scaleFactor));
        transitRouteRenderer_->setRouteLineWidth(liveRouteWidth * scaleFactor);
    }
    if (vehicleHaloRenderer_)  vehicleHaloRenderer_->setPointScale(static_cast<float>(scaleFactor));
    if (countsHaloRenderer_)   countsHaloRenderer_->setPointScale(static_cast<float>(scaleFactor));
    // Lixel marks are sized in pixels from lixelSize * zoom, so they need the
    // same correction as the other point sprites to cover the same ground.
    if (heatmapRenderer_)      heatmapRenderer_->setPointScale(static_cast<float>(scaleFactor));

    fbo.bind();
    glViewport(0, 0, outW, outH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (tileRenderer_)         tileRenderer_->render();
    if (networkRenderer_)      networkRenderer_->render();
    if (heatmapRenderer_)      heatmapRenderer_->render();
    if (countsRenderer_)       countsRenderer_->render();
    if (countsHaloRenderer_)   countsHaloRenderer_->render();
    if (transitRouteRenderer_) transitRouteRenderer_->render();
    if (personRouteRenderer_)  personRouteRenderer_->render();
    if (vehicleHaloRenderer_)  vehicleHaloRenderer_->render();
    if (showVehicles_ && vehicleRenderer_) vehicleRenderer_->render();

    QImage image = fbo.toImage();
    fbo.release();

    // The legend is a QPainter overlay on the live widget, not GL, so the FBO
    // does not contain it. Paint it onto the exported image at the matching
    // scale, or the export shows colors with nothing to read them against.
    if (!image.isNull()) {
        QPainter painter(&image);
        drawHeatmapLegend(painter, image.size(), dpr * scaleFactor);
    }

    // Restore live state. The renderers' live viewport size is whatever Qt last
    // passed to resizeGL (device pixels = logical size * devicePixelRatio).
    const int liveW = static_cast<int>(baseW * dpr);
    const int liveH = static_cast<int>(baseH * dpr);
    applyExportState(projectionMatrix_, liveW, liveH);
    if (vehicleRenderer_)      vehicleRenderer_->setPointScale(1.0f);
    if (transitRouteRenderer_) {
        transitRouteRenderer_->setPointScale(1.0f);
        transitRouteRenderer_->setRouteLineWidth(liveRouteWidth);
    }
    if (vehicleHaloRenderer_)  vehicleHaloRenderer_->setPointScale(1.0f);
    if (countsHaloRenderer_)   countsHaloRenderer_->setPointScale(1.0f);
    if (heatmapRenderer_)      heatmapRenderer_->setPointScale(1.0f);

    doneCurrent();
    update();  // repaint the live widget with restored state

    return image;
}
void MapWidget::drawHeatmapLegend(QPainter& painter, const QSize& deviceSize, qreal scale) {
    if (!heatmapRenderer_ || !heatmapRenderer_->visible() ||
        !heatmapRenderer_->hasData()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    const int kBarW = static_cast<int>(180 * scale);
    const int kBarH = static_cast<int>(12 * scale);
    const int kPad = static_cast<int>(12 * scale);
    const int x = kPad;
    const int y = deviceSize.height() - kPad - kBarH - static_cast<int>(34 * scale);

    // Point sizes are device independent, so an export at N x the pixels needs
    // them multiplied by N for the text to keep its share of the image.
    QFont titleFont = painter.font();
    titleFont.setPointSizeF(9 * scale);
    titleFont.setBold(true);
    QFont tickFont = painter.font();
    tickFont.setPointSizeF(8 * scale);

    // Panel behind the key, so it stays readable over any map content.
    QRectF panel(x - 8 * scale, y - 20 * scale,
                 kBarW + 16 * scale, kBarH + 46 * scale);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(panel, 6 * scale, 6 * scale);

    painter.setFont(titleFont);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(x, y - 18 * scale, kBarW, 16 * scale),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     heatmapTitle_.isEmpty()
                         ? tr("Activity density")
                         : tr("%1 density").arg(heatmapTitle_));

    // The ramp, sampled the same way the marks are, including the
    // logarithmic transform, so the key matches what is on the map.
    for (int i = 0; i < kBarW; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kBarW - 1);
        const heatmap::Rgb c = heatmap::sample(t);
        painter.setPen(QColor::fromRgbF(c.r, c.g, c.b));
        painter.drawLine(x + i, y, x + i, y + kBarH);
    }

    painter.setFont(tickFont);
    painter.setPen(QColor(220, 220, 220));
    painter.drawText(QRectF(x, y + kBarH + 2 * scale, kBarW / 2, 14 * scale),
                     Qt::AlignLeft, tr("low"));
    painter.drawText(QRectF(x + kBarW / 2, y + kBarH + 2 * scale, kBarW / 2, 14 * scale),
                     Qt::AlignRight, tr("high"));

    // State what the top of the ramp means, and whether the scale is
    // shared. Without this a reader cannot tell whether two maps are
    // comparable, which is the easiest mistake to make here.
    const float anchor = heatmapRenderer_->scaleMax();
    const float ownPeak = heatmapRenderer_->anchorValue();
    QString scaleText;
    if (!heatmapRenderer_->usingSharedScale()) {
        scaleText = tr("to %1 /m (this map only)").arg(anchor, 0, 'g', 3);
    } else if (anchor > ownPeak * 1.001f) {
        // The shared anchor comes from a denser type, so this map does
        // not reach the top of the ramp. Show its own peak too, or the
        // reader cannot tell how far short it falls.
        scaleText = tr("to %1 /m (shared) - this map peaks at %2")
                        .arg(anchor, 0, 'g', 3).arg(ownPeak, 0, 'g', 3);
    } else {
        // This map is the densest one computed so far, so both scales
        // give the same colors. Say so, or switching the setting looks
        // like it did nothing.
        scaleText = tr("to %1 /m (shared - densest type so far)")
                        .arg(anchor, 0, 'g', 3);
    }
    painter.drawText(QRectF(x, y + kBarH + 16 * scale, kBarW * 2, 14 * scale),
                     Qt::AlignLeft, scaleText);

    painter.restore();
}

} // namespace simvis
