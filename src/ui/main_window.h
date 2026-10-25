#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QAction>
#include <QMenu>
#include <QDir>
#include <QTimer>
#include <memory>

#include "map_widget.h"
#include "timeline_widget.h"
#include "info_panel.h"
#include "scenario_panel.h"
#include "data/network_index.h"
#include "data/vehicle_index.h"
#include "parsers/preprocessor.h"
#include "parsers/counts_parser.h"
#include "core/crs_transform.h"
#include "core/video_recorder.h"
#include "screenshot_exporter.h"
#include "analysis/nkdv_network.h"
#include "analysis/heatmap_cache.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <cstdint>
#include <unordered_map>
#include <vector>

class QLineEdit;

namespace simvis {

class RibbonBar;
class HelpWindow;
class SidePanel;
class LoadingIndicator;

// Hourly traffic for every link: 24 counts per link, laid end to end and
// indexed by link id (ids are interned sequentially from 0). A flat array
// rather than a map of vectors - one allocation instead of one per link, and
// the scan is roughly ten times faster on a large scenario.
struct LinkHourlyVolumes {
    std::vector<uint32_t> hours;
    // Which load this result describes. A scan that lands after the user has
    // already loaded another scenario is discarded on this stamp.
    uint64_t generation = 0;

    bool empty() const { return hours.empty(); }
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

public slots:
    void openFolder();
    void openFiles();
    // Scan a directory for network/events/transit files and load them.
    // Used by Open Folder, Recent Folders, and the CLI folder argument.
    void loadFolder(const QString& dirPath);
    void loadNetworkFile(const QString& path);
    void loadEventsFile(const QString& path);

public:
    // Scene capture (--scenes): no recent-folder entry and no background
    // workers that would share the CPU with the timed frames
    void setCaptureMode(bool on) { captureMode_ = on; }
    MapWidget* mapWidget() const { return mapWidget_; }
    // Internal id of a vehicle from its MATSim id, or kNoVehicle
    uint32_t vehicleIdOf(const QString& matsimId) const {
        return vehicleIndex_ ? vehicleIndex_->findVehicleByOriginalId(matsimId.toStdString())
                             : kNoVehicle;
    }

signals:
    // A run is loaded and on the map (from the cache or after preprocessing)
    void runLoaded();

protected:
    void closeEvent(QCloseEvent* event) override;

    // Dropping a scenario folder onto the window loads it. Finding the right
    // three files inside a MATSim output directory is the step users get wrong
    // most often, and dragging the folder skips it entirely.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    // Compute and show the density map for one activity type. Runs on a worker
    // thread, because a computation takes seconds.
    void onHeatmapTypeSelected(const QString& activityType);
    void onClearHeatmap();
    void onHeatmapSharedScaleToggled(bool shared);

    void onPreprocessingProgress(int percent, const QString& message);
    void onPreprocessingComplete(bool success);
    void onSimulationTimeChanged(float time);
    void onActiveVehicleCountChanged(size_t count);

    // View actions
    void onShowNodesToggled(bool checked);
    void onShowLinksToggled(bool checked);
    void onShowVehiclesToggled(bool checked);
    void onFitToNetwork();

    // Vehicle options
    void onVehicleShapeCircle();
    void onVehicleShapeTriangle();
    void onVehicleShapeRectangle();
    void onVehicleShapeDiamond();
    void onVehicleShapeAuto();
    void onVehicleSizeSmall();
    void onVehicleSizeMedium();
    void onVehicleSizeLarge();
    void onVehicleColorSpeed();
    void onVehicleColorMode();

    // Transit options
    void onShowBusRoutesToggled(bool checked);
    void onShowBusStopsToggled(bool checked);
    void onShowTramRoutesToggled(bool checked);
    void onShowRailRoutesToggled(bool checked);
    void onRouteThicknessThin();
    void onRouteThicknessMedium();
    void onRouteThicknessThick();

    // Info panel
    void onVehicleTrackingChanged(uint32_t vehicleId);
    void onShowPersonRouteToggled(bool show);
    void onTripClicked(int tripIndex);
    void onTransitStopClicked(int stopIndex);
    void onTransitRouteClicked(uint32_t lineId);
    void onInfoPanelStopClicked(uint32_t stopId);
    void onInfoPanelRouteClicked(uint32_t lineId);
    void onInfoPanelPanTo(float x, float y);

