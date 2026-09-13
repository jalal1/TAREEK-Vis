#include "main_window.h"
#include "panel_style.h"
#include "progress_dialog.h"
#include "video_settings_dialog.h"
#include "counts_chart_dialog.h"
#include "core/config.h"
#include "core/logger.h"
#include "analysis/nkde_scatter.h"

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QTimer>
#include <map>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QSplitter>
#include <QKeyEvent>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QApplication>
#include <QActionGroup>
#include <QProgressDialog>
#include <QInputDialog>
#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace simvis {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("TAREEK-Vis - Simulation Visualizer");
    resize(1280, 800);
    setAcceptDrops(true);

    setupUi();
    setupMenus();
    setupStatusBar();

    // Initialize data managers
    networkIndex_ = std::make_unique<NetworkIndex>();
    vehicleIndex_ = std::make_unique<VehicleIndex>();

    // Initialize video recorder
    videoRecorder_ = std::make_unique<VideoRecorder>(this);
    mapWidget_->setVideoRecorder(videoRecorder_.get());

    // Connect video recorder signals
    connect(videoRecorder_.get(), &VideoRecorder::recordingStarted,
            this, &MainWindow::onRecordingStarted);
    connect(videoRecorder_.get(), &VideoRecorder::recordingStopped,
            this, &MainWindow::onRecordingStopped);
    connect(videoRecorder_.get(), &VideoRecorder::progressChanged,
            this, &MainWindow::onRecordingProgress);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Horizontal splitter: MapWidget | InfoPanel
    auto* splitter = new QSplitter(Qt::Horizontal, centralWidget);
    splitter->setChildrenCollapsible(false);

    // Map widget (main visualization area)
    mapWidget_ = new MapWidget(splitter);
    splitter->addWidget(mapWidget_);

    // Info panel (right side, initially hidden)
    infoPanel_ = new InfoPanel(splitter);
    infoPanel_->hide();
    splitter->addWidget(infoPanel_);

    // Map takes all available space, info panel has fixed width
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);

    layout->addWidget(splitter, 1);

    // Timeline widget (bottom)
    timelineWidget_ = new TimelineWidget(this);
    layout->addWidget(timelineWidget_);

    centralWidget->setLayout(layout);
    setCentralWidget(centralWidget);

    // Connect timeline signals
    connect(mapWidget_, &MapWidget::simulationTimeChanged,
            timelineWidget_, &TimelineWidget::setCurrentTime);

    connect(mapWidget_, &MapWidget::activeVehicleCountChanged,
            this, &MainWindow::onActiveVehicleCountChanged);

    connect(timelineWidget_, &TimelineWidget::timeChanged,
            mapWidget_, &MapWidget::setSimulationTime);

    connect(timelineWidget_, &TimelineWidget::playingChanged,
            mapWidget_, &MapWidget::setPlaying);

    connect(timelineWidget_, &TimelineWidget::playbackSpeedChanged,
            mapWidget_, &MapWidget::setPlaybackSpeed);

    // Connect transit click signals
    connect(mapWidget_, &MapWidget::transitStopClicked,
            this, &MainWindow::onTransitStopClicked);

    connect(mapWidget_, &MapWidget::transitRouteClicked,
            this, &MainWindow::onTransitRouteClicked);

    // Connect info panel signals
    connect(infoPanel_, &InfoPanel::stopClicked,
            this, &MainWindow::onInfoPanelStopClicked);

    connect(infoPanel_, &InfoPanel::routeClicked,
            this, &MainWindow::onInfoPanelRouteClicked);

    connect(infoPanel_, &InfoPanel::panToRequested,
            this, &MainWindow::onInfoPanelPanTo);

    connect(infoPanel_, &InfoPanel::closeRequested, this, [this]() {
        infoPanel_->hide();
        if (showInfoPanelAction_) showInfoPanelAction_->setChecked(false);
    });

    // Connect counts click signal
    connect(mapWidget_, &MapWidget::countLinkClicked,
            this, &MainWindow::onCountLinkClicked);

    // Network link click -> show link details in the info panel
    connect(mapWidget_, &MapWidget::networkLinkClicked,
            this, &MainWindow::onNetworkLinkClicked);

    // Show vehicle info in the panel when tracking starts/stops
    connect(mapWidget_, &MapWidget::vehicleTrackingChanged,
            this, &MainWindow::onVehicleTrackingChanged);

    // Person full-day route overlay toggle from the info panel
    connect(infoPanel_, &InfoPanel::showPersonRouteToggled,
            this, &MainWindow::onShowPersonRouteToggled);

    // Single-trip overlay when a trip card is clicked
    connect(infoPanel_, &InfoPanel::tripClicked,
            this, &MainWindow::onTripClicked);

    connect(&volumeWatcher_, &QFutureWatcher<std::unordered_map<uint32_t, std::vector<uint32_t>>>::finished, this, [this]() {
        linkHourlyVolumes_ = volumeWatcher_.result();
        LOG_INFO("Background volume aggregation completed.");
    });

}

