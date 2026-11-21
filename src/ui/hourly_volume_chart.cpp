// SPDX-License-Identifier: GPL-3.0-or-later

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
#include <cmath>

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
// steps CountsChartWidget uses for whole numbers.
double niceAxisMax(double peak, bool whole) {
    if (whole) {
        const uint32_t p = static_cast<uint32_t>(std::ceil(peak));
        if (p == 0) return 10;
        if (p <= 100) return ((p / 10) + 1) * 10;
        if (p <= 1000) return ((p / 100) + 1) * 100;
        if (p <= 10000) return ((p / 1000) + 1) * 1000;
        return ((p / 5000) + 1) * 5000;
    }
    if (!(peak > 0.0)) return 1.0;
    // 1, 2, 2.5, 4 or 5 times a power of 10: each divides into kYTicks steps
    const double pow10 = std::pow(10.0, std::floor(std::log10(peak)));
    for (double f : {1.0, 2.0, 2.5, 4.0, 5.0, 10.0})
        if (f * pow10 >= peak) return f * pow10;
    return 10.0 * pow10;
}

QString axisNumber(double v, bool whole) {
    if (whole || std::abs(v - std::round(v)) < 1e-9) return QString::number(std::llround(v));
    return QString::number(v, 'g', 3);
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
    values_.assign(volumes.begin(), volumes.end());
    maxValue_ = values_.empty() ? 0.0 : *std::max_element(values_.begin(), values_.end());
    slotSeconds_ = 3600;
    unit_.clear();
    decimals_ = 0;
    volumes_ = true;
    referenceValue_ = 0.0;
    hoverHour_ = -1;
    update();
}

void HourlyVolumeChart::setSeries(const std::vector<double>& values, int slotSeconds,
                                  const QString& unit, int decimals, double referenceValue) {
    values_ = values;
    maxValue_ = referenceValue;
    for (double v : values_)
        if (std::isfinite(v)) maxValue_ = std::max(maxValue_, v);
    slotSeconds_ = std::max(60, slotSeconds);
    unit_ = unit;
    decimals_ = decimals;
    volumes_ = false;
    referenceValue_ = referenceValue;
    hoverHour_ = -1;
    update();
}

int HourlyVolumeChart::slotCount() const {
    // At least one day, so a short run still reads as a day
    const int day = 86400 / slotSeconds_;
    return std::max(static_cast<int>(values_.size()), day);
}

double HourlyVolumeChart::hours() const {
    return static_cast<double>(slotCount()) * slotSeconds_ / 3600.0;
}

QSize HourlyVolumeChart::sizeHint() const {
    return QSize(240, 200);
}

int HourlyVolumeChart::slotAt(int x) const {
    const int plotW = width() - kMarginLeft - kMarginRight;
    if (values_.empty() || plotW <= 0) return -1;
    if (x < kMarginLeft || x >= kMarginLeft + plotW) return -1;
    const int n = slotCount();
    const int slot = (x - kMarginLeft) * n / plotW;
    return std::clamp(slot, 0, n - 1);
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
    if (values_.empty()) return;
    QPainter p(this);
    paintChart(p, rect(), screenColors(), hoverHour_, false);
}