    // Video recording
    void onStartRecording();
    void onStopRecording();
    void onRecordingStarted();
    void onRecordingStopped(bool success, const QString& message);
    void onRecordingProgress(int percent);

    // Counts
    void onLoadCounts();
    void onShowCountsToggled(bool checked);
    void onCountLinkClicked(uint32_t linkId);

    // Network link details
    void onNetworkLinkClicked(uint32_t linkId);

    // Scenario selection: Ctrl+click adds a road (both directions) or
    // removes it when it is already in the list.
    void onNetworkLinkCtrlClicked(uint32_t linkId);

    // Search box: find a link, a vehicle or a person by its MATSim id. A link
    // shows like a plain click and zooms to it; a vehicle or a person gets a
    // halo where it is at the current time. When the id names more than one
    // type, a menu asks which. A person and their own car are one result.
    void onFind();

    // Export
    void onExportPdf();
    void onExportPng();

private:
    void setupUi();
    // Every action of the window, with its shortcut and slot
    void setupActions();
    // Tabs and groups of buttons for those actions; in place of a menu bar
    void setupRibbon();
    void setupStatusBar();

    // Common tail of all open flows: set paths, title, cache dir, then load
    // from cache or start preprocessing. Transit path may be empty.
    // rebuildCache preprocesses again even when the cache is up to date.
    void loadFromPaths(const QString& networkPath, const QString& eventsPath,
                       const QString& transitPath, bool rebuildCache = false);
    // Find the file for one role (e.g. "network") in dir. Matches
    // *<role>*.xml / *<role>*.xml.gz case-insensitively, prefers the
    // standard MATSim "output_" prefix, asks the user if still ambiguous.
    // Returns empty string if no candidate (or user cancelled the chooser).
    QString resolveInputFile(const QDir& dir, const QString& role);

    // Recent folders (persisted via QSettings, newest first, max 5)
    void addRecentFolder(const QString& dirPath);
    void updateRecentFoldersMenu();

    // Assemble VehicleInfo for the tracked vehicle, or for the person the
    // search found, and refresh the panel. No-op if neither is set.
    void refreshVehicleInfoPanel();
    // The person's id and day of trips, for the panel
    void fillPersonTrips(VehicleInfo& info, uint32_t personId, TimeMs nowMs) const;
    // Show a vehicle in the panel, or a person: their own car when they
    // drive one, else their trips only
    void inspectVehicle(uint32_t vehicleId);
    void inspectPerson(uint32_t personId);

    // Build the tracked person's route geometry (vehicle legs follow the
    // network via trajectory segments; teleported legs are straight lines).
    // tripIndex = -1 builds the full day; >= 0 builds only that trip.
    std::vector<PersonRouteRenderer::Segment> buildPersonRouteSegments(
        uint32_t personId, int tripIndex = -1) const;

    // Activity icon markers for the trips included in the overlay
    std::vector<MapWidget::ActivityMarker> buildActivityMarkers(
        uint32_t personId, int tripIndex = -1) const;

    // Rebuild the heatmap menu from the activity types the loaded scenario
    // actually contains. Types are scenario-defined, so they cannot be fixed.
    void populateHeatmapMenu();
    // View > Other Modes: the modes of the run without their own button
    void populateOtherModesMenu();
    // Air > Altitude Band: the height bands of the run
    void populateAltitudeBandMenu();

    // Read the run's cached network and vehicle index on a worker thread,
    // with the transit schedule when the run comes from the cache. A card
    // over the map shows the progress. applyLoadedFiles() finishes the load.
    void loadBinaryFiles(bool fromCache);

    // The files of a run, read on a worker thread by loadBinaryFiles()
    struct LoadedFiles {
        std::shared_ptr<NetworkIndex> network;
        std::shared_ptr<VehicleIndex> vehicles;
        std::unique_ptr<TransitScheduleParser::Result> transit;  // null: not read
        QString error;  // empty when every file was read
    };
    // Put the files in place and set up the window for the new run
    void applyLoadedFiles(LoadedFiles& files, bool fromCache, const QString& vidxPath);

    // Kick off the background per-link hourly volume scan for the scenario
    // that was just loaded, superseding any scan still running.
    void startVolumeAggregation();
    void startPreprocessing();
    void applyTransitData();
    void loadCachedCRS(const QString& crsPath);

    // UI components
    MapWidget* mapWidget_;
    TimelineWidget* timelineWidget_;
    InfoPanel* infoPanel_;
    QLabel* statusLabel_;
    QLabel* vehicleCountLabel_;

