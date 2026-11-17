#pragma once

#include <QWidget>
#include <QString>
#include <cstdint>
#include <vector>

namespace simvis {

// A link value over the day as bars: the hourly volume (one bar per hour), or
// a metric of the link table per 15-minute bin (roadmap item 9). The bars
// cover the whole run, also the hours after 24:00.
//
// Painted directly rather than with Qt Charts: QBarCategoryAxis elides its
// category labels when 24 of them have to fit the 280px info panel, which left
// no way to tell which hour a bar belonged to. Painting gives every hour its own
// slot and tick, numbers the even hours from 0, and names whichever hour is
// under the cursor. It matches CountsChartWidget, which already draws its
// 24-hour comparison the same way.
class HourlyVolumeChart : public QWidget {
    Q_OBJECT

public:
    explicit HourlyVolumeChart(QWidget* parent = nullptr);

    // Vehicles per hour, hour 0 first. Empty clears the chart.
    void setVolumes(const std::vector<uint32_t>& volumes);
    // Any value per slot of slotSeconds, slot 0 at 00:00. NaN = no data (no
    // bar). unit is for the tooltip, for example "veh/km/lane"; decimals for
    // the numbers. referenceValue > 0 draws a dashed line (v/c = 1).
    void setSeries(const std::vector<double>& values, int slotSeconds, const QString& unit,
                   int decimals, double referenceValue = 0.0);

    // Title and subtitle drawn on an exported chart (not on screen, where the
    // info panel already names the link). `fileStem` is the suggested file
    // name without extension.
    void setExportInfo(const QString& title, const QString& subtitle,
                       const QString& fileStem);

    QSize sizeHint() const override;

public slots:
    // Ask for a file and write the chart as PNG or PDF, by its extension.
    void exportChart();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

public:
    // Colors for one rendering (screen or paper), defined in the .cpp.
    struct Colors;

private:
    // Draw the whole chart into `area`. The on-screen widget and the export
    // share this, so the file shows what the panel shows.
    void paintChart(QPainter& p, const QRect& area, const Colors& c,
                    int hoverHour, bool withTitle) const;

    // Index of the slot under a widget-space x, or -1 outside the plot.
    int slotAt(int x) const;
    // Slots of the plot: the data, at least 24 hours
    int slotCount() const;
    double hours() const;

    std::vector<double> values_;
    double maxValue_ = 0.0;
    int slotSeconds_ = 3600;
    QString unit_;
    int decimals_ = 0;
    bool volumes_ = true;          // whole vehicles ("3 vehicles")
    double referenceValue_ = 0.0;
    int hoverHour_ = -1;           // the slot under the cursor

    QString exportTitle_;
    QString exportSubtitle_;
    QString exportFileStem_;
};

} // namespace simvis