void MainWindow::setupMenus() {
    // File menu
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* openFolderAction = fileMenu->addAction("&Open Folder...");
    openFolderAction->setShortcut(QKeySequence::Open);
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);

    auto* openAction = fileMenu->addAction("Open Files (&Advanced)...");
    connect(openAction, &QAction::triggered, this, &MainWindow::openFiles);

    recentFoldersMenu_ = fileMenu->addMenu("&Recent Folders");
    recentFoldersMenu_->setToolTipsVisible(true);  // show full path on hover
    updateRecentFoldersMenu();

    fileMenu->addSeparator();

    loadCountsAction_ = fileMenu->addAction("Load &Counts...");
    loadCountsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    loadCountsAction_->setEnabled(false);  // Enabled after network+events loaded
    connect(loadCountsAction_, &QAction::triggered, this, &MainWindow::onLoadCounts);

    fileMenu->addSeparator();

    auto* exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // View menu
    auto* viewMenu = menuBar()->addMenu("&View");

    // --- Network section ---
    showNodesAction_ = viewMenu->addAction("Show &Nodes");
    showNodesAction_->setCheckable(true);
    showNodesAction_->setChecked(false);  // Off by default: reduces clutter
    connect(showNodesAction_, &QAction::toggled, this, &MainWindow::onShowNodesToggled);

    showLinksAction_ = viewMenu->addAction("Show &Links");
    showLinksAction_->setCheckable(true);
    showLinksAction_->setChecked(true);
    connect(showLinksAction_, &QAction::toggled, this, &MainWindow::onShowLinksToggled);

    showCountsAction_ = viewMenu->addAction("Show Link &Counts");
    showCountsAction_->setCheckable(true);
    showCountsAction_->setChecked(false);
    showCountsAction_->setEnabled(false);  // Enabled after counts loaded
    connect(showCountsAction_, &QAction::toggled, this, &MainWindow::onShowCountsToggled);

    // --- Activity density heatmap ---
    // One entry per activity type, filled in once a scenario is loaded, plus a
    // command to clear the map.
    heatmapMenu_ = viewMenu->addMenu("Activity &Heatmap");
    heatmapMenu_->setEnabled(false);

    viewMenu->addSeparator();

    // --- Vehicles section ---
    // Show Vehicles submenu: master toggle + per-type visibility
    auto* showVehiclesMenu = viewMenu->addMenu("Show &Vehicles");

    showVehiclesAction_ = showVehiclesMenu->addAction("Show &All Vehicles");
    showVehiclesAction_->setCheckable(true);
    showVehiclesAction_->setChecked(true);
    connect(showVehiclesAction_, &QAction::toggled, this, &MainWindow::onShowVehiclesToggled);

    showVehiclesMenu->addSeparator();

    showCarsAction_ = showVehiclesMenu->addAction("Show &Private Vehicles");
    showCarsAction_->setCheckable(true);
    showCarsAction_->setChecked(true);
    connect(showCarsAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowCars(checked);
    });

    showBusVehiclesAction_ = showVehiclesMenu->addAction("Show &Buses");
    showBusVehiclesAction_->setCheckable(true);
    showBusVehiclesAction_->setChecked(true);
    connect(showBusVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowBuses(checked);
    });

    showTramVehiclesAction_ = showVehiclesMenu->addAction("Show &Trams");
    showTramVehiclesAction_->setCheckable(true);
    showTramVehiclesAction_->setChecked(true);
    connect(showTramVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowTrams(checked);
    });

    showRailVehiclesAction_ = showVehiclesMenu->addAction("Show &Rail");
    showRailVehiclesAction_->setCheckable(true);
    showRailVehiclesAction_->setChecked(true);
    connect(showRailVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowRailVehicles(checked);
    });

    // Vehicle Shape submenu
    auto* vehicleShapeMenu = viewMenu->addMenu("Vehicle &Shape");
    auto* shapeGroup = new QActionGroup(this);

    vehicleShapeCircleAction_ = vehicleShapeMenu->addAction("&Circle");
    vehicleShapeCircleAction_->setCheckable(true);
    vehicleShapeCircleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeCircleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeCircle);

    vehicleShapeTriangleAction_ = vehicleShapeMenu->addAction("&Triangle (Arrow)");
    vehicleShapeTriangleAction_->setCheckable(true);
    vehicleShapeTriangleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeTriangleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeTriangle);

    vehicleShapeRectangleAction_ = vehicleShapeMenu->addAction("&Rectangle");
    vehicleShapeRectangleAction_->setCheckable(true);
    vehicleShapeRectangleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeRectangleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeRectangle);

    vehicleShapeDiamondAction_ = vehicleShapeMenu->addAction("&Diamond");
    vehicleShapeDiamondAction_->setCheckable(true);
    vehicleShapeDiamondAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeDiamondAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeDiamond);

    vehicleShapeMenu->addSeparator();

    vehicleShapeAutoAction_ = vehicleShapeMenu->addAction("&Auto (by mode)");
    vehicleShapeAutoAction_->setCheckable(true);
    vehicleShapeAutoAction_->setChecked(true);  // Default
    vehicleShapeAutoAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeAutoAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeAuto);

    // Vehicle Size submenu
    auto* vehicleSizeMenu = viewMenu->addMenu("Vehicle Si&ze");
    auto* sizeGroup = new QActionGroup(this);

    vehicleSizeSmallAction_ = vehicleSizeMenu->addAction("&Small (3px)");
    vehicleSizeSmallAction_->setCheckable(true);
    vehicleSizeSmallAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeSmallAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeSmall);

    vehicleSizeMediumAction_ = vehicleSizeMenu->addAction("&Medium (5px)");
    vehicleSizeMediumAction_->setCheckable(true);
    vehicleSizeMediumAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeMediumAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeMedium);

    vehicleSizeLargeAction_ = vehicleSizeMenu->addAction("&Large (8px)");
    vehicleSizeLargeAction_->setCheckable(true);
    vehicleSizeLargeAction_->setChecked(true);  // Default
    vehicleSizeLargeAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeLargeAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeLarge);

    // Vehicle Color submenu
    auto* vehicleColorMenu = viewMenu->addMenu("Vehicle &Color");
    auto* colorGroup = new QActionGroup(this);

    vehicleColorSpeedAction_ = vehicleColorMenu->addAction("By &Speed");
    vehicleColorSpeedAction_->setCheckable(true);
    vehicleColorSpeedAction_->setChecked(true);  // Default
    vehicleColorSpeedAction_->setActionGroup(colorGroup);
    connect(vehicleColorSpeedAction_, &QAction::triggered, this, &MainWindow::onVehicleColorSpeed);

    vehicleColorModeAction_ = vehicleColorMenu->addAction("By &Mode (Icons)");
    vehicleColorModeAction_->setCheckable(true);
    vehicleColorModeAction_->setActionGroup(colorGroup);
    connect(vehicleColorModeAction_, &QAction::triggered, this, &MainWindow::onVehicleColorMode);

    viewMenu->addSeparator();

    // --- Transit section ---
    auto* transitMenu = viewMenu->addMenu("&Transit");

    // All transit layers start hidden; users enable what they need
    showBusRoutesAction_ = transitMenu->addAction("Show &Bus Routes");
    showBusRoutesAction_->setCheckable(true);
    showBusRoutesAction_->setChecked(false);
    connect(showBusRoutesAction_, &QAction::toggled, this, &MainWindow::onShowBusRoutesToggled);

    showTramRoutesAction_ = transitMenu->addAction("Show T&ram Routes");
    showTramRoutesAction_->setCheckable(true);
    showTramRoutesAction_->setChecked(false);
    connect(showTramRoutesAction_, &QAction::toggled, this, &MainWindow::onShowTramRoutesToggled);

    showRailRoutesAction_ = transitMenu->addAction("Show Ra&il Routes");
    showRailRoutesAction_->setCheckable(true);
    showRailRoutesAction_->setChecked(false);
    connect(showRailRoutesAction_, &QAction::toggled, this, &MainWindow::onShowRailRoutesToggled);

    showBusStopsAction_ = transitMenu->addAction("Show Bus &Stops");
    showBusStopsAction_->setCheckable(true);
    showBusStopsAction_->setChecked(false);
    connect(showBusStopsAction_, &QAction::toggled, this, &MainWindow::onShowBusStopsToggled);

    transitMenu->addSeparator();

    // Route Thickness submenu
    auto* thicknessMenu = transitMenu->addMenu("Route &Thickness");
    auto* thicknessGroup = new QActionGroup(this);

    routeThicknessThinAction_ = thicknessMenu->addAction("T&hin (1px)");
    routeThicknessThinAction_->setCheckable(true);
    routeThicknessThinAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessThinAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessThin);

    routeThicknessMediumAction_ = thicknessMenu->addAction("&Medium (2px)");
    routeThicknessMediumAction_->setCheckable(true);
    routeThicknessMediumAction_->setChecked(true);  // Default
    routeThicknessMediumAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessMediumAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessMedium);

    routeThicknessThickAction_ = thicknessMenu->addAction("Thic&k (4px)");
    routeThicknessThickAction_->setCheckable(true);
    routeThicknessThickAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessThickAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessThick);

    viewMenu->addSeparator();

    // --- Info Panel toggle ---
    showInfoPanelAction_ = viewMenu->addAction("Show &Info Panel");
    showInfoPanelAction_->setCheckable(true);
    showInfoPanelAction_->setChecked(false);
    connect(showInfoPanelAction_, &QAction::toggled, this, [this](bool checked) {
        infoPanel_->setVisible(checked);
        if (!checked) infoPanel_->clear();
    });

    viewMenu->addSeparator();

    // --- Background Map section ---
    auto* bgMapMenu = viewMenu->addMenu("&Background Map");

    showBackgroundMapAction_ = bgMapMenu->addAction("&Enable Background Map");
    showBackgroundMapAction_->setCheckable(true);
    showBackgroundMapAction_->setChecked(false);
    connect(showBackgroundMapAction_, &QAction::toggled, this, [this](bool checked) {
        if (checked && !networkCrs_.isValid()) {
            QMessageBox::warning(this, "Background Map",
                "CRS not detected from network file.\n"
                "Background map requires a known coordinate reference system.");
            showBackgroundMapAction_->setChecked(false);
            return;
        }
        mapWidget_->setShowBackgroundMap(checked);
    });

    bgMapMenu->addSeparator();

    auto* tileSourceGroup = new QActionGroup(this);

    tileSourceOsmAction_ = bgMapMenu->addAction("&OpenStreetMap");
    tileSourceOsmAction_->setCheckable(true);
    tileSourceOsmAction_->setChecked(true);
    tileSourceOsmAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceOsmAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::OpenStreetMap);
    });

    tileSourceSatAction_ = bgMapMenu->addAction("&Satellite (ESRI)");
    tileSourceSatAction_->setCheckable(true);
    tileSourceSatAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceSatAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::Satellite);
    });

    tileSourceTopoAction_ = bgMapMenu->addAction("&Topo Map");
    tileSourceTopoAction_->setCheckable(true);
    tileSourceTopoAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceTopoAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::Topo);
    });

    viewMenu->addSeparator();

    // --- Navigation section ---
    auto* fitAction = viewMenu->addAction("&Fit to Network");
    fitAction->setShortcut(Qt::Key_F);
    connect(fitAction, &QAction::triggered, this, &MainWindow::onFitToNetwork);

    auto* zoomInAction = viewMenu->addAction("Zoom &In");
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, mapWidget_, &MapWidget::zoomIn);

    auto* zoomOutAction = viewMenu->addAction("Zoom &Out");
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, mapWidget_, &MapWidget::zoomOut);

    // Export menu
    auto* exportMenu = menuBar()->addMenu("&Export");

    auto* exportPdfAction = exportMenu->addAction("Export to &PDF...");
    exportPdfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(exportPdfAction, &QAction::triggered, this, &MainWindow::onExportPdf);

    auto* exportPngAction = exportMenu->addAction("Export to P&NG...");
    exportPngAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(exportPngAction, &QAction::triggered, this, &MainWindow::onExportPng);

    // Tools menu
    auto* toolsMenu = menuBar()->addMenu("&Tools");

    startRecordingAction_ = toolsMenu->addAction("&Record Video...");
    startRecordingAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    startRecordingAction_->setIcon(QIcon::fromTheme("media-record"));
    connect(startRecordingAction_, &QAction::triggered, this, &MainWindow::onStartRecording);

    stopRecordingAction_ = toolsMenu->addAction("&Stop Recording");
    stopRecordingAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    stopRecordingAction_->setIcon(QIcon::fromTheme("media-playback-stop"));
    stopRecordingAction_->setEnabled(false);  // Disabled until recording starts
    connect(stopRecordingAction_, &QAction::triggered, this, &MainWindow::onStopRecording);

    // Help menu
    auto* helpMenu = menuBar()->addMenu("&Help");

    auto* aboutAction = helpMenu->addAction("&About");
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About TAREEK-Vis",
            "TAREEK-Vis - Simulation Visualizer\n"
            "Version 1.0.0\n\n"
            "A high-performance visualization tool for MATSim simulation outputs.\n\n"
            "Copyright (c) 2026 TAREEK-Vis Contributors\n"
            "Licensed under the GNU General Public License v3.0.\n\n"
            "Third-Party Libraries:\n"
            "  - Qt6 (LGPL v3.0) - https://www.qt.io\n"
            "  - PugiXML 1.15 (MIT) - https://pugixml.org\n"
            "  - ZLIB (Zlib License) - https://www.zlib.net\n"
            "  - FFmpeg (LGPL/GPL, optional) - https://ffmpeg.org\n\n"
            "See THIRD_PARTY_LICENSES.md for full license details.");
    });
}

void MainWindow::setupStatusBar() {
    statusLabel_ = new QLabel("Ready");
    statusBar()->addWidget(statusLabel_, 1);

    // Heatmap activity indicator: a spinner while a map is computing, then a
    // check mark, beside a message in its own color. It sits left of the
    // permanent counters and hides itself when there is nothing to report.
    heatmapSpinnerLabel_ = new QLabel();
    heatmapSpinnerLabel_->setFixedWidth(16);
    heatmapSpinnerLabel_->setAlignment(Qt::AlignCenter);
    heatmapSpinnerLabel_->hide();
    statusBar()->addPermanentWidget(heatmapSpinnerLabel_);

    heatmapStatusLabel_ = new QLabel();
    heatmapStatusLabel_->hide();
    statusBar()->addPermanentWidget(heatmapStatusLabel_);

    // Drives the spinner. Only runs while something is computing.
    heatmapSpinnerTimer_ = new QTimer(this);
    heatmapSpinnerTimer_->setInterval(120);
    connect(heatmapSpinnerTimer_, &QTimer::timeout, this, [this]() {
        // Braille dots read as a smooth rotation at a small size and need no
        // image resources.
        static const char* kFrames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
        heatmapSpinnerFrame_ = (heatmapSpinnerFrame_ + 1) % 10;
        heatmapSpinnerLabel_->setText(
            QString::fromUtf8(kFrames[heatmapSpinnerFrame_]));
    });

    vehicleCountLabel_ = new QLabel("Vehicles: 0");
    statusBar()->addPermanentWidget(vehicleCountLabel_);

    zoomLabel_ = new QLabel("Zoom: 100%");
    statusBar()->addPermanentWidget(zoomLabel_);
}

void MainWindow::setHeatmapStatus(const QString& message, HeatmapStatus state) {
    if (!heatmapStatusLabel_) return;

    if (state == HeatmapStatus::Idle || message.isEmpty()) {
        heatmapSpinnerTimer_->stop();
        heatmapSpinnerLabel_->hide();
        heatmapStatusLabel_->hide();
        return;
    }

    heatmapStatusLabel_->setText(message);
    heatmapStatusLabel_->show();

    switch (state) {
        case HeatmapStatus::Working:
            // Amber: work in progress, worth noticing but not a problem.
            heatmapStatusLabel_->setStyleSheet(
                "color: #d98324; font-weight: 600;");
            heatmapSpinnerLabel_->setStyleSheet("color: #d98324;");
            heatmapSpinnerLabel_->show();
            if (!heatmapSpinnerTimer_->isActive()) heatmapSpinnerTimer_->start();
            break;

        case HeatmapStatus::Done:
            heatmapSpinnerTimer_->stop();
            heatmapStatusLabel_->setStyleSheet(
                "color: #3f9142; font-weight: 600;");
            heatmapSpinnerLabel_->setStyleSheet("color: #3f9142;");
            heatmapSpinnerLabel_->setText(QString::fromUtf8("✓"));
            heatmapSpinnerLabel_->show();
            // Clear after a moment: a finished message that stays forever
            // becomes noise.
            QTimer::singleShot(4000, this, [this, message]() {
                if (heatmapStatusLabel_ && heatmapStatusLabel_->text() == message)
                    setHeatmapStatus(QString(), HeatmapStatus::Idle);
            });
            break;

        case HeatmapStatus::Failed:
            heatmapSpinnerTimer_->stop();
            heatmapStatusLabel_->setStyleSheet(
                "color: #c8442b; font-weight: 600;");
            heatmapSpinnerLabel_->setStyleSheet("color: #c8442b;");
            heatmapSpinnerLabel_->setText(QString::fromUtf8("✕"));
            heatmapSpinnerLabel_->show();
            break;

        case HeatmapStatus::Idle:
            break;
    }
}

