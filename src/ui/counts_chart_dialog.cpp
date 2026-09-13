#include "counts_chart_dialog.h"
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace simvis {

// ============================================================================
// Chart Widget
// ============================================================================

CountsChartWidget::CountsChartWidget(const CountData& data, QWidget* parent)
    : QWidget(parent), countData_(data)
{
    setMinimumSize(560, 360);
}

void CountsChartWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();

    // Margins
    const int marginLeft = 60;
    const int marginRight = 20;
    const int marginTop = 40;
    const int marginBottom = 50;

    const int chartW = w - marginLeft - marginRight;
    const int chartH = h - marginTop - marginBottom;

    if (chartW < 100 || chartH < 50) return;

    // Background
    p.fillRect(rect(), QColor(32, 32, 32));

    // Find max value for Y-axis scaling
    uint32_t maxVal = 0;
    for (int i = 0; i < 24; ++i) {
        maxVal = std::max(maxVal, countData_.groundTruthVolumes[i]);
        maxVal = std::max(maxVal, countData_.simulatedVolumes[i]);
    }
    if (maxVal == 0) maxVal = 100;

    // Round up to a nice number
    uint32_t yMax = maxVal;
    if (yMax <= 100) yMax = ((yMax / 10) + 1) * 10;
    else if (yMax <= 1000) yMax = ((yMax / 100) + 1) * 100;
    else if (yMax <= 10000) yMax = ((yMax / 1000) + 1) * 1000;
    else yMax = ((yMax / 5000) + 1) * 5000;

    // Draw axes
    p.setPen(QPen(QColor(180, 180, 180), 1));
    // X-axis
    p.drawLine(marginLeft, h - marginBottom, w - marginRight, h - marginBottom);
    // Y-axis
    p.drawLine(marginLeft, marginTop, marginLeft, h - marginBottom);

    // Y-axis grid lines and labels
    p.setFont(QFont("Arial", 8));
    int numYTicks = 5;
    for (int i = 0; i <= numYTicks; ++i) {
        int y = h - marginBottom - (i * chartH / numYTicks);
        uint32_t val = i * yMax / numYTicks;

        // Grid line
        p.setPen(QPen(QColor(60, 60, 60), 1));
        p.drawLine(marginLeft + 1, y, w - marginRight, y);

        // Label
        p.setPen(QColor(180, 180, 180));
        QString label = QString::number(val);
        p.drawText(0, y - 8, marginLeft - 5, 16, Qt::AlignRight | Qt::AlignVCenter, label);
    }

    // Bar dimensions
    double barGroupWidth = static_cast<double>(chartW) / 24.0;
    double barWidth = barGroupWidth * 0.35;
    double gap = barGroupWidth * 0.05;

    // Colors
    QColor simColor(220, 50, 50);        // Red for simulated
    QColor truthColor(50, 180, 80);      // Green for ground truth

    // Draw bars
    for (int i = 0; i < 24; ++i) {
        double groupX = marginLeft + i * barGroupWidth;

        // Simulated bar (left)
        {
            double val = countData_.simulatedVolumes[i];
            double barH = (val / static_cast<double>(yMax)) * chartH;
            double x = groupX + gap;
            double y = h - marginBottom - barH;
            p.fillRect(QRectF(x, y, barWidth, barH), simColor);
        }

        // Ground truth bar (right)
        {
            double val = countData_.groundTruthVolumes[i];
            double barH = (val / static_cast<double>(yMax)) * chartH;
            double x = groupX + gap + barWidth + gap;
            double y = h - marginBottom - barH;
            p.fillRect(QRectF(x, y, barWidth, barH), truthColor);
        }

        // X-axis label (hour)
        p.setPen(QColor(180, 180, 180));
        // Hour of day, 0-23: index i holds the traffic from i:00 to i+1:00.
        // Labelling it i+1 disagreed with the link histogram, which numbers the
        // same data from 0.
        QString hourLabel = QString::number(i);
        int labelX = static_cast<int>(groupX);
        int labelW = static_cast<int>(barGroupWidth);
        p.drawText(labelX, h - marginBottom + 5, labelW, 20,
                   Qt::AlignHCenter | Qt::AlignTop, hourLabel);
    }

    // Axis labels
    p.setFont(QFont("Arial", 9));
    p.setPen(QColor(200, 200, 200));

    // X-axis label
    p.drawText(marginLeft, h - 15, chartW, 15, Qt::AlignHCenter, "Hour of Day");

    // Y-axis label (rotated)
    p.save();
    p.translate(12, marginTop + chartH / 2);
    p.rotate(-90);
    p.drawText(-chartH / 2, 0, chartH, 15, Qt::AlignHCenter, "Vehicles");
    p.restore();

    // Legend
    int legendX = marginLeft + 10;
    int legendY = marginTop - 25;
    int boxSize = 12;

    p.fillRect(legendX, legendY, boxSize, boxSize, simColor);
    p.setPen(QColor(200, 200, 200));
    p.drawText(legendX + boxSize + 5, legendY, 80, boxSize, Qt::AlignVCenter, "Simulated");

    int legend2X = legendX + 100;
    p.fillRect(legend2X, legendY, boxSize, boxSize, truthColor);
    p.drawText(legend2X + boxSize + 5, legendY, 100, boxSize, Qt::AlignVCenter, "Ground Truth");
}

// ============================================================================
// Dialog
// ============================================================================

CountsChartDialog::CountsChartDialog(const CountData& data,
                                     const std::string& linkOriginalId,
                                     QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QString("Link %1 — %2")
        .arg(QString::fromStdString(linkOriginalId))
        .arg(QString::fromStdString(data.stationId)));

    resize(640, 420);
    setMinimumSize(480, 320);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);

    auto* chart = new CountsChartWidget(data, this);
    layout->addWidget(chart);
}

} // namespace simvis
