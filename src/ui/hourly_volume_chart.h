#pragma once

#include <QWidget>
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

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // Index of the hour slot under a widget-space x, or -1 outside the plot.
    int hourAt(int x) const;

    std::vector<uint32_t> volumes_;
    uint32_t maxVolume_ = 0;
    int hoverHour_ = -1;
};

} // namespace simvis