void MainWindow::openFolder() {
    QString dirPath = QFileDialog::getExistingDirectory(
        this,
        "Open Simulation Output Folder",
        QString()
    );
    if (dirPath.isEmpty()) return;
    loadFolder(dirPath);
}

void MainWindow::loadFolder(const QString& dirPath) {
    QDir dir(dirPath);
    if (!dir.exists()) {
        QMessageBox::warning(this, "Folder Not Found",
            QString("The folder no longer exists:\n%1").arg(dirPath));
        // Drop it from recents if it was there
        QSettings settings;
        QStringList recent = settings.value("recentFolders").toStringList();
        if (recent.removeAll(dirPath) > 0) {
            settings.setValue("recentFolders", recent);
            updateRecentFoldersMenu();
        }
        return;
    }

    QString networkPath = resolveInputFile(dir, "network");
    QString eventsPath  = resolveInputFile(dir, "events");
    if (networkPath.isEmpty() || eventsPath.isEmpty()) {
        QStringList missing;
        if (networkPath.isEmpty()) missing << "network (*network*.xml[.gz])";
        if (eventsPath.isEmpty())  missing << "events (*events*.xml[.gz])";
        QMessageBox::warning(this, "Required Files Not Found",
            QString("Could not find required file(s) in:\n%1\n\nMissing: %2\n\n"
                    "Use File > Open Files (Advanced)... to pick files manually.")
                .arg(dirPath, missing.join(", ")));
        return;
    }

    // Transit schedule is optional: load silently if present, skip if not
    QString transitPath = resolveInputFile(dir, "transitSchedule");

    loadFromPaths(networkPath, eventsPath, transitPath);
}

QString MainWindow::resolveInputFile(const QDir& dir, const QString& role) {
    QStringList patterns;
    patterns << QString("*%1*.xml").arg(role) << QString("*%1*.xml.gz").arg(role);
    QStringList names = dir.entryList(patterns, QDir::Files, QDir::Name);

    // Exclude networkChangeEvents files: they contain both "network" and
    // "events" so they would otherwise shadow both roles.
    names.erase(std::remove_if(names.begin(), names.end(), [](const QString& n) {
        return n.contains("changeEvents", Qt::CaseInsensitive) ||
               n.contains("change_events", Qt::CaseInsensitive);
    }), names.end());

    if (names.isEmpty()) return QString();
    if (names.size() == 1) return dir.absoluteFilePath(names.first());

    // Multiple candidates: prefer the standard MATSim "output_" prefix
    QStringList outputNames;
    for (const QString& n : names) {
        if (n.startsWith("output_", Qt::CaseInsensitive)) outputNames << n;
    }
    if (outputNames.size() == 1) return dir.absoluteFilePath(outputNames.first());
    const QStringList& choices = outputNames.isEmpty() ? names : outputNames;

    // Still ambiguous: ask the user
    bool ok = false;
    QString picked = QInputDialog::getItem(
        this,
        QString("Select %1 File").arg(role),
        QString("Multiple %1 files found in this folder.\nSelect one:").arg(role),
        choices, 0, false, &ok);
    if (!ok || picked.isEmpty()) return QString();
    return dir.absoluteFilePath(picked);
}

void MainWindow::addRecentFolder(const QString& dirPath) {
    // Normalize so the same folder reached via different paths dedupes
    const QString canonical = QDir(dirPath).canonicalPath();
    const QString path = canonical.isEmpty() ? QDir::cleanPath(dirPath) : canonical;

    QSettings settings;
    QStringList recent = settings.value("recentFolders").toStringList();
    // Case-insensitive dedupe (Windows paths)
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const QString& p) {
        return QString::compare(p, path, Qt::CaseInsensitive) == 0;
    }), recent.end());
    recent.prepend(path);
    while (recent.size() > 5) recent.removeLast();
    settings.setValue("recentFolders", recent);
    updateRecentFoldersMenu();
}

void MainWindow::updateRecentFoldersMenu() {
    recentFoldersMenu_->clear();

    QSettings settings;
    const QStringList recent = settings.value("recentFolders").toStringList();

    if (recent.isEmpty()) {
        auto* empty = recentFoldersMenu_->addAction("(No Recent Folders)");
        empty->setEnabled(false);
        return;
    }

    for (const QString& dirPath : recent) {
        // Show last 2 path segments for readability, full path as tooltip
        QDir dir(dirPath);
        QString dirName = dir.dirName();
        QDir parent(dirPath);
        parent.cdUp();
        QString label = parent.dirName() + "/" + dirName;

        auto* action = recentFoldersMenu_->addAction(label);
        action->setToolTip(dirPath);
        connect(action, &QAction::triggered, this, [this, dirPath]() {
            loadFolder(dirPath);
        });
    }

    recentFoldersMenu_->addSeparator();
    auto* clearAction = recentFoldersMenu_->addAction("Clear Recent Folders");
    connect(clearAction, &QAction::triggered, this, [this]() {
        QSettings settings;
        settings.remove("recentFolders");
        updateRecentFoldersMenu();
    });
}

void MainWindow::openFiles() {
    // Manual per-file selection for non-standard layouts (e.g. network and
    // events in different folders).
    QString networkPath = QFileDialog::getOpenFileName(
        this,
        "Open Network File",
        QString(),
        "Network Files (*.xml *.xml.gz);;All Files (*)"
    );

    if (networkPath.isEmpty()) return;

    QString eventsPath = QFileDialog::getOpenFileName(
        this,
        "Open Events File",
        QFileInfo(networkPath).absolutePath(),
        "Events Files (*.xml *.xml.gz);;All Files (*)"
    );

    if (eventsPath.isEmpty()) return;

    // Transit schedule: auto-detect next to the network file (no dialog)
    QString transitPath = resolveInputFile(QFileInfo(networkPath).absoluteDir(),
                                           "transitSchedule");

    loadFromPaths(networkPath, eventsPath, transitPath);
}

void MainWindow::resetScenarioState() {
    // Stop background density work first. A map computed for the old network
    // must never be shown over the new one, and its result must not be written
    // into the new scenario's cache.
    heatmapPrecomputeQueue_.clear();
    heatmapPrecomputeActive_ = false;
    heatmapPendingUserType_.clear();
    heatmapLongRunAccepted_ = false;
    heatmapAutoPrecompute_ = true;
    // Densities are not comparable between scenarios, so the shared scale
    // starts again from nothing. populateHeatmapMenu seeds it from the new
    // scenario's cached maps.
    heatmapSharedPeak_ = 0.0f;
    heatmapActivityTypes_.clear();
    if (heatmapRunning_) {
        // A run in progress is asked to stop, but may still be finishing, so
        // mark this generation
        // stale instead: the finish handler compares and drops the result.
        NkdeScatter::cancelAll();
    }
    // A new generation invalidates every result still in flight.
    ++heatmapGeneration_;
    heatmapCache_.reset();
    nkdvNetwork_.reset();
    heatmapSourceTime_ = QDateTime();
    setHeatmapStatus(QString(), HeatmapStatus::Idle);

    // Clear the map overlays that belong to the old network.
    if (mapWidget_) {
        mapWidget_->clearHeatmap();
        mapWidget_->clearAllHighlights();
    }
    if (heatmapMenu_) {
        heatmapMenu_->clear();
        heatmapMenu_->setEnabled(false);
    }

    // Drop the selection: person and vehicle ids mean nothing in a new scenario.
    routePersonPlus1_ = 0;
    if (infoPanel_) infoPanel_->resetPersonRouteState();
}

void MainWindow::loadFromPaths(const QString& networkPath, const QString& eventsPath,
                               const QString& transitPath) {
    // Every scenario change passes through here, so this is where the previous
    // scenario's state has to go. Without it a density map, a route overlay or
    // a selection from the old network stays on screen over the new one.
    resetScenarioState();

    networkFilePath_ = networkPath;
    eventsFilePath_ = eventsPath;
    transitSchedulePath_ = transitPath;

    // Update window title with experiment path (last 2 directory segments if long)
    QFileInfo networkInfo(networkPath);
    QString expDir = networkInfo.absolutePath();
    {
        QDir dir(expDir);
        QString dirName = dir.dirName();
        dir.cdUp();
        QString parentName = dir.dirName();
        QString shortPath = parentName + "/" + dirName;
        setWindowTitle(QString("TAREEK-Vis - Simulation Visualizer  [%1]").arg(shortPath));
    }

    // Remember this folder for File > Recent Folders
    addRecentFolder(networkInfo.absolutePath());

    // Set up cache directory
    cacheDirectory_ = networkInfo.absolutePath() + "/.simvis_cache";
    QDir().mkpath(cacheDirectory_);

    // Redirect log to cache directory
    Logger::redirectLogFile(cacheDirectory_);
    LOG_INFO(QString("Files selected - network: %1").arg(networkPath));
    LOG_INFO(QString("  events: %1").arg(eventsPath));
    if (!transitPath.isEmpty())
        LOG_INFO(QString("  transit: %1").arg(transitPath));

    // Configure preprocessor
    auto& config = Config::instance();
    config.setInputFiles(networkPath.toStdString(), eventsPath.toStdString());

    // Check if binary cache files already exist and are fresh
    QString networkBaseName = networkInfo.completeBaseName();
    if (networkBaseName.endsWith(".xml")) {
        networkBaseName = networkBaseName.left(networkBaseName.length() - 4);
    }
    QString eventsBaseName = QFileInfo(eventsPath).completeBaseName();
    if (eventsBaseName.endsWith(".xml")) {
        eventsBaseName = eventsBaseName.left(eventsBaseName.length() - 4);
    }

    QString networkBin = cacheDirectory_ + "/" + networkBaseName + ".bin";
    QString vidxPath = cacheDirectory_ + "/" + eventsBaseName + ".vidx";
    QString crsPath = cacheDirectory_ + "/network.crs";

    bool cacheValid = false;
    if (QFileInfo::exists(networkBin) && QFileInfo::exists(vidxPath)) {
        QDateTime binTime = QFileInfo(networkBin).lastModified();
        QDateTime vidxTime = QFileInfo(vidxPath).lastModified();
        QDateTime netSrcTime = QFileInfo(networkPath).lastModified();
        QDateTime evtSrcTime = QFileInfo(eventsPath).lastModified();
        cacheValid = binTime > netSrcTime && vidxTime > evtSrcTime;

        // Old-format caches must be regenerated (loadFile accepts only the
        // current version and would otherwise show a hard error)
        uint32_t vidxVersion = VehicleIndex::fileVersion(vidxPath);
        if (cacheValid && vidxVersion != VIDX_FILE_VERSION) {
            cacheValid = false;
            LOG_INFO(QString("Cache .vidx version %1 != current %2 - will re-preprocess")
                .arg(vidxVersion).arg(VIDX_FILE_VERSION));
        }

        LOG_INFO(QString("Cache check: bin=%1 vidx=%2 valid=%3")
            .arg(QFileInfo::exists(networkBin) ? "yes" : "no")
            .arg(QFileInfo::exists(vidxPath) ? "yes" : "no")
            .arg(cacheValid ? "yes" : "no"));
    } else {
        LOG_INFO("Cache files missing - will preprocess");
    }

    if (cacheValid) {
        // Load cached .bin and .vidx
        loadBinaryFiles();
        // Restore CRS from sidecar file
        loadCachedCRS(crsPath);
        // Parse transit schedule separately if requested (not cached)
        if (!transitSchedulePath_.isEmpty()) {
            parseTransitOnly();
        }
    } else {
        // Need to preprocess (network/events will be regenerated, transit parsed)
        startPreprocessing();
    }
}

