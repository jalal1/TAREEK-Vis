#include "main_window.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include "side_panel.h"
#include "search_box.h"
#include "loading_indicator.h"
#include "progress_dialog.h"
#include "video_settings_dialog.h"
#include "counts_chart_dialog.h"
#include "help_window.h"
#include "views_panel.h"
#include "views_dialogs.h"
#include "core/config.h"
#include "core/logger.h"
#include "analysis/nkde_scatter.h"

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QTimer>
#include <map>
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
#include <QDesktopServices>
#include <QApplication>
#include <QActionGroup>
#include <QProgressDialog>
#include <QInputDialog>
#include <QSettings>
#include "core/modes.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QLineEdit>
#include <QToolTip>
#include <QPointer>
#include <QBuffer>
#include <QEventLoop>
#include <QLocale>
#include <QSaveFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <functional>
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
    setupActions();
    setupRibbon();
    setupStatusBar();

    // Initialize data managers
    networkIndex_ = std::make_unique<NetworkIndex>();
    vehicleIndex_ = std::make_shared<VehicleIndex>();

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

MainWindow::~MainWindow() {
    // Destroying the watcher while its future is still running leaves the
    // worker signalling into freed memory. closeEvent normally waits already;
    // this covers teardown paths that never route through it.
    if (volumeWatcher_.isRunning()) {
        volumeWatcher_.waitForFinished();
    }
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Horizontal splitter: MapWidget | side panel
    auto* splitter = new QSplitter(Qt::Horizontal, centralWidget);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(3);

    // Map widget (main visualization area)
    mapWidget_ = new MapWidget(splitter);
    splitter->addWidget(mapWidget_);
    loadingIndicator_ = new LoadingIndicator(mapWidget_);

    // One panel at the right (initially hidden) for everything that is shown
    // beside the map: the details of what was clicked, the links selected
    // for a what-if scenario, and the saved views
    sidePanel_ = new SidePanel(splitter);
    infoPanel_ = new InfoPanel();
    infoPage_ = sidePanel_->addPage(infoPanel_, "Info");
    scenarioPanel_ = new ScenarioPanel();
    selectedLinksPage_ = sidePanel_->addPage(scenarioPanel_, "Selected Links", true);
    viewsPanel_ = new ViewsPanel();
    viewsPage_ = sidePanel_->addPage(viewsPanel_, "Views");
    sidePanel_->hide();
    splitter->addWidget(sidePanel_);

    // Map takes all available space; the panel keeps its width
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);

    layout->addWidget(splitter, 1);

    // Timeline widget (bottom)
    timelineWidget_ = new TimelineWidget(this);
    layout->addWidget(timelineWidget_);

    centralWidget->setLayout(layout);
    setCentralWidget(centralWidget);

    connect(sidePanel_, &SidePanel::closeRequested, this, &MainWindow::hideSidePanel);

    connect(viewsPanel_, &ViewsPanel::viewActivated, this, &MainWindow::applyView);
    connect(viewsPanel_, &ViewsPanel::saveRequested, this, &MainWindow::saveView);
    connect(viewsPanel_, &ViewsPanel::updateRequested, this, [this](int row) {
        viewsPanel_->replaceView(row, currentView(QString()));
        statusLabel_->setText(QString("Updated \"%1\" from the current view")
            .arg(viewsPanel_->view(row).value("name").toString()));
    });
    connect(viewsPanel_, &ViewsPanel::message, this, [this](const QString& text) {
        statusBar()->showMessage(text, 8000);
    });

    connect(mapWidget_, &MapWidget::networkLinkCtrlClicked,
            this, &MainWindow::onNetworkLinkCtrlClicked);
    connect(scenarioPanel_, &ScenarioPanel::selectionChanged,
            mapWidget_, &MapWidget::setSelectedLinks);
    connect(scenarioPanel_, &ScenarioPanel::linkActivated, this, [this](uint32_t id) {
        onNetworkLinkClicked(id);
        zoomToLink(id);
    });

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

    // Connect counts click signal
    connect(mapWidget_, &MapWidget::countLinkClicked,
            this, &MainWindow::onCountLinkClicked);

    // Network link click -> show link details in the info panel
    connect(mapWidget_, &MapWidget::networkLinkClicked,
            this, &MainWindow::onNetworkLinkClicked);

    // Show vehicle info in the panel when tracking starts/stops
    connect(mapWidget_, &MapWidget::vehicleTrackingChanged,
            this, &MainWindow::onVehicleTrackingChanged);
    // A click on empty map pans; it lets go of a found object, as it stops
    // tracking a clicked vehicle
    connect(mapWidget_, &MapWidget::emptyMapClicked,
            this, &MainWindow::endSearchFollow);

    // Person full-day route overlay toggle from the info panel
    connect(infoPanel_, &InfoPanel::showPersonRouteToggled,
            this, &MainWindow::onShowPersonRouteToggled);
    // A passenger of a transit vehicle shows like a search for that person:
    // their day in the panel, and the halo on them
    connect(infoPanel_, &InfoPanel::passengerClicked, this,
            [this](uint32_t personId) { showPerson(personId); });

    // Single-trip overlay when a trip card is clicked
    connect(infoPanel_, &InfoPanel::tripClicked,
            this, &MainWindow::onTripClicked);

    // The volume scan runs while the user is already clicking around, so when
    // it lands, refresh a link panel that was opened before the numbers existed
    // - otherwise it keeps saying the link has no traffic.
    connect(&volumeWatcher_, &QFutureWatcher<LinkHourlyVolumes>::finished,
            this, [this]() {
        LinkHourlyVolumes result = volumeWatcher_.result();
        if (result.generation != volumeGeneration_) {
            LOG_INFO("Discarding volume scan from a superseded scenario.");
            return;
        }
        linkHourlyVolumes_ = std::move(result);
        linkHourlyVolumesReady_ = true;
        LOG_INFO("Background volume aggregation completed.");

        if (selectedLinkId_ != kNoLink) {
            onNetworkLinkClicked(selectedLinkId_);
        }
    });

}

