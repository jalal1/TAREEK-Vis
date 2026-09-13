#include "info_panel.h"
#include "panel_style.h"
#include "hourly_volume_chart.h"
#include <QScrollBar>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMouseEvent>
#include <QVariant>
#include <algorithm>

namespace simvis {

InfoPanel::InfoPanel(QWidget* parent)
    : QWidget(parent)
{
    setFixedWidth(280);

    mainLayout_ = new QVBoxLayout(this);
    mainLayout_->setContentsMargins(0, 0, 0, 0);
    mainLayout_->setSpacing(0);

    // Title bar with close button
    auto* titleBar = new QWidget(this);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(12, 8, 8, 8);

    titleLabel_ = new QLabel("Info Panel", titleBar);
    titleLabel_->setStyleSheet("font-weight: bold; font-size: 13pt;");
    titleLayout->addWidget(titleLabel_, 1);

    closeButton_ = new QPushButton("x", titleBar);
    closeButton_->setFixedSize(24, 24);
    closeButton_->setFlat(true);
    closeButton_->setStyleSheet(
        "QPushButton { font-weight: bold; border: none; font-size: 12pt; }"
        "QPushButton:hover { background-color: rgba(255,255,255,0.1); border-radius: 4px; }");
    connect(closeButton_, &QPushButton::clicked, this, &InfoPanel::closeRequested);
    titleLayout->addWidget(closeButton_);

    mainLayout_->addWidget(titleBar);

    // Divider under title
    auto* titleDivider = new QFrame(this);
    titleDivider->setFrameShape(QFrame::HLine);
    titleDivider->setStyleSheet("color: rgba(255,255,255,0.2);");
    mainLayout_->addWidget(titleDivider);

    // Scrollable content area
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setFrameShape(QFrame::NoFrame);

    contentWidget_ = new QWidget();
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(12, 12, 12, 12);
    contentLayout_->setSpacing(4);
    contentLayout_->addStretch();

    scrollArea_->setWidget(contentWidget_);
    mainLayout_->addWidget(scrollArea_, 1);
}

void InfoPanel::clearContent() {
    // Remove all widgets from content layout except the trailing stretch
    while (contentLayout_->count() > 0) {
        auto* item = contentLayout_->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    contentLayout_->addStretch();
}

QLabel* InfoPanel::addHeader(const QString& text) {
    auto* label = new QLabel(text, contentWidget_);
    label->setStyleSheet("font-weight: bold; font-size: 13pt; margin-bottom: 4px;");
    label->setWordWrap(true);
    // Insert before the trailing stretch
    contentLayout_->insertWidget(contentLayout_->count() - 1, label);
    return label;
}

QLabel* InfoPanel::addBody(const QString& text) {
    auto* label = new QLabel(text, contentWidget_);
    label->setStyleSheet("font-size: 11pt; padding-left: 8px;");
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout_->insertWidget(contentLayout_->count() - 1, label);
    return label;
}

QFrame* InfoPanel::addDivider() {
    auto* divider = new QFrame(contentWidget_);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet("color: rgba(255,255,255,0.15); margin-top: 8px; margin-bottom: 8px;");
    divider->setFixedHeight(1);
    contentLayout_->insertWidget(contentLayout_->count() - 1, divider);

    // Add spacing around the divider
    auto* spacerTop = new QWidget(contentWidget_);
    spacerTop->setFixedHeight(8);
    contentLayout_->insertWidget(contentLayout_->count() - 2, spacerTop);

    auto* spacerBottom = new QWidget(contentWidget_);
    spacerBottom->setFixedHeight(8);
    contentLayout_->insertWidget(contentLayout_->count() - 1, spacerBottom);

    return divider;
}

QLabel* InfoPanel::addClickableItem(const QString& text, std::function<void()> onClick) {
    // Card-style clickable list item (shared panel style; hover tints accent)
    auto* button = new QPushButton(text, contentWidget_);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QString(
        "QPushButton { text-align: left; padding: 6px 10px; font-size: 11pt; "
        "color: palette(text); background-color: %1; "
        "border: 1px solid %2; border-radius: 6px; }"
        "QPushButton:hover { background-color: %3; border: 1px solid %4; }")
        .arg(panelstyle::kCardBg, panelstyle::kCardBorder,
             panelstyle::kAccentBgTint, panelstyle::kAccent));
    connect(button, &QPushButton::clicked, onClick);
    contentLayout_->insertWidget(contentLayout_->count() - 1, button);

    // Small gap between items, matching the trip-card list rhythm
    auto* gap = new QWidget(contentWidget_);
    gap->setFixedHeight(4);
    contentLayout_->insertWidget(contentLayout_->count() - 1, gap);

    return nullptr;
}

void InfoPanel::clear() {
    clearContent();
    titleLabel_->setText("Info Panel");
}

bool InfoPanel::eventFilter(QObject* watched, QEvent* event) {
    // Trip cards: click toggles single-trip display on the map
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            QVariant idx = watched->property("tripIndex");
            if (idx.isValid()) {
                int tripIndex = idx.toInt();
                // Clicking the selected card again deselects it
                selectedTripIndex_ = (selectedTripIndex_ == tripIndex) ? -1 : tripIndex;
                emit tripClicked(selectedTripIndex_);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

static QString formatHms(float seconds) {
    int total = static_cast<int>(seconds + 0.5f);
    return QString("%1:%2:%3")
        .arg(total / 3600, 2, 10, QChar('0'))
        .arg((total / 60) % 60, 2, 10, QChar('0'))
        .arg(total % 60, 2, 10, QChar('0'));
}

using panelstyle::actIcon;
using panelstyle::modeIcon;

void InfoPanel::showVehicleInfo(const VehicleInfo& info) {
    clearContent();
    titleLabel_->setText("Vehicle Info");

    // Header: person id for driver panels ("person_8509"); transit vehicles
    // keep their vehicle name ("Tram — veh_6760_tram")
    QString title;
    if (info.passengerCount >= 0) {
        title = info.mode;
        if (!info.vehicleName.isEmpty())
            title += QString(" — %1").arg(info.vehicleName);
    } else if (!info.personName.isEmpty()) {
        title = info.personName;
    } else if (!info.vehicleName.isEmpty()) {
        title = info.vehicleName;
    } else {
        title = QString("%1 #%2").arg(info.mode).arg(info.vehicleId);
    }
    addHeader(title);

    // Transit service section
    if (info.passengerCount >= 0) {
        addDivider();
        addHeader("Service");
        if (!info.lineName.isEmpty())
            addBody(QString("Line: %1").arg(info.lineName));
        if (!info.routeName.isEmpty())
            addBody(QString("Route: %1").arg(info.routeName));
        addBody(QString("Passengers aboard: %1").arg(info.passengerCount));
    }

    // Trips (cars): card-style list. The person id already appears in the
    // panel title, so it is not repeated here.
    if (!info.trips.empty()) {
        addDivider();
        addHeader(QString("Trips (%1)").arg(info.trips.size()));

        for (int tripIdx = 0; tripIdx < static_cast<int>(info.trips.size()); ++tripIdx) {
            const auto& trip = info.trips[tripIdx];
            QString from = trip.fromAct.isEmpty() ? QString::fromUtf8("…") : trip.fromAct;
            QString to   = trip.toAct.isEmpty()   ? QString::fromUtf8("…") : trip.toAct;

            // Selection (blue card) = user clicked this trip to show it on the
            // map. The trip active at the current sim time only gets a filled
            // blue pill, so the two states stay visually distinct.
            const bool selected = tripIdx == selectedTripIndex_;
            const bool pillAccent = trip.active || selected;

            auto* card = panelstyle::makeCard(contentWidget_, contentLayout_, selected);
            // Hover feedback (like the route button) so cards read as clickable
            card->setStyleSheet(card->styleSheet() + QString(
                "#card:hover { background-color: %1; border: 1px solid %2; }")
                .arg(panelstyle::kAccentBgTint, panelstyle::kAccent));
            card->setCursor(Qt::PointingHandCursor);
            card->setProperty("tripIndex", tripIdx);
            card->installEventFilter(this);   // click -> tripClicked(index)

            // 3-column grid keeps every element aligned to its endpoint:
            //   [🏠 Home]        ( 🚗 car )        [💼 Work]
            //   06:32:15         (00:39:16)         07:12:31
            auto* grid = new QGridLayout(card);
            grid->setContentsMargins(10, 7, 10, 7);
            grid->setHorizontalSpacing(6);
            grid->setVerticalSpacing(3);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 0);
            grid->setColumnStretch(2, 1);

            auto* fromLabel = new QLabel(
                QString("%1 %2").arg(actIcon(trip.fromAct), from), card);
            fromLabel->setStyleSheet(panelstyle::cardTitleStyle(selected));
            grid->addWidget(fromLabel, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

            auto* modePill = new QLabel(
                QString("%1 %2").arg(modeIcon(trip.mode), trip.mode), card);
            modePill->setAlignment(Qt::AlignCenter);
            modePill->setStyleSheet(panelstyle::pillStyle(pillAccent));
            grid->addWidget(modePill, 0, 1, Qt::AlignHCenter | Qt::AlignVCenter);

            auto* toLabel = new QLabel(
                QString("%1 %2").arg(actIcon(trip.toAct), to), card);
            toLabel->setStyleSheet(panelstyle::cardTitleStyle(selected));
            grid->addWidget(toLabel, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

            auto* departLabel = new QLabel(formatHms(trip.departSec), card);
            departLabel->setStyleSheet(panelstyle::cardDetailStyle(selected));
            grid->addWidget(departLabel, 1, 0, Qt::AlignLeft);

            QString durationText = trip.arriveSec >= 0.0f
                ? QString("(%1)").arg(formatHms(trip.arriveSec - trip.departSec))
                : QString();
            auto* durationLabel = new QLabel(durationText, card);
            durationLabel->setStyleSheet(panelstyle::cardDetailStyle(selected));
            grid->addWidget(durationLabel, 1, 1, Qt::AlignHCenter);

            QString arriveText = trip.arriveSec >= 0.0f
                ? formatHms(trip.arriveSec) : QString("stuck");
            auto* arriveLabel = new QLabel(arriveText, card);
            arriveLabel->setStyleSheet(panelstyle::cardDetailStyle(selected));
            grid->addWidget(arriveLabel, 1, 2, Qt::AlignRight);

            auto* gap = new QWidget(contentWidget_);
            gap->setFixedHeight(6);
            contentLayout_->insertWidget(contentLayout_->count() - 1, gap);
        }

        // Show/hide the full-day route overlay on the map.
        // Solid filled button so it clearly reads as clickable.
        auto* routeButton = new QPushButton(
            personRouteShown_
                ? QString::fromUtf8("\U0001F5FA  Hide Person Route")
                : QString::fromUtf8("\U0001F5FA  Show Person Route"),
            contentWidget_);
        routeButton->setCursor(Qt::PointingHandCursor);
        routeButton->setMinimumHeight(34);
        routeButton->setStyleSheet(personRouteShown_
            ? panelstyle::kButtonStyleOn : panelstyle::kButtonStyle);
        connect(routeButton, &QPushButton::clicked, this, [this, routeButton]() {
            personRouteShown_ = !personRouteShown_;
            routeButton->setText(personRouteShown_
                ? QString::fromUtf8("\U0001F5FA  Hide Person Route")
                : QString::fromUtf8("\U0001F5FA  Show Person Route"));
            routeButton->setStyleSheet(personRouteShown_
                ? panelstyle::kButtonStyleOn : panelstyle::kButtonStyle);
            emit showPersonRouteToggled(personRouteShown_);
        });
        contentLayout_->insertWidget(contentLayout_->count() - 1, routeButton);
    } else if (info.passengerCount < 0) {
        addDivider();
        addBody("No trip information available");
    }
}

void InfoPanel::showLinkInfo(const LinkInfo& info) {
    clearContent();
    titleLabel_->setText("Link Info");

    addHeader(QString("Link %1").arg(info.linkId));

    // Key/value rows in a single card for a clean, aligned look
    auto* card = panelstyle::makeCard(contentWidget_, contentLayout_, false);
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(10, 8, 10, 8);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    int row = 0;
    auto addRow = [&](const QString& key, const QString& value) {
        auto* k = new QLabel(key, card);
        k->setStyleSheet("color: gray; background: transparent; border: none;");
        auto* v = new QLabel(value, card);
        v->setStyleSheet("font-weight: bold; background: transparent; border: none;");
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setAlignment(Qt::AlignRight);
        grid->addWidget(k, row, 0, Qt::AlignLeft);
        grid->addWidget(v, row, 1, Qt::AlignRight);
        ++row;
    };

    const double kmh = info.freeSpeedMs * 3.6;
    // Free-flow travel time across the link (length / freespeed)
    const double ffSec = info.freeSpeedMs > 0.0 ? info.lengthM / info.freeSpeedMs : 0.0;

    addRow("Length", info.lengthM >= 1000.0
        ? QString("%1 km").arg(info.lengthM / 1000.0, 0, 'f', 2)
        : QString("%1 m").arg(info.lengthM, 0, 'f', 0));
    addRow("Free speed", QString("%1 m/s  (%2 km/h)")
        .arg(info.freeSpeedMs, 0, 'f', 1).arg(kmh, 0, 'f', 0));
    addRow("Capacity", QString("%1 veh/h").arg(info.capacityVehH, 0, 'f', 0));
    addRow("Lanes", QString::number(info.lanes));
    if (!info.roadType.isEmpty() && info.roadType != "Unknown")
        addRow("Road type", info.roadType);
    if (ffSec > 0.0) {
        int s = static_cast<int>(ffSec + 0.5);
        addRow("Free-flow time", s >= 60
            ? QString("%1m %2s").arg(s / 60).arg(s % 60)
            : QString("%1 s").arg(s));
    }

    addDivider();
    addHeader("Nodes");
    auto* nodeCard = panelstyle::makeCard(contentWidget_, contentLayout_, false);
    auto* nodeGrid = new QGridLayout(nodeCard);
    nodeGrid->setContentsMargins(10, 8, 10, 8);
    nodeGrid->setHorizontalSpacing(10);
    nodeGrid->setVerticalSpacing(5);
    nodeGrid->setColumnStretch(1, 1);
    {
        auto kv = [&](int r, const QString& key, const QString& value) {
            auto* k = new QLabel(key, nodeCard);
            k->setStyleSheet("color: gray; background: transparent; border: none;");
            auto* v = new QLabel(value, nodeCard);
            v->setStyleSheet("font-weight: bold; background: transparent; border: none;");
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->setAlignment(Qt::AlignRight);
            nodeGrid->addWidget(k, r, 0, Qt::AlignLeft);
            nodeGrid->addWidget(v, r, 1, Qt::AlignRight);
        };
        kv(0, "From", info.fromNode);
        kv(1, "To", info.toNode);
    }

    if (info.hourlyVolumes.size() == 24) {
        addDivider();
        addHeader("Hourly Volume");

        auto* chart = new HourlyVolumeChart();
        chart->setVolumes(info.hourlyVolumes);
        contentLayout_->insertWidget(contentLayout_->count() - 1, chart);

        uint32_t total = 0;
        for (uint32_t v : info.hourlyVolumes) total += v;
        if (total > 0) {
            const int peak = static_cast<int>(
                std::max_element(info.hourlyVolumes.begin(), info.hourlyVolumes.end())
                - info.hourlyVolumes.begin());
            addBody(QString("%1 vehicles over the day, busiest %2:00-%3:00")
                        .arg(total)
                        .arg(peak, 2, 10, QChar('0'))
                        .arg((peak + 1) % 24, 2, 10, QChar('0')));
        } else {
            addBody("No vehicles recorded on this link.");
        }
    }

    if (info.hasCounts) {
        addDivider();
        addHeader("Counts");
        addBody(QString("Count station: %1").arg(info.countStationId));
        addBody("Open File > Load Counts chart from the map to compare volumes.");
    }

    scrollArea_->verticalScrollBar()->setValue(0);
    show();
}

void InfoPanel::showStopInfo(const TransitStop& stop,
                              const std::vector<const TransitLine*>& servingLines) {
    clearContent();
    titleLabel_->setText("Stop Info");

    // Stop name
    addHeader(QString::fromStdString(stop.name.empty() ? stop.originalId : stop.name));

    addDivider();

    // Stop ID
    addHeader("Stop ID");
    addBody(QString::fromStdString(stop.originalId));

    addDivider();

    // Location
    addHeader("Location");
    addBody(QString("%1, %2").arg(stop.x, 0, 'f', 2).arg(stop.y, 0, 'f', 2));

    addDivider();

    // Lines serving this stop
    if (!servingLines.empty()) {
        addHeader(QString("Lines Serving This Stop (%1)").arg(servingLines.size()));

        for (const auto* line : servingLines) {
            QString modeTag;
            switch (line->primaryMode) {
                case TransitMode::Bus:  modeTag = "[B]"; break;
                case TransitMode::Tram: modeTag = "[T]"; break;
                case TransitMode::Rail: modeTag = "[R]"; break;
                default: modeTag = "[?]"; break;
            }

            QString displayName = QString::fromStdString(line->name);
            if (displayName.isEmpty()) displayName = QString::fromStdString(line->originalId);

            uint32_t lineId = line->id;
            addClickableItem(
                QString("%1 %2").arg(modeTag, displayName),
                [this, lineId]() { emit routeClicked(lineId); }
            );
        }

        addDivider();
    }

    // Network link
    if (stop.linkRefId != 0) {
        addHeader("Network Link");
        addBody(QString::number(stop.linkRefId));
    }

    // Scroll to top
    scrollArea_->verticalScrollBar()->setValue(0);
    show();
}

void InfoPanel::showRouteInfo(const TransitLine& line,
                               const TransitRoute& route,
                               const TransitScheduleParser::Result& transitData) {
    clearContent();
    titleLabel_->setText("Route Info");

    // Route/Line name
    QString displayName = QString::fromStdString(line.name);
    if (displayName.isEmpty()) displayName = QString::fromStdString(line.originalId);
    addHeader(displayName);

    // Mode
    QString modeName = QString::fromStdString(std::string(transitModeName(route.mode)));
    addBody(modeName);

    // Line ID
    addBody(QString("Line: %1").arg(QString::fromStdString(line.originalId)));

    addDivider();

    // Stops list
    addHeader(QString("Stops (%1)").arg(route.stops.size()));

    int stopNum = 1;
    for (const auto& routeStop : route.stops) {
        auto it = transitData.stopIdToIndex.find(routeStop.stopId);
        if (it == transitData.stopIdToIndex.end()) {
            stopNum++;
            continue;
        }

        const auto& stop = transitData.stops[it->second];
        QString stopName = QString::fromStdString(
            stop.name.empty() ? stop.originalId : stop.name);

        float sx = stop.x;
        float sy = stop.y;
        uint32_t sid = routeStop.stopId;

        addClickableItem(
            QString("%1. %2").arg(stopNum).arg(stopName),
            [this, sx, sy, sid]() {
                emit panToRequested(sx, sy);
                emit stopClicked(sid);
            }
        );
        stopNum++;
    }

    addDivider();

    // Schedule summary
    if (!route.departures.empty()) {
        addHeader("Schedule");
        addBody(QString("Departures: %1").arg(route.departures.size()));

        // Find first and last departure times
        uint32_t earliest = route.departures[0].departureTimeMs;
        uint32_t latest = route.departures[0].departureTimeMs;
        for (const auto& dep : route.departures) {
            if (dep.departureTimeMs < earliest) earliest = dep.departureTimeMs;
            if (dep.departureTimeMs > latest) latest = dep.departureTimeMs;
        }

        auto msToTimeStr = [](uint32_t ms) -> QString {
            uint32_t totalSec = ms / 1000;
            uint32_t hours = totalSec / 3600;
            uint32_t minutes = (totalSec % 3600) / 60;
            return QString("%1:%2").arg(hours, 2, 10, QChar('0')).arg(minutes, 2, 10, QChar('0'));
        };

        addBody(QString("First: %1").arg(msToTimeStr(earliest)));
        addBody(QString("Last: %1").arg(msToTimeStr(latest)));
    }

    // Scroll to top
    scrollArea_->verticalScrollBar()->setValue(0);
    show();
}

} // namespace simvis