void MainWindow::startPreprocessing() {
    preprocessor_ = std::make_unique<Preprocessor>();
    preprocessor_->setNetworkFile(networkFilePath_);
    preprocessor_->setEventsFile(eventsFilePath_);
    preprocessor_->setTransitScheduleFile(transitSchedulePath_);
    preprocessor_->setOutputDirectory(cacheDirectory_);

    // Create progress dialog
    auto* progressDialog = new ProgressDialog(this);
    progressDialog->setTitle("Preprocessing simulation files...");

    connect(preprocessor_.get(), &Preprocessor::progressChanged,
            progressDialog, &ProgressDialog::setProgress);

    connect(preprocessor_.get(), &Preprocessor::allCompleted,
            this, [this, progressDialog](bool success) {
        if (success) {
            LOG_INFO("Preprocessing completed successfully");
            // Extract CRS from parsed network and save to sidecar file
            QString crsStr = preprocessor_->networkCRS();
            if (!crsStr.isEmpty()) {
                networkCrs_ = parseCRS(crsStr.toStdString());
                if (networkCrs_.isValid()) {
                    mapWidget_->setCRS(networkCrs_);
                }
                // Save CRS to cache for future loads
                QFile crsFile(cacheDirectory_ + "/network.crs");
                if (crsFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    crsFile.write(crsStr.toUtf8());
                    crsFile.close();
                }
            }
            // Take transit data from preprocessor before it's destroyed
            if (preprocessor_->transitResult()) {
                transitData_ = preprocessor_->takeTransitResult();
            }
            progressDialog->setCompleted(true, "Files processed successfully!");
        } else {
            LOG_ERROR("Preprocessing failed");
            progressDialog->setCompleted(false, "Processing failed.");
        }
    });

    connect(progressDialog, &ProgressDialog::cancelRequested,
            preprocessor_.get(), &Preprocessor::cancel);

    connect(progressDialog, &QDialog::accepted,
            this, [this]() {
        loadBinaryFiles();
        applyTransitData();
    });

    // Run preprocessing in a separate thread
    auto* thread = new QThread();
    preprocessor_->moveToThread(thread);

    connect(thread, &QThread::started, preprocessor_.get(), &Preprocessor::processAll);
    connect(preprocessor_.get(), &Preprocessor::allCompleted, thread, &QThread::quit);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    thread->start();
    progressDialog->exec();
}

void MainWindow::loadBinaryFiles() {
    statusLabel_->setText("Loading network...");

    QString networkBaseName = QFileInfo(networkFilePath_).completeBaseName();
    if (networkBaseName.endsWith(".xml")) {
        networkBaseName = networkBaseName.left(networkBaseName.length() - 4);
    }
    QString eventsBaseName = QFileInfo(eventsFilePath_).completeBaseName();
    if (eventsBaseName.endsWith(".xml")) {
        eventsBaseName = eventsBaseName.left(eventsBaseName.length() - 4);
    }

    QString networkBin = cacheDirectory_ + "/" + networkBaseName + ".bin";
    QString vidxPath = cacheDirectory_ + "/" + eventsBaseName + ".vidx";

    // Load network
    LOG_INFO(QString("Loading network binary: %1").arg(networkBin));
    if (!networkIndex_->loadFile(networkBin)) {
        LOG_ERROR(QString("Failed to load network binary: %1").arg(networkBin));
        QMessageBox::critical(this, "Error", "Failed to load network file.");
        return;
    }

    statusLabel_->setText("Loading vehicle index...");

    // Load vehicle index
    LOG_INFO(QString("Loading vehicle index: %1").arg(vidxPath));
    if (!vehicleIndex_->loadFile(vidxPath)) {
        LOG_ERROR(QString("Failed to load vehicle index: %1").arg(vidxPath));
        QMessageBox::critical(this, "Error", "Failed to load vehicle index file.");
        return;
    }

    // Connect to map widget
    mapWidget_->setNetworkIndex(networkIndex_.get());
    mapWidget_->setVehicleIndex(vehicleIndex_.get());

    // Set up timeline
    float minTime = VehicleIndex::toSeconds(vehicleIndex_->minTime());
    float maxTime = VehicleIndex::toSeconds(vehicleIndex_->maxTime());
    timelineWidget_->setTimeRange(minTime, maxTime);
    timelineWidget_->setCurrentTime(minTime);

    // Update status
    LOG_INFO(QString("Loaded: %1 nodes, %2 links, %3 vehicles, time %4s-%5s")
        .arg(networkIndex_->nodeCount())
        .arg(networkIndex_->linkCount())
        .arg(vehicleIndex_->vehicleCount())
        .arg(minTime, 0, 'f', 1)
        .arg(maxTime, 0, 'f', 1));
    statusLabel_->setText(QString("Loaded: %1 nodes, %2 links, %3 vehicles")
        .arg(networkIndex_->nodeCount())
        .arg(networkIndex_->linkCount())
        .arg(vehicleIndex_->vehicleCount()));

    // Enable counts loading now that network+events are available
    loadCountsAction_->setEnabled(true);

    // Offer a heatmap for each activity type this scenario contains. Anything
    // cached before the events index was written describes older data, so that
    // file's timestamp is the freshness mark for every cached map.
    heatmapSourceTime_ = QFileInfo(vidxPath).lastModified();
    populateHeatmapMenu();

    // Compute the maps that are not cached yet, in the background, so the first
    // click on a type is instant. Deferred so the window is shown and painted
    // first, and it runs one map at a time on a worker thread.
    QTimer::singleShot(0, this, [this]() { startHeatmapPrecompute(); });

    // Fit view to network
    mapWidget_->fitToNetwork();

    VehicleIndex* vIdx = vehicleIndex_.get();
    QFuture<std::unordered_map<uint32_t, std::vector<uint32_t>>> future = QtConcurrent::run([vIdx]() {
        std::unordered_map<uint32_t, std::vector<uint32_t>> volumes;
        if (!vIdx) return volumes;
        
        size_t count = vIdx->vehicleCount();
        for (size_t i = 0; i < count; ++i) {
            const auto* traj = vIdx->trajectory(static_cast<uint32_t>(i));
            if (!traj) continue;
            
            for (const auto& seg : traj->segments) {
                const float seconds = VehicleIndex::toSeconds(seg.enterTime);
                if (seconds < 0.0f) continue;

                // MATSim counts seconds from midnight and routinely runs past
                // 24:00 - this scenario ends at 107999s, hour 29 - so fold back
                // to hour of day. Truncating at 23 silently dropped every trip
                // after midnight instead of counting it against the right hour.
                const int hour = static_cast<int>(seconds / 3600.0f) % 24;

                if (volumes[seg.linkId].empty()) volumes[seg.linkId].assign(24, 0);
                volumes[seg.linkId][hour]++;
            }
        }
        return volumes;
    });
    volumeWatcher_.setFuture(future);
}

namespace {

// The heatmap settings. These are fixed for now; when they become adjustable
// they must also go into the cache key, which already carries them.
constexpr int kHeatmapLixelLength = 25;
constexpr double kHeatmapBandwidth = 500.0;

} // namespace

void MainWindow::populateHeatmapMenu() {
    if (!heatmapMenu_) return;
    heatmapMenu_->clear();
    heatmapMenu_->setEnabled(false);
    nkdvNetwork_.reset();

    // A new scenario means new sizes and new answers, so forget what the last
    // one decided.
    heatmapPrecomputeQueue_.clear();
    heatmapPrecomputeActive_ = false;
    heatmapPendingUserType_.clear();
    heatmapLongRunAccepted_ = false;
    heatmapAutoPrecompute_ = true;
    setHeatmapStatus(QString(), HeatmapStatus::Idle);

    if (!vehicleIndex_ || !networkIndex_) return;

    // A scenario switch cancels work in flight. Clear that now the new scenario
    // is ready, or every later computation would refuse to start.
    NkdeScatter::resetCancel();

    // Cache computed maps beside the other cache files. A map is worth about
    // 1.7 MB and a few seconds, and it never changes for a given scenario.
    heatmapCache_ = std::make_unique<HeatmapCache>(cacheDirectory_);

    // Count the activities per type. Types are scenario-defined, so read them
    // from the data instead of assuming a fixed list.
    std::map<QString, size_t> counts;
    for (const auto& act : vehicleIndex_->activities()) {
        QString type = vehicleIndex_->actTypeString(act.actTypeId);
        // A transit transfer is an artifact of routing, not a real activity.
        if (type.isEmpty() || type == "pt interaction") continue;
        ++counts[type];
    }

    if (counts.empty()) {
        QAction* none = heatmapMenu_->addAction(tr("No activities in this scenario"));
        none->setEnabled(false);
        heatmapMenu_->setEnabled(true);
        return;
    }

    QStringList allTypes;
    for (const auto& [type, count] : counts) {
        allTypes << type;
        QAction* action = heatmapMenu_->addAction(
            tr("%1 (%2 activities)").arg(type).arg(count));
        connect(action, &QAction::triggered, this,
                [this, type]() { onHeatmapTypeSelected(type); });
    }
    heatmapActivityTypes_ = allTypes;

    // Anchor the shared scale on every map already computed for this scenario,
    // not only the ones opened in this session. Without this the same map is
    // colored differently depending on which types the user happened to click
    // first, because the anchor only ever grew as maps were opened.
    heatmapSharedPeak_ = heatmapCache_->peakAcrossAll(
        allTypes, kHeatmapLixelLength, kHeatmapBandwidth, heatmapSourceTime_);
    if (heatmapSharedPeak_ > 0.0f) {
        LOG_INFO(QString("Heatmap: shared scale starts at %1 /m from cached maps")
            .arg(heatmapSharedPeak_));
    }

    heatmapMenu_->addSeparator();

    // A map is stored as activities per metre either way. This setting only
    // decides where the top of the color ramp sits, so make the choice visible
    // instead of leaving the reader to guess which one they are looking at.
    heatmapSharedScaleAction_ = heatmapMenu_->addAction(
        tr("Compare types on one &scale"));
    heatmapSharedScaleAction_->setCheckable(true);
    heatmapSharedScaleAction_->setChecked(heatmapSharedScale_);
    heatmapSharedScaleAction_->setToolTip(
        tr("Anchor every map's colors to the same density, so a bright road "
           "means the same on all of them. Off, each map uses its own peak, "
           "which shows one map's shape but cannot be compared with another.\n\n"
           "The two settings look the same until a second type is computed, "
           "and always look the same on the densest type, because that map "
           "sets the shared value."));
    connect(heatmapSharedScaleAction_, &QAction::toggled,
            this, &MainWindow::onHeatmapSharedScaleToggled);

    QAction* clearAction = heatmapMenu_->addAction(tr("&Clear heatmap"));
    connect(clearAction, &QAction::triggered, this, &MainWindow::onClearHeatmap);

    heatmapMenu_->setEnabled(true);
}