    // Data managers
    // shared_ptr, like vehicleIndex_: a density map worker keeps its own
    // reference while a new scenario replaces this one
    std::shared_ptr<NetworkIndex> networkIndex_;
    // shared_ptr, not unique_ptr: the background volume scan holds its own
    // reference for as long as it runs, so loading a new scenario replaces this
    // pointer instead of mutating an index a worker thread is reading.
    std::shared_ptr<VehicleIndex> vehicleIndex_;
    std::unique_ptr<Preprocessor> preprocessor_;

    // Transit data (owned by MainWindow after preprocessing)
    std::unique_ptr<TransitScheduleParser::Result> transitData_;

    // Counts data
    std::unique_ptr<CountsData> countsData_;

    // Video recording
    std::unique_ptr<VideoRecorder> videoRecorder_;
    QAction* startRecordingAction_;
    QAction* stopRecordingAction_;

    // File paths
    QString networkFilePath_;
    QString eventsFilePath_;
    QString transitSchedulePath_;
    QString cacheDirectory_;

    // File, navigation and playback actions
    QAction* openFolderAction_ = nullptr;
    QAction* openFilesAction_ = nullptr;
    // Preprocess the open run again, in place of deleting .simvis_cache
    QAction* rebuildCacheAction_ = nullptr;
    // True while that runs. If it fails or is cancelled, the cache from
    // before still loads, so the run does not go away.
    bool rebuildingCache_ = false;
    bool captureMode_ = false;
    QAction* fitAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* panModeAction_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* goToStartAction_ = nullptr;
    QAction* goToEndAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* exportPdfAction_ = nullptr;
    QAction* exportPngAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    QAction* keyboardShortcutsAction_ = nullptr;
    QAction* userGuideAction_ = nullptr;
    QAction* reportProblemAction_ = nullptr;
    // Made when the help is first opened
    HelpWindow* helpWindow_ = nullptr;
    // The Clear and Export buttons of the "Selected links" panel
    QAction* clearSelectionAction_ = nullptr;
    QAction* exportScenarioAction_ = nullptr;
    // Playback speeds; the timeline keeps the current one
    QMenu* speedMenu_ = nullptr;

    // Menus of radio choices, shown as drop-down buttons on the ribbon
    QMenu* vehicleShapeMenu_ = nullptr;
    QMenu* vehicleSizeMenu_ = nullptr;
    QMenu* vehicleColorMenu_ = nullptr;
    QMenu* otherModesMenu_ = nullptr;
    QAction* showAirLinksAction_ = nullptr;
    QAction* showVertiportsAction_ = nullptr;
    QAction* showTrailsAction_ = nullptr;
    QMenu* altitudeLiftMenu_ = nullptr;
    QMenu* altitudeBandMenu_ = nullptr;
    QAction* showLossesAction_ = nullptr;
    QAction* showHoldsAction_ = nullptr;
    QMenu* routeThicknessMenu_ = nullptr;
    QMenu* tileSourceMenu_ = nullptr;

    RibbonBar* ribbon_ = nullptr;

    // Layer and vehicle actions
    QAction* showNodesAction_;
    QAction* showLinksAction_;
    QAction* showVehiclesAction_;
    QAction* showCarsAction_;
    QAction* showBusVehiclesAction_;
    QAction* showTramVehiclesAction_;
    QAction* showRailVehiclesAction_;

    // Vehicle shape/size/color actions
    QAction* vehicleShapeCircleAction_;
    QAction* vehicleShapeTriangleAction_;
    QAction* vehicleShapeRectangleAction_;
    QAction* vehicleShapeDiamondAction_;
    QAction* vehicleShapeAutoAction_;
    QAction* vehicleSizeSmallAction_;
    QAction* vehicleSizeMediumAction_;
    QAction* vehicleSizeLargeAction_;
    QAction* vehicleColorSpeedAction_;
    QAction* vehicleColorModeAction_;

    // Transit actions
    QAction* showBusRoutesAction_;
    QAction* showBusStopsAction_;
    QAction* showTramRoutesAction_;
    QAction* showRailRoutesAction_;
    QAction* routeThicknessThinAction_;
    QAction* routeThicknessMediumAction_;
    QAction* routeThicknessThickAction_;

    // Side panel page actions (see updateSidePanelActions)
    QAction* showInfoPanelAction_;
    QAction* showSelectedLinksAction_ = nullptr;

