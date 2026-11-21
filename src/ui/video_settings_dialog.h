// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include "core/video_recorder.h"

namespace simvis {

class VideoSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit VideoSettingsDialog(float minTime, float maxTime, float currentTime, QWidget* parent = nullptr);

    VideoSettings getSettings() const;
    float getStartTime() const;
    float getEndTime() const;

private slots:
    void onBrowseOutput();
    void onStartTimeChanged();
    void onEndTimeChanged();
    void updateDurationLabel();

private:
    void setupUi();
    void updateTimeFromSpinBoxes(bool isStart);
    void updateSpinBoxesFromTime(bool isStart);
    float getTimeInSeconds(bool isStart) const;
    void setTimeInSeconds(float seconds, bool isStart);

    // Simulation time range
    float minTime_;
    float maxTime_;
    float currentTime_;

    // UI components
    QComboBox* resolutionCombo_;
    QComboBox* frameRateCombo_;
    QComboBox* speedCombo_;
    QLineEdit* outputPathEdit_;
    QPushButton* browseButton_;

    // Time input widgets (HH:MM:SS format)
    QSpinBox* startHoursSpin_;
    QSpinBox* startMinutesSpin_;
    QSpinBox* startSecondsSpin_;
    QSpinBox* endHoursSpin_;
    QSpinBox* endMinutesSpin_;
    QSpinBox* endSecondsSpin_;

    QLabel* durationLabel_;
    QLabel* estimatedSizeLabel_;
};

} // namespace simvis
