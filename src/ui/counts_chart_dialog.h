#pragma once

#include "parsers/counts_parser.h"
#include <QDialog>
#include <QImage>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace simvis {

// Custom widget that paints the bar chart
class CountsChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit CountsChartWidget(const CountData& data, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const CountData& countData_;
};

// Dialog window showing the 24-hour volume comparison chart
class CountsChartDialog : public QDialog {
    Q_OBJECT
public:
    CountsChartDialog(const CountData& data,
                      const std::string& linkOriginalId,
                      QWidget* parent = nullptr);
};

// The chart of a station as an image, with `title` in a strip above it, at
// `scale` times the screen size
QImage countChartImage(const CountData& data, const QString& title, int scale);

// Analysis > Export Count Charts: which stations, the image scale, the folder
// and the summary table
class ExportCountChartsDialog : public QDialog {
    Q_OBJECT
public:
    // stations: all stations. selected: the stations on the selected links.
    ExportCountChartsDialog(int stations, int selected, const QString& folder,
                            QWidget* parent = nullptr);

    bool selectedOnly() const;
    int scale() const;
    QString folder() const;
    bool writeSummary() const;
    bool openFolderWhenDone() const;

    void accept() override;

private:
    void updateState();

    QRadioButton* allButton_;
    QRadioButton* selectedButton_;
    QComboBox* scaleCombo_;
    QLabel* sizeLabel_;
    QLineEdit* folderEdit_;
    QCheckBox* summaryBox_;
    QCheckBox* openFolderBox_;
    QPushButton* exportButton_;
    int stations_;
    int selected_;
};

} // namespace simvis