void MainWindow::setupActions() {
    // File
    openFolderAction_ = new QAction("&Open Folder...", this);
    openFolderAction_->setShortcut(QKeySequence::Open);
    connect(openFolderAction_, &QAction::triggered, this, &MainWindow::openFolder);

    openFilesAction_ = new QAction("Open Files (&Advanced)...", this);
    connect(openFilesAction_, &QAction::triggered, this, &MainWindow::openFiles);

    recentFoldersMenu_ = new QMenu("&Recent Folders", this);
    recentFoldersMenu_->setToolTipsVisible(true);  // show full path on hover
    updateRecentFoldersMenu();

    rebuildCacheAction_ = new QAction("Re&build Cache...", this);
    rebuildCacheAction_->setToolTip("Preprocess the open run again");
    rebuildCacheAction_->setEnabled(false);  // Enabled after a run is loaded
    connect(rebuildCacheAction_, &QAction::triggered, this, [this]() {
        if (networkFilePath_.isEmpty()) return;
        const auto answer = QMessageBox::question(this, "Rebuild Cache",
            "Preprocess this run again? The network and the events file are "
            "read again. On a large run this takes some minutes.\n\n"
            "If you cancel, the current cache stays.");
        if (answer != QMessageBox::Yes) return;
        // loadFromPaths sets the paths again, so copy them first
        const QString network = networkFilePath_;
        const QString events = eventsFilePath_;
        const QString transit = transitSchedulePath_;
        loadFromPaths(network, events, transit, true);
    });

    loadCountsAction_ = new QAction("Load &Counts...", this);
    loadCountsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    loadCountsAction_->setEnabled(false);  // Enabled after network+events loaded
    connect(loadCountsAction_, &QAction::triggered, this, &MainWindow::onLoadCounts);


    // --- Network section ---
    showNodesAction_ = new QAction("Show &Nodes", this);
    showNodesAction_->setCheckable(true);
    showNodesAction_->setChecked(false);  // Off by default: reduces clutter
    connect(showNodesAction_, &QAction::toggled, this, &MainWindow::onShowNodesToggled);

    showLinksAction_ = new QAction("Show &Links", this);
    showLinksAction_->setCheckable(true);
    showLinksAction_->setChecked(true);
    connect(showLinksAction_, &QAction::toggled, this, &MainWindow::onShowLinksToggled);

    showCountsAction_ = new QAction("Show Link &Counts", this);
    showCountsAction_->setCheckable(true);
    showCountsAction_->setChecked(false);
    showCountsAction_->setEnabled(false);  // Enabled after counts loaded
    connect(showCountsAction_, &QAction::toggled, this, &MainWindow::onShowCountsToggled);

    // --- Activity density heatmap ---
    // One entry per activity type, filled in once a scenario is loaded, plus a
    // command to clear the map.
    heatmapMenu_ = new QMenu("Activity &Heatmap", this);
    heatmapMenu_->setEnabled(false);

    // --- Vehicles section ---
    // Master toggle + per-type visibility
    showVehiclesAction_ = new QAction("Show &All Vehicles", this);
    showVehiclesAction_->setCheckable(true);
    showVehiclesAction_->setChecked(true);
    connect(showVehiclesAction_, &QAction::toggled, this, &MainWindow::onShowVehiclesToggled);

    showCarsAction_ = new QAction("Show &Private Vehicles", this);
    showCarsAction_->setCheckable(true);
    showCarsAction_->setChecked(true);
    connect(showCarsAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setModeVisible(modes::kCar, checked);
    });

    showBusVehiclesAction_ = new QAction("Show &Buses", this);
    showBusVehiclesAction_->setCheckable(true);
    showBusVehiclesAction_->setChecked(true);
    connect(showBusVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setModeVisible(modes::kBus, checked);
    });

    showTramVehiclesAction_ = new QAction("Show &Trams", this);
    showTramVehiclesAction_->setCheckable(true);
    showTramVehiclesAction_->setChecked(true);
    connect(showTramVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setModeVisible(modes::kTram, checked);
    });

    showRailVehiclesAction_ = new QAction("Show &Rail", this);
    showRailVehiclesAction_->setCheckable(true);
    showRailVehiclesAction_->setChecked(true);
    connect(showRailVehiclesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setModeVisible(modes::kRail, checked);
    });

    // The other modes of the registry (air, taxi, bike, ...). The menu is
    // filled for each run with the modes that it contains.
    otherModesMenu_ = new QMenu("Other &Modes", this);
    otherModesMenu_->setEnabled(false);

    // --- Air section (tab Air, shown when the run has aircraft) ---
    showAirLinksAction_ = new QAction("Show Air &Links", this);
    showAirLinksAction_->setCheckable(true);
    showAirLinksAction_->setChecked(false);  // many, and they cross: off by default
    connect(showAirLinksAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowAirLinks(checked);
    });

    showVertiportsAction_ = new QAction("Show &Vertiports", this);
    showVertiportsAction_->setCheckable(true);
    showVertiportsAction_->setChecked(true);
    connect(showVertiportsAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowVertiports(checked);
    });

    showTrailsAction_ = new QAction("Show &Trails", this);
    showTrailsAction_->setCheckable(true);
    showTrailsAction_->setChecked(true);
    connect(showTrailsAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowTrails(checked);
    });

    // Height lift in screen pixels per metre: 500 m is 10, 20 or 40 px
    altitudeLiftMenu_ = new QMenu("Altitude &Lift", this);
    auto* liftGroup = new QActionGroup(this);
    const std::pair<const char*, float> lifts[] = {
        {"&No Lift", 0.0f}, {"&Low Lift", 0.02f}, {"&Medium Lift", 0.04f}, {"&High Lift", 0.08f}};
    for (const auto& [text, pxPerMetre] : lifts) {
        QAction* action = altitudeLiftMenu_->addAction(text);
        action->setCheckable(true);
        action->setChecked(pxPerMetre == 0.04f);
        action->setActionGroup(liftGroup);
        const float value = pxPerMetre;
        connect(action, &QAction::triggered, this, [this, value]() {
            mapWidget_->setAltitudeLift(value);
        });
    }

    // Height bands, filled for each run (populateAltitudeBandMenu)
    altitudeBandMenu_ = new QMenu("Altitude &Band", this);

    // tareek-air events
    showLossesAction_ = new QAction("Show Lo&sses", this);
    showLossesAction_->setCheckable(true);
    showLossesAction_->setChecked(true);
    connect(showLossesAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowLosses(checked);
    });
    showHoldsAction_ = new QAction("Show &Holds", this);
    showHoldsAction_->setCheckable(true);
    showHoldsAction_->setChecked(true);
    connect(showHoldsAction_, &QAction::toggled, this, [this](bool checked) {
        mapWidget_->setShowHolds(checked);
    });

    // Vehicle shape choices
    vehicleShapeMenu_ = new QMenu("Vehicle &Shape", this);
    auto* shapeGroup = new QActionGroup(this);

    vehicleShapeCircleAction_ = vehicleShapeMenu_->addAction("&Circle");
    vehicleShapeCircleAction_->setCheckable(true);
    vehicleShapeCircleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeCircleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeCircle);

    vehicleShapeTriangleAction_ = vehicleShapeMenu_->addAction("&Triangle (Arrow)");
    vehicleShapeTriangleAction_->setCheckable(true);
    vehicleShapeTriangleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeTriangleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeTriangle);

    vehicleShapeRectangleAction_ = vehicleShapeMenu_->addAction("&Rectangle");
    vehicleShapeRectangleAction_->setCheckable(true);
    vehicleShapeRectangleAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeRectangleAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeRectangle);

    vehicleShapeDiamondAction_ = vehicleShapeMenu_->addAction("&Diamond");
    vehicleShapeDiamondAction_->setCheckable(true);
    vehicleShapeDiamondAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeDiamondAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeDiamond);

    vehicleShapeMenu_->addSeparator();

    vehicleShapeAutoAction_ = vehicleShapeMenu_->addAction("&Auto (by mode)");
    vehicleShapeAutoAction_->setCheckable(true);
    vehicleShapeAutoAction_->setChecked(true);  // Default
    vehicleShapeAutoAction_->setActionGroup(shapeGroup);
    connect(vehicleShapeAutoAction_, &QAction::triggered, this, &MainWindow::onVehicleShapeAuto);

    // Vehicle size choices
    vehicleSizeMenu_ = new QMenu("Vehicle Si&ze", this);
    auto* sizeGroup = new QActionGroup(this);

    vehicleSizeSmallAction_ = vehicleSizeMenu_->addAction("&Small (3px)");
    vehicleSizeSmallAction_->setCheckable(true);
    vehicleSizeSmallAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeSmallAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeSmall);

    vehicleSizeMediumAction_ = vehicleSizeMenu_->addAction("&Medium (5px)");
    vehicleSizeMediumAction_->setCheckable(true);
    vehicleSizeMediumAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeMediumAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeMedium);

    vehicleSizeLargeAction_ = vehicleSizeMenu_->addAction("&Large (8px)");
    vehicleSizeLargeAction_->setCheckable(true);
    vehicleSizeLargeAction_->setChecked(true);  // Default
    vehicleSizeLargeAction_->setActionGroup(sizeGroup);
    connect(vehicleSizeLargeAction_, &QAction::triggered, this, &MainWindow::onVehicleSizeLarge);

    // Vehicle color choices
    vehicleColorMenu_ = new QMenu("Vehicle &Color", this);
    auto* colorGroup = new QActionGroup(this);

    vehicleColorSpeedAction_ = vehicleColorMenu_->addAction("By &Speed");
    vehicleColorSpeedAction_->setCheckable(true);
    vehicleColorSpeedAction_->setChecked(true);  // Default
    vehicleColorSpeedAction_->setActionGroup(colorGroup);
    connect(vehicleColorSpeedAction_, &QAction::triggered, this, &MainWindow::onVehicleColorSpeed);

    vehicleColorModeAction_ = vehicleColorMenu_->addAction("By &Mode (Icons)");
    vehicleColorModeAction_->setCheckable(true);
    vehicleColorModeAction_->setActionGroup(colorGroup);
    connect(vehicleColorModeAction_, &QAction::triggered, this, &MainWindow::onVehicleColorMode);

    // --- Transit section ---
    // All transit layers start hidden; users enable what they need
    showBusRoutesAction_ = new QAction("Show &Bus Routes", this);
    showBusRoutesAction_->setCheckable(true);
    showBusRoutesAction_->setChecked(false);
    connect(showBusRoutesAction_, &QAction::toggled, this, &MainWindow::onShowBusRoutesToggled);

    showTramRoutesAction_ = new QAction("Show T&ram Routes", this);
    showTramRoutesAction_->setCheckable(true);
    showTramRoutesAction_->setChecked(false);
    connect(showTramRoutesAction_, &QAction::toggled, this, &MainWindow::onShowTramRoutesToggled);

    showRailRoutesAction_ = new QAction("Show Ra&il Routes", this);
    showRailRoutesAction_->setCheckable(true);
    showRailRoutesAction_->setChecked(false);
    connect(showRailRoutesAction_, &QAction::toggled, this, &MainWindow::onShowRailRoutesToggled);

    showBusStopsAction_ = new QAction("Show Bus &Stops", this);
    showBusStopsAction_->setCheckable(true);
    showBusStopsAction_->setChecked(false);
    connect(showBusStopsAction_, &QAction::toggled, this, &MainWindow::onShowBusStopsToggled);

    // Route thickness choices
    routeThicknessMenu_ = new QMenu("Route &Thickness", this);
    auto* thicknessGroup = new QActionGroup(this);

    routeThicknessThinAction_ = routeThicknessMenu_->addAction("T&hin (1px)");
    routeThicknessThinAction_->setCheckable(true);
    routeThicknessThinAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessThinAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessThin);

    routeThicknessMediumAction_ = routeThicknessMenu_->addAction("&Medium (2px)");
    routeThicknessMediumAction_->setCheckable(true);
    routeThicknessMediumAction_->setChecked(true);  // Default
    routeThicknessMediumAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessMediumAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessMedium);

    routeThicknessThickAction_ = routeThicknessMenu_->addAction("Thic&k (4px)");
    routeThicknessThickAction_->setCheckable(true);
    routeThicknessThickAction_->setActionGroup(thicknessGroup);
    connect(routeThicknessThickAction_, &QAction::triggered, this, &MainWindow::onRouteThicknessThick);

    // --- Side panel pages ---
    // Each action shows its page of the side panel, and is checked while that
    // page is in front. Using the action of the page in front closes the panel.
    showInfoPanelAction_ = new QAction("Show &Info Panel", this);
    showInfoPanelAction_->setCheckable(true);
    connect(showInfoPanelAction_, &QAction::triggered, this, [this](bool checked) {
        if (checked) sidePanel_->showPage(infoPage_);
        else hideSidePanel();
    });

    showSelectedLinksAction_ = new QAction("Show &Selected Links Panel", this);
    showSelectedLinksAction_->setCheckable(true);
    connect(showSelectedLinksAction_, &QAction::triggered, this, [this](bool checked) {
        if (checked) sidePanel_->showPage(selectedLinksPage_);
        else hideSidePanel();
    });

    showViewsAction_ = new QAction("Show &Views Panel", this);
    showViewsAction_->setCheckable(true);
    connect(showViewsAction_, &QAction::triggered, this, [this](bool checked) {
        if (checked) sidePanel_->showPage(viewsPage_);
        else hideSidePanel();
    });

    connect(sidePanel_, &SidePanel::stateChanged, this, &MainWindow::updateSidePanelActions);

    // --- Background Map section ---
    showBackgroundMapAction_ = new QAction("&Enable Background Map", this);
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

    auto* tileSourceGroup = new QActionGroup(this);

    tileSourceOsmAction_ = new QAction("&OpenStreetMap", this);
    tileSourceOsmAction_->setCheckable(true);
    tileSourceOsmAction_->setChecked(true);
    tileSourceOsmAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceOsmAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::OpenStreetMap);
    });

    tileSourceSatAction_ = new QAction("&Satellite (ESRI)", this);
    tileSourceSatAction_->setCheckable(true);
    tileSourceSatAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceSatAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::Satellite);
    });

    tileSourceTopoAction_ = new QAction("&Topo Map", this);
    tileSourceTopoAction_->setCheckable(true);
    tileSourceTopoAction_->setActionGroup(tileSourceGroup);
    connect(tileSourceTopoAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setTileSource(TileSource::Topo);
    });


    // --- Navigation section ---
    fitAction_ = new QAction("&Fit to Network", this);
    fitAction_->setShortcut(Qt::Key_F);
    connect(fitAction_, &QAction::triggered, this, &MainWindow::onFitToNetwork);

    zoomInAction_ = new QAction("Zoom &In", this);
    zoomInAction_->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction_, &QAction::triggered, mapWidget_, &MapWidget::zoomIn);

    zoomOutAction_ = new QAction("Zoom &Out", this);
    zoomOutAction_->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction_, &QAction::triggered, mapWidget_, &MapWidget::zoomOut);

    // While on, a left-button drag only moves the map: it selects nothing and
    // keeps the selection and its halo
    panModeAction_ = new QAction("&Pan Map", this);
    panModeAction_->setCheckable(true);
    panModeAction_->setShortcut(Qt::Key_H);
    connect(panModeAction_, &QAction::toggled, mapWidget_, &MapWidget::setPanMode);

    // Find a link, a person or a vehicle by its MATSim id. The box sits in
    // the tab row of the ribbon, at the right (setupRibbon); Ctrl+F puts the
    // cursor in it.
    searchEdit_ = new SearchBox("Find link, person or vehicle ID (Ctrl+F)",
                                "Find ID (Ctrl+F)", this);
    searchEdit_->setToolTip("Type a MATSim link, person or vehicle ID and press Enter");
    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::onFind);

    findAction_ = new QAction("&Find a Link, Person or Vehicle", this);
    findAction_->setShortcut(QKeySequence::Find);
    connect(findAction_, &QAction::triggered, this, [this]() {
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    });

    // Export
    exportPdfAction_ = new QAction("Export to &PDF...", this);
    exportPdfAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(exportPdfAction_, &QAction::triggered, this, &MainWindow::onExportPdf);

    exportPngAction_ = new QAction("Export to P&NG...", this);
    exportPngAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(exportPngAction_, &QAction::triggered, this, &MainWindow::onExportPng);

    // Video recording
    startRecordingAction_ = new QAction("&Record Video...", this);
    startRecordingAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(startRecordingAction_, &QAction::triggered, this, &MainWindow::onStartRecording);

    stopRecordingAction_ = new QAction("&Stop Recording", this);
    stopRecordingAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    stopRecordingAction_->setEnabled(false);  // Disabled until recording starts
    connect(stopRecordingAction_, &QAction::triggered, this, &MainWindow::onStopRecording);

    // Help
    aboutAction_ = new QAction("&About", this);
    connect(aboutAction_, &QAction::triggered, this, [this]() {
        // Rich text, so that the links open in the browser
        QMessageBox box(this);
        box.setWindowTitle("About TAREEK-Vis");
        box.setTextFormat(Qt::RichText);
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);
        box.setText(
            "<h3>TAREEK-Vis - Simulation Visualizer</h3>"
            "<p>Version " TAREEK_VIS_VERSION "<br>"
            "A visualization tool for MATSim simulation outputs.</p>"
            "<p>Maintained by <a href='https://jalal1.github.io/'>Jalal Khalil</a>.<br>"
            "Contributions are welcome and encouraged: "
            "<a href='https://github.com/jalal1/TAREEK-Vis'>github.com/jalal1/TAREEK-Vis</a></p>"
            "<p><b>Privacy:</b> TAREEK-Vis reads your files on this computer only. "
            "It does not send them anywhere and collects no usage data. "
            "It connects to the internet only to download map tiles while "
            "the background map is on. See Help &gt; User Guide &gt; "
            "Privacy and security.</p>"
            "<p>Copyright (c) 2026 TAREEK-Vis Contributors. Licensed under the "
            "<a href='https://www.gnu.org/licenses/gpl-3.0.html'>GNU General Public "
            "License v3.0</a>, without warranty.</p>"
            "<p>Third-party libraries: Qt6 (LGPL v3.0), PugiXML 1.15 (MIT), "
            "ZLIB (Zlib License), FFmpeg (LGPL/GPL, optional), Lucide icons (ISC). "
            "See THIRD_PARTY_LICENSES.md for the full license texts.</p>");
        box.exec();
    });
}

