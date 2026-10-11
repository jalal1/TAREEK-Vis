#pragma once

#include <QWidget>
#include <QString>
#include <cstdint>
#include <vector>

namespace simvis {

// Hourly link volume as 24 bars, one per hour of the day.
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

    // Expects 24 entries, hour 0 first. Anything else clears the chart.
    void setVolumes(const std::vector<uint32_t>& volumes);

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

    // Index of the hour slot under a widget-space x, or -1 outside the plot.
    int hourAt(int x) const;

    std::vector<uint32_t> volumes_;
    uint32_t maxVolume_ = 0;
    int hoverHour_ = -1;

    QString exportTitle_;
    QString exportSubtitle_;
    QString exportFileStem_;
};

} // namespace simvis