void HourlyVolumeChart::paintChart(QPainter& p, const QRect& area, const Colors& c,
                                   int hoverHour, bool withTitle) const {
    if (values_.empty()) return;

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
    const int n = slotCount();
    const int nHours = static_cast<int>(std::ceil(hours()));
    const double slotW = static_cast<double>(plotW) / n;
    const double hourW = slotW * 3600.0 / slotSeconds_;
    const int step = labelStep(fm, hourW);
    const int labelRowH = fm.height();
    const int hourLabelBand = labelRowH + kMajorTick + 2;

    const int plotH = areaH - top - hourLabelBand - kAxisCaptionBand;
    if (plotW < 60 || plotH < 40) { p.restore(); return; }

    const int baseline = top + plotH;
    const double axisMax = niceAxisMax(maxValue_, volumes_);

    // Horizontal grid with its value, so bar heights can be read off
    for (int i = 0; i <= kYTicks; ++i) {
        const int y = baseline - i * plotH / kYTicks;
        p.setPen(QPen(c.grid, 1));
        p.drawLine(kMarginLeft, y, kMarginLeft + plotW, y);

        p.setPen(c.value);
        p.drawText(0, y - 7, kMarginLeft - 4, 14,
                   Qt::AlignRight | Qt::AlignVCenter,
                   axisNumber(axisMax * i / kYTicks, volumes_));
    }

    const double barW = std::max(1.0, slotW * (volumes_ ? 0.74 : 0.85));
    const QColor barColor(panelstyle::kAccent);
    const QColor hoverColor = barColor.lighter(135);

    // Every slot keeps its place. A slot with no value (or 0 vehicles) draws
    // no bar, which is what an empty slot should look like.
    for (int k = 0; k < static_cast<int>(values_.size()); ++k) {
        const double v = values_[k];
        if (!std::isfinite(v) || v <= 0.0) continue;
        const double barX = kMarginLeft + k * slotW + (slotW - barW) / 2.0;
        const double barH = std::min(1.0, v / axisMax) * plotH;
        p.fillRect(QRectF(barX, baseline - barH, barW, barH),
                   k == hoverHour ? hoverColor : barColor);
    }

    // A reference line, for example v/c = 1
    if (referenceValue_ > 0.0 && referenceValue_ <= axisMax) {
        const double y = baseline - referenceValue_ / axisMax * plotH;
        QPen pen(c.value, 1, Qt::DashLine);
        p.setPen(pen);
        p.drawLine(QPointF(kMarginLeft, y), QPointF(kMarginLeft + plotW, y));
    }

    // A tick at each hour: long where the hour has a number. With hourly bars
    // the tick is at the middle of the bar, else at the start of the hour.
    const bool hourly = slotSeconds_ == 3600;
    auto hourX = [&](int h) { return kMarginLeft + h * hourW + (hourly ? hourW / 2.0 : 0.0); };
    const int hotHour = hoverHour >= 0 ? hoverHour * slotSeconds_ / 3600 : -1;
    for (int h = 0; h <= nHours - (hourly ? 1 : 0); ++h) {
        const int len = (h % step == 0) ? kMajorTick : kMinorTick;
        p.setPen(QPen(h == hotHour ? c.tickHot : c.tick, 1));
        p.drawLine(QPointF(hourX(h), baseline + 1), QPointF(hourX(h), baseline + 1 + len));
    }

    // Hour numbers, one row, starting from 0. The hour under the cursor is
    // numbered too when it falls between two labels, and any label it would
    // run into steps aside while it is shown.
    auto labelRect = [&](int h) {
        const double w = fm.horizontalAdvance(QString::number(h));
        return QRectF(hourX(h) - w / 2.0, baseline + kMajorTick + 2, w, labelRowH);
    };

    std::vector<int> numbered;
    for (int h = 0; h <= nHours - (hourly ? 1 : 0); h += step) numbered.push_back(h);

    if (hotHour >= 0 && hotHour % step != 0) {
        const QRectF hotRect = labelRect(hotHour).adjusted(-3, 0, 3, 0);
        numbered.erase(std::remove_if(numbered.begin(), numbered.end(),
                                      [&](int h) { return labelRect(h).intersects(hotRect); }),
                       numbered.end());
        numbered.push_back(hotHour);
    }

    for (int h : numbered) {
        p.setPen(h == hotHour ? c.valueHot : c.value);
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
               Qt::AlignHCenter | Qt::AlignVCenter, tr("Hour"));

    p.restore();
}

void HourlyVolumeChart::setExportInfo(const QString& title, const QString& subtitle,
                                      const QString& fileStem) {
    exportTitle_ = title;
    exportSubtitle_ = subtitle;
    exportFileStem_ = fileStem;
}

void HourlyVolumeChart::contextMenuEvent(QContextMenuEvent* event) {
    if (values_.empty()) return;
    QMenu menu(this);
    menu.addAction(tr("Export chart..."), this, &HourlyVolumeChart::exportChart);
    menu.exec(event->globalPos());
}

void HourlyVolumeChart::exportChart() {
    if (values_.empty()) return;
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
    const int slot = slotAt(event->pos().x());
    if (slot != hoverHour_) {
        hoverHour_ = slot;
        update();
    }
    if (slot >= 0) {
        // Spell out the clock range, so the hour numbering cannot be misread:
        // "Hour 7" alone leaves it open whether that means 7:00 or 7:00-8:00.
        // Times after 24:00 are not wrapped: "25:00-26:00", not a range that
        // runs backwards.
        auto clock = [](int seconds) {
            return QString("%1:%2").arg(seconds / 3600, 2, 10, QChar('0'))
                                   .arg((seconds % 3600) / 60, 2, 10, QChar('0'));
        };
        const double v = slot < static_cast<int>(values_.size()) ? values_[slot] : 0.0;
        QString value;
        if (volumes_) {
            const int n = static_cast<int>(std::llround(v));
            value = n == 0 ? tr("No vehicles") : tr("%n vehicle(s)", nullptr, n);
        } else if (!std::isfinite(v)) {
            value = tr("No value (no vehicles)");
        } else {
            value = QString::number(v, 'f', decimals_) + (unit_.isEmpty() ? QString() : " " + unit_);
        }
        QToolTip::showText(
            event->globalPosition().toPoint(),
            tr("%1-%2\n%3").arg(clock(slot * slotSeconds_), clock((slot + 1) * slotSeconds_), value),
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
