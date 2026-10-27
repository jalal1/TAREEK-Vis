#pragma once

#include <QDialog>
#include <QList>
#include <QSize>
#include <QStringList>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace simvis {

// Views > Capture Views: which views to save as PNG images, at which scale,
// and in which folder
class CaptureViewsDialog : public QDialog {
    Q_OBJECT

public:
    // names: the views, in list order. mapSize: the map in screen pixels,
    // to show the size of the images.
    CaptureViewsDialog(const QStringList& names, const QString& folder,
                       const QSize& mapSize, QWidget* parent = nullptr);

    QList<int> rows() const;  // the checked views
    int scale() const;
    QString folder() const;
    bool openFolderWhenDone() const;

    void accept() override;

private:
    void updateState();

    QListWidget* list_;
    QComboBox* scaleCombo_;
    QLabel* sizeLabel_;
    QLineEdit* folderEdit_;
    QCheckBox* openFolderBox_;
    QPushButton* captureButton_;
    QSize mapSize_;
};

// Views > Save Over Time: one view at the current map position for each
// time from "From" to "To"
class SaveOverTimeDialog : public QDialog {
    Q_OBJECT

public:
    // now: the time on the map. first, last: the time span of the run.
    SaveOverTimeDialog(double now, double first, double last,
                       const QString& name, QWidget* parent = nullptr);

    // The times of the views, in seconds. Empty when the input is not valid.
    std::vector<double> times() const;
    QString name() const;

private:
    void updateState();

    QLineEdit* nameEdit_;
    QLineEdit* fromEdit_;
    QLineEdit* toEdit_;
    QComboBox* stepCombo_;
    QLabel* previewLabel_;
    QPushButton* saveButton_;
    double first_;
    double last_;
};

} // namespace simvis