void MainWindow::setupRibbon() {
    // Playback. The timeline owns the state; these actions give it a button
    // and a shortcut on the ribbon.
    playPauseAction_ = new QAction("Play/Pause", this);
    playPauseAction_->setShortcut(Qt::Key_Space);
    connect(playPauseAction_, &QAction::triggered,
            timelineWidget_, &TimelineWidget::togglePlayPause);
    auto showPlaying = [this](bool playing) {
        playPauseAction_->setIconText(playing ? "Pause" : "Play");
        playPauseAction_->setIcon(ribbonIcon(playing ? "pause" : "play"));
    };
    showPlaying(timelineWidget_->isPlaying());
    connect(timelineWidget_, &TimelineWidget::playingChanged, this, showPlaying);

    goToStartAction_ = new QAction("Go to Start", this);
    goToStartAction_->setShortcut(Qt::Key_Home);
    goToStartAction_->setIcon(ribbonIcon("skip-back"));
    connect(goToStartAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setSimulationTime(vehicleIndex_ ? VehicleIndex::toSeconds(vehicleIndex_->minTime()) : 0);
    });

    goToEndAction_ = new QAction("Go to End", this);
    goToEndAction_->setShortcut(Qt::Key_End);
    goToEndAction_->setIcon(ribbonIcon("skip-forward"));
    connect(goToEndAction_, &QAction::triggered, this, [this]() {
        mapWidget_->setSimulationTime(vehicleIndex_ ? VehicleIndex::toSeconds(vehicleIndex_->maxTime()) : 0);
    });

    // The speeds the timeline offers. The timeline keeps the current speed
    // and tells the map and this menu when it changes.
    speedMenu_ = new QMenu("Speed", this);
    speedMenu_->menuAction()->setIcon(ribbonIcon("gauge"));
    auto* speedGroup = new QActionGroup(this);
    for (double speed : timelineWidget_->playbackSpeeds()) {
        auto* action = speedMenu_->addAction(QString("%1x").arg(speed));
        action->setCheckable(true);
        action->setData(speed);
        action->setActionGroup(speedGroup);
        connect(action, &QAction::triggered, this, [this, speed]() {
            timelineWidget_->setPlaybackSpeed(speed);
        });
    }
    auto showSpeed = [this](double speed) {
        for (QAction* action : speedMenu_->actions()) {
            action->setChecked(action->data().toDouble() == speed);
        }
        speedMenu_->menuAction()->setIconText(QString("Speed %1x").arg(speed));
    };
    showSpeed(timelineWidget_->playbackSpeed());
    connect(timelineWidget_, &TimelineWidget::playbackSpeedChanged, this, showSpeed);

    // Icons, and shorter labels where the menu text is long
    openFolderAction_->setIcon(ribbonIcon("folder-open"));
    openFolderAction_->setIconText("Open\nFolder");
    recentFoldersMenu_->menuAction()->setIcon(ribbonIcon("history"));
    recentFoldersMenu_->menuAction()->setIconText("Recent\nFolders");
    openFilesAction_->setIcon(ribbonIcon("files"));
    openFilesAction_->setIconText("Open Files");
    loadCountsAction_->setIcon(ribbonIcon("chart-column"));
    rebuildCacheAction_->setIcon(ribbonIcon("refresh-cw"));
    fitAction_->setIcon(ribbonIcon("maximize"));
    fitAction_->setIconText("Fit to\nNetwork");
    zoomInAction_->setIcon(ribbonIcon("zoom-in"));
    zoomOutAction_->setIcon(ribbonIcon("zoom-out"));
    panModeAction_->setIcon(ribbonIcon("hand"));
    panModeAction_->setIconText("Pan");

    ribbon_ = new RibbonBar(this);
    ribbon_->setSearchWidget(searchEdit_);
    // Ctrl+F has no button: the search box is its control. The ribbon holds
    // the action so that the shortcut works.
    ribbon_->addAction(findAction_);

    auto* home = ribbon_->addTab("Home");

    auto* data = home->addGroup("Data");
    data->addLargeAction(openFolderAction_);
    data->addLargeAction(recentFoldersMenu_->menuAction());
    data->addSmallAction(openFilesAction_);
    data->addSmallAction(loadCountsAction_);
    data->addSmallAction(rebuildCacheAction_);

    auto* playback = home->addGroup("Playback");
    playback->addLargeAction(playPauseAction_);
    playback->addSmallAction(goToStartAction_);
    playback->addSmallAction(goToEndAction_);
    playback->addSmallAction(speedMenu_->menuAction());

    auto* navigate = home->addGroup("Navigate");
    navigate->addLargeAction(fitAction_);
    navigate->addLargeAction(panModeAction_);
    navigate->addSmallAction(zoomInAction_);
    navigate->addSmallAction(zoomOutAction_);

    // --- View tab ---
    showNodesAction_->setIcon(ribbonIcon("circle-dot"));
    showNodesAction_->setIconText("Nodes");
    showLinksAction_->setIcon(ribbonIcon("spline"));
    showLinksAction_->setIconText("Links");
    showCountsAction_->setIcon(ribbonIcon("hash"));
    showCountsAction_->setIconText("Link Counts");
    showInfoPanelAction_->setIcon(ribbonIcon("panel-left"));
    showInfoPanelAction_->setIconText("Info\nPanel");
    showVehiclesAction_->setIcon(ribbonIcon("eye"));
    showVehiclesAction_->setIconText("All\nVehicles");
    showCarsAction_->setIcon(ribbonIcon("car"));
    showCarsAction_->setIconText("Private");
    showBusVehiclesAction_->setIcon(ribbonIcon("bus"));
    showBusVehiclesAction_->setIconText("Bus");
    showTramVehiclesAction_->setIcon(ribbonIcon("tram-front"));
    showTramVehiclesAction_->setIconText("Tram");
    showRailVehiclesAction_->setIcon(ribbonIcon("train-front"));
    showRailVehiclesAction_->setIconText("Rail");
    vehicleShapeMenu_->menuAction()->setIcon(ribbonIcon("shapes"));
    vehicleShapeMenu_->menuAction()->setIconText("Shape");
    vehicleSizeMenu_->menuAction()->setIcon(ribbonIcon("ruler"));
    vehicleSizeMenu_->menuAction()->setIconText("Size");
    vehicleColorMenu_->menuAction()->setIcon(ribbonIcon("palette"));
    vehicleColorMenu_->menuAction()->setIconText("Color");
    showBackgroundMapAction_->setIcon(ribbonIcon("map"));
    showBackgroundMapAction_->setIconText("Background\nMap");

    // The tile sources, for the Source button beside the on/off button.
    // The actions keep their action group, so one source stays selected.
    tileSourceMenu_ = new QMenu("Map &Source", this);
    tileSourceMenu_->addAction(tileSourceOsmAction_);
    tileSourceMenu_->addAction(tileSourceSatAction_);
    tileSourceMenu_->addAction(tileSourceTopoAction_);
    tileSourceMenu_->menuAction()->setIcon(ribbonIcon("layers"));
    tileSourceMenu_->menuAction()->setIconText("Source");

    auto* view = ribbon_->addTab("View");

    auto* layers = view->addGroup("Layers");
    layers->addSmallAction(showNodesAction_);
    layers->addSmallAction(showLinksAction_);
    layers->addSmallAction(showCountsAction_);
    layers->addLargeAction(showInfoPanelAction_);

    otherModesMenu_->menuAction()->setIcon(ribbonIcon("list-checks"));
    otherModesMenu_->menuAction()->setIconText("Other\nModes");

    auto* vehicles = view->addGroup("Vehicles");
    vehicles->addLargeAction(showVehiclesAction_);
    vehicles->addSmallAction(showCarsAction_);
    vehicles->addSmallAction(showBusVehiclesAction_);
    vehicles->addSmallAction(showTramVehiclesAction_);
    vehicles->addSmallAction(showRailVehiclesAction_);
    vehicles->addLargeAction(otherModesMenu_->menuAction());

    auto* vehicleStyle = view->addGroup("Vehicle Style");
    vehicleStyle->addSmallAction(vehicleShapeMenu_->menuAction());
    vehicleStyle->addSmallAction(vehicleSizeMenu_->menuAction());
    vehicleStyle->addSmallAction(vehicleColorMenu_->menuAction());

    auto* backgroundMap = view->addGroup("Background Map");
    backgroundMap->addLargeAction(showBackgroundMapAction_);
    backgroundMap->addSmallAction(tileSourceMenu_->menuAction());

    // --- Transit tab ---
    showBusRoutesAction_->setIcon(ribbonIcon("bus"));
    showBusRoutesAction_->setIconText("Bus");
    showTramRoutesAction_->setIcon(ribbonIcon("tram-front"));
    showTramRoutesAction_->setIconText("Tram");
    showRailRoutesAction_->setIcon(ribbonIcon("train-front"));
    showRailRoutesAction_->setIconText("Rail");
    showBusStopsAction_->setIcon(ribbonIcon("map-pin"));
    showBusStopsAction_->setIconText("Bus\nStops");
    routeThicknessMenu_->menuAction()->setIcon(ribbonIcon("pen-line"));
    routeThicknessMenu_->menuAction()->setIconText("Route\nThickness");

    // --- Air tab ---
    showAirLinksAction_->setIcon(ribbonIcon("spline"));
    showAirLinksAction_->setIconText("Air\nLinks");
    showVertiportsAction_->setIcon(ribbonIcon("map-pin"));
    showVertiportsAction_->setIconText("Vertiports");
    showTrailsAction_->setIcon(ribbonIcon("history"));
    showTrailsAction_->setIconText("Trails");
    altitudeLiftMenu_->menuAction()->setIcon(ribbonIcon("chart-line"));
    altitudeLiftMenu_->menuAction()->setIconText("Altitude\nLift");
    altitudeBandMenu_->menuAction()->setIcon(ribbonIcon("layers"));
    altitudeBandMenu_->menuAction()->setIconText("Altitude\nBand");

    auto* air = ribbon_->addTab("Air");
    auto* airLayers = air->addGroup("Layers");
    airLayers->addLargeAction(showAirLinksAction_);
    airLayers->addSmallAction(showVertiportsAction_);
    airLayers->addSmallAction(showTrailsAction_);
    auto* airHeight = air->addGroup("Height");
    airHeight->addLargeAction(altitudeLiftMenu_->menuAction());
    airHeight->addLargeAction(altitudeBandMenu_->menuAction());
    showLossesAction_->setIcon(ribbonIcon("x"));
    showLossesAction_->setIconText("Losses");
    showHoldsAction_->setIcon(ribbonIcon("loader-circle"));
    showHoldsAction_->setIconText("Holds");
    auto* airEvents = air->addGroup("Events");
    airEvents->addLargeAction(showLossesAction_);
    airEvents->addLargeAction(showHoldsAction_);
    ribbon_->setTabVisible("Air", false);

    auto* transit = ribbon_->addTab("Transit");

    auto* routes = transit->addGroup("Routes");
    routes->addLargeAction(showBusRoutesAction_);
    routes->addLargeAction(showTramRoutesAction_);
    routes->addLargeAction(showRailRoutesAction_);

    auto* stops = transit->addGroup("Stops");
    stops->addLargeAction(showBusStopsAction_);

    auto* transitStyle = transit->addGroup("Style");
    transitStyle->addLargeAction(routeThicknessMenu_->menuAction());

    // --- Analysis tab ---
    // populateHeatmapMenu() rebuilds this menu for each scenario and enables
    // it; the button follows, because it shows the menu's own action.
    heatmapMenu_->menuAction()->setIcon(ribbonIcon("flame"));
    heatmapMenu_->menuAction()->setIconText("Activity\nHeatmap");

    auto* analysis = ribbon_->addTab("Analysis");

    auto* heatmap = analysis->addGroup("Heatmap");
    heatmap->addLargeAction(heatmapMenu_->menuAction());

    exportCountChartsAction_ = new QAction("Export Count Charts...", this);
    exportCountChartsAction_->setIcon(ribbonIcon("chart-column"));
    exportCountChartsAction_->setIconText("Export Count\nCharts");
    exportCountChartsAction_->setToolTip("Save the chart of each count station, and a "
                                         "table of the totals, the ratio and the GEH");
    exportCountChartsAction_->setEnabled(false);  // Enabled after counts loaded
    connect(exportCountChartsAction_, &QAction::triggered, this, &MainWindow::exportCountCharts);

    auto* countsGroup = analysis->addGroup("Counts");
    countsGroup->addLargeAction(exportCountChartsAction_);

    // --- Scenario tab ---
    showSelectedLinksAction_->setIcon(ribbonIcon("list-checks"));
    showSelectedLinksAction_->setIconText("Selected\nLinks");

    // The Clear and Export buttons of the panel, as actions for the ribbon
    clearSelectionAction_ = new QAction("Clear Selection", this);
    clearSelectionAction_->setIcon(ribbonIcon("eraser"));
    clearSelectionAction_->setEnabled(!scenarioPanel_->isEmpty());
    connect(clearSelectionAction_, &QAction::triggered,
            scenarioPanel_, &ScenarioPanel::clearLinks);
    connect(scenarioPanel_, &ScenarioPanel::linkCountChanged, this, [this](int count) {
        clearSelectionAction_->setEnabled(count > 0);
    });

    exportScenarioAction_ = new QAction("Export Scenario File...", this);
    exportScenarioAction_->setIcon(ribbonIcon("file-output"));
    exportScenarioAction_->setIconText("Export\nScenario");
    // The scenario name and the closure type are set in the panel, so the
    // panel opens first
    connect(exportScenarioAction_, &QAction::triggered, this, [this]() {
        sidePanel_->showPage(selectedLinksPage_);
        scenarioPanel_->onExport();
    });

    auto* scenario = ribbon_->addTab("Scenario");

    auto* selection = scenario->addGroup("Selection");
    selection->addLargeAction(showSelectedLinksAction_);
    selection->addSmallAction(clearSelectionAction_);

    auto* scenarioExport = scenario->addGroup("Export");
    scenarioExport->addLargeAction(exportScenarioAction_);

    // --- Views tab ---
    saveViewAction_ = new QAction("Save &View", this);
    saveViewAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    saveViewAction_->setIcon(ribbonIcon("bookmark-plus"));
    saveViewAction_->setIconText("Save\nView");
    saveViewAction_->setToolTip("Save the time, the map position, the layers, "
                                "the selected links and the heatmap as a view (Ctrl+B)");
    saveViewAction_->setEnabled(false);  // Enabled after network+events loaded
    connect(saveViewAction_, &QAction::triggered, this, &MainWindow::saveView);

    saveOverTimeAction_ = new QAction("Save Over Time...", this);
    saveOverTimeAction_->setIcon(ribbonIcon("timer"));
    saveOverTimeAction_->setToolTip("Save a view of the current map position at each "
                                    "time, for example every hour from 06:00 to 20:00");
    saveOverTimeAction_->setEnabled(false);
    connect(saveOverTimeAction_, &QAction::triggered, this, &MainWindow::saveViewsOverTime);

    captureViewsAction_ = new QAction("Capture Views...", this);
    captureViewsAction_->setIcon(ribbonIcon("images"));
    captureViewsAction_->setIconText("Capture\nViews");
    captureViewsAction_->setToolTip("Save a PNG image of each view");
    captureViewsAction_->setEnabled(false);
    connect(captureViewsAction_, &QAction::triggered, this, &MainWindow::captureViews);

    showViewsAction_->setIcon(ribbonIcon("bookmark"));
    showViewsAction_->setIconText("Views\nPanel");

    importViewsAction_ = new QAction("Import Views...", this);
    importViewsAction_->setIcon(ribbonIcon("file-input"));
    importViewsAction_->setToolTip("Add the views of a views file to this run");
    importViewsAction_->setEnabled(false);
    connect(importViewsAction_, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, "Import Views", QFileInfo(networkFilePath_).absolutePath(),
            "Views Files (*.json);;All Files (*)");
        if (path.isEmpty()) return;
        const int before = viewsPanel_->count();
        QString error;
        if (!viewsPanel_->importFrom(path, &error)) {
            QMessageBox::warning(this, "Import Views",
                QString("Cannot import the views.\n%1").arg(error));
            return;
        }
        sidePanel_->showPage(viewsPage_);
        statusLabel_->setText(QString("Imported %1 view(s)").arg(viewsPanel_->count() - before));
    });

    exportViewsAction_ = new QAction("Export Views...", this);
    exportViewsAction_->setIcon(ribbonIcon("file-output"));
    exportViewsAction_->setToolTip("Save the views of this run to a file, to send to someone");
    exportViewsAction_->setEnabled(false);
    connect(exportViewsAction_, &QAction::triggered, this, [this]() {
        const QString suggested = QDir(QFileInfo(networkFilePath_).absolutePath())
            .absoluteFilePath(experimentName_.isEmpty() ? QString("views.json")
                                                        : experimentName_ + "-views.json");
        const QString path = QFileDialog::getSaveFileName(
            this, "Export Views", suggested, "Views Files (*.json)");
        if (path.isEmpty()) return;
        QString error;
        if (!viewsPanel_->exportTo(path, &error)) {
            QMessageBox::warning(this, "Export Views",
                QString("Cannot save the views.\n%1").arg(error));
            return;
        }
        statusLabel_->setText(QString("Exported %1 view(s) to %2")
            .arg(viewsPanel_->count()).arg(QDir::toNativeSeparators(path)));
    });
    connect(viewsPanel_, &ViewsPanel::countChanged, this, [this](int count) {
        exportViewsAction_->setEnabled(count > 0);
        captureViewsAction_->setEnabled(count > 0);
    });

    auto* viewsTab = ribbon_->addTab("Views");

    auto* viewsGroup = viewsTab->addGroup("Views");
    viewsGroup->addLargeAction(saveViewAction_);
    viewsGroup->addSmallAction(saveOverTimeAction_);
    viewsGroup->addLargeAction(showViewsAction_);

    auto* capture = viewsTab->addGroup("Capture");
    capture->addLargeAction(captureViewsAction_);

    auto* share = viewsTab->addGroup("Share");
    share->addSmallAction(importViewsAction_);
    share->addSmallAction(exportViewsAction_);

    // --- Export tab ---
    exportPngAction_->setIcon(ribbonIcon("image"));
    exportPngAction_->setIconText("PNG");
    exportPdfAction_->setIcon(ribbonIcon("file-text"));
    exportPdfAction_->setIconText("PDF");
    startRecordingAction_->setIcon(ribbonIcon("video"));
    startRecordingAction_->setIconText("Record");
    stopRecordingAction_->setIcon(ribbonIcon("square"));
    stopRecordingAction_->setIconText("Stop");

    auto* exportTab = ribbon_->addTab("Export");

    auto* image = exportTab->addGroup("Image");
    image->addLargeAction(exportPngAction_);
    image->addLargeAction(exportPdfAction_);

    auto* video = exportTab->addGroup("Video");
    video->addLargeAction(startRecordingAction_);
    video->addLargeAction(stopRecordingAction_);

    // --- Help tab ---
    aboutAction_->setIcon(ribbonIcon("info"));

    // The list is made from the actions, so it cannot go out of date
    keyboardShortcutsAction_ = new QAction("&Keyboard Shortcuts", this);
    keyboardShortcutsAction_->setIcon(ribbonIcon("keyboard"));
    keyboardShortcutsAction_->setIconText("Keyboard\nShortcuts");
    connect(keyboardShortcutsAction_, &QAction::triggered, this, [this]() {
        QString rows;
        for (const QAction* action : ribbon_->actions()) {
            if (action->shortcut().isEmpty()) continue;
            QString text = action->text();
            text.remove(QLatin1Char('&'));
            if (text.endsWith(QLatin1String("..."))) text.chop(3);
            rows += QString("<tr><td><b>%1</b></td><td>%2</td></tr>")
                .arg(action->shortcut().toString(QKeySequence::NativeText).toHtmlEscaped(),
                     text.toHtmlEscaped());
        }
        rows += "<tr><td><b>Del</b></td><td>Remove the selected rows from the "
                "Selected Links panel</td></tr>";
        QMessageBox box(this);
        box.setWindowTitle("Keyboard Shortcuts");
        box.setTextFormat(Qt::RichText);
        box.setText("<table cellspacing='0' cellpadding='3'>" + rows + "</table>");
        box.exec();
    });

    // F1 opens the page about the tab in front. The pages are in help/.
    userGuideAction_ = new QAction("&User Guide", this);
    userGuideAction_->setShortcut(QKeySequence::HelpContents);
    userGuideAction_->setIcon(ribbonIcon("book-open"));
    userGuideAction_->setIconText("User\nGuide");
    connect(userGuideAction_, &QAction::triggered, this, [this]() {
        static const QHash<QString, QString> pageOfTab = {
            {"Home", "getting-started.md"},
            {"View", "navigate-the-map.md"},
            {"Transit", "transit.md"},
            {"Air", "air-traffic.md"},
            {"Analysis", "heatmap.md"},
            {"Scenario", "scenarios.md"},
            {"Views", "views.md"},
            {"Export", "export.md"},
        };
        if (!helpWindow_) helpWindow_ = new HelpWindow(this);
        helpWindow_->showPage(pageOfTab.value(ribbon_->currentTabTitle(), "index.md"));
    });

    reportProblemAction_ = new QAction("Report a &Problem", this);
    reportProblemAction_->setIcon(ribbonIcon("bug"));
    reportProblemAction_->setIconText("Report a\nProblem");
    reportProblemAction_->setToolTip("Open the issue page of TAREEK-Vis on GitHub");
    connect(reportProblemAction_, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/jalal1/TAREEK-Vis/issues/new"));
    });

    auto* help = ribbon_->addTab("Help");

    auto* helpGroup = help->addGroup("Help");
    helpGroup->addLargeAction(userGuideAction_);
    helpGroup->addLargeAction(keyboardShortcutsAction_);
    helpGroup->addLargeAction(reportProblemAction_);
    helpGroup->addLargeAction(aboutAction_);

    // The ribbon takes the place of the menu bar
    setMenuWidget(ribbon_);
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
                    "Use Home > Open Files to pick files manually.")
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
    infoShowsPerson_ = false;
    foundVehiclePlus1_ = 0;
    foundPersonPlus1_ = 0;
    if (infoPanel_) {
        infoPanel_->resetPersonRouteState();
        infoPanel_->clear();
    }

    // The views belong to the old run
    viewsPanel_->setRunFolder(QString());
    saveViewAction_->setEnabled(false);
    saveOverTimeAction_->setEnabled(false);
    importViewsAction_->setEnabled(false);

    // Playback starts again, paused, at the new scenario's start time.
    timelineWidget_->setPlaying(false);

    // Counts, transit and the CRS describe the old network. A new scenario
    // without them must not show the old ones.
    countsData_.reset();
    mapWidget_->setCountsData(nullptr);
    showCountsAction_->setChecked(false);
    showCountsAction_->setEnabled(false);
    exportCountChartsAction_->setEnabled(false);
    transitData_.reset();
    mapWidget_->setTransitData(nullptr);
    networkCrs_ = CRSInfo();
}

