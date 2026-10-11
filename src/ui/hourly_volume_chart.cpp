#include "hourly_volume_chart.h"
#include "panel_style.h"
#include "screenshot_exporter.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QToolTip>
#include <algorithm>

namespace simvis {
namespace {

// Room for the y-axis numbers and the axis caption.
constexpr int kMarginLeft = 34;
constexpr int kMarginRight = 6;
constexpr int kMarginTop = 8;
constexpr int kAxisCaptionBand = 14; // "Hour of Day"

// Numbered hours get a long tick, the hours between them a short one, so every
// hour is marked on the axis whether or not it carries a number.
constexpr int kMajorTick = 4;
constexpr int kMinorTick = 2;

constexpr int kYTicks = 4;

// Round the axis top up to something a reader can divide by eye, matching the
// steps CountsChartWidget uses.
uint32_t niceAxisMax(uint32_t peak) {
    if (peak == 0) return 10;
    if (peak <= 100) return ((peak / 10) + 1) * 10;
    if (peak <= 1000) return ((peak / 100) + 1) * 100;
    if (peak <= 10000) return ((peak / 1000) + 1) * 1000;
    return ((peak / 5000) + 1) * 5000;
}

// Numbers go on even hours: every second hour, or every fourth or sixth where
// the panel is too narrow for that. Always counted from 0, so in the usual case
// the axis reads 0, 2, 4 ... 22.
int labelStep(const QFontMetrics& fm, double slotW) {
    const int widest = fm.horizontalAdvance(QStringLiteral("22")) + 4;
    for (int step : {2, 4, 6}) {
        if (step * slotW >= widest) return step;
    }
    return 6;
}

} // namespace

HourlyVolumeChart::HourlyVolumeChart(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(180);
}

void HourlyVolumeChart::setVolumes(const std::vector<uint32_t>& volumes) {
    if (volumes.size() != 24) {
        volumes_.clear();
        maxVolume_ = 0;
    } else {
        volumes_ = volumes;
        maxVolume_ = *std::max_element(volumes_.begin(), volumes_.end());
    }
    hoverHour_ = -1;
    update();
}

QSize HourlyVolumeChart::sizeHint() const {
    return QSize(240, 200);
}

int HourlyVolumeChart::hourAt(int x) const {
    const int plotW = width() - kMarginLeft - kMarginRight;
    if (volumes_.size() != 24 || plotW <= 0) return -1;
    if (x < kMarginLeft || x >= kMarginLeft + plotW) return -1;
    const int hour = (x - kMarginLeft) * 24 / plotW;
    return std::clamp(hour, 0, 23);
}

// Colors for one rendering. The panel is dark; an exported file goes on white
// paper, where the panel's light grid lines would not show.
struct HourlyVolumeChart::Colors {
    QColor background;  // invalid = leave transparent
    QColor grid;
    QColor value;       // y-axis numbers and hour numbers
    QColor valueHot;    // the hour under the cursor
    QColor tick;
    QColor tickHot;
    QColor axis;
    QColor caption;
    QColor title;
};

namespace {

const HourlyVolumeChart::Colors& screenColors();
const HourlyVolumeChart::Colors& paperColors();

} // namespace

void HourlyVolumeChart::paintEvent(QPaintEvent* /*event*/) {
    if (volumes_.size() != 24) return;
    QPainter p(this);
    paintChart(p, rect(), screenColors(), hoverHour_, false);
}

void HourlyVolumeChart::paintChart(QPainter& p, const QRect& area, const Colors& c,
                                   int hoverHour, bool withTitle) const {
    if (volumes_.size() != 24) return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (c.background.isValid()) p.fillRect(area, c.background);
    p.translate(area.topLeft());
    const int areaW = area.width();
    const int areaH = area.height();

    // Title band for an exported chart: link and experiment, so two files
    // (baseline and scenario) can be told apart.
    int top = kMarginTop;
    if (withTitle && !exportTitle_.isEmpty()) {
        QFont titleFont = font();
        titleFont.setPointSize(10);
        titleFont.setBold(true);
        QFont subFont = font();
        subFont.setPointSize(8);
        const int titleH = QFontMetrics(titleFont).height();
        const int subH = exportSubtitle_.isEmpty() ? 0 : QFontMetrics(subFont).height();

        p.setPen(c.title);
        p.setFont(titleFont);
        p.drawText(kMarginLeft, top, areaW - kMarginLeft - kMarginRight, titleH,
                   Qt::AlignLeft | Qt::AlignVCenter, exportTitle_);
        if (subH > 0) {
            p.setPen(c.caption);
            p.setFont(subFont);
            p.drawText(kMarginLeft, top + titleH, areaW - kMarginLeft - kMarginRight, subH,
                       Qt::AlignLeft | Qt::AlignVCenter, exportSubtitle_);
        }
        top += titleH + subH + 10;
    }

    QFont tickFont = font();
    tickFont.setPointSize(7);
    p.setFont(tickFont);

    const QFontMetrics fm(tickFont);
    const int plotW = areaW - kMarginLeft - kMarginRight;
    const double slotW = static_cast<double>(plotW) / 24.0;
    const int step = labelStep(fm, slotW);
    const int labelRowH = fm.height();
    const int hourLabelBand = labelRowH + kMajorTick + 2;

    const int plotH = areaH - top - hourLabelBand - kAxisCaptionBand;
    if (plotW < 60 || plotH < 40) { p.restore(); return; }

    const int baseline = top + plotH;
    const uint32_t axisMax = niceAxisMax(maxVolume_);

    // Horizontal grid with its value, so bar heights can be read off
    for (int i = 0; i <= kYTicks; ++i) {
        const int y = baseline - i * plotH / kYTicks;
        p.setPen(QPen(c.grid, 1));
        p.drawLine(kMarginLeft, y, kMarginLeft + plotW, y);

        p.setPen(c.value);
        p.drawText(0, y - 7, kMarginLeft - 4, 14,
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(static_cast<uint32_t>(i) * axisMax / kYTicks));
    }

    const double barW = std::max(2.0, slotW * 0.74);
    const QColor barColor(panelstyle::kAccent);
    const QColor hoverColor = barColor.lighter(135);

    auto centreOf = [&](int h) { return kMarginLeft + (h + 0.5) * slotW; };

    for (int h = 0; h < 24; ++h) {
        const double barX = kMarginLeft + h * slotW + (slotW - barW) / 2.0;
        const bool hot = (h == hoverHour);

        // Every hour keeps its slot. An hour with no traffic simply draws no
        // bar, which is what an empty hour should look like.
        if (volumes_[h] > 0) {
            const double barH =
                static_cast<double>(volumes_[h]) / axisMax * plotH;
            p.fillRect(QRectF(barX, baseline - barH, barW, barH),
                       hot ? hoverColor : barColor);
        }

        const int len = (h % step == 0) ? kMajorTick : kMinorTick;
        p.setPen(QPen(hot ? c.tickHot : c.tick, 1));
        p.drawLine(QPointF(centreOf(h), baseline + 1),
                   QPointF(centreOf(h), baseline + 1 + len));
    }

    // Hour numbers, one row, starting from 0. The hour under the cursor is
    // numbered too when it falls between two labels, and any label it would
    // run into steps aside while it is shown.
    auto labelRect = [&](int h) {
        const double w = fm.horizontalAdvance(QString::number(h));
        return QRectF(centreOf(h) - w / 2.0, baseline + kMajorTick + 2, w, labelRowH);
    };

    std::vector<int> numbered;
    for (int h = 0; h < 24; h += step) numbered.push_back(h);

    if (hoverHour >= 0 && hoverHour % step != 0) {
        const QRectF hotRect = labelRect(hoverHour).adjusted(-3, 0, 3, 0);
        numbered.erase(std::remove_if(numbered.begin(), numbered.end(),
                                      [&](int h) { return labelRect(h).intersects(hotRect); }),
                       numbered.end());
        numbered.push_back(hoverHour);
    }

    for (int h : numbered) {
        p.setPen(h == hoverHour ? c.valueHot : c.value);
        p.drawText(labelRect(h).adjusted(-4, 0, 4, 0),
                   Qt::AlignHCenter | Qt::AlignVCenter, QString::number(h));
    }

    // Axis line and caption
    p.setPen(QPen(c.axis, 1));
    p.drawLine(kMarginLeft, baseline, kMarginLeft + plotW, baseline);

    QFont captionFont = font();
    captionFont.setPointSize(8);
    p.setFont(captionFont);
    p.setPen(c.caption);
    p.drawText(kMarginLeft, baseline + hourLabelBand, plotW, kAxisCaptionBand,
               Qt::AlignHCenter | Qt::AlignVCenter, tr("Hour of Day"));

    p.restore();
}

void HourlyVolumeChart::setExportInfo(const QString& title, const QString& subtitle,
                                      const QString& fileStem) {
    exportTitle_ = title;
    exportSubtitle_ = subtitle;
    exportFileStem_ = fileStem;
}

void HourlyVolumeChart::contextMenuEvent(QContextMenuEvent* event) {
    if (volumes_.size() != 24) return;
    QMenu menu(this);
    menu.addAction(tr("Export chart..."), this, &HourlyVolumeChart::exportChart);
    menu.exec(event->globalPos());
}

void HourlyVolumeChart::exportChart() {
    if (volumes_.size() != 24) return;
    // A fixed page size, so every exported chart has the same shape and two of
    // them can sit side by side in a report.
    ScreenshotExporter::exportDrawing(
        this, tr("Export chart"), exportFileStem_, QSize(640, 360),
        [this](QPainter& p, const QRect& area) {
            paintChart(p, area, paperColors(), -1, true);
        });
}

namespace {

const HourlyVolumeChart::Colors& screenColors() {
    static const HourlyVolumeChart::Colors c{
        QColor(),
        QColor(255, 255, 255, 30),
        QColor(150, 150, 155),
        QColor(235, 235, 240),
        QColor(120, 120, 126),
        QColor(220, 220, 226),
        QColor(160, 160, 165),
        QColor(170, 170, 175),
        QColor(235, 235, 240),
    };
    return c;
}

const HourlyVolumeChart::Colors& paperColors() {
    static const HourlyVolumeChart::Colors c{
        QColor(Qt::white),
        QColor(0, 0, 0, 30),
        QColor(90, 90, 95),
        QColor(20, 20, 20),
        QColor(130, 130, 135),
        QColor(40, 40, 40),
        QColor(80, 80, 85),
        QColor(90, 90, 95),
        QColor(20, 20, 20),
    };
    return c;
}

} // namespace

void HourlyVolumeChart::mouseMoveEvent(QMouseEvent* event) {
    const int hour = hourAt(event->pos().x());
    if (hour != hoverHour_) {
        hoverHour_ = hour;
        update();
    }
    if (hour >= 0) {
        // Spell out the clock range, so the hour numbering cannot be misread:
        // "Hour 7" alone leaves it open whether that means 7:00 or 7:00-8:00.
        const uint32_t v = volumes_[hour];
        QToolTip::showText(
            event->globalPosition().toPoint(),
            tr("%1:00-%2:00\n%3")
                .arg(hour, 2, 10, QChar('0'))
                // The last bar ends at 24:00, not 00:00: wrapping it to midnight
                // reads as a range running backwards.
                .arg(hour + 1, 2, 10, QChar('0'))
                .arg(v == 0 ? tr("No vehicles")
                            : tr("%n vehicle(s)", nullptr, static_cast<int>(v))),
            this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void HourlyVolumeChart::leaveEvent(QEvent* event) {
    if (hoverHour_ != -1) {
        hoverHour_ = -1;
        update();
    }
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

} // namespace simvis