void MainWindow::onClearHeatmap() {
    mapWidget_->clearHeatmap();
    setHeatmapStatus(QString(), HeatmapStatus::Idle);
    statusLabel_->setText(tr("Heatmap cleared"));
}

void MainWindow::onHeatmapSharedScaleToggled(bool shared) {
    heatmapSharedScale_ = shared;
    applyHeatmapScale();
}

void MainWindow::applyHeatmapScale() {
    if (!mapWidget_) return;

    if (!heatmapSharedScale_) {
        mapWidget_->setHeatmapScaleMax(0.0f);  // each map uses its own peak
        return;
    }

    // The shared anchor is the highest density seen across the maps computed so
    // far. It grows as more types are computed, so a map shown earlier may need
    // rescaling; that is why this is applied whenever a map is shown.
    mapWidget_->setHeatmapScaleMax(heatmapSharedPeak_);
}

void MainWindow::noteHeatmapPeak(float peak) {
    if (peak > heatmapSharedPeak_) heatmapSharedPeak_ = peak;
}

void MainWindow::onHeatmapTypeSelected(const QString& activityType) {
    if (!vehicleIndex_ || !networkIndex_ || !heatmapCache_) return;

    // A cached map is the common case once the background pass has run, and it
    // loads in milliseconds, so try it before starting any work.
    HeatmapCache::Key key{activityType, kHeatmapLixelLength, kHeatmapBandwidth};
    std::vector<NkdvNetwork::Lixel> cached;
    if (heatmapCache_->load(key, heatmapSourceTime_, cached)) {
        mapWidget_->setHeatmap(cached, kHeatmapLixelLength, activityType);
        noteHeatmapPeak(mapWidget_->heatmapPeak());
        applyHeatmapScale();
        mapWidget_->setHeatmapVisible(true);
        setHeatmapStatus(tr("%1 density: %2 lixels").arg(activityType).arg(cached.size()),
                         HeatmapStatus::Done);
        return;
    }

    // On a large network a single map takes many minutes, so say so and let the
    // user decide instead of appearing to hang. Ask once per scenario.
    if (!heatmapAutoPrecompute_ && !heatmapLongRunAccepted_) {
        const auto answer = QMessageBox::question(this, tr("Activity Heatmap"),
            tr("This network has %1 links, so computing a density map will take "
               "several minutes.\n\nThe window stays usable while it runs, and "
               "the result is saved so the map opens instantly next time.\n\n"
               "Compute the %2 map now?")
                .arg(networkIndex_->linkCount()).arg(activityType),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes) return;
        heatmapLongRunAccepted_ = true;
    }

    // A request from the user outranks anything running in the background.
    if (heatmapRunning_) {
        // A computation already running may take a moment to notice, so the
        // current one has to finish. What we can do is make sure this type is
        // next, and that its result is shown rather than only stored.
        heatmapPrecomputeQueue_.removeAll(activityType);
        heatmapPrecomputeQueue_.prepend(activityType);
        heatmapPendingUserType_ = activityType;
        heatmapPrecomputeActive_ = true;   // keep the queue draining
        setHeatmapStatus(tr("Waiting for the current map, then %1").arg(activityType),
                         HeatmapStatus::Working);
        return;
    }

    runHeatmapAsync(activityType, /*showWhenDone=*/true);
}

void MainWindow::runHeatmapAsync(const QString& activityType, bool showWhenDone) {
    // Build the undirected graph once per scenario and keep it.
    if (!nkdvNetwork_) {
        auto net = std::make_unique<NkdvNetwork>();
        std::string error;
        if (!net->build(*networkIndex_, error)) {
            if (showWhenDone) {
                QMessageBox::warning(this, tr("Heatmap"),
                    tr("Cannot prepare the network: %1")
                        .arg(QString::fromStdString(error)));
            }
            return;
        }
        nkdvNetwork_ = std::move(net);
    }

    // Collect this type's activities.
    std::vector<ActivityRecord> points;
    for (const auto& act : vehicleIndex_->activities()) {
        if (vehicleIndex_->actTypeString(act.actTypeId) == activityType)
            points.push_back(act);
    }
    if (points.empty()) {
        if (showWhenDone)
            statusLabel_->setText(tr("No %1 activities to map").arg(activityType));
        precomputeNextHeatmap();
        return;
    }

    NkdeScatter::Params params;
    params.lixelLength = kHeatmapLixelLength;
    params.bandwidth = kHeatmapBandwidth;

    heatmapRunning_ = true;
    // The menu stays enabled and no busy cursor is set: the work happens on a
    // worker thread, so the window must keep responding normally.
    if (showWhenDone) {
        setHeatmapStatus(tr("Computing %1 map...").arg(activityType),
                         HeatmapStatus::Working);
    } else {
        const int remaining = heatmapPrecomputeQueue_.size();
        setHeatmapStatus(remaining > 0
            ? tr("Preparing %1 map (%2 more)").arg(activityType).arg(remaining)
            : tr("Preparing %1 map").arg(activityType),
            HeatmapStatus::Working);
    }

    NkdvNetwork* net = nkdvNetwork_.get();
    const NetworkIndex* netIndex = networkIndex_.get();
    const uint64_t generation = heatmapGeneration_;
    auto* watcher = new QFutureWatcher<NkdeScatter::Result>(this);
    connect(watcher, &QFutureWatcher<NkdeScatter::Result>::finished, this,
            [this, watcher, activityType, showWhenDone, generation]() {
        const NkdeScatter::Result result = watcher->result();
        watcher->deleteLater();
        heatmapRunning_ = false;

        // The window is closing: drop the result and start nothing new.
        if (NkdeScatter::cancelled()) return;

        // The scenario changed while this ran. The map describes a network that
        // is no longer loaded, so it must not be shown or cached.
        if (generation != heatmapGeneration_) {
            LOG_INFO(QString("Heatmap: dropped a stale %1 map from a previous "
                             "scenario").arg(activityType));
            return;
        }

        if (!result.success) {
            if (showWhenDone) {
                setHeatmapStatus(tr("%1 map failed").arg(activityType),
                                 HeatmapStatus::Failed);
                QMessageBox::warning(this, tr("Heatmap"), result.errorMessage);
            }
            // A failure is usually about this type, not the rest, so keep going.
            precomputeNextHeatmap();
            return;
        }

        // Keep the result, so this type is instant from now on. A failure to
        // store is not fatal: the map still works, it just is not remembered.
        if (heatmapCache_) {
            HeatmapCache::Key key{activityType, kHeatmapLixelLength,
                                  kHeatmapBandwidth};
            heatmapCache_->store(key, result.lixels);
        }

        // A map computed in the background still counts towards the shared
        // scale, so comparing types does not depend on which were opened. Read
        // the anchor back from the entry just stored, so it is the same high
        // percentile the renderer uses rather than this map's highest lixel.
        if (heatmapCache_) {
            const HeatmapCache::Key peakKey{activityType, kHeatmapLixelLength,
                                            kHeatmapBandwidth};
            noteHeatmapPeak(heatmapCache_->peakOf(peakKey, heatmapSourceTime_));
        }

        // An activity whose link is missing from the network cannot be placed,
        // so the map is drawn from fewer activities than the menu counted. That
        // is usually a handful and harmless, but silently dropping a large
        // share would make the map wrong with nothing to show for it.
        if (result.pointsSkipped > 0) {
            const double skippedShare =
                100.0 * static_cast<double>(result.pointsSkipped) /
                static_cast<double>(result.pointsPlaced + result.pointsSkipped);
            LOG_WARN(QString("Heatmap: %1 of %2 %3 activities could not be "
                             "placed on the network (%4%)")
                .arg(result.pointsSkipped)
                .arg(result.pointsPlaced + result.pointsSkipped)
                .arg(activityType)
                .arg(skippedShare, 0, 'f', 2));
        }

        if (showWhenDone) {
            mapWidget_->setHeatmap(result.lixels, kHeatmapLixelLength, activityType);
            noteHeatmapPeak(mapWidget_->heatmapPeak());
            mapWidget_->setHeatmapVisible(true);

            // Only mention dropped activities when enough went missing to
            // change what the map means; a few are normal and not worth noise.
            const size_t total = result.pointsPlaced + result.pointsSkipped;
            const bool manySkipped =
                result.pointsSkipped > 0 && total > 0 &&
                static_cast<double>(result.pointsSkipped) /
                    static_cast<double>(total) > 0.01;
            setHeatmapStatus(manySkipped
                ? tr("%1 density: %2 lixels in %3 s (%4 activities not on the "
                     "network)").arg(activityType).arg(result.lixels.size())
                     .arg(result.seconds, 0, 'f', 1).arg(result.pointsSkipped)
                : tr("%1 density: %2 lixels in %3 s")
                     .arg(activityType).arg(result.lixels.size())
                     .arg(result.seconds, 0, 'f', 1),
                HeatmapStatus::Done);
        }

        // A background map can raise the shared peak above the value the map on
        // screen was drawn with. Re-anchor it here too, or it stays too bright
        // until the user opens a different type.
        applyHeatmapScale();

        precomputeNextHeatmap();
    });

    watcher->setFuture(QtConcurrent::run(
        [net, netIndex, points, params]() {
            // Scatter from the activities outwards. Most edges carry no
            // activity of a given type, and this searches only from the ones
            // that do. See src/analysis/nkde_scatter.h.
            return NkdeScatter::run(*net, *netIndex, points, params,
                                    NkdeScatter::cancelFlag());
        }));
}

void MainWindow::startHeatmapPrecompute() {
    if (!vehicleIndex_ || !heatmapCache_) return;
    if (!networkIndex_) return;

    // Precomputing every type is only sensible while it takes seconds. Cost
    // grows with the network and the activity count: a 92 K-link
    // network computes a map in about 3 s, while a 200 K-link one with a
    // million activities takes many minutes. Above the threshold, compute a
    // map only when it is asked for, so loading a large scenario does not
    // start half an hour of background work nobody requested.
    constexpr size_t kAutoPrecomputeMaxLinks = 150'000;
    const size_t linkCount = networkIndex_->linkCount();
    heatmapAutoPrecompute_ = linkCount <= kAutoPrecomputeMaxLinks;

    if (!heatmapAutoPrecompute_) {
        LOG_INFO(QString("Heatmap precompute: skipped, network has %1 links "
                         "(limit %2). Maps are computed on request.")
            .arg(linkCount).arg(kAutoPrecomputeMaxLinks));
        return;
    }

    // Queue the types that are not cached yet, largest first: those take
    // longest, and the map a user asks for first is usually Home or Work.
    std::map<QString, size_t> counts;
    for (const auto& act : vehicleIndex_->activities()) {
        QString type = vehicleIndex_->actTypeString(act.actTypeId);
        if (type.isEmpty() || type == "pt interaction") continue;
        ++counts[type];
    }

    std::vector<std::pair<size_t, QString>> byCount;
    for (const auto& [type, count] : counts) {
        HeatmapCache::Key key{type, kHeatmapLixelLength, kHeatmapBandwidth};
        if (heatmapCache_->contains(key, heatmapSourceTime_)) continue;
        byCount.push_back({count, type});
    }
    std::sort(byCount.rbegin(), byCount.rend());

    heatmapPrecomputeQueue_.clear();
    for (const auto& [count, type] : byCount)
        heatmapPrecomputeQueue_.append(type);

    if (heatmapPrecomputeQueue_.isEmpty()) {
        LOG_INFO("Heatmap precompute: every activity type is already cached");
        return;
    }

    LOG_INFO(QString("Heatmap precompute: %1 types to compute")
        .arg(heatmapPrecomputeQueue_.size()));
    heatmapPrecomputeActive_ = true;
    setHeatmapStatus(tr("Preparing activity maps (%1)")
        .arg(heatmapPrecomputeQueue_.size()), HeatmapStatus::Working);
    precomputeNextHeatmap();
}

