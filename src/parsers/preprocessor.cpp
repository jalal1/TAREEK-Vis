#include "preprocessor.h"
#include "data/vehicle_index.h"
#include "core/logger.h"
#include "core/modes.h"
#include <QDir>
#include <QFileInfo>
#include <filesystem>

namespace simvis {

Preprocessor::Preprocessor(QObject* parent)
    : QObject(parent)
{
}

Preprocessor::~Preprocessor() = default;

void Preprocessor::setNetworkFile(const QString& path) {
    networkXmlPath_ = path;
}

void Preprocessor::setEventsFile(const QString& path) {
    eventsXmlPath_ = path;
}

void Preprocessor::setTransitScheduleFile(const QString& path) {
    transitSchedulePath_ = path;
}

void Preprocessor::setOutputDirectory(const QString& path) {
    outputDir_ = path;

    // Create directory if it doesn't exist
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}

bool Preprocessor::hasCachedFiles() const {
    // A file of an older format counts as missing, so it is made again
    return QFileInfo::exists(networkBinaryPath()) &&
           QFileInfo::exists(vehicleIndexPath()) &&
           NetworkParser::readBinaryHeader(networkBinaryPath().toStdString()).magic
               == NETWORK_FILE_MAGIC &&
           VehicleIndex::fileVersion(vehicleIndexPath()) == VIDX_FILE_VERSION;
}

QString Preprocessor::networkBinaryPath() const {
    if (outputDir_.isEmpty() || networkXmlPath_.isEmpty()) return QString();

    QFileInfo info(networkXmlPath_);
    QString baseName = info.completeBaseName();  // removes .gz
    if (baseName.endsWith(".xml")) {
        baseName = baseName.left(baseName.length() - 4);
    }
    return outputDir_ + "/" + baseName + ".bin";
}

QString Preprocessor::vehicleIndexPath() const {
    if (outputDir_.isEmpty() || eventsXmlPath_.isEmpty()) return QString();

    QFileInfo info(eventsXmlPath_);
    QString baseName = info.completeBaseName();
    if (baseName.endsWith(".xml")) {
        baseName = baseName.left(baseName.length() - 4);
    }
    return outputDir_ + "/" + baseName + ".vidx";
}

void Preprocessor::processNetwork() {
    cancelled_ = false;
    LOG_INFO(QString("Preprocessor: parsing network XML: %1").arg(networkXmlPath_));

    emit progressChanged(0, "Starting network preprocessing...");

    auto progressCb = [this](size_t current, size_t total, const std::string& msg) {
        if (cancelled_) return;

        int percent = (total > 0) ? static_cast<int>(100 * current / total) : 0;
        // Network is 0-50% of total progress
        emit progressChanged(percent / 2, QString::fromStdString(msg));
    };

    auto result = NetworkParser::parseGzipFile(
        networkXmlPath_.toStdString(),
        progressCb
    );

    if (cancelled_) {
        emit networkCompleted(false, "Cancelled");
        return;
    }

    if (!result.success) {
        LOG_ERROR(QString("Preprocessor: network parse failed - %1")
            .arg(QString::fromStdString(result.errorMessage)));
        emit networkCompleted(false, QString::fromStdString(result.errorMessage));
        return;
    }

    // Save result for event parsing
    networkResult_ = std::make_unique<NetworkParser::Result>(std::move(result));

    // Write binary file
    emit progressChanged(45, "Writing network binary file...");

    bool writeSuccess = NetworkParser::writeBinary(
        networkBinaryPath().toStdString(),
        *networkResult_
    );

    if (writeSuccess) {
        QString msg = QString("Network: %1 nodes, %2 links")
            .arg(networkResult_->nodes.size())
            .arg(networkResult_->links.size());
        LOG_INFO(QString("Preprocessor: network done - %1").arg(msg));
        emit networkCompleted(true, msg);
    } else {
        LOG_ERROR("Preprocessor: failed to write network .bin");
        emit networkCompleted(false, "Failed to write binary file");
    }
}

void Preprocessor::processEvents() {
    if (!networkResult_) {
        emit eventsCompleted(false, "Network must be processed first");
        return;
    }

    cancelled_ = false;
    LOG_INFO(QString("Preprocessor: parsing events XML: %1").arg(eventsXmlPath_));

    emit progressChanged(50, "Starting events preprocessing...");

    auto progressCb = [this](size_t current, size_t total, const std::string& msg) {
        if (cancelled_) return;

        int percent = (total > 0) ? static_cast<int>(100 * current / total) : 0;
        // Events is 50-90% of total progress
        emit progressChanged(50 + percent * 40 / 100, QString::fromStdString(msg));
    };

    // Single-pass: parse XML and build vehicle index simultaneously
    auto streamResult = EventParser::streamProcessEvents(
        eventsXmlPath_.toStdString(),
        networkResult_->linkIds,
        progressCb
    );

    if (cancelled_) {
        emit eventsCompleted(false, "Cancelled");
        return;
    }

    if (!streamResult.success) {
        LOG_ERROR(QString("Preprocessor: events parse failed - %1")
            .arg(QString::fromStdString(streamResult.errorMessage)));
        emit eventsCompleted(false, QString::fromStdString(streamResult.errorMessage));
        return;
    }

    // Store result - defer writing until after transit parsing (for mode cross-reference)
    eventsResult_ = std::make_unique<EventParser::StreamResult>(std::move(streamResult));

    size_t totalSegments = 0;
    for (const auto& t : eventsResult_->vehicleIndex.vehicles_)
        totalSegments += t.segments.size();

    QString msg = QString("Events: %1 vehicles, %2 total segments (time: %3s - %4s)")
        .arg(eventsResult_->vehicleIndex.vehicleCount())
        .arg(totalSegments)
        .arg(VehicleIndex::toSeconds(eventsResult_->vehicleIndex.minTime()), 0, 'f', 1)
        .arg(VehicleIndex::toSeconds(eventsResult_->vehicleIndex.maxTime()), 0, 'f', 1);
    LOG_INFO(QString("Preprocessor: events done - %1").arg(msg));
    emit eventsCompleted(true, msg);
}

void Preprocessor::processTransitSchedule() {
    if (!networkResult_) {
        emit transitCompleted(false, "Network must be processed first");
        return;
    }

    if (transitSchedulePath_.isEmpty()) {
        // Transit is optional - not an error
        emit transitCompleted(true, "No transit schedule file provided");
        return;
    }

    LOG_INFO(QString("Preprocessor: parsing transit schedule: %1").arg(transitSchedulePath_));
    emit progressChanged(90, "Parsing transit schedule...");

    auto progressCb = [this](size_t current, size_t total, const std::string& msg) {
        if (cancelled_) return;
        int percent = (total > 0) ? static_cast<int>(100 * current / total) : 0;
        // Transit is 90-100% of total progress
        emit progressChanged(90 + percent / 10, QString::fromStdString(msg));
    };

    // Detect gzip vs plain XML
    bool isGzip = transitSchedulePath_.endsWith(".gz", Qt::CaseInsensitive);

    TransitScheduleParser::Result result;
    if (isGzip) {
        result = TransitScheduleParser::parseGzipFile(
            transitSchedulePath_.toStdString(),
            networkResult_->linkIds,
            progressCb
        );
    } else {
        result = TransitScheduleParser::parse(
            transitSchedulePath_.toStdString(),
            networkResult_->linkIds,
            progressCb
        );
    }

    if (cancelled_) {
        emit transitCompleted(false, "Cancelled");
        return;
    }

    if (!result.success) {
        LOG_ERROR(QString("Preprocessor: transit parse failed - %1")
            .arg(QString::fromStdString(result.errorMessage)));
        emit transitCompleted(false, QString::fromStdString(result.errorMessage));
        return;
    }

    QString msg = QString("Transit: %1 stops, %2 lines, %3 vehicle mappings")
        .arg(result.stops.size())
        .arg(result.lines.size())
        .arg(result.vehicleToMode.size());

    LOG_INFO(QString("Preprocessor: transit done - %1").arg(msg));
    transitResult_ = std::make_unique<TransitScheduleParser::Result>(std::move(result));
    emit transitCompleted(true, msg);
}

void Preprocessor::writeVehicleIndex() {
    if (!eventsResult_) {
        return;
    }

    // Cross-reference transit vehicle modes into VehicleIndex
    if (transitResult_ && transitResult_->success) {
        auto& vidx = eventsResult_->vehicleIndex;
        const auto& vehicleToMode = transitResult_->vehicleToMode;
        const auto& vehicleIds = eventsResult_->vehicleIds;

        size_t classified = 0;
        for (size_t i = 0; i < vehicleIds.size(); ++i) {
            const auto& vehicleName = vehicleIds.strings()[i];
            auto it = vehicleToMode.find(vehicleName);
            if (it != vehicleToMode.end()) {
                vidx.vehicleModes_[i] = static_cast<uint8_t>(it->second);
                ++classified;
            }
        }

        LOG_INFO(QString("Preprocessor: classified %1 transit vehicles").arg(classified));
        emit progressChanged(97, QString("Classified %1 transit vehicles").arg(classified));
    }

    classifyVehicleModes();
    placeHolds();

    emit progressChanged(98, "Writing vehicle index...");

    // Move the events-side ID interners into the index so they are serialized
    // into the .vidx (v3 string tables). Must happen before eventsResult_.reset().
    {
        auto& vidx = eventsResult_->vehicleIndex;
        vidx.vehicleIdStrings_ = std::move(eventsResult_->vehicleIds);
        vidx.personIdStrings_ = std::move(eventsResult_->personIds);
    }

    bool writeSuccess = VehicleIndex::writeFile(
        vehicleIndexPath(), eventsResult_->vehicleIndex);

    if (!writeSuccess) {
        LOG_ERROR("Preprocessor: failed to write .vidx");
        emit eventsCompleted(false, "Failed to write vehicle index file");
    }

    eventsResult_.reset();
}

void Preprocessor::classifyVehicleModes() {
    // The transit schedule has set the transit modes. For every other
    // vehicle, the mode comes from the leg mode of its driver's trip in it,
    // through the mode registry. Only the driver counts, so a passenger's
    // "pt" leg does not change a bus. Last, a vehicle that starts on an air
    // link is an aircraft, for files without person events.
    auto& vidx = eventsResult_->vehicleIndex;
    std::vector<uint8_t> modeOfLeg(vidx.legModeStrings_.size());
    for (size_t i = 0; i < modeOfLeg.size(); ++i)
        modeOfLeg[i] = modes::fromLegMode(vidx.legModeStrings_.strings()[i]);

    const auto& links = networkResult_->links;
    std::vector<size_t> perMode(modes::all().size(), 0);
    for (size_t v = 0; v < vidx.vehicles_.size(); ++v) {
        uint8_t& mode = vidx.vehicleModes_[v];
        if (mode != modes::kCar) {
            if (mode < perMode.size()) ++perMode[mode];
            continue;
        }
        const uint32_t driver = v < vidx.driverOfVehicle_.size() ? vidx.driverOfVehicle_[v] : 0;
        if (driver != 0 && driver - 1 < vidx.personTrips_.size()) {
            for (const PersonTrip& trip : vidx.personTrips_[driver - 1]) {
                if (trip.vehicleId != v + 1 || trip.legModeId >= modeOfLeg.size()) continue;
                if (modeOfLeg[trip.legModeId] != modes::kNone) {
                    mode = modeOfLeg[trip.legModeId];
                    break;
                }
            }
        }
        // Network links are interned in file order, so link id i is
        // normally links[i]; the id test covers a file with a repeated id
        const auto& segments = vidx.vehicles_[v].segments;
        const uint32_t first = segments.empty() ? kNoLinkId : segments.front().linkId;
        if (mode == modes::kCar && first < links.size() && links[first].id == first &&
            isAirRoadType(static_cast<RoadType>(links[first].roadType))) {
            mode = modes::kAir;
        }
        ++perMode[mode];
    }
    for (size_t m = 0; m < perMode.size(); ++m) {
        if (perMode[m] == 0) continue;
        LOG_INFO(QString("Preprocessor: %1 vehicles of mode %2")
            .arg(perMode[m]).arg(modes::all()[m].key));
    }
}

void Preprocessor::placeHolds() {
    // A hold is at a vertiport, and the event names it only by id. A ground
    // hold is at the start of the flight (the from-node of its first
    // segment), an airborne hold at its end (the to-node of its last one).
    auto& vidx = eventsResult_->vehicleIndex;
    if (vidx.overlayEvents_.empty()) return;
    const auto& net = *networkResult_;
    auto nodeOf = [&net](uint32_t linkId, bool from) -> const NodeRecord* {
        if (linkId >= net.links.size() || net.links[linkId].id != linkId) return nullptr;
        const uint32_t n = from ? net.links[linkId].fromNode : net.links[linkId].toNode;
        return (n < net.nodes.size() && net.nodes[n].id == n) ? &net.nodes[n] : nullptr;
    };

    size_t counts[4] = {0, 0, 0, 0};
    size_t unplaced = 0;
    for (OverlayEvent& ev : vidx.overlayEvents_) {
        if (ev.kind < 4) ++counts[ev.kind];
        if (ev.kind == static_cast<uint8_t>(OverlayKind::Loss)) continue;
        const bool ground = ev.kind == static_cast<uint8_t>(OverlayKind::GroundHold);
        const NodeRecord* node = nullptr;
        if (ev.vehicleA < vidx.vehicles_.size() && !vidx.vehicles_[ev.vehicleA].segments.empty()) {
            const auto& segs = vidx.vehicles_[ev.vehicleA].segments;
            node = nodeOf(ground ? segs.front().linkId : segs.back().linkId, ground);
        }
        if (node) {
            ev.x = node->x;
            ev.y = node->y;
            ev.flags |= OVERLAY_FLAG_PLACED;
        } else {
            ++unplaced;
        }
    }
    LOG_INFO(QString("Preprocessor: overlay events: %1 losses, %2 ground holds, "
                     "%3 airborne holds (%4 holds without a position)")
        .arg(counts[1]).arg(counts[2]).arg(counts[3]).arg(unplaced));
}

void Preprocessor::processAll() {
    processNetwork();

    if (cancelled_) {
        emit allCompleted(false);
        return;
    }

    if (!networkResult_ || networkResult_->nodes.empty()) {
        emit allCompleted(false);
        return;
    }

    processEvents();

    if (cancelled_) {
        emit allCompleted(false);
        return;
    }

    // Parse transit schedule (optional - won't fail if not provided)
    processTransitSchedule();

    if (cancelled_) {
        emit allCompleted(false);
        return;
    }

    // Cross-reference transit modes and write .vidx (must be after both events + transit)
    writeVehicleIndex();

    emit progressChanged(100, "Preprocessing complete");
    emit allCompleted(!cancelled_);
}

void Preprocessor::cancel() {
    cancelled_ = true;
}

} // namespace simvis