    // Counts actions
    QAction* loadCountsAction_;
    QAction* showCountsAction_;

    // Recent folders menu (rebuilt from QSettings on every change)
    QMenu* recentFoldersMenu_ = nullptr;

    // Activity density heatmap menu, filled in once a scenario is loaded
    QMenu* heatmapMenu_ = nullptr;
    // The undirected graph the density engine works on. Built once per
    // scenario, because it
    // takes a moment and never changes while the scenario is loaded.
    std::shared_ptr<NkdvNetwork> nkdvNetwork_;
    // True while a density computation is running, so a second one cannot be
    // started on top of it.
    bool heatmapRunning_ = false;

    // Density maps computed for this scenario, kept beside the other cache
    // files so a map is computed once rather than once per session.
    std::unique_ptr<HeatmapCache> heatmapCache_;
    // When the events cache was written. A cache entry older than this
    // describes data from before the last re-preprocess.
    QDateTime heatmapSourceTime_;

    // Types still to precompute in the background, newest request first.
    QStringList heatmapPrecomputeQueue_;
    bool heatmapPrecomputeActive_ = false;
    // Set when the user asks for a type while another map is computing. That
    // type is computed next and its result is shown, not only stored.
    QString heatmapPendingUserType_;
    // False on scenarios too large to precompute every type up front, where
    // maps are computed only when asked for.
    bool heatmapAutoPrecompute_ = true;
    // Set once the user accepts the long-run warning on a large scenario, so
    // the question is asked once rather than on every map.
    bool heatmapLongRunAccepted_ = false;
    // Raised on every scenario change. A computation carries the generation it
    // started in, so a result that arrives after a switch is dropped instead of
    // being shown over the new network.
    uint64_t heatmapGeneration_ = 0;

    // Clear everything that belongs to the scenario being replaced.
    void resetScenarioState();
    // A background map needs the network's CRS. Turn it off when the scenario
    // just loaded has none, so old tiles do not stay under a network they do
    // not match.
    void checkBackgroundMapCrs();

    // Close the side panel and drop the inspected link with it
    void hideSidePanel();
    // Check the action of the page in front, uncheck the others
    void updateSidePanelActions();

    // Search results: a message under the search box, and the vehicle or
    // person that was found
    void showSearchMessage(const QString& message);
    // Show a found vehicle or person where it is at the current time, and
    // pan the map to it. The time does not change. The halo then follows the
    // object as the time changes, until the user selects something else.
    // False when the object has no place on the map.
    bool showVehicle(uint32_t vehicleId);
    bool showPerson(uint32_t personId);

    // Where a found object is at a time: on a vehicle that is on the road
    // (vehiclePlus1, VehicleId + 1), else at a fixed point (hasPoint).
    struct FoundPlace {
        uint32_t vehiclePlus1 = 0;
        bool hasPoint = false;
        float x = 0.0f;
        float y = 0.0f;
    };
    // A vehicle off the road is where it stopped: at the activity of its
    // driver when the driver left it there and is still at that activity,
    // else at the end of its last link (start of its first link before it
    // moves). A person aboard a vehicle on the road is on it; else at the
    // activity they are at, or at the last one they left (also while they
    // wait at a stop).
    FoundPlace placeOfVehicle(uint32_t vehicleId, TimeMs now) const;
    FoundPlace placeOfPerson(uint32_t personId, TimeMs now) const;
    // The person's activity in progress at the time; when there is none and
    // orLastEnded is set, the latest one that ended before it. Null: none.
    const ActivityRecord* activityAt(uint32_t personId, TimeMs now,
                                     bool orLastEnded) const;
    // Map point of an activity: its x/y, or the middle of its link when the
    // event had no x/y. False when neither is known.
    bool activityPoint(const ActivityRecord& act, float& x, float& y) const;
    // Put the halo where the found object is now: track its vehicle (as a
    // click does) or set a fixed halo. panToHalo moves the map to a fixed
    // halo; a tracked vehicle is centered by the tracking.
    bool updateFoundHalo(bool panToHalo);
    // Stop following the found object and remove its fixed halo
    void endSearchFollow();

    // MATSim id string of a link (internal id), or the number if unknown.
    QString linkOriginalId(uint32_t linkId) const;
    // A row for the "Selected links" panel.
    ScenarioPanel::LinkRow scenarioRow(uint32_t linkId) const;
    void zoomToLink(uint32_t linkId);