void MainWindow::precomputeNextHeatmap() {
    if (!heatmapPrecomputeActive_) return;
    if (heatmapRunning_) return;   // the finish handler will call again

    // Drop anything that became cached while we waited, including the map the
    // user opened, which stored itself when it finished.
    while (!heatmapPrecomputeQueue_.isEmpty()) {
        const QString type = heatmapPrecomputeQueue_.takeFirst();

        // The type the user asked for must be shown, not only stored. If it
        // arrived while another map was running, this is where it gets its turn.
        const bool isUserRequest = (type == heatmapPendingUserType_);

        HeatmapCache::Key key{type, kHeatmapLixelLength, kHeatmapBandwidth};
        std::vector<NkdvNetwork::Lixel> cached;
        if (heatmapCache_ && heatmapCache_->load(key, heatmapSourceTime_, cached)) {
            if (isUserRequest) {
                // Finished in the background while the user waited: show it.
                heatmapPendingUserType_.clear();
                mapWidget_->setHeatmap(cached, kHeatmapLixelLength, type);
                noteHeatmapPeak(mapWidget_->heatmapPeak());
                applyHeatmapScale();
                mapWidget_->setHeatmapVisible(true);
                setHeatmapStatus(tr("%1 density: %2 lixels").arg(type).arg(cached.size()),
                                 HeatmapStatus::Done);
            }
            continue;
        }

        if (isUserRequest) heatmapPendingUserType_.clear();
        runHeatmapAsync(type, /*showWhenDone=*/isUserRequest);
        return;
    }

    heatmapPrecomputeActive_ = false;
    LOG_INFO("Heatmap precompute: finished");
    if (heatmapAutoPrecompute_) {
        setHeatmapStatus(tr("All activity maps ready"), HeatmapStatus::Done);
    }
}

void MainWindow::applyTransitData() {
    if (!transitData_ || !transitData_->success) return;

    statusLabel_->setText(QString("Loaded transit: %1 stops, %2 lines")
        .arg(transitData_->stops.size())
        .arg(transitData_->lines.size()));

    mapWidget_->setTransitData(transitData_.get());
}

void MainWindow::loadCachedCRS(const QString& crsPath) {
    QFile crsFile(crsPath);
    if (crsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString crsStr = QString::fromUtf8(crsFile.readAll()).trimmed();
        crsFile.close();
        if (!crsStr.isEmpty()) {
            networkCrs_ = parseCRS(crsStr.toStdString());
            if (networkCrs_.isValid()) {
                mapWidget_->setCRS(networkCrs_);
            }
        }
    }
}

void MainWindow::parseTransitOnly() {
    statusLabel_->setText("Parsing transit schedule...");
    LOG_INFO(QString("Parsing transit schedule: %1").arg(transitSchedulePath_));

    bool isGzip = transitSchedulePath_.endsWith(".gz", Qt::CaseInsensitive);

    TransitScheduleParser::Result result;
    if (isGzip) {
        result = TransitScheduleParser::parseGzipFile(
            transitSchedulePath_.toStdString(),
            networkIndex_->linkIds(),
            nullptr
        );
    } else {
        result = TransitScheduleParser::parse(
            transitSchedulePath_.toStdString(),
            networkIndex_->linkIds(),
            nullptr
        );
    }

    if (result.success) {
        LOG_INFO(QString("Transit parsed: %1 stops, %2 lines")
            .arg(result.stops.size()).arg(result.lines.size()));
        transitData_ = std::make_unique<TransitScheduleParser::Result>(std::move(result));
        applyTransitData();
    } else {
        LOG_ERROR(QString("Transit parse failed: %1")
            .arg(QString::fromStdString(result.errorMessage)));
        QMessageBox::warning(this, "Transit Schedule",
            QString("Failed to parse transit schedule:\n%1")
                .arg(QString::fromStdString(result.errorMessage)));
    }
}

void MainWindow::loadNetworkFile(const QString& path) {
    networkFilePath_ = path;
}

void MainWindow::loadEventsFile(const QString& path) {
    eventsFilePath_ = path;
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Space:
            timelineWidget_->togglePlayPause();
            break;
        case Qt::Key_Home:
            mapWidget_->setSimulationTime(vehicleIndex_ ? VehicleIndex::toSeconds(vehicleIndex_->minTime()) : 0);
            break;
        case Qt::Key_End:
            mapWidget_->setSimulationTime(vehicleIndex_ ? VehicleIndex::toSeconds(vehicleIndex_->maxTime()) : 0);
            break;
        default:
            QMainWindow::keyPressEvent(event);
    }
}