void MainWindow::hideSidePanel() {
    sidePanel_->hide();
    // Nothing is inspected while the panel is closed
    selectedLinkId_ = kNoLink;
    infoPanel_->clear();
}

void MainWindow::updateSidePanelActions() {
    const bool shown = !sidePanel_->isHidden();
    const int page = sidePanel_->currentPage();
    showInfoPanelAction_->setChecked(shown && page == infoPage_);
    showSelectedLinksAction_->setChecked(shown && page == selectedLinksPage_);
    showViewsAction_->setChecked(shown && page == viewsPage_);
}

void MainWindow::checkBackgroundMapCrs() {
    if (showBackgroundMapAction_->isChecked() && !networkCrs_.isValid()) {
        showBackgroundMapAction_->setChecked(false);
        statusBar()->showMessage(
            tr("Background map turned off: this network has no known CRS"), 5000);
    }
}

void MainWindow::loadFromPaths(const QString& networkPath, const QString& eventsPath,
                               const QString& transitPath, bool rebuildCache) {
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

    // Remember this folder for Home > Recent Folders. A scene capture is not
    // the user's own work, so it stays out of the list.
    if (!captureMode_) addRecentFolder(networkInfo.absolutePath());

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
        // The same for the network: readBinaryHeader gives an empty header
        // for a file of another version
        if (cacheValid &&
            NetworkParser::readBinaryHeader(networkBin.toStdString()).magic != NETWORK_FILE_MAGIC) {
            cacheValid = false;
            LOG_INFO("Cache .bin is of an older version - will re-preprocess");
        }

        LOG_INFO(QString("Cache check: bin=%1 vidx=%2 valid=%3")
            .arg(QFileInfo::exists(networkBin) ? "yes" : "no")
            .arg(QFileInfo::exists(vidxPath) ? "yes" : "no")
            .arg(cacheValid ? "yes" : "no"));
    } else {
        LOG_INFO("Cache files missing - will preprocess");
    }

    // Home > Rebuild Cache. Remember it only when there is a cache to fall
    // back on.
    rebuildingCache_ = rebuildCache && cacheValid;
    if (rebuildCache) {
        LOG_INFO("Cache rebuild requested - will re-preprocess");
        cacheValid = false;
    }

    if (cacheValid) {
        // Load cached .bin and .vidx, the CRS sidecar and the transit
        // schedule (not cached), on a worker thread
        loadBinaryFiles(true);
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

    // Progress card. It closes by itself on success (accepted: the run
    // loads); after a failure the user closes it (rejected: nothing loads).
    auto* progressDialog = new ProgressDialog(this);
    progressDialog->setAttribute(Qt::WA_DeleteOnClose);

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
            checkBackgroundMapCrs();
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
        rebuildingCache_ = false;
        loadBinaryFiles(false);
    });
    // A rebuild that failed or was cancelled: load the cache again. The .vidx
    // is written last, after every check for a cancel, so the old one is
    // still there; the .bin is the old one or a new one of the same network.
    connect(progressDialog, &QDialog::rejected,
            this, [this]() {
        if (!rebuildingCache_) return;
        rebuildingCache_ = false;
        loadBinaryFiles(true);
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

void MainWindow::loadBinaryFiles(bool fromCache) {
    QString networkBaseName = QFileInfo(networkFilePath_).completeBaseName();
    if (networkBaseName.endsWith(".xml")) {
        networkBaseName = networkBaseName.left(networkBaseName.length() - 4);
    }
    QString eventsBaseName = QFileInfo(eventsFilePath_).completeBaseName();
    if (eventsBaseName.endsWith(".xml")) {
        eventsBaseName = eventsBaseName.left(eventsBaseName.length() - 4);
    }

    const QString networkBin = cacheDirectory_ + "/" + networkBaseName + ".bin";
    const QString vidxPath = cacheDirectory_ + "/" + eventsBaseName + ".vidx";
    // From the cache, the transit schedule is read here too (it is not
    // cached). After preprocessing, the preprocessor has read it already.
    const QString transitPath = fromCache ? transitSchedulePath_ : QString();

    // Internal link ids mean nothing in the network about to load. Keep the
    // inspected link by its MATSim id, so the same road opens in this run, and
    // drop the scenario selection: it belongs to the old baseline.
    carriedLinkId_.clear();
    if (selectedLinkId_ != kNoLink && networkIndex_->getLink(selectedLinkId_))
        carriedLinkId_ = linkOriginalId(selectedLinkId_).toStdString();
    selectedLinkId_ = kNoLink;
    scenarioPanel_->clearLinks();

    LOG_INFO(QString("Loading network binary: %1").arg(networkBin));
    LOG_INFO(QString("Loading vehicle index: %1").arg(vidxPath));
    statusLabel_->setText("Loading...");
    loadingIndicator_->start(tr("Reading the network..."));

    // The files are read into new objects on a worker thread, so the window
    // stays live and the indicator turns. The objects on screen stay in use
    // until the new ones are complete. A newer load raises the generation, and
    // this one's result is then dropped.
    const uint64_t generation = ++loadGeneration_;
    QPointer<LoadingIndicator> indicator = loadingIndicator_;
    auto step = [indicator](const QString& text) {
        QMetaObject::invokeMethod(indicator, [indicator, text]() {
            if (indicator) indicator->setText(text);
        }, Qt::QueuedConnection);
    };
    QThread* uiThread = thread();

    auto* watcher = new QFutureWatcher<std::shared_ptr<LoadedFiles>>(this);
    connect(watcher, &QFutureWatcher<std::shared_ptr<LoadedFiles>>::finished, this,
            [this, watcher, generation, fromCache, vidxPath]() {
        const std::shared_ptr<LoadedFiles> files = watcher->result();
        watcher->deleteLater();
        if (generation != loadGeneration_) return;  // a newer load replaced it
        loadingIndicator_->stop();
        applyLoadedFiles(*files, fromCache, vidxPath);
    });

    watcher->setFuture(QtConcurrent::run(
        [networkBin, vidxPath, transitPath, step, uiThread]() {
            auto files = std::make_shared<LoadedFiles>();

            auto network = std::make_shared<NetworkIndex>();
            if (!network->loadFile(networkBin)) {
                files->error = "Failed to load network file.";
                return files;
            }
            // Made on this thread, used on the UI thread from now on
            network->moveToThread(uiThread);
            files->network = network;

            step(QObject::tr("Reading the vehicles..."));
            auto vehicles = std::make_shared<VehicleIndex>();
            if (!vehicles->loadFile(vidxPath)) {
                files->error = "Failed to load vehicle index file.";
                return files;
            }
            files->vehicles = vehicles;

            if (!transitPath.isEmpty()) {
                step(QObject::tr("Reading the transit schedule..."));
                const std::string path = transitPath.toStdString();
                files->transit = std::make_unique<TransitScheduleParser::Result>(
                    transitPath.endsWith(".gz", Qt::CaseInsensitive)
                        ? TransitScheduleParser::parseGzipFile(path, network->linkIds(), nullptr)
                        : TransitScheduleParser::parse(path, network->linkIds(), nullptr));
            }
            return files;
        }));
}

void MainWindow::applyLoadedFiles(LoadedFiles& files, bool fromCache, const QString& vidxPath) {
    if (!files.error.isEmpty()) {
        LOG_ERROR(files.error);
        statusLabel_->setText(files.error);
        QMessageBox::critical(this, "Error", files.error);
        return;
    }

    // The new run replaces the old one. Anything still reading the old
    // objects (a volume scan, a density map) holds its own reference.
    networkIndex_ = files.network;
    vehicleIndex_ = files.vehicles;

    {
        QDir dir(QFileInfo(networkFilePath_).absolutePath());
        if (dir.dirName().compare("output", Qt::CaseInsensitive) == 0) dir.cdUp();
        experimentName_ = dir.dirName();
        scenarioPanel_->setBaseExperiment(experimentName_);
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
    rebuildCacheAction_->setEnabled(true);

    // The saved views of this run, from the file beside the network file
    viewsPanel_->setRunFolder(QFileInfo(networkFilePath_).absolutePath());
    saveViewAction_->setEnabled(true);
    saveOverTimeAction_->setEnabled(true);
    importViewsAction_->setEnabled(true);

    // Offer a heatmap for each activity type this scenario contains. Anything
    // cached before the events index was written describes older data, so that
    // file's timestamp is the freshness mark for every cached map.
    heatmapSourceTime_ = QFileInfo(vidxPath).lastModified();
    populateHeatmapMenu();
    populateOtherModesMenu();
    populateAltitudeBandMenu();
    ribbon_->setTabVisible("Air", mapWidget_->hasAirVehicles());

    // Compute the maps that are not cached yet, in the background, so the first
    // click on a type is instant. Deferred so the window is shown and painted
    // first, and it runs one map at a time on a worker thread.
    // Not in a scene capture: the worker would share the CPU with the timed
    // frames.
    if (!captureMode_)
        QTimer::singleShot(0, this, [this]() { startHeatmapPrecompute(); });

    // Fit view to network
    mapWidget_->fitToNetwork();

    // Hourly link volumes for the info panel, on a worker thread. The scan
    // touches every segment in the scenario, so it must not block the load.
    if (!captureMode_) startVolumeAggregation();

    // After the scan has started, so the panel waits for this run's volumes
    // rather than showing the last run's.
    if (!carriedLinkId_.empty()) {
        const uint32_t id = networkIndex_->findLinkByOriginalId(carriedLinkId_);
        if (id != UINT32_MAX) {
            onNetworkLinkClicked(id);
            zoomToLink(id);
        } else {
            statusLabel_->setText(statusLabel_->text() +
                QString("  |  Link %1 is not in this run.")
                    .arg(QString::fromStdString(carriedLinkId_)));
        }
    }

    // From the cache, the CRS comes from the sidecar file and the transit
    // schedule was read with the other files
    if (fromCache) {
        loadCachedCRS(cacheDirectory_ + "/network.crs");
        checkBackgroundMapCrs();
        if (files.transit) {
            if (files.transit->success) {
                LOG_INFO(QString("Transit parsed: %1 stops, %2 lines")
                    .arg(files.transit->stops.size()).arg(files.transit->lines.size()));
                transitData_ = std::move(files.transit);
            } else {
                LOG_ERROR(QString("Transit parse failed: %1")
                    .arg(QString::fromStdString(files.transit->errorMessage)));
                QMessageBox::warning(this, "Transit Schedule",
                    QString("Failed to parse transit schedule:\n%1")
                        .arg(QString::fromStdString(files.transit->errorMessage)));
            }
        }
    }
    applyTransitData();
    emit runLoaded();
}

void MainWindow::populateOtherModesMenu() {
    // One checkable entry for each mode after the four with their own buttons
    // (car, bus, rail, tram) that has vehicles in this run. A new run shows
    // all of them again.
    otherModesMenu_->clear();
    std::vector<size_t> perMode(modes::all().size(), 0);
    for (uint8_t id : vehicleIndex_->vehicleModes_)
        if (id < perMode.size()) ++perMode[id];
    for (const TrafficMode& mode : modes::all()) {
        if (mode.id <= modes::kTram) continue;
        mapWidget_->setModeVisible(mode.id, true);
        if (perMode[mode.id] == 0) continue;
        QAction* action = otherModesMenu_->addAction(
            QString("%1 (%2)").arg(QString::fromUtf8(mode.name)).arg(perMode[mode.id]));
        action->setCheckable(true);
        action->setChecked(true);
        const uint8_t id = mode.id;
        connect(action, &QAction::toggled, this, [this, id](bool checked) {
            mapWidget_->setModeVisible(id, checked);
        });
    }
    otherModesMenu_->setEnabled(!otherModesMenu_->isEmpty());
}

void MainWindow::populateAltitudeBandMenu() {
    // "All Heights", then bands of 300 m up to the highest node of the run.
    // A new run shows all heights again.
    altitudeBandMenu_->clear();
    auto* group = new QActionGroup(altitudeBandMenu_);
    auto addBand = [this, group](const QString& text, float minZ, float maxZ, bool checked) {
        QAction* action = altitudeBandMenu_->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this, [this, minZ, maxZ]() {
            mapWidget_->setAltitudeBand(minZ, maxZ);
        });
    };
    addBand("All Heights", -1e9f, 1e9f, true);
    mapWidget_->setAltitudeBand(-1e9f, 1e9f);
    constexpr float kBandM = 300.0f;
    const float top = mapWidget_->airTopHeight();
    if (top > 0.0f) altitudeBandMenu_->addSeparator();
    for (float low = 0.0f; low < top; low += kBandM) {
        // The lowest band also holds a vehicle a little below the datum
        addBand(QString("%1 to %2 m").arg(low, 0, 'f', 0).arg(low + kBandM, 0, 'f', 0),
                low == 0.0f ? -1e9f : low, low + kBandM, false);
    }
}

void MainWindow::startVolumeAggregation() {
    // Any scan still running describes the previous scenario. Let it finish
    // into a result nobody reads rather than blocking the load on it; the
    // generation stamp tells us which scenario a result belongs to.
    const uint64_t generation = ++volumeGeneration_;

    linkHourlyVolumes_ = LinkHourlyVolumes{};
    linkHourlyVolumesReady_ = false;

    // The worker holds its own reference to the index, so a second load
    // swapping vehicleIndex_ cannot pull the data out from under a live scan.
    std::shared_ptr<const VehicleIndex> index = vehicleIndex_;
    const size_t linkCount = networkIndex_ ? networkIndex_->linkCount() : 0;

    QFuture<LinkHourlyVolumes> future =
        QtConcurrent::run([index, linkCount, generation]() {
            LinkHourlyVolumes result;
            result.generation = generation;
            if (!index || linkCount == 0) return result;

            // Link ids are interned sequentially from 0, so they index this
            // array directly: one allocation instead of one per link, and no
            // hash lookup per segment. On an 8.2M-segment scenario this is
            // ~130ms against ~1.2s for a hash map of per-link vectors.
            result.hours.assign(linkCount * 24, 0);

            const size_t vehicles = index->vehicleCount();
            for (size_t v = 0; v < vehicles; ++v) {
                const auto* traj = index->trajectory(static_cast<uint32_t>(v));
                if (!traj) continue;

                for (const auto& seg : traj->segments) {
                    if (seg.linkId >= linkCount) continue;

                    // MATSim counts seconds from midnight and routinely runs
                    // past 24:00 - scenarios here end near hour 26 - so fold
                    // back to hour of day. Truncating at 23 silently dropped
                    // every trip after midnight instead of counting it.
                    const uint32_t hour = (seg.enterTime / 3600000u) % 24u;
                    result.hours[static_cast<size_t>(seg.linkId) * 24 + hour]++;
                }
            }
            return result;
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

    // The worker keeps its own references: a scenario change may replace
    // both while it is still finishing
    std::shared_ptr<NkdvNetwork> net = nkdvNetwork_;
    std::shared_ptr<const NetworkIndex> netIndex = networkIndex_;
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

void MainWindow::loadNetworkFile(const QString& path) {
    networkFilePath_ = path;
}

void MainWindow::loadEventsFile(const QString& path) {
    eventsFilePath_ = path;
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

    // The volume scan cannot be cancelled part-way, but it is short. Wait for
    // it rather than let the watcher be destroyed while it is still running,
    // which would leave the future signalling into a dead object.
    if (volumeWatcher_.isRunning()) {
        statusLabel_->setText(tr("Finishing link volume scan..."));
        QApplication::processEvents();
        volumeWatcher_.waitForFinished();
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::onPreprocessingProgress(int percent, const QString& message) {
    statusLabel_->setText(message);
    Q_UNUSED(percent);
}

void MainWindow::onPreprocessingComplete(bool success) {
    if (success) {
        loadBinaryFiles(false);
    }
}

void MainWindow::onSimulationTimeChanged(float time) {
    // The halo of a found object moves with the time
    const bool following = foundVehiclePlus1_ != 0 || foundPersonPlus1_ != 0;
    if (following) updateFoundHalo(false);

    // Keep the vehicle info panel live while tracking (throttled to ~2 Hz of
    // wall time is overkill to compute; throttle on sim time delta instead)
    if ((mapWidget_->isTrackingVehicle() || following) && infoPanel_->isVisible()) {
        if (std::abs(time - lastVehicleInfoRefreshTime_) >= 1.0f) {
            lastVehicleInfoRefreshTime_ = time;
            refreshVehicleInfoPanel();
        }
    }
}

void MainWindow::onVehicleTrackingChanged(uint32_t vehicleId) {
    // The search tracks its vehicle while it is on the road and lets go of it
    // when it stops. The panel keeps what the search put there.
    if (searchTracking_) return;

    if (vehicleId == kNoVehicle) {
        // Tracking stopped (e.g. user clicked empty map to pan). Keep the
        // panel content and any route overlay so panning doesn't wipe them.
        return;
    }

    // A click on a vehicle replaces the found object
    endSearchFollow();
    inspectVehicle(vehicleId);
}

void MainWindow::inspectVehicle(uint32_t vehicleId) {
    // A (new) vehicle: reset route state and remember its person
    infoPanel_->resetPersonRouteState();
    mapWidget_->setPersonRouteVisible(false);
    mapWidget_->setPersonRoute({});
    routePersonPlus1_ = vehicleIndex_ ? vehicleIndex_->vehicleDriver(vehicleId) : 0;
    infoVehicleId_ = vehicleId;
    infoShowsPerson_ = false;

    lastVehicleInfoRefreshTime_ = mapWidget_->simulationTime();
    refreshVehicleInfoPanel();
    sidePanel_->showPage(infoPage_);
}

void MainWindow::inspectPerson(uint32_t personId) {
    infoPanel_->resetPersonRouteState();
    mapWidget_->setPersonRouteVisible(false);
    mapWidget_->setPersonRoute({});
    routePersonPlus1_ = personId + 1;
    infoShowsPerson_ = true;

    // Their own car: the first vehicle of the day that they drive
    infoVehicleId_ = kNoVehicle;
    if (const auto* trips = vehicleIndex_->personTrips(personId)) {
        for (const PersonTrip& trip : *trips) {
            if (trip.vehicleId != 0 &&
                vehicleIndex_->vehicleDriver(trip.vehicleId - 1) == personId + 1) {
                infoVehicleId_ = trip.vehicleId - 1;
                break;
            }
        }
    }

    // A found person shows with their route of the day, as after a click on
    // "Show Person Route"
    infoPanel_->setPersonRouteShown(true);
    lastVehicleInfoRefreshTime_ = mapWidget_->simulationTime();
    refreshVehicleInfoPanel();
    sidePanel_->showPage(infoPage_);
    onShowPersonRouteToggled(true);
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

        float x, y;
        if (!activityPoint(act, x, y)) continue;
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

bool MainWindow::activityPoint(const ActivityRecord& act, float& x, float& y) const {
    if (!(act.flags & ACT_FLAG_DERIVED_COORD)) {
        x = act.x;
        y = act.y;
        return true;
    }
    // Fallback for events that carried no x/y: the midpoint of the activity's
    // link. Closer than either endpoint, though still an approximation.
    if (act.linkId == TRIP_NO_LINK || !networkIndex_) return false;
    const auto* link = networkIndex_->getLink(act.linkId);
    if (!link) return false;
    const auto* from = networkIndex_->getNode(link->fromNode);
    const auto* to = networkIndex_->getNode(link->toNode);
    if (!from || !to) return false;
    x = static_cast<float>((from->x + to->x) * 0.5);
    y = static_cast<float>((from->y + to->y) * 0.5);
    return true;
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

    // The activity a leg leaves from (it ends at the departure) or arrives
    // at (it starts at the arrival), as the activity markers find them
    const auto* actIndices = vehicleIndex_->activitiesForPerson(personId);
    const auto& allActs = vehicleIndex_->activities();
    auto activityAround = [&](TimeMs time, bool endsThen, float& x, float& y) -> bool {
        if (!actIndices || time == TRIP_NO_TIME) return false;
        for (uint32_t idx : *actIndices) {
            const auto& act = allActs[idx];
            if ((endsThen ? act.endTimeMs : act.startTimeMs) == time)
                return activityPoint(act, x, y);
        }
        return false;
    };

    for (int i = 0; i < static_cast<int>(trips->size()); ++i) {
        if (tripIndex >= 0 && i != tripIndex) continue;  // single-trip mode
        const auto& trip = (*trips)[i];
        if (trip.vehicleId != 0) {
            // Vehicle leg: follow the vehicle's trajectory links from the
            // boarding to the arrival (link-by-link path on the network).
            // Not from the departure: on a pt leg the vehicle is elsewhere
            // while the person waits at the stop.
            const TimeMs board = trip.boardTimeMs != TRIP_NO_TIME
                ? trip.boardTimeMs : trip.departTimeMs;
            const auto* traj = vehicleIndex_->trajectory(trip.vehicleId - 1);
            const size_t first = segments.size();
            if (traj) {
                for (const auto& seg : traj->segments) {
                    if (seg.leaveTime <= board) continue;
                    if (trip.arriveTimeMs != TRIP_NO_TIME &&
                        seg.enterTime >= trip.arriveTimeMs) break;
                    float fx, fy, tx, ty;
                    if (linkEndpoints(seg.linkId, fx, fy, tx, ty)) {
                        segments.push_back({fx, fy, tx, ty, false});
                    }
                }
            }
            if (segments.size() > first) {
                // The activities (a stop, a home) are beside the road, not on
                // a node. Short connectors join them to the path, so the legs
                // of the day form one line.
                const auto start = segments[first];
                const auto end = segments.back();
                float x, y;
                if (activityAround(trip.departTimeMs, true, x, y))
                    segments.push_back({x, y, start.x1, start.y1, true});
                if (activityAround(trip.arriveTimeMs, false, x, y))
                    segments.push_back({end.x2, end.y2, x, y, true});
                continue;
            }
            // No trajectory overlap (e.g. pt rider whose vehicle has no
            // movement events): fall through to a straight connector
        }

        // Teleported leg (walk/bike) or fallback: straight from->to connector,
        // between the activities where the markers are
        float ax, ay, bx, by;
        if (activityAround(trip.departTimeMs, true, ax, ay) &&
            activityAround(trip.arriveTimeMs, false, bx, by)) {
            segments.push_back({ax, ay, bx, by, true});
            continue;
        }
        // Without those activities: the links' end nodes
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
    // panel keeps working after tracking stops (e.g. user panned the map).
    // A person found by the search stays in the panel, whatever vehicle the
    // halo is on.
    uint32_t vehicleId = infoShowsPerson_ ? infoVehicleId_ : mapWidget_->trackedVehicle();
    if (vehicleId == kNoVehicle) vehicleId = infoVehicleId_;
    if (!vehicleIndex_) return;
    if (vehicleId == kNoVehicle && !(infoShowsPerson_ && routePersonPlus1_ != 0)) return;

    VehicleInfo info;
    info.currentTime = mapWidget_->simulationTime();
    const TimeMs nowMs = VehicleIndex::toTimeMs(info.currentTime);

    if (vehicleId == kNoVehicle) {
        // A person with no car of their own: their trips only
        fillPersonTrips(info, routePersonPlus1_ - 1, nowMs);
        selectedLinkId_ = kNoLink;
        infoPanel_->showVehicleInfo(info);
        return;
    }

    info.vehicleId = vehicleId;
    info.vehicleName = vehicleIndex_->vehicleIdString(vehicleId);

    const bool isTransit = vehicleIndex_->isTransitVehicle(vehicleId);
    info.mode = QString::fromUtf8(modes::get(vehicleIndex_->modeId(vehicleId)).name);

    // Live state (speed / on-network status)
    const VehicleState* state = mapWidget_->vehicleState(vehicleId);
    if (state && state->active) {
        info.active = true;
        info.speedRatio = state->speedRatio;
        if (networkIndex_) {
            if (const auto* link = networkIndex_->getLink(state->currentLink)) {
                info.speedKmh = state->speedRatio * link->freespeed * 3.6f;
                if (state->is3D) {
                    info.hasHeight = true;
                    info.heightM = state->z;
                    info.verticalRate = state->verticalRate;
                    switch (static_cast<RoadType>(link->roadType)) {
                        case RoadType::AirVertical:
                            info.phase = state->verticalRate >= 0.0f ? "Take-off" : "Landing";
                            break;
                        case RoadType::AirClimb:   info.phase = "Climb"; break;
                        case RoadType::AirCruise:  info.phase = "Cruise"; break;
                        case RoadType::AirDescent: info.phase = "Descent"; break;
                        case RoadType::AirHover:   info.phase = "Hover"; break;
                        default: break;
                    }
                }
            }
        }
    }

    if (isTransit) {
        // Transit vehicle: line/route + the passengers aboard now
        if (const auto* svc = vehicleIndex_->transitServiceInfo(vehicleId)) {
            info.lineName = vehicleIndex_->transitLineString(svc->lineStrId);
            info.routeName = vehicleIndex_->transitRouteString(svc->routeStrId);
        }
        // The driver is aboard too, but is not a passenger
        const uint32_t driver = vehicleIndex_->vehicleDriver(vehicleId);
        for (uint32_t personId : vehicleIndex_->personsAboard(vehicleId, nowMs)) {
            if (personId + 1 == driver) continue;
            info.passengers.push_back({personId, vehicleIndex_->personIdString(personId)});
        }
        info.passengerCount = static_cast<int>(info.passengers.size());
    } else {
        // Car: resolve the driver person and show their day of trips
        uint32_t driver = vehicleIndex_->vehicleDriver(vehicleId);
        if (driver != 0) fillPersonTrips(info, driver - 1, nowMs);
    }

    selectedLinkId_ = kNoLink;
    infoPanel_->showVehicleInfo(info);
}

void MainWindow::fillPersonTrips(VehicleInfo& info, uint32_t personId, TimeMs nowMs) const {
    info.personName = vehicleIndex_->personIdString(personId);
    const auto* trips = vehicleIndex_->personTrips(personId);
    if (!trips) return;
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

void MainWindow::onActiveVehicleCountChanged(size_t count) {
    QString text = QString("Vehicles: %1").arg(count);
    // A run with aircraft: also the aircraft, losses and holds now
    if (mapWidget_->hasAirVehicles()) {
        const MapWidget::AirStatus air = mapWidget_->airStatus();
        text += QString("  |  Aircraft: %1  |  Losses: %2  |  Holds: %3")
                    .arg(air.aircraft).arg(air.losses).arg(air.holds);
    }
    vehicleCountLabel_->setText(text);
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

    selectedLinkId_ = kNoLink;
    infoPanel_->showStopInfo(stop, servingLines);
    sidePanel_->showPage(infoPage_);
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
            selectedLinkId_ = kNoLink;
            infoPanel_->showRouteInfo(line, *best, *transitData_);
            sidePanel_->showPage(infoPage_);
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
        statusLabel_->setText("Counts loading cancelled");
        return;
    }

    if (!loadCounts(countsPath, scaleFactor)) {
        QMessageBox::warning(this, "Load Counts",
            "No valid count locations found in the file.\n"
            "Check that loc_id values match network link IDs.");
    }
}

bool MainWindow::loadCounts(const QString& countsPath, double scaleFactor) {
    if (!networkIndex_ || !vehicleIndex_) return false;

    LOG_INFO(QString("Loading counts file: %1").arg(countsPath));
    statusLabel_->setText("Loading counts...");
    QApplication::processEvents();

    // Parse counts.xml
    countsData_ = CountsParser::parse(
        countsPath.toStdString(),
        *networkIndex_);

    if (!countsData_ || countsData_->counts.empty()) {
        countsData_.reset();
        exportCountChartsAction_->setEnabled(false);
        statusLabel_->setText("Counts loading failed");
        return false;
    }

    // Remember the sample rate the user just told us about. The link histogram
    // reports raw event counts, and this is the only place the app learns what
    // fraction of the population they represent.
    countsScaleFactor_ = scaleFactor;

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
    exportCountChartsAction_->setEnabled(true);

    statusLabel_->setText(QString("Loaded %1 count locations").arg(countsData_->counts.size()));
    LOG_INFO(QString("Counts loaded: %1 locations").arg(countsData_->counts.size()));

    // A link panel opened before this now has a scale factor to apply.
    if (selectedLinkId_ != kNoLink) {
        onNetworkLinkClicked(selectedLinkId_);
    }
    return true;
}

void MainWindow::findForCapture(const QString& id) {
    searchEdit_->setText(id);
    onFind();
}

bool MainWindow::selectLinkForCapture(const QString& linkId) {
    if (!networkIndex_) return false;
    const uint32_t id = networkIndex_->findLinkByOriginalId(linkId.toStdString());
    if (id == UINT32_MAX) return false;
    onNetworkLinkCtrlClicked(id);
    return true;
}

QWidget* MainWindow::showCountChartForCapture(const QString& linkId) {
    if (!networkIndex_ || !countsData_) return nullptr;
    const uint32_t id = networkIndex_->findLinkByOriginalId(linkId.toStdString());
    if (id == UINT32_MAX || !countsData_->linkToCount.count(id)) return nullptr;
    onCountLinkClicked(id);
    const auto dialogs = findChildren<CountsChartDialog*>();
    return dialogs.isEmpty() ? nullptr : dialogs.last();
}

QWidget* MainWindow::sidePanel() const {
    return sidePanel_;
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
        case RoadType::AirVertical: return "Air: vertical";
        case RoadType::AirClimb:    return "Air: climb";
        case RoadType::AirCruise:   return "Air: cruise";
        case RoadType::AirDescent:  return "Air: descent";
        case RoadType::AirHover:    return "Air: hover";
        case RoadType::AirOther:    return "Air: other";
        default:                    return "Unknown";
    }
}

void MainWindow::onNetworkLinkClicked(uint32_t linkId) {
    if (!networkIndex_) return;
    const auto* link = networkIndex_->getLink(linkId);
    if (!link) return;

    selectedLinkId_ = linkId;

    // A link selection replaces every other highlight on the map, then draws
    // this link in yellow.
    endSearchFollow();
    mapWidget_->clearAllHighlights();
    infoPanel_->resetPersonRouteState();
    routePersonPlus1_ = 0;
    infoVehicleId_ = kNoVehicle;
    infoShowsPerson_ = false;
    mapWidget_->highlightLink(linkId);

    InfoPanel::LinkInfo info;
    info.linkId = linkOriginalId(linkId);
    info.experimentName = experimentName_;

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
    // Until the scan finishes, leave hourlyVolumes empty: the panel then omits
    // the chart entirely rather than drawing 24 zeros, which would read as "no
    // traffic on this link" when the truth is "not counted yet".
    info.volumeScaleFactor = countsScaleFactor_;

    const size_t base = static_cast<size_t>(linkId) * 24;
    if (linkHourlyVolumesReady_ && base + 24 <= linkHourlyVolumes_.hours.size()) {
        const auto begin = linkHourlyVolumes_.hours.begin() + base;
        info.hourlyVolumes.assign(begin, begin + 24);
    }

    infoPanel_->showLinkInfo(info);
    sidePanel_->showPage(infoPage_);
}

QString MainWindow::linkOriginalId(uint32_t linkId) const {
    const auto& linkIds = networkIndex_->linkIds();
    if (linkId < linkIds.size())
        return QString::fromStdString(linkIds.getString(linkId));
    return QString::number(linkId);
}

ScenarioPanel::LinkRow MainWindow::scenarioRow(uint32_t linkId) const {
    ScenarioPanel::LinkRow row;
    row.internalId = linkId;
    row.linkId = linkOriginalId(linkId);
    if (const auto* link = networkIndex_->getLink(linkId)) {
        const auto& nodeIds = networkIndex_->nodeIds();
        if (link->fromNode < nodeIds.size())
            row.fromNode = QString::fromStdString(nodeIds.getString(link->fromNode));
        if (link->toNode < nodeIds.size())
            row.toNode = QString::fromStdString(nodeIds.getString(link->toNode));
        row.roadType = roadTypeName(static_cast<RoadType>(link->roadType));
        row.lengthM = link->length;
    }
    return row;
}

QString MainWindow::toggleKey(const QAction* action) {
    static const QRegularExpression count(QStringLiteral("\\s*\\([\\d,]+\\)$"));
    QString key = action->text();
    key.remove(QLatin1Char('&'));
    key.remove(count);
    return key;
}

QList<QAction*> MainWindow::viewToggles() const {
    const QList<const QAction*> notView = {
        showInfoPanelAction_, showSelectedLinksAction_, showViewsAction_, panModeAction_};
    QList<QAction*> toggles;
    for (QAction* action : findChildren<QAction*>()) {
        if (!action->isCheckable() || action->text().isEmpty() || notView.contains(action))
            continue;
        // The window's own actions and those of its menus. Not the ribbon's
        // collapse button or the help window.
        const QObject* owner = action->parent();
        const bool ownMenu = qobject_cast<const QMenu*>(owner) &&
                             owner->parent() == this && owner != speedMenu_;
        if (owner == this || ownMenu) toggles << action;
    }
    return toggles;
}

QJsonObject MainWindow::currentView(const QString& name) const {
    QJsonObject view{
        {"name", name},
        {"time", mapWidget_->simulationTime()},
        {"center", QJsonArray{mapWidget_->viewCenterX(), mapWidget_->viewCenterY()}},
        {"m_per_px", mapWidget_->metresPerPixel()},
    };

    QJsonObject actions;
    for (const QAction* action : viewToggles()) {
        if (!action->isEnabled()) continue;
        // Of a group of choices, only the one that is chosen
        if (action->actionGroup() && action->actionGroup()->isExclusive()) {
            if (action->isChecked()) actions[toggleKey(action)] = true;
        } else {
            actions[toggleKey(action)] = action->isChecked();
        }
    }
    view["actions"] = actions;

    if (mapWidget_->isTrackingVehicle() && vehicleIndex_) {
        view["track"] = vehicleIndex_->vehicleIdString(mapWidget_->trackedVehicle());
    }
    // Only a view that has links changes the selection when it is applied, so
    // going to a view never loses a selection that the user is making
    if (!scenarioPanel_->isEmpty()) {
        QJsonArray links;
        for (uint32_t id : scenarioPanel_->linkIds()) links.append(linkOriginalId(id));
        view["select"] = links;
    }
    if (mapWidget_->heatmapVisible() && !mapWidget_->heatmapTitle().isEmpty()) {
        view["heatmap"] = mapWidget_->heatmapTitle();
    }

    // The card picture: 2x the size on the card, so it is sharp on a
    // high-DPI screen
    const QImage image = mapWidget_->renderToImage(1);
    if (!image.isNull()) {
        const QSize size(192, 120);
        const QImage scaled = image.scaled(size, Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
        const QImage thumb = scaled.copy((scaled.width() - size.width()) / 2,
                                         (scaled.height() - size.height()) / 2,
                                         size.width(), size.height());
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        thumb.save(&buffer, "PNG");
        view["thumbnail"] = QString::fromLatin1(png.toBase64());
    }
    return view;
}

void MainWindow::applyView(const QJsonObject& view) {
    if (!networkIndex_ || !vehicleIndex_) return;
    QStringList missing;

    // A toggle that is not in the view, or is not available now (Show Link
    // Counts before counts are loaded), stays as it is
    const QList<QAction*> toggles = viewToggles();
    const QJsonObject actions = view.value("actions").toObject();
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        QAction* found = nullptr;
        for (QAction* action : toggles) {
            if (toggleKey(action) == it.key()) { found = action; break; }
        }
        if (!found || !found->isEnabled()) continue;
        // trigger() so that the slots on triggered() and on toggled() both run
        const bool on = it.value().toBool();
        if (found->actionGroup() && found->actionGroup()->isExclusive()) {
            if (on && !found->isChecked()) found->trigger();
        } else if (found->isChecked() != on) {
            found->trigger();
        }
    }

    const QString heatmap = view.value("heatmap").toString();
    if (heatmap.isEmpty()) {
        if (mapWidget_->heatmapVisible()) onClearHeatmap();
    } else if (!mapWidget_->heatmapVisible() || mapWidget_->heatmapTitle() != heatmap) {
        if (heatmapActivityTypes_.contains(heatmap)) onHeatmapTypeSelected(heatmap);
        else missing << QString("heatmap %1").arg(heatmap);
    }

    if (view.contains("select")) {
        std::vector<ScenarioPanel::LinkRow> rows;
        int notFound = 0;
        for (const QJsonValue& v : view.value("select").toArray()) {
            const uint32_t id = networkIndex_->findLinkByOriginalId(v.toString().toStdString());
            if (id == UINT32_MAX) ++notFound;
            else rows.push_back(scenarioRow(id));
        }
        scenarioPanel_->clearLinks();
        if (!rows.empty()) scenarioPanel_->addLinks(rows);
        if (notFound > 0) missing << QString("%1 link(s)").arg(notFound);
    }

    if (view.contains("time")) {
        mapWidget_->setSimulationTime(static_cast<float>(view.value("time").toDouble()));
    }
    const QJsonArray center = view.value("center").toArray();
    if (center.size() == 2) {
        mapWidget_->setView(center[0].toDouble(), center[1].toDouble(),
                            view.value("m_per_px").toDouble(mapWidget_->metresPerPixel()));
    }

    const QString track = view.value("track").toString();
    if (track.isEmpty()) {
        if (mapWidget_->isTrackingVehicle()) mapWidget_->stopTracking();
    } else {
        const uint32_t id = vehicleIdOf(track);
        if (id == kNoVehicle) missing << QString("vehicle %1").arg(track);
        else mapWidget_->setTrackedVehicle(id);
    }

    QString text = QString("View \"%1\"").arg(view.value("name").toString());
    if (!missing.isEmpty()) text += "  |  Not in this run: " + missing.join(", ");
    statusLabel_->setText(text);
}

void MainWindow::saveView() {
    if (!networkIndex_ || !vehicleIndex_) return;
    const QString name = viewsPanel_->nextName();
    viewsPanel_->addView(currentView(name));
    sidePanel_->showPage(viewsPage_);
    statusLabel_->setText(QString("Saved \"%1\". To rename it, press F2 in the Views panel.")
        .arg(name));
}

void MainWindow::saveViewsOverTime() {
    if (!networkIndex_ || !vehicleIndex_) return;
    SaveOverTimeDialog dialog(mapWidget_->simulationTime(),
                              VehicleIndex::toSeconds(vehicleIndex_->minTime()),
                              VehicleIndex::toSeconds(vehicleIndex_->maxTime()),
                              viewsPanel_->nextName(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const bool wasPlaying = mapWidget_->isPlaying();
    if (wasPlaying) mapWidget_->setPlaying(false);
    const float before = mapWidget_->simulationTime();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<double> times = dialog.times();
    for (double t : times) {
        // The vehicles at that time, for the card picture
        mapWidget_->setSimulationTime(static_cast<float>(t));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        viewsPanel_->addView(currentView(dialog.name() + " · " + ViewsPanel::clockText(t)));
    }
    mapWidget_->setSimulationTime(before);
    QApplication::restoreOverrideCursor();
    if (wasPlaying) mapWidget_->setPlaying(true);

    sidePanel_->showPage(viewsPage_);
    statusLabel_->setText(QString("Saved %1 views of \"%2\"").arg(times.size()).arg(dialog.name()));
}

// A file name without extension from `text`: "08:00" becomes "08.00", and
// the other characters that Windows does not allow become "_". " (2)", " (3)"
// ... is added to a name that is in `used`, and the result is added to it.
static QString uniqueFileStem(const QString& text, const QString& fallback,
                              QStringList& used) {
    QString stem = text;
    stem.replace(QLatin1Char(':'), QLatin1Char('.'));
    stem.replace(QRegularExpression(QStringLiteral("[\\\\/*?\"<>|]")), QStringLiteral("_"));
    stem = stem.trimmed();
    if (stem.isEmpty()) stem = fallback;
    QString unique = stem;
    for (int n = 2; used.contains(unique, Qt::CaseInsensitive); ++n) {
        unique = QString("%1 (%2)").arg(stem).arg(n);
    }
    used << unique;
    return unique;
}

void MainWindow::captureViews() {
    if (!networkIndex_ || !vehicleIndex_ || viewsPanel_->count() == 0) return;
    QStringList names;
    for (int i = 0; i < viewsPanel_->count(); ++i) {
        names << viewsPanel_->view(i).value("name").toString();
    }
    const QString runFolder = QFileInfo(networkFilePath_).absolutePath();
    CaptureViewsDialog dialog(names, QDir(runFolder).absoluteFilePath("captures"),
                              mapWidget_->size(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const QString folder = dialog.folder();
    if (!QDir().mkpath(folder)) {
        QMessageBox::warning(this, "Capture Views",
            QString("Cannot make the folder\n%1").arg(QDir::toNativeSeparators(folder)));
        return;
    }

    const bool wasPlaying = mapWidget_->isPlaying();
    if (wasPlaying) mapWidget_->setPlaying(false);
    // The map comes back to this after the capture. An empty selection is
    // written, so that the links of the last view do not stay.
    QJsonObject before = currentView(QString());
    if (!before.contains("select")) before["select"] = QJsonArray();

    const QList<int> rows = dialog.rows();
    QProgressDialog progress(QString(), "Cancel", 0, static_cast<int>(rows.size()), this);
    progress.setWindowTitle("Capture Views");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setMinimumWidth(360);

    // Lets the window paint and the heatmap worker report, without a busy loop
    auto pause = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };

    int saved = 0;
    QStringList failed;
    QStringList usedNames;
    for (int i = 0; i < rows.size() && !progress.wasCanceled(); ++i) {
        const QJsonObject view = viewsPanel_->view(rows[i]);
        const QString name = view.value("name").toString();
        const QString label = QString("View %1 of %2: %3").arg(i + 1).arg(rows.size()).arg(name);
        progress.setLabelText(label);
        progress.setValue(i);

        applyView(view);
        pause(50);
        // A heatmap on a large network takes a while to compute
        while (heatmapRunning_ && !progress.wasCanceled()) {
            progress.setLabelText(label + "\nWaiting for the heatmap...");
            pause(200);
        }
        if (progress.wasCanceled()) break;

        const QString unique = uniqueFileStem(name, QString("view %1").arg(rows[i] + 1),
                                              usedNames);

        const QString path = QDir(folder).absoluteFilePath(unique + ".png");
        const QImage image = mapWidget_->renderToImage(dialog.scale());
        if (image.isNull() || !image.save(path, "PNG")) {
            failed << name;
            LOG_ERROR(QString("Capture Views: cannot save %1").arg(path));
        } else {
            ++saved;
        }
    }
    const bool cancelled = progress.wasCanceled();
    progress.setValue(static_cast<int>(rows.size()));

    applyView(before);
    if (wasPlaying) mapWidget_->setPlaying(true);

    LOG_INFO(QString("Capture Views: %1 image(s) in %2").arg(saved).arg(folder));
    statusLabel_->setText(QString("Saved %1 image(s) in %2%3")
        .arg(saved).arg(QDir::toNativeSeparators(folder), cancelled ? " (cancelled)" : ""));
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, "Capture Views",
            QString("Cannot save the image of:\n%1").arg(failed.join("\n")));
    }
    if (saved > 0 && dialog.openFolderWhenDone()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
}

void MainWindow::zoomToLink(uint32_t linkId) {
    const auto* link = networkIndex_ ? networkIndex_->getLink(linkId) : nullptr;
    if (!link) return;
    const auto* a = networkIndex_->getNode(link->fromNode);
    const auto* b = networkIndex_->getNode(link->toNode);
    if (!a || !b) return;
    BoundingBox box;
    box.expand(a->x, a->y);
    box.expand(b->x, b->y);
    mapWidget_->zoomToBounds(box, 1.0);
}

void MainWindow::onNetworkLinkCtrlClicked(uint32_t linkId) {
    if (!networkIndex_) return;
    const auto* link = networkIndex_->getLink(linkId);
    if (!link) return;

    // The two directions of a road are drawn on top of each other, and the
    // click finds only one of them. Take every link between the two nodes, in
    // both directions, so the whole road is added or removed.
    std::vector<uint32_t> group = networkIndex_->linksBetween(link->fromNode, link->toNode);
    for (uint32_t id : networkIndex_->linksBetween(link->toNode, link->fromNode))
        group.push_back(id);
    if (std::find(group.begin(), group.end(), linkId) == group.end())
        group.push_back(linkId);

    if (scenarioPanel_->contains(linkId)) {
        scenarioPanel_->removeLinks(group);
        statusLabel_->setText(QString("Removed %1 link(s) from the selection.").arg(group.size()));
        return;
    }

    std::vector<ScenarioPanel::LinkRow> rows;
    rows.reserve(group.size());
    for (uint32_t id : group) rows.push_back(scenarioRow(id));
    scenarioPanel_->addLinks(rows);
    sidePanel_->showPage(selectedLinksPage_);
    statusLabel_->setText(QString("Added %1 link(s) to the selection.").arg(group.size()));
}

void MainWindow::onFind() {
    const QString wanted = searchEdit_->text().trimmed();
    if (wanted.isEmpty()) return;

    if (!networkIndex_ || networkIndex_->linkCount() == 0) {
        showSearchMessage("Open a run first.");
        return;
    }

    // MATSim ids are unique only within one type, so one text can name a
    // link, a vehicle and a person. Each match is one choice.
    struct Match {
        QIcon icon;
        QString text;
        std::function<void()> show;
    };
    std::vector<Match> matches;
    const std::string key = wanted.toStdString();

    const uint32_t linkId = networkIndex_->findLinkByOriginalId(key);
    if (linkId != UINT32_MAX) {
        matches.push_back({ribbonIcon("spline"), QString("Link %1").arg(wanted),
            [this, linkId, wanted]() {
                onNetworkLinkClicked(linkId);
                zoomToLink(linkId);
                statusLabel_->setText(QString("Link %1").arg(wanted));
            }});
    }
    if (vehicleIndex_) {
        const uint32_t vehicleId = vehicleIndex_->findVehicleByOriginalId(key);
        const uint32_t personId = vehicleIndex_->findPersonByOriginalId(key);
        // MATSim gives a private car the id of its driver. The person and the
        // car are then one result: showing the person follows the car.
        const bool ownCar = vehicleId != UINT32_MAX && personId != UINT32_MAX &&
                            vehicleIndex_->vehicleDriver(vehicleId) == personId + 1;
        if (ownCar) {
            matches.push_back({ribbonIcon("user"),
                QString("Person %1 (in vehicle %1)").arg(wanted),
                [this, personId]() { showPerson(personId); }});
        } else if (vehicleId != UINT32_MAX) {
            matches.push_back({ribbonIcon("car"), QString("Vehicle %1").arg(wanted),
                [this, vehicleId]() { showVehicle(vehicleId); }});
        }
        if (personId != UINT32_MAX && !ownCar) {
            matches.push_back({ribbonIcon("user"), QString("Person %1").arg(wanted),
                [this, personId]() { showPerson(personId); }});
        }
    }

    if (matches.empty()) {
        showSearchMessage(QString("No link, person or vehicle with ID %1").arg(wanted));
        return;
    }
    if (matches.size() == 1) {
        matches.front().show();
        mapWidget_->setFocus();
        return;
    }

    // More than one type has this id: let the user choose, never guess
    QMenu menu(this);
    for (const Match& match : matches) {
        QAction* action = menu.addAction(match.icon, match.text);
        connect(action, &QAction::triggered, this, [this, show = match.show]() {
            show();
            mapWidget_->setFocus();
        });
    }
    menu.exec(searchEdit_->mapToGlobal(QPoint(0, searchEdit_->height())));
}

void MainWindow::showSearchMessage(const QString& message) {
    statusLabel_->setText(message);
    QToolTip::showText(searchEdit_->mapToGlobal(QPoint(0, searchEdit_->height())),
                       message, searchEdit_);
}

bool MainWindow::showVehicle(uint32_t vehicleId) {
    const QString name = vehicleIndex_->vehicleIdString(vehicleId);
    endSearchFollow();
    foundVehiclePlus1_ = vehicleId + 1;
    if (!updateFoundHalo(true)) {
        foundVehiclePlus1_ = 0;
        showSearchMessage(QString("Vehicle %1 cannot be shown on the map.").arg(name));
        return false;
    }
    // After the halo, so the panel does not take a vehicle tracked before
    inspectVehicle(vehicleId);
    statusLabel_->setText(QString("Vehicle %1").arg(name));
    return true;
}

bool MainWindow::showPerson(uint32_t personId) {
    const QString name = vehicleIndex_->personIdString(personId);
    endSearchFollow();
    foundPersonPlus1_ = personId + 1;
    if (!updateFoundHalo(true)) {
        foundPersonPlus1_ = 0;
        showSearchMessage(QString("Person %1 has no known place on the map.").arg(name));
        return false;
    }
    inspectPerson(personId);
    if (mapWidget_->isTrackingVehicle()) {
        statusLabel_->setText(QString("Person %1, in vehicle %2")
            .arg(name, vehicleIndex_->vehicleIdString(mapWidget_->trackedVehicle())));
    } else {
        statusLabel_->setText(QString("Person %1").arg(name));
    }
    return true;
}

bool MainWindow::updateFoundHalo(bool panToHalo) {
    if (!vehicleIndex_) return false;
    if (foundVehiclePlus1_ == 0 && foundPersonPlus1_ == 0) return false;
    const TimeMs now = VehicleIndex::toTimeMs(mapWidget_->simulationTime());
    const FoundPlace place = foundPersonPlus1_ != 0
        ? placeOfPerson(foundPersonPlus1_ - 1, now)
        : placeOfVehicle(foundVehiclePlus1_ - 1, now);

    searchTracking_ = true;
    // Tracking id 0 means "no vehicle", so internal vehicle 0 gets a fixed
    // halo at its position instead, moved on each time change
    if (place.vehiclePlus1 > 1) {
        mapWidget_->clearHaloPosition();
        mapWidget_->setTrackedVehicle(place.vehiclePlus1 - 1);
    } else {
        mapWidget_->stopTracking();
        if (place.hasPoint) {
            mapWidget_->setHaloPosition(place.x, place.y);
            if (panToHalo) mapWidget_->panTo(place.x, place.y);
        } else {
            mapWidget_->clearHaloPosition();
        }
    }
    searchTracking_ = false;
    return place.vehiclePlus1 != 0 || place.hasPoint;
}

void MainWindow::endSearchFollow() {
    foundVehiclePlus1_ = 0;
    foundPersonPlus1_ = 0;
    mapWidget_->clearHaloPosition();
}

MainWindow::FoundPlace MainWindow::placeOfVehicle(uint32_t vehicleId, TimeMs now) const {
    FoundPlace place;
    if (const VehicleState* state = mapWidget_->vehicleState(vehicleId)) {
        place.vehiclePlus1 = vehicleId + 1;
        place.hasPoint = true;
        place.x = state->x;
        place.y = state->y;
        return place;
    }

    // Parked where the driver left it: the driver's last leg that ended
    // before now was in this vehicle, or, before the first leg, the first
    // leg is. The halo is at the activity only while the driver is there.
    const uint32_t driver = vehicleIndex_->vehicleDriver(vehicleId);
    const auto* trips = driver != 0 ? vehicleIndex_->personTrips(driver - 1) : nullptr;
    if (trips && !trips->empty()) {
        const PersonTrip* lastLeg = nullptr;
        for (const PersonTrip& trip : *trips) {
            if (trip.arriveTimeMs == TRIP_NO_TIME || trip.arriveTimeMs > now) break;
            lastLeg = &trip;
        }
        const PersonTrip& leg = lastLeg ? *lastLeg : trips->front();
        if (leg.vehicleId == vehicleId + 1) {
            const ActivityRecord* act = activityAt(driver - 1, now, false);
            if (act && activityPoint(*act, place.x, place.y)) {
                place.hasPoint = true;
                return place;
            }
        }
    }

    // Else on the network: the end of its last link before now, or the
    // start of its first link when it has not moved yet
    const VehicleTrajectory* trajectory = vehicleIndex_->trajectory(vehicleId);
    if (!trajectory || trajectory->segments.empty() || !networkIndex_) return place;
    const VehicleSegment* last = nullptr;
    for (const VehicleSegment& segment : trajectory->segments) {
        if (segment.leaveTime > now) break;
        last = &segment;
    }
    const auto* link = networkIndex_->getLink(
        last ? last->linkId : trajectory->segments.front().linkId);
    if (!link) return place;
    const auto* node = networkIndex_->getNode(last ? link->toNode : link->fromNode);
    if (!node) return place;
    place.hasPoint = true;
    place.x = static_cast<float>(node->x);
    place.y = static_cast<float>(node->y);
    return place;
}

MainWindow::FoundPlace MainWindow::placeOfPerson(uint32_t personId, TimeMs now) const {
    FoundPlace place;
    // Aboard a vehicle that is on the road: on the vehicle. Before boarding
    // (the wait at a pt stop) the person is at the last activity, which on a
    // pt trip is the "pt interaction" at the stop.
    if (const auto* trips = vehicleIndex_->personTrips(personId)) {
        for (const PersonTrip& trip : *trips) {
            if (trip.departTimeMs > now) break;
            const bool aboard = trip.boardTimeMs != TRIP_NO_TIME &&
                                trip.boardTimeMs <= now &&
                                (trip.arriveTimeMs == TRIP_NO_TIME || now < trip.arriveTimeMs);
            if (!aboard || trip.vehicleId == 0) continue;
            if (const VehicleState* state = mapWidget_->vehicleState(trip.vehicleId - 1)) {
                place.vehiclePlus1 = trip.vehicleId;
                place.hasPoint = true;
                place.x = state->x;
                place.y = state->y;
                return place;
            }
        }
    }

    // Else at the activity in progress, or at the last one before a walk, a
    // teleported leg or a vehicle that is not on the road
    if (const ActivityRecord* act = activityAt(personId, now, true))
        place.hasPoint = activityPoint(*act, place.x, place.y);
    return place;
}

const ActivityRecord* MainWindow::activityAt(uint32_t personId, TimeMs now,
                                             bool orLastEnded) const {
    const auto* indices = vehicleIndex_->activitiesForPerson(personId);
    if (!indices) return nullptr;
    const auto& activities = vehicleIndex_->activities();
    const ActivityRecord* lastEnded = nullptr;
    for (uint32_t idx : *indices) {
        const ActivityRecord& act = activities[idx];
        // The first activity of the day has no start time
        if (act.startTimeMs != ACT_NO_TIME && act.startTimeMs > now) break;
        if (act.endTimeMs == ACT_NO_TIME || now < act.endTimeMs) return &act;
        lastEnded = &act;
    }
    return orLastEnded ? lastEnded : nullptr;
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

void MainWindow::exportCountCharts() {
    if (!countsData_ || !networkIndex_) return;

    // The stations on the links of the Selected Links page, in list order
    std::vector<size_t> onSelected;
    for (uint32_t id : scenarioPanel_->linkIds()) {
        const auto it = countsData_->linkToCount.find(id);
        if (it != countsData_->linkToCount.end() &&
            std::find(onSelected.begin(), onSelected.end(), it->second) == onSelected.end()) {
            onSelected.push_back(it->second);
        }
    }
    const QString runFolder = QFileInfo(networkFilePath_).absolutePath();
    ExportCountChartsDialog dialog(static_cast<int>(countsData_->counts.size()),
                                   static_cast<int>(onSelected.size()),
                                   QDir(runFolder).absoluteFilePath("count-charts"), this);
    if (dialog.exec() != QDialog::Accepted) return;

    std::vector<size_t> stations = onSelected;
    if (!dialog.selectedOnly()) {
        stations.resize(countsData_->counts.size());
        for (size_t i = 0; i < stations.size(); ++i) stations[i] = i;
    }
    const QString folder = dialog.folder();
    if (!QDir().mkpath(folder)) {
        QMessageBox::warning(this, "Export Count Charts",
            QString("Cannot make the folder\n%1").arg(QDir::toNativeSeparators(folder)));
        return;
    }

    QProgressDialog progress(QString(), "Cancel", 0, static_cast<int>(stations.size()), this);
    progress.setWindowTitle("Export Count Charts");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    progress.setMinimumWidth(360);

    // GEH of one hour: the usual measure of fit for counts. Under 5 is a good fit.
    auto geh = [](double simulated, double observed) {
        const double sum = simulated + observed;
        return sum > 0.0 ? std::sqrt(2.0 * (simulated - observed) * (simulated - observed) / sum)
                         : 0.0;
    };
    auto csvField = [](QString text) {
        if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"'))) return text;
        text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QString("\"%1\"").arg(text);
    };

    QStringList rows{"link_id,station_id,observed_total,simulated_total,ratio,difference,"
                     "max_hourly_geh,hours_geh_below_5,image"};
    QStringList usedNames;
    QStringList failed;
    int saved = 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (size_t i = 0; i < stations.size() && !progress.wasCanceled(); ++i) {
        progress.setValue(static_cast<int>(i));
        const CountData& count = countsData_->counts[stations[i]];
        const QString link = linkOriginalId(count.linkId);
        const QString station = QString::fromStdString(count.stationId);
        progress.setLabelText(QString("Station %1 of %2: link %3")
            .arg(i + 1).arg(stations.size()).arg(link));

        uint64_t observed = 0;
        uint64_t simulated = 0;
        double maxGeh = 0.0;
        int goodHours = 0;
        for (int h = 0; h < 24; ++h) {
            observed += count.groundTruthVolumes[h];
            simulated += count.simulatedVolumes[h];
            const double g = geh(count.simulatedVolumes[h], count.groundTruthVolumes[h]);
            maxGeh = std::max(maxGeh, g);
            if (g < 5.0) ++goodHours;
        }
        const double ratio = observed > 0 ? static_cast<double>(simulated) / observed : 0.0;

        QString title = QString("Link %1").arg(link);
        if (!station.isEmpty()) title += QString(" · station %1").arg(station);
        title += QString(" · observed %1 · simulated %2")
            .arg(QLocale().toString(static_cast<qulonglong>(observed)),
                 QLocale().toString(static_cast<qulonglong>(simulated)));
        if (observed > 0) title += QString(" (%1 %)").arg(std::round(ratio * 100.0), 0, 'f', 0);

        const QString stem = uniqueFileStem(
            station.isEmpty() ? QString("link %1").arg(link)
                              : QString("link %1 - %2").arg(link, station),
            QString("station %1").arg(i + 1), usedNames);
        const QString path = QDir(folder).absoluteFilePath(stem + ".png");
        const QImage image = countChartImage(count, title, dialog.scale());
        if (image.isNull() || !image.save(path, "PNG")) {
            failed << QString("link %1").arg(link);
            LOG_ERROR(QString("Export Count Charts: cannot save %1").arg(path));
        } else {
            ++saved;
        }

        rows << QStringList{
            csvField(link), csvField(station),
            QString::number(observed), QString::number(simulated),
            observed > 0 ? QString::number(ratio, 'f', 3) : QString(),
            QString::number(static_cast<qint64>(simulated) - static_cast<qint64>(observed)),
            QString::number(maxGeh, 'f', 2), QString::number(goodHours),
            csvField(stem + ".png"),
        }.join(QLatin1Char(','));
    }
    const bool cancelled = progress.wasCanceled();
    progress.setValue(static_cast<int>(stations.size()));

    if (dialog.writeSummary() && !cancelled) {
        QSaveFile file(QDir(folder).absoluteFilePath("summary.csv"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
            file.write((rows.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8()) < 0 ||
            !file.commit()) {
            failed << "summary.csv";
        }
    }
    QApplication::restoreOverrideCursor();

    LOG_INFO(QString("Export Count Charts: %1 chart(s) in %2").arg(saved).arg(folder));
    statusLabel_->setText(QString("Saved %1 count chart(s) in %2%3")
        .arg(saved).arg(QDir::toNativeSeparators(folder), cancelled ? " (cancelled)" : ""));
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, "Export Count Charts",
            QString("Cannot save:\n%1").arg(failed.join("\n")));
    }
    if (saved > 0 && dialog.openFolderWhenDone()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
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
