// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QEvent>

#include "core/transit_types.h"
#include "parsers/transit_schedule_parser.h"
#include "analysis/link_metrics.h"

namespace simvis {

// Snapshot of everything the panel shows about a tracked vehicle.
// Assembled by MainWindow from VehicleIndex/NetworkIndex/VehicleState.
struct VehicleInfo {
    uint32_t vehicleId = kNoVehicle;   // kNoVehicle: a person and their trips only
    QString vehicleName;      // original MATSim vehicle id string
    QString mode;             // "Car", "Bus", ...
    bool active = false;      // currently on the network
    float speedKmh = -1.0f;   // < 0 = unknown
    float speedRatio = -1.0f; // actual/freeflow, < 0 = unknown
    float currentTime = 0.0f; // seconds

    // Aircraft (3D modes) on the network now
    bool hasHeight = false;
    float heightM = 0.0f;       // above the region datum
    float verticalRate = 0.0f;  // m/s, > 0 climbing
    QString phase;              // Take-off, Climb, Cruise, Descent, Landing, Hover

    // Driver / trips (cars: the person's full day; empty for transit vehicles)
    QString personName;       // driver's original MATSim person id
    struct TripRow {
        QString fromAct;      // "" = mid-journey leg (no activity boundary)
        QString toAct;
        QString mode;         // legMode string: car/walk/bike/pt/...
        float departSec = 0.0f;
        float arriveSec = -1.0f;  // < 0 = never arrived (stuck)
        bool active = false;      // currentTime within [depart, arrive)
        bool midJourney = false;  // no act types on either end
    };
    std::vector<TripRow> trips;

    // Transit service (transit vehicles only)
    QString lineName;
    QString routeName;
    int passengerCount = -1;  // -1 = not a transit vehicle
    struct Passenger {
        uint32_t personId;    // internal id
        QString name;         // original MATSim person id
    };
    std::vector<Passenger> passengers;  // aboard now, in boarding order
};

class InfoPanel : public QWidget {
    Q_OBJECT

public:
    explicit InfoPanel(QWidget* parent = nullptr);

    // Show different content types
    void showVehicleInfo(const VehicleInfo& info);
    void showStopInfo(const TransitStop& stop,
                      const std::vector<const TransitLine*>& servingLines);
    void showRouteInfo(const TransitLine& line,
                       const TransitRoute& route,
                       const TransitScheduleParser::Result& transitData);

    // Network link details (assembled by MainWindow from the LinkRecord)
    struct LinkInfo {
        QString linkId;      // original MATSim id string
        QString fromNode;    // original node id strings
        QString toNode;
        double lengthM = 0.0;
        double freeSpeedMs = 0.0;
        double capacityVehH = 0.0;
        float lanes = 0.0f;
        QString roadType;
        bool hasCounts = false;     // a count station is attached to this link
        QString countStationId;
        // Vehicles that entered the link in each hour of the run (at least
        // 24 hours, not folded at 24:00). Empty until the link table is ready.
        std::vector<uint32_t> hourlyVolumes;
        // The sums of the link in each bin of the link table, for the metric
        // charts (empty until the table is ready)
        std::vector<LinkSums> binSums;
        uint32_t binSeconds = 0;
        linkmetrics::RunFactors runFactors;
        LinkRecord link{};
        // countsScaleFactor the user supplied with the counts file, or 0 when
        // no counts are loaded and the sample rate is therefore unknown.
        double volumeScaleFactor = 0.0;
        QString volumeScaleSource;  // where the factor came from
        // Name of the experiment folder, shown on an exported chart.
        QString experimentName;
    };
    void showLinkInfo(const LinkInfo& info);

    void clear();

signals:
    void stopClicked(uint32_t stopId);       // User clicked a stop name in the list
    void routeClicked(uint32_t lineId);      // User clicked a route name in the list
    void panToRequested(float x, float y);   // Request map to pan to coordinates
    void showPersonRouteToggled(bool show);  // Show/hide full-day person route overlay
    void tripClicked(int tripIndex);         // User clicked a trip card (-1 = deselect)
    void passengerClicked(uint32_t personId); // User clicked a passenger in the list

private:
    // The metric of the link chart that the user chose last (a metric key)
    QString lastMetricKey_ = QStringLiteral("speed_ratio");
    void clearContent();
    QLabel* addHeader(const QString& text);
    // A header with a button that copies an id (to paste in the search box)
    QLabel* addHeader(const QString& text, const QString& copyId);
    QLabel* addBody(const QString& text);
    // A line of plain text qualifying the figures above it.
    QLabel* addCaption(const QString& text);
    QFrame* addDivider();
    QLabel* addClickableItem(const QString& text, std::function<void()> onClick);

    QVBoxLayout* mainLayout_;
    QScrollArea* scrollArea_;
    QWidget* contentWidget_;
    QVBoxLayout* contentLayout_;
    QLabel* titleLabel_;

    // Whether the person-route overlay is currently shown (button label state).
    // Reset by MainWindow via resetPersonRouteState() when tracking changes.
    bool personRouteShown_ = false;

    // Trip card selected for single-trip map display (-1 = none).
    // Reset alongside personRouteShown_ on tracking changes.
    int selectedTripIndex_ = -1;

    // The panel is rebuilt each second of simulated time while a vehicle is
    // tracked. The passenger list keeps its scroll position across rebuilds
    // of the same vehicle.
    uint32_t passengerListVehicle_ = kNoVehicle;
    int passengerListScroll_ = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

public:
    void resetPersonRouteState() { personRouteShown_ = false; selectedTripIndex_ = -1; }
    // Show the route button as "Hide" when MainWindow shows the route itself
    void setPersonRouteShown(bool shown) { personRouteShown_ = shown; }
};

} // namespace simvis