    // The panel at the right of the map. Its pages are the info panel and the
    // "Selected links" panel; the second opens on the first Ctrl+click.
    SidePanel* sidePanel_ = nullptr;
    // Card over the map while a run loads
    LoadingIndicator* loadingIndicator_ = nullptr;
    // Raised by every load; a load whose result arrives late is dropped
    uint64_t loadGeneration_ = 0;
    // MATSim id of the link inspected before a load, opened again after it
    std::string carriedLinkId_;
    int infoPage_ = 0;
    int selectedLinksPage_ = 0;
    ScenarioPanel* scenarioPanel_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;

    // TAREEK experiment folder name of the loaded run: the folder that holds
    // the network file, or its parent when that folder is named "output".
    QString experimentName_;

    // Colors mean the same on every map when this is on. Off, each map is
    // scaled to its own peak, which reads one map well but cannot be compared.
    bool heatmapSharedScale_ = true;
    QAction* heatmapSharedScaleAction_ = nullptr;
    // Highest density across every map computed for this scenario, cached ones
    // included, which anchors the shared scale. Seeded from the cache when a
    // scenario loads so the colors do not depend on which types were opened.
    float heatmapSharedPeak_ = 0.0f;
    // Activity types this scenario contains, for finding that peak.
    QStringList heatmapActivityTypes_;

    void applyHeatmapScale();
    void noteHeatmapPeak(float peak);

    // Status shown beside the spinner in the status bar.
    enum class HeatmapStatus { Idle, Working, Done, Failed };
    void setHeatmapStatus(const QString& message, HeatmapStatus state);

    QLabel* heatmapStatusLabel_ = nullptr;
    QLabel* heatmapSpinnerLabel_ = nullptr;
    QTimer* heatmapSpinnerTimer_ = nullptr;
    int heatmapSpinnerFrame_ = 0;

    // Start computing the maps that are not cached yet, one at a time, so the
    // first click on a type is instant. Does nothing when all are cached.
    void startHeatmapPrecompute();
    void precomputeNextHeatmap();

    // Run one density computation off the UI thread. `onDone` runs on the UI
    // thread when it finishes.
    void runHeatmapAsync(const QString& activityType, bool showWhenDone);

    // Background map actions
    QAction* showBackgroundMapAction_;
    QAction* tileSourceOsmAction_;
    QAction* tileSourceSatAction_;
    QAction* tileSourceTopoAction_;

    // CRS info
    CRSInfo networkCrs_;

    // countsScaleFactor from the config the user loaded counts with: the factor
    // that turns this run's sampled vehicle counts into full-population ones.
    // 0 until counts are loaded, meaning the sample rate is still unknown.
    double countsScaleFactor_ = 0.0;

    // Link whose info panel is open, so a volume scan that finishes afterwards
    // can refresh it. kNoLink when the panel is showing anything else.
    static constexpr uint32_t kNoLink = UINT32_MAX;
    uint32_t selectedLinkId_ = kNoLink;

    // Per-link hourly traffic, 24 counts per link laid end to end and indexed
    // by link id. Filled by a worker thread; empty until that finishes.
    LinkHourlyVolumes linkHourlyVolumes_;
    bool linkHourlyVolumesReady_ = false;
    uint64_t volumeGeneration_ = 0;
    QFutureWatcher<LinkHourlyVolumes> volumeWatcher_;

    // Throttle for live vehicle-info panel refresh (sim seconds of last update)
    float lastVehicleInfoRefreshTime_ = -1.0f;

    // Person whose trips/route the panel currently shows (personId + 1, 0 = none)
    // and the vehicle the panel describes. Both survive tracking loss (clicking
    // empty map to pan) so the route overlay and trip clicks keep working until
    // a different vehicle is selected.
    uint32_t routePersonPlus1_ = 0;
    uint32_t infoVehicleId_ = kNoVehicle;
    // The panel describes the person routePersonPlus1_ (found by the search),
    // not the tracked vehicle. infoVehicleId_ is then their own car, or kNoVehicle.
    bool infoShowsPerson_ = false;

    // The vehicle or person the search found and the halo follows (id + 1,
    // 0 = none; at most one of the two is set)
    uint32_t foundVehiclePlus1_ = 0;
    uint32_t foundPersonPlus1_ = 0;
    // Set while the search itself starts or stops tracking, so that
    // onVehicleTrackingChanged does not take it as a click
    bool searchTracking_ = false;
};

} // namespace simvis
