#include "counts_chart_dialog.h"
#include "ribbon/ribbon.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QToolButton>
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

// ============================================================================
// Export of all charts
// ============================================================================

namespace {

// The chart as in the dialog, and the strip with the title above it
constexpr QSize kChartSize(640, 400);
constexpr int kTitleHeight = 34;

} // anonymous namespace

QImage countChartImage(const CountData& data, const QString& title, int scale) {
    const QSize size(kChartSize.width(), kChartSize.height() + kTitleHeight);
    QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(QColor(32, 32, 32));
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(QColor(230, 230, 230));
        p.setFont(QFont("Arial", 10, QFont::Bold));
        p.drawText(QRect(16, 0, size.width() - 32, kTitleHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, title);
    }
    // The widget is never shown: it only paints into the image
    CountsChartWidget chart(data);
    chart.resize(kChartSize);
    chart.render(&image, QPoint(0, kTitleHeight));
    return image;
}

ExportCountChartsDialog::ExportCountChartsDialog(int stations, int selected,
                                                 const QString& folder, QWidget* parent)
    : QDialog(parent), stations_(stations), selected_(selected)
{
    setWindowTitle("Export Count Charts");
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("Save the chart of each count station as a PNG image, "
                             "and a table of the stations.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    allButton_ = new QRadioButton(QString("All stations (%1)").arg(stations), this);
    selectedButton_ = new QRadioButton(
        QString("Stations on the selected links (%1)").arg(selected), this);
    selectedButton_->setEnabled(selected > 0);
    if (selected == 0) selectedButton_->setToolTip("Ctrl+click roads with a count station first");
    (selected > 0 ? selectedButton_ : allButton_)->setChecked(true);
    layout->addWidget(allButton_);
    layout->addWidget(selectedButton_);
    connect(allButton_, &QRadioButton::toggled, this, &ExportCountChartsDialog::updateState);

    QSettings settings;
    auto* form = new QFormLayout();

    // The choices of Export > PNG
    scaleCombo_ = new QComboBox(this);
    scaleCombo_->addItem("1x (screen resolution)", 1);
    scaleCombo_->addItem("2x (recommended)", 2);
    scaleCombo_->addItem("4x (high quality)", 4);
    scaleCombo_->addItem(QString::fromUtf8("8x (very high — large file)"), 8);
    const int lastScale = scaleCombo_->findData(settings.value("counts/exportScale", 2).toInt());
    scaleCombo_->setCurrentIndex(lastScale >= 0 ? lastScale : 1);
    connect(scaleCombo_, &QComboBox::currentIndexChanged,
            this, &ExportCountChartsDialog::updateState);
    form->addRow("Resolution:", scaleCombo_);

    sizeLabel_ = new QLabel(this);
    sizeLabel_->setStyleSheet("color: gray;");
    form->addRow(QString(), sizeLabel_);

    auto* folderRow = new QHBoxLayout();
    folderEdit_ = new QLineEdit(QDir::toNativeSeparators(folder), this);
    connect(folderEdit_, &QLineEdit::textChanged, this, &ExportCountChartsDialog::updateState);
    folderRow->addWidget(folderEdit_, 1);
    auto* browse = new QToolButton(this);
    browse->setIcon(ribbonIcon("folder-open"));
    browse->setToolTip("Select the folder");
    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, "Folder for the charts",
                                                              folderEdit_->text());
        if (!dir.isEmpty()) folderEdit_->setText(QDir::toNativeSeparators(dir));
    });
    folderRow->addWidget(browse);
    form->addRow("Folder:", folderRow);
    layout->addLayout(form);

    summaryBox_ = new QCheckBox("Also save summary.csv (totals, ratio and GEH of each station)",
                                this);
    summaryBox_->setChecked(settings.value("counts/writeSummary", true).toBool());
    layout->addWidget(summaryBox_);

    openFolderBox_ = new QCheckBox("Open the folder when done", this);
    openFolderBox_->setChecked(settings.value("views/openFolder", true).toBool());
    layout->addWidget(openFolderBox_);

    auto* note = new QLabel("Files with the same name are replaced.", this);
    note->setStyleSheet("color: gray;");
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    exportButton_ = buttons->addButton("Export", QDialogButtonBox::AcceptRole);
    exportButton_->setIcon(ribbonIcon("chart-column"));
    exportButton_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(440, sizeHint().height());
    updateState();
}

bool ExportCountChartsDialog::selectedOnly() const {
    return selectedButton_->isChecked();
}

int ExportCountChartsDialog::scale() const {
    return scaleCombo_->currentData().toInt();
}

QString ExportCountChartsDialog::folder() const {
    return QDir::fromNativeSeparators(folderEdit_->text().trimmed());
}

bool ExportCountChartsDialog::writeSummary() const {
    return summaryBox_->isChecked();
}

bool ExportCountChartsDialog::openFolderWhenDone() const {
    return openFolderBox_->isChecked();
}

void ExportCountChartsDialog::accept() {
    QSettings settings;
    settings.setValue("counts/exportScale", scale());
    settings.setValue("counts/writeSummary", writeSummary());
    settings.setValue("views/openFolder", openFolderWhenDone());
    QDialog::accept();
}

void ExportCountChartsDialog::updateState() {
    const int count = selectedOnly() ? selected_ : stations_;
    sizeLabel_->setText(QString("%1 chart%2 of %3 x %4 pixels")
        .arg(count).arg(count == 1 ? "" : "s")
        .arg(kChartSize.width() * scale())
        .arg((kChartSize.height() + kTitleHeight) * scale()));
    exportButton_->setEnabled(count > 0 && !folder().isEmpty());
}

} // namespace simvis