namespace {

// A drop is loadable if it carries a directory, or a file we can take the
// containing directory from. loadFolder() then does the usual resolution, so a
// user can drag either the scenario folder or any one file inside it.
QString droppedScenarioPath(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return QString();

    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QFileInfo info(url.toLocalFile());
        if (info.isDir()) return info.absoluteFilePath();
        if (info.isFile()) {
            const QString name = info.fileName();
            if (name.endsWith(".xml", Qt::CaseInsensitive) ||
                name.endsWith(".xml.gz", Qt::CaseInsensitive)) {
                return info.absolutePath();
            }
        }
    }
    return QString();
}

} // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!droppedScenarioPath(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QString path = droppedScenarioPath(event->mimeData());
    if (path.isEmpty()) return;

    event->acceptProposedAction();
    LOG_INFO(QString("Scenario dropped onto window: %1").arg(path));
    loadFolder(path);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Clean up
    mapWidget_->setPlaying(false);

    // Stop background density work before the window goes away. Without this
    // the process stays alive until the last computation finishes, which on a
    // large scenario is minutes after the window has closed.
    heatmapPrecomputeQueue_.clear();
    heatmapPrecomputeActive_ = false;
    NkdeScatter::cancelAll();

    // Wait for the worker to notice. A run checks between source edges, so a
    // run already inside it has to finish; the wait is bounded so that closing
    // never hangs, and the process exits either way.
    if (heatmapRunning_) {
        statusLabel_->setText(tr("Stopping background work..."));
        QApplication::processEvents();
        QThreadPool::globalInstance()->waitForDone(5000);
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::onPreprocessingProgress(int percent, const QString& message) {
    statusLabel_->setText(message);
    Q_UNUSED(percent);
}

void MainWindow::onPreprocessingComplete(bool success) {
    if (success) {
        loadBinaryFiles();
    }
}

void MainWindow::onSimulationTimeChanged(float time) {
    updateStatusBar();

    // Keep the vehicle info panel live while tracking (throttled to ~2 Hz of
    // wall time is overkill to compute; throttle on sim time delta instead)
    if (mapWidget_->isTrackingVehicle() && infoPanel_->isVisible()) {
        if (std::abs(time - lastVehicleInfoRefreshTime_) >= 1.0f) {
            lastVehicleInfoRefreshTime_ = time;
            refreshVehicleInfoPanel();
        }
    }
}

void MainWindow::onVehicleTrackingChanged(uint32_t vehicleId) {
    if (vehicleId == 0) {
        // Tracking stopped (e.g. user clicked empty map to pan). Keep the
        // panel content and any route overlay so panning doesn't wipe them.
        return;
    }

    // Tracking a (new) vehicle: reset route state and remember its person
    infoPanel_->resetPersonRouteState();
    mapWidget_->setPersonRouteVisible(false);
    mapWidget_->setPersonRoute({});
    routePersonPlus1_ = vehicleIndex_ ? vehicleIndex_->vehicleDriver(vehicleId) : 0;
    infoVehicleId_ = vehicleId;

    lastVehicleInfoRefreshTime_ = mapWidget_->simulationTime();
    refreshVehicleInfoPanel();
    infoPanel_->show();
    showInfoPanelAction_->setChecked(true);
}

void MainWindow::onShowPersonRouteToggled(bool show) {
    if (!show) {
        mapWidget_->setPersonRouteVisible(false);
        return;
    }
    if (routePersonPlus1_ == 0 || !vehicleIndex_) return;

    mapWidget_->setPersonRoute(buildPersonRouteSegments(routePersonPlus1_ - 1));
    mapWidget_->setActivityMarkers(buildActivityMarkers(routePersonPlus1_ - 1));
    mapWidget_->setPersonRouteVisible(true);
}

void MainWindow::onTripClicked(int tripIndex) {
    if (routePersonPlus1_ == 0 || !vehicleIndex_) return;

    if (tripIndex < 0) {
        // Deselected: hide the single-trip overlay
        mapWidget_->setPersonRouteVisible(false);
    } else {
        mapWidget_->setPersonRoute(
            buildPersonRouteSegments(routePersonPlus1_ - 1, tripIndex));
        mapWidget_->setActivityMarkers(
            buildActivityMarkers(routePersonPlus1_ - 1, tripIndex));
        mapWidget_->setPersonRouteVisible(true);
    }
    // Repaint the panel so the selected card highlight moves to the new trip
    refreshVehicleInfoPanel();
}

std::vector<MapWidget::ActivityMarker>
MainWindow::buildActivityMarkers(uint32_t personId, int tripIndex) const {
    std::vector<MapWidget::ActivityMarker> markers;
    if (!vehicleIndex_ || !networkIndex_) return markers;

    const auto* actIndices = vehicleIndex_->activitiesForPerson(personId);
    if (!actIndices) return markers;
    const auto& allActs = vehicleIndex_->activities();

    // Fallback for events that carried no x/y: the midpoint of the activity's
    // link. Closer than either endpoint, though still an approximation.
    auto linkMidpoint = [&](uint32_t linkId, float& x, float& y) -> bool {
        if (linkId == TRIP_NO_LINK) return false;
        const auto* link = networkIndex_->getLink(linkId);
        if (!link) return false;
        const auto* from = networkIndex_->getNode(link->fromNode);
        const auto* to = networkIndex_->getNode(link->toNode);
        if (!from || !to) return false;
        x = static_cast<float>((from->x + to->x) * 0.5);
        y = static_cast<float>((from->y + to->y) * 0.5);
        return true;
    };

    // Single-trip mode: show only the activities bounding the selected leg,
    // i.e. the one ending as it departs and the one starting as it arrives.
    TimeMs windowStart = 0, windowEnd = TRIP_NO_TIME;
    if (tripIndex >= 0) {
        const auto* trips = vehicleIndex_->personTrips(personId);
        if (!trips || tripIndex >= static_cast<int>(trips->size())) return markers;
        const auto& trip = (*trips)[tripIndex];
        windowStart = trip.departTimeMs;
        windowEnd = trip.arriveTimeMs;
    }

    for (uint32_t idx : *actIndices) {
        const auto& act = allActs[idx];

        if (tripIndex >= 0) {
            // Keep activities that touch the leg's endpoints: one ends at
            // departure, the next starts at arrival.
            bool endsAtDeparture = act.endTimeMs == windowStart;
            bool startsAtArrival = windowEnd != TRIP_NO_TIME &&
                                   act.startTimeMs == windowEnd;
            if (!endsAtDeparture && !startsAtArrival) continue;
        }

        QString label = vehicleIndex_->actTypeString(act.actTypeId);
        if (label.isEmpty()) continue;

        float x = act.x, y = act.y;
        if (act.flags & ACT_FLAG_DERIVED_COORD) {
            if (!linkMidpoint(act.linkId, x, y)) continue;
        }
        markers.push_back({x, y, panelstyle::actIcon(label), label});
    }

    // Dedupe genuinely coincident activities of the same type (e.g. an actend
    // and actstart pair emitted at the same place). Distinct activities now
    // hold distinct coordinates, so this no longer merges unrelated stops.
    auto sameMarker = [](const MapWidget::ActivityMarker& a,
                         const MapWidget::ActivityMarker& b) {
        return a.label == b.label &&
               std::abs(a.x - b.x) < 1.0f && std::abs(a.y - b.y) < 1.0f;
    };
    std::vector<MapWidget::ActivityMarker> unique;
    for (const auto& m : markers) {
        bool dup = false;
        for (const auto& u : unique) {
            if (sameMarker(m, u)) { dup = true; break; }
        }
        if (!dup) unique.push_back(m);
    }
    return unique;
}

std::vector<PersonRouteRenderer::Segment>
MainWindow::buildPersonRouteSegments(uint32_t personId, int tripIndex) const {
    std::vector<PersonRouteRenderer::Segment> segments;
    if (!vehicleIndex_ || !networkIndex_) return segments;
    const auto* trips = vehicleIndex_->personTrips(personId);
    if (!trips) return segments;

    auto linkEndpoints = [&](uint32_t linkId, float& fx, float& fy,
                             float& tx, float& ty) -> bool {
        const auto* link = networkIndex_->getLink(linkId);
        if (!link) return false;
        const auto* from = networkIndex_->getNode(link->fromNode);
        const auto* to = networkIndex_->getNode(link->toNode);
        if (!from || !to) return false;
        fx = from->x; fy = from->y;
        tx = to->x;   ty = to->y;
        return true;
    };

    for (int i = 0; i < static_cast<int>(trips->size()); ++i) {
        if (tripIndex >= 0 && i != tripIndex) continue;  // single-trip mode
        const auto& trip = (*trips)[i];
        if (trip.vehicleId != 0) {
            // Vehicle leg: follow the vehicle's trajectory links within the
            // trip's time window (link-by-link path on the network)
            const auto* traj = vehicleIndex_->trajectory(trip.vehicleId - 1);
            bool any = false;
            if (traj) {
                for (const auto& seg : traj->segments) {
                    if (seg.leaveTime <= trip.departTimeMs) continue;
                    if (trip.arriveTimeMs != TRIP_NO_TIME &&
                        seg.enterTime >= trip.arriveTimeMs) break;
                    float fx, fy, tx, ty;
                    if (linkEndpoints(seg.linkId, fx, fy, tx, ty)) {
                        segments.push_back({fx, fy, tx, ty, false});
                        any = true;
                    }
                }
            }
            if (any) continue;
            // No trajectory overlap (e.g. pt rider whose vehicle has no
            // movement events): fall through to a straight connector
        }

        // Teleported leg (walk/bike) or fallback: straight from->to connector
        float f1x, f1y, f2x, f2y, t1x, t1y, t2x, t2y;
        if (trip.fromLinkId != TRIP_NO_LINK && trip.toLinkId != TRIP_NO_LINK &&
            linkEndpoints(trip.fromLinkId, f1x, f1y, f2x, f2y) &&
            linkEndpoints(trip.toLinkId, t1x, t1y, t2x, t2y)) {
            // Use the to-node of the origin link and to-node of the dest link
            segments.push_back({f2x, f2y, t2x, t2y, true});
        }
    }

    return segments;
}

void MainWindow::refreshVehicleInfoPanel() {
    // Prefer the live tracked vehicle; fall back to the last shown one so the
    // panel keeps working after tracking stops (e.g. user panned the map)
    uint32_t vehicleId = mapWidget_->trackedVehicle();
    if (vehicleId == 0) vehicleId = infoVehicleId_;
    if (vehicleId == 0 || !vehicleIndex_) return;

    VehicleInfo info;
    info.vehicleId = vehicleId;
    info.vehicleName = vehicleIndex_->vehicleIdString(vehicleId);
    info.currentTime = mapWidget_->simulationTime();
    const TimeMs nowMs = VehicleIndex::toTimeMs(info.currentTime);

    const bool isTransit = vehicleIndex_->isTransitVehicle(vehicleId);
    switch (vehicleIndex_->getVehicleMode(vehicleId)) {
        case TransitMode::Bus:  info.mode = "Bus"; break;
        case TransitMode::Tram: info.mode = "Tram"; break;
        case TransitMode::Rail: info.mode = "Rail"; break;
        default:                info.mode = "Car"; break;
    }

    // Live state (speed / on-network status)
    const VehicleState* state = mapWidget_->vehicleState(vehicleId);
    if (state && state->active) {
        info.active = true;
        info.speedRatio = state->speedRatio;
        if (networkIndex_) {
            if (const auto* link = networkIndex_->getLink(state->currentLink)) {
                info.speedKmh = state->speedRatio * link->freespeed * 3.6f;
            }
        }
    }

    if (isTransit) {
        // Transit vehicle: line/route + live passenger count
        if (const auto* svc = vehicleIndex_->transitServiceInfo(vehicleId)) {
            info.lineName = vehicleIndex_->transitLineString(svc->lineStrId);
            info.routeName = vehicleIndex_->transitRouteString(svc->routeStrId);
        }
        int aboard = vehicleIndex_->passengersAboard(vehicleId, nowMs);
        // Exclude the driver if they are counted among occupants
        uint32_t driver = vehicleIndex_->vehicleDriver(vehicleId);
        if (driver != 0 && aboard > 0) --aboard;
        info.passengerCount = std::max(aboard, 0);
    } else {
        // Car: resolve the driver person and show their day of trips
        uint32_t driver = vehicleIndex_->vehicleDriver(vehicleId);
        if (driver != 0) {
            uint32_t personId = driver - 1;
            info.personName = vehicleIndex_->personIdString(personId);
            if (const auto* trips = vehicleIndex_->personTrips(personId)) {
                info.trips.reserve(trips->size());
                for (const auto& t : *trips) {
                    VehicleInfo::TripRow row;
                    row.fromAct = vehicleIndex_->actTypeString(t.fromActTypeId);
                    row.toAct   = vehicleIndex_->actTypeString(t.toActTypeId);
                    row.mode    = vehicleIndex_->legModeString(t.legModeId);
                    row.departSec = VehicleIndex::toSeconds(t.departTimeMs);
                    row.arriveSec = (t.arriveTimeMs == TRIP_NO_TIME)
                        ? -1.0f : VehicleIndex::toSeconds(t.arriveTimeMs);
                    row.midJourney = row.fromAct.isEmpty() && row.toAct.isEmpty();
                    row.active = nowMs >= t.departTimeMs &&
                                 (t.arriveTimeMs == TRIP_NO_TIME || nowMs < t.arriveTimeMs);
                    info.trips.push_back(std::move(row));
                }
            }
        }
    }

    infoPanel_->showVehicleInfo(info);
}

void MainWindow::onActiveVehicleCountChanged(size_t count) {
    vehicleCountLabel_->setText(QString("Vehicles: %1").arg(count));
}

void MainWindow::updateStatusBar() {
    // Could update zoom level, etc.
}

void MainWindow::onShowNodesToggled(bool checked) {
    mapWidget_->setShowNodes(checked);
}

void MainWindow::onShowLinksToggled(bool checked) {
    mapWidget_->setShowLinks(checked);
}

void MainWindow::onShowVehiclesToggled(bool checked) {
    mapWidget_->setShowVehicles(checked);
}

void MainWindow::onFitToNetwork() {
    mapWidget_->fitToNetwork();
}

void MainWindow::onVehicleShapeCircle() {
    mapWidget_->setVehicleShape(0);
}

void MainWindow::onVehicleShapeTriangle() {
    mapWidget_->setVehicleShape(1);
}

void MainWindow::onVehicleShapeRectangle() {
    mapWidget_->setVehicleShape(2);
}

void MainWindow::onVehicleShapeDiamond() {
    mapWidget_->setVehicleShape(3);
}

void MainWindow::onVehicleShapeAuto() {
    mapWidget_->setVehicleShape(-1);
}

void MainWindow::onVehicleSizeSmall() {
    mapWidget_->setVehicleSize(3.0f);
}

void MainWindow::onVehicleSizeMedium() {
    mapWidget_->setVehicleSize(5.0f);
}

void MainWindow::onVehicleSizeLarge() {
    mapWidget_->setVehicleSize(8.0f);
}

void MainWindow::onVehicleColorSpeed() {
    mapWidget_->setVehicleColorMode(VehicleColorMode::Speed);
}

void MainWindow::onVehicleColorMode() {
    mapWidget_->setVehicleColorMode(VehicleColorMode::TransitMode);
}

// Transit options
void MainWindow::onShowBusStopsToggled(bool checked) {
    mapWidget_->setShowBusStops(checked);
}

void MainWindow::onShowBusRoutesToggled(bool checked) {
    mapWidget_->setShowBusRoutes(checked);
}

void MainWindow::onShowTramRoutesToggled(bool checked) {
    mapWidget_->setShowTramRoutes(checked);
}

void MainWindow::onShowRailRoutesToggled(bool checked) {
    mapWidget_->setShowRailRoutes(checked);
}

void MainWindow::onRouteThicknessThin() {
    mapWidget_->setRouteLineWidth(1.0f);
}

void MainWindow::onRouteThicknessMedium() {
    mapWidget_->setRouteLineWidth(2.0f);
}

void MainWindow::onRouteThicknessThick() {
    mapWidget_->setRouteLineWidth(4.0f);
}

// Info panel slots
void MainWindow::onTransitStopClicked(int stopIndex) {
    if (!transitData_ || stopIndex < 0 ||
        stopIndex >= static_cast<int>(transitData_->stops.size())) return;

    const auto& stop = transitData_->stops[stopIndex];

    // Find all lines serving this stop
    std::vector<const TransitLine*> servingLines;
    for (const auto& line : transitData_->lines) {
        for (const auto& route : line.routes) {
            bool found = false;
            for (const auto& rs : route.stops) {
                if (rs.stopId == stop.id) {
                    servingLines.push_back(&line);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    infoPanel_->showStopInfo(stop, servingLines);
    showInfoPanelAction_->setChecked(true);
}

void MainWindow::onTransitRouteClicked(uint32_t lineId) {
    if (!transitData_) return;

    for (const auto& line : transitData_->lines) {
        if (line.id == lineId && !line.routes.empty()) {
            // Pick representative route (most links)
            const TransitRoute* best = &line.routes[0];
            for (const auto& route : line.routes) {
                if (route.linkIds.size() > best->linkIds.size()) {
                    best = &route;
                }
            }
            infoPanel_->showRouteInfo(line, *best, *transitData_);
            showInfoPanelAction_->setChecked(true);
            return;
        }
    }
}

void MainWindow::onInfoPanelStopClicked(uint32_t stopId) {
    if (!transitData_) return;

    auto it = transitData_->stopIdToIndex.find(stopId);
    if (it != transitData_->stopIdToIndex.end()) {
        onTransitStopClicked(static_cast<int>(it->second));
    }
}

void MainWindow::onInfoPanelRouteClicked(uint32_t lineId) {
    // Highlight the route on the map
    mapWidget_->setTransitHighlightedLine(lineId);
    onTransitRouteClicked(lineId);
}

void MainWindow::onInfoPanelPanTo(float x, float y) {
    mapWidget_->panTo(x, y);
}

// Counts implementation
void MainWindow::onLoadCounts() {
    if (!networkIndex_ || !vehicleIndex_) {
        QMessageBox::warning(this, "Load Counts",
            "Please load network and events data first.");
        return;
    }

    QString countsPath = QFileDialog::getOpenFileName(
        this,
        "Open Counts File",
        QFileInfo(networkFilePath_).absolutePath(),
        "Counts Files (*.xml *.xml.gz);;All Files (*)"
    );

    if (countsPath.isEmpty()) return;

    LOG_INFO(QString("Loading counts file: %1").arg(countsPath));
    statusLabel_->setText("Loading counts...");
    QApplication::processEvents();

    // Parse counts.xml
    countsData_ = CountsParser::parse(
        countsPath.toStdString(),
        *networkIndex_);

    if (!countsData_ || countsData_->counts.empty()) {
        QMessageBox::warning(this, "Load Counts",
            "No valid count locations found in the file.\n"
            "Check that loc_id values match network link IDs.");
        countsData_.reset();
        statusLabel_->setText("Counts loading failed");
        return;
    }

    // Ask user for countsScaleFactor
    bool ok = false;
    double scaleFactor = QInputDialog::getDouble(
        this,
        "Counts Scale Factor",
        "Enter the countsScaleFactor from your config.xml\n"
        "(under <module name=\"counts\">).\n\n"
        "The simulated events may represent a scaled sample of\n"
        "the full population. Simulated volumes will be multiplied\n"
        "by this factor to match the real scale.\n\n"
        "countsScaleFactor:",
        1.0,    // default
        0.001,  // min
        10000,  // max
        3,      // decimals
        &ok
    );
    if (!ok) {
        countsData_.reset();
        statusLabel_->setText("Counts loading cancelled");
        return;
    }

    // Compute simulated volumes from vehicle index
    statusLabel_->setText("Computing simulated volumes...");
    QApplication::processEvents();

    CountsParser::computeSimulatedVolumes(*countsData_, *vehicleIndex_);

    // Apply scale factor to simulated volumes
    if (std::abs(scaleFactor - 1.0) > 1e-9) {
        for (auto& c : countsData_->counts) {
            for (int h = 0; h < 24; ++h) {
                c.simulatedVolumes[h] = static_cast<uint32_t>(
                    std::round(c.simulatedVolumes[h] * scaleFactor));
            }
        }
        LOG_INFO(QString("Applied countsScaleFactor: %1").arg(scaleFactor));
    }

    // Pass to map widget
    mapWidget_->setCountsData(countsData_.get());

    // Enable and activate the show counts toggle
    showCountsAction_->setEnabled(true);
    showCountsAction_->setChecked(true);

    statusLabel_->setText(QString("Loaded %1 count locations").arg(countsData_->counts.size()));
    LOG_INFO(QString("Counts loaded: %1 locations").arg(countsData_->counts.size()));
}

void MainWindow::onShowCountsToggled(bool checked) {
    mapWidget_->setShowCounts(checked);
}

static QString roadTypeName(RoadType t) {
    switch (t) {
        case RoadType::Motorway:    return "Motorway";
        case RoadType::Primary:     return "Primary";
        case RoadType::Secondary:   return "Secondary";
        case RoadType::Tertiary:    return "Tertiary";
        case RoadType::Residential: return "Residential";
        case RoadType::Service:     return "Service";
        case RoadType::Other:       return "Other";
        default:                    return "Unknown";
    }
}

void MainWindow::onNetworkLinkClicked(uint32_t linkId) {
    if (!networkIndex_) return;
    const auto* link = networkIndex_->getLink(linkId);
    if (!link) return;

    // A link selection replaces every other highlight on the map, then draws
    // this link in yellow.
    mapWidget_->clearAllHighlights();
    infoPanel_->resetPersonRouteState();
    routePersonPlus1_ = 0;
    infoVehicleId_ = 0;
    mapWidget_->highlightLink(linkId);

    InfoPanel::LinkInfo info;
    const auto& linkIds = networkIndex_->linkIds();
    if (linkId < linkIds.size())
        info.linkId = QString::fromStdString(linkIds.getString(linkId));
    else
        info.linkId = QString::number(linkId);

    const auto& nodeIds = networkIndex_->nodeIds();
    if (link->fromNode < nodeIds.size())
        info.fromNode = QString::fromStdString(nodeIds.getString(link->fromNode));
    if (link->toNode < nodeIds.size())
        info.toNode = QString::fromStdString(nodeIds.getString(link->toNode));

    info.lengthM = link->length;
    info.freeSpeedMs = link->freespeed;
    info.capacityVehH = link->capacity;
    info.lanes = link->lanes;
    info.roadType = roadTypeName(static_cast<RoadType>(link->roadType));

    // Attach count-station info if counts are loaded for this link
    if (countsData_) {
        auto it = countsData_->linkToCount.find(linkId);
        if (it != countsData_->linkToCount.end()) {
            info.hasCounts = true;
            info.countStationId =
                QString::fromStdString(countsData_->counts[it->second].stationId);
        }
    }
    auto volIt = linkHourlyVolumes_.find(linkId);
    if (volIt != linkHourlyVolumes_.end()) {
        info.hourlyVolumes = volIt->second;
    } else {
        info.hourlyVolumes.assign(24, 0); // No traffic recorded
    }

    infoPanel_->showLinkInfo(info);
    showInfoPanelAction_->setChecked(true);
}

void MainWindow::onCountLinkClicked(uint32_t linkId) {
    if (!countsData_) return;

    auto it = countsData_->linkToCount.find(linkId);
    if (it == countsData_->linkToCount.end()) return;

    const auto& countData = countsData_->counts[it->second];

    // Get original link ID string for the title
    std::string linkOriginalId;
    if (networkIndex_) {
        const auto& linkIds = networkIndex_->linkIds();
        if (linkId < linkIds.size()) {
            linkOriginalId = linkIds.getString(linkId);
        }
    }

    auto* dialog = new CountsChartDialog(countData, linkOriginalId, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

// Video recording implementation
void MainWindow::onStartRecording() {
    if (!vehicleIndex_ || !networkIndex_) {
        QMessageBox::warning(this, "Record Video",
            "Please load network and events data before recording.");
        return;
    }

    // Get time range from vehicle index
    float minTime = VehicleIndex::toSeconds(vehicleIndex_->minTime());
    float maxTime = VehicleIndex::toSeconds(vehicleIndex_->maxTime());
    float currentTime = mapWidget_->simulationTime();

    // Show settings dialog
    VideoSettingsDialog dialog(minTime, maxTime, currentTime, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;  // User cancelled
    }

    VideoSettings settings = dialog.getSettings();
    float startTime = dialog.getStartTime();
    float endTime = dialog.getEndTime();

    // Validate settings
    if (settings.outputPath.isEmpty()) {
        QMessageBox::warning(this, "Record Video", "Please specify an output file path.");
        return;
    }

    // Pause playback if playing
    bool wasPlaying = mapWidget_->isPlaying();
    if (wasPlaying) {
        mapWidget_->setPlaying(false);
    }

    // Start recording
    LOG_INFO(QString("Recording started: %1, %2x%3, %4fps, speed %5x, t=[%6,%7]")
        .arg(settings.outputPath)
        .arg(settings.width()).arg(settings.height())
        .arg(settings.fps()).arg(settings.speedMult())
        .arg(startTime, 0, 'f', 1).arg(endTime, 0, 'f', 1));
    if (videoRecorder_->startRecording(settings, startTime, endTime)) {
        // Recording started successfully
        startRecordingAction_->setEnabled(false);
        stopRecordingAction_->setEnabled(true);

        // Set simulation to start time
        mapWidget_->setSimulationTime(startTime);

        // Set playback speed based on speed multiplier to ensure smooth frame capture
        // We want to generate frames fast enough but not so fast we skip frames
        // Target: ~2x the required frame rate for smooth capture
        int speedMult = settings.speedMult();  // sim seconds per video second
        int fps = settings.fps();  // frames per video second
        // simTimePerFrame = speedMult / fps
        // We want to advance through simulation faster than real-time but not too fast
        // A good heuristic: playback at 10-30x real-time gives smooth recording
        double playbackSpeed = std::min(100.0, speedMult * 2.0);
        mapWidget_->setPlaybackSpeed(playbackSpeed);
        mapWidget_->setPlaying(true);
    } else {
        QMessageBox::critical(this, "Record Video",
            "Failed to start recording. Check that FFmpeg is installed or bundled in ffmpeg/ directory.");

        if (wasPlaying) {
            mapWidget_->setPlaying(true);
        }
    }
}

void MainWindow::onStopRecording() {
    if (videoRecorder_ && videoRecorder_->isRecording()) {
        videoRecorder_->stopRecording();
    }
}

void MainWindow::onRecordingStarted() {
    statusLabel_->setText("Recording video...");

    // Store playback state and disable controls during recording
    startRecordingAction_->setEnabled(false);
    stopRecordingAction_->setEnabled(true);
}

void MainWindow::onRecordingStopped(bool success, const QString& message) {
    startRecordingAction_->setEnabled(true);
    stopRecordingAction_->setEnabled(false);

    // Stop playback that was driving frame generation
    mapWidget_->setPlaying(false);
    mapWidget_->setPlaybackSpeed(1.0);  // Reset to normal speed

    if (success) {
        LOG_INFO(QString("Recording complete: %1").arg(message));
        statusLabel_->setText("Recording complete");
        QMessageBox::information(this, "Record Video", message);
    } else {
        LOG_ERROR(QString("Recording failed: %1").arg(message));
        statusLabel_->setText("Recording failed");
        QMessageBox::critical(this, "Record Video", "Recording failed:\n" + message);
    }
}

void MainWindow::onRecordingProgress(int percent) {
    statusLabel_->setText(QString("Recording video: %1%").arg(percent));
}

// Export implementation
void MainWindow::onExportPdf() {
    // Pause playback during export to get a clean frame
    bool wasPlaying = mapWidget_->isPlaying();
    if (wasPlaying) mapWidget_->setPlaying(false);

    ScreenshotExporter::exportPdf(mapWidget_, this, 4);

    if (wasPlaying) mapWidget_->setPlaying(true);
}

void MainWindow::onExportPng() {
    // Pause playback during export
    bool wasPlaying = mapWidget_->isPlaying();
    if (wasPlaying) mapWidget_->setPlaying(false);

    ScreenshotExporter::exportPng(mapWidget_, this);

    if (wasPlaying) mapWidget_->setPlaying(true);
}

} // namespace simvis
