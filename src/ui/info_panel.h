#pragma once
#include <QGraphicsLayout>
#include <QWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QEvent>

#include "core/transit_types.h"
#include "parsers/transit_schedule_parser.h"

namespace simvis {

// Snapshot of everything the panel shows about a tracked vehicle.
// Assembled by MainWindow from VehicleIndex/NetworkIndex/VehicleState.
struct VehicleInfo {
    uint32_t vehicleId = 0;
    QString vehicleName;      // original MATSim vehicle id string
    QString mode;             // "Car", "Bus", ...
    bool active = false;      // currently on the network
    float speedKmh = -1.0f;   // < 0 = unknown
    float speedRatio = -1.0f; // actual/freeflow, < 0 = unknown
    float currentTime = 0.0f; // seconds

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
        int lanes = 0;
        QString roadType;
        bool hasCounts = false;     // a count station is attached to this link
        QString countStationId;
        std::vector<uint32_t> hourlyVolumes; // 24 elements representing traffic per hour
    };
    void showLinkInfo(const LinkInfo& info);

    void clear();

signals:
    void stopClicked(uint32_t stopId);       // User clicked a stop name in the list
    void routeClicked(uint32_t lineId);      // User clicked a route name in the list
    void panToRequested(float x, float y);   // Request map to pan to coordinates
    void closeRequested();                   // User clicked close button
    void showPersonRouteToggled(bool show);  // Show/hide full-day person route overlay
    void tripClicked(int tripIndex);         // User clicked a trip card (-1 = deselect)

private:
    void clearContent();
    QLabel* addHeader(const QString& text);
    QLabel* addBody(const QString& text);
    QFrame* addDivider();
    QLabel* addClickableItem(const QString& text, std::function<void()> onClick);

    QVBoxLayout* mainLayout_;
    QScrollArea* scrollArea_;
    QWidget* contentWidget_;
    QVBoxLayout* contentLayout_;
    QPushButton* closeButton_;
    QLabel* titleLabel_;

    // Whether the person-route overlay is currently shown (button label state).
    // Reset by MainWindow via resetPersonRouteState() when tracking changes.
    bool personRouteShown_ = false;

    // Trip card selected for single-trip map display (-1 = none).
    // Reset alongside personRouteShown_ on tracking changes.
    int selectedTripIndex_ = -1;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

public:
    void resetPersonRouteState() { personRouteShown_ = false; selectedTripIndex_ = -1; }
};

} // namespace simvis
