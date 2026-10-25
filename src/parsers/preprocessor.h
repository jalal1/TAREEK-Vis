#pragma once

#include "core/types.h"
#include "network_parser.h"
#include "event_parser.h"
#include "transit_schedule_parser.h"
#include <QObject>
#include <QString>
#include <memory>

namespace simvis {

// Preprocessor that converts XML files to binary format
// Runs in a separate thread and emits progress signals for UI
class Preprocessor : public QObject {
    Q_OBJECT

public:
    explicit Preprocessor(QObject* parent = nullptr);
    ~Preprocessor() override;

    // Set input files
    void setNetworkFile(const QString& path);
    void setEventsFile(const QString& path);
    void setTransitScheduleFile(const QString& path);
    void setOutputDirectory(const QString& path);

    // Check if cached binary files exist and are valid
    bool hasCachedFiles() const;

    // Get output paths
    QString networkBinaryPath() const;
    QString vehicleIndexPath() const;

    // Access parsed transit data (available after processAll completes)
    const TransitScheduleParser::Result* transitResult() const { return transitResult_.get(); }

    // Transfer ownership of transit result to caller
    std::unique_ptr<TransitScheduleParser::Result> takeTransitResult() { return std::move(transitResult_); }

    // Get CRS string parsed from network XML (e.g. "EPSG:26915")
    QString networkCRS() const {
        return networkResult_ ? QString::fromStdString(networkResult_->crs) : QString();
    }

signals:
    void progressChanged(int percent, const QString& message);
    void networkCompleted(bool success, const QString& message);
    void eventsCompleted(bool success, const QString& message);
    void transitCompleted(bool success, const QString& message);
    void allCompleted(bool success);

public slots:
    void processNetwork();
    void processEvents();
    void processTransitSchedule();
    void writeVehicleIndex();
    void processAll();
    void cancel();

private:
    // Sets the mode of each non-transit vehicle (core/modes.h)
    void classifyVehicleModes();
    // Sets the vertiport position of each hold event from its flight
    void placeHolds();

    QString networkXmlPath_;
    QString eventsXmlPath_;
    QString transitSchedulePath_;
    QString outputDir_;
    bool cancelled_ = false;

    // Cached results from network parsing (needed for event parsing and transit)
    std::unique_ptr<NetworkParser::Result> networkResult_;

    // Events result (held temporarily until .vidx is written with mode data)
    std::unique_ptr<EventParser::StreamResult> eventsResult_;

    // Transit schedule result (kept for UI to access after preprocessing)
    std::unique_ptr<TransitScheduleParser::Result> transitResult_;
};

} // namespace simvis
