// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_settings_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <cmath>

namespace simvis {

VideoSettingsDialog::VideoSettingsDialog(float minTime, float maxTime, float currentTime, QWidget* parent)
    : QDialog(parent)
    , minTime_(minTime)
    , maxTime_(maxTime)
    , currentTime_(currentTime)
{
    setupUi();
    setWindowTitle("Video Recording Settings");
    resize(500, 450);
}

void VideoSettingsDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Video settings group
    auto* videoGroup = new QGroupBox("Video Settings", this);
    auto* videoLayout = new QFormLayout(videoGroup);

    // Resolution combo
    resolutionCombo_ = new QComboBox(this);
    resolutionCombo_->addItem("720p (1280x720)", static_cast<int>(VideoSettings::Resolution::HD_720p));
    resolutionCombo_->addItem("1080p (1920x1080)", static_cast<int>(VideoSettings::Resolution::FullHD_1080p));
    resolutionCombo_->addItem("4K (3840x2160)", static_cast<int>(VideoSettings::Resolution::UHD_4K));
    resolutionCombo_->setCurrentIndex(1);  // Default to 1080p
    videoLayout->addRow("Resolution:", resolutionCombo_);

    // Frame rate combo
    frameRateCombo_ = new QComboBox(this);
    frameRateCombo_->addItem("30 fps", static_cast<int>(VideoSettings::FrameRate::FPS_30));
    frameRateCombo_->addItem("60 fps", static_cast<int>(VideoSettings::FrameRate::FPS_60));
    frameRateCombo_->setCurrentIndex(0);  // Default to 30 fps
    videoLayout->addRow("Frame Rate:", frameRateCombo_);

    // Speed multiplier combo
    speedCombo_ = new QComboBox(this);
    speedCombo_->addItem("1x (real-time)", static_cast<int>(VideoSettings::SpeedMultiplier::Speed_1x));
    speedCombo_->addItem("10x", static_cast<int>(VideoSettings::SpeedMultiplier::Speed_10x));
    speedCombo_->addItem("30x", static_cast<int>(VideoSettings::SpeedMultiplier::Speed_30x));
    speedCombo_->addItem("60x", static_cast<int>(VideoSettings::SpeedMultiplier::Speed_60x));
    speedCombo_->setCurrentIndex(2);  // Default to 30x
    videoLayout->addRow("Speed:", speedCombo_);

    auto* speedHelp = new QLabel("Speed determines how many simulation seconds per video second.\n"
                                 "E.g., 30x means 30 sim seconds = 1 video second.", this);
    speedHelp->setWordWrap(true);
    speedHelp->setStyleSheet("QLabel { color: gray; font-size: 9pt; }");
    videoLayout->addRow("", speedHelp);

    mainLayout->addWidget(videoGroup);

    // Time range group
    auto* timeGroup = new QGroupBox("Time Range", this);
    auto* timeLayout = new QFormLayout(timeGroup);

    // Start time (HH:MM:SS format)
    auto* startTimeLayout = new QHBoxLayout();
    startHoursSpin_ = new QSpinBox(this);
    startHoursSpin_->setRange(0, 99);
    startHoursSpin_->setValue(0);
    startHoursSpin_->setSuffix(" h");
    startHoursSpin_->setMinimumWidth(70);
    connect(startHoursSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onStartTimeChanged);

    startMinutesSpin_ = new QSpinBox(this);
    startMinutesSpin_->setRange(0, 59);
    startMinutesSpin_->setValue(0);
    startMinutesSpin_->setSuffix(" m");
    startMinutesSpin_->setMinimumWidth(70);
    connect(startMinutesSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onStartTimeChanged);

    startSecondsSpin_ = new QSpinBox(this);
    startSecondsSpin_->setRange(0, 59);
    startSecondsSpin_->setValue(0);
    startSecondsSpin_->setSuffix(" s");
    startSecondsSpin_->setMinimumWidth(70);
    connect(startSecondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onStartTimeChanged);

    startTimeLayout->addWidget(startHoursSpin_);
    startTimeLayout->addWidget(startMinutesSpin_);
    startTimeLayout->addWidget(startSecondsSpin_);
    startTimeLayout->addStretch();
    timeLayout->addRow("Start Time:", startTimeLayout);

    // End time (HH:MM:SS format)
    auto* endTimeLayout = new QHBoxLayout();
    endHoursSpin_ = new QSpinBox(this);
    endHoursSpin_->setRange(0, 99);
    endHoursSpin_->setValue(0);
    endHoursSpin_->setSuffix(" h");
    endHoursSpin_->setMinimumWidth(70);
    connect(endHoursSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onEndTimeChanged);

    endMinutesSpin_ = new QSpinBox(this);
    endMinutesSpin_->setRange(0, 59);
    endMinutesSpin_->setValue(0);
    endMinutesSpin_->setSuffix(" m");
    endMinutesSpin_->setMinimumWidth(70);
    connect(endMinutesSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onEndTimeChanged);

    endSecondsSpin_ = new QSpinBox(this);
    endSecondsSpin_->setRange(0, 59);
    endSecondsSpin_->setValue(0);
    endSecondsSpin_->setSuffix(" s");
    endSecondsSpin_->setMinimumWidth(70);
    connect(endSecondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VideoSettingsDialog::onEndTimeChanged);

    endTimeLayout->addWidget(endHoursSpin_);
    endTimeLayout->addWidget(endMinutesSpin_);
    endTimeLayout->addWidget(endSecondsSpin_);
    endTimeLayout->addStretch();
    timeLayout->addRow("End Time:", endTimeLayout);

    // Initialize time values (start = current slider position, end = current + 5 minutes)
    setTimeInSeconds(currentTime_, true);
    setTimeInSeconds(std::min(currentTime_ + 300.0f, maxTime_), false);  // +5 minutes

    // Duration label
    durationLabel_ = new QLabel(this);
    timeLayout->addRow("Duration:", durationLabel_);

    mainLayout->addWidget(timeGroup);

    // Output file group
    auto* outputGroup = new QGroupBox("Output File", this);
    auto* outputLayout = new QVBoxLayout(outputGroup);

    auto* pathLayout = new QHBoxLayout();
    outputPathEdit_ = new QLineEdit(this);

    // Default output path
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString defaultPath = documentsPath + "/simvis_recording.mp4";
    outputPathEdit_->setText(defaultPath);

    browseButton_ = new QPushButton("Browse...", this);
    connect(browseButton_, &QPushButton::clicked, this, &VideoSettingsDialog::onBrowseOutput);

    pathLayout->addWidget(outputPathEdit_);
    pathLayout->addWidget(browseButton_);
    outputLayout->addLayout(pathLayout);

    // Estimated file size
    estimatedSizeLabel_ = new QLabel(this);
    estimatedSizeLabel_->setStyleSheet("QLabel { color: gray; font-size: 9pt; margin-top: 5px; }");
    outputLayout->addWidget(estimatedSizeLabel_);

    mainLayout->addWidget(outputGroup);

    // Info label
    auto* infoLabel = new QLabel(
        "Note: Recording will render frames at simulation time, independent of playback speed. "
        "The window can be minimized during recording.", this);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("QLabel { background-color: #f0f0f0; padding: 10px; border-radius: 5px; }");
    mainLayout->addWidget(infoLabel);

    mainLayout->addStretch();

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    auto* startButton = new QPushButton("Start Recording", this);
    startButton->setDefault(true);
    connect(startButton, &QPushButton::clicked, this, &QDialog::accept);

    auto* cancelButton = new QPushButton("Cancel", this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(startButton);

    mainLayout->addLayout(buttonLayout);

    // Initialize duration label
    updateDurationLabel();

    // Connect signals for estimated size updates
    connect(resolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VideoSettingsDialog::updateDurationLabel);
    connect(frameRateCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VideoSettingsDialog::updateDurationLabel);
    connect(speedCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VideoSettingsDialog::updateDurationLabel);
}

VideoSettings VideoSettingsDialog::getSettings() const {
    VideoSettings settings;

    settings.resolution = static_cast<VideoSettings::Resolution>(
        resolutionCombo_->currentData().toInt());
    settings.frameRate = static_cast<VideoSettings::FrameRate>(
        frameRateCombo_->currentData().toInt());
    settings.speedMultiplier = static_cast<VideoSettings::SpeedMultiplier>(
        speedCombo_->currentData().toInt());
    settings.outputPath = outputPathEdit_->text();

    return settings;
}

void VideoSettingsDialog::onBrowseOutput() {
    QString defaultPath = outputPathEdit_->text();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/simvis_recording.mp4";
    }

    QString fileName = QFileDialog::getSaveFileName(
        this,
        "Save Video As",
        defaultPath,
        "MP4 Video (*.mp4);;All Files (*.*)"
    );

    if (!fileName.isEmpty()) {
        // Ensure .mp4 extension
        if (!fileName.endsWith(".mp4", Qt::CaseInsensitive)) {
            fileName += ".mp4";
        }
        outputPathEdit_->setText(fileName);
    }
}

void VideoSettingsDialog::onStartTimeChanged() {
    float startTime = getTimeInSeconds(true);
    float endTime = getTimeInSeconds(false);

    // Ensure start < end (at least 1 second difference)
    if (startTime >= endTime) {
        setTimeInSeconds(startTime + 1.0f, false);
    }
    updateDurationLabel();
}

void VideoSettingsDialog::onEndTimeChanged() {
    float startTime = getTimeInSeconds(true);
    float endTime = getTimeInSeconds(false);

    // Ensure end > start (at least 1 second difference)
    if (endTime <= startTime) {
        setTimeInSeconds(endTime - 1.0f, true);
    }
    updateDurationLabel();
}

float VideoSettingsDialog::getTimeInSeconds(bool isStart) const {
    if (isStart) {
        return startHoursSpin_->value() * 3600.0f +
               startMinutesSpin_->value() * 60.0f +
               startSecondsSpin_->value();
    } else {
        return endHoursSpin_->value() * 3600.0f +
               endMinutesSpin_->value() * 60.0f +
               endSecondsSpin_->value();
    }
}

void VideoSettingsDialog::setTimeInSeconds(float seconds, bool isStart) {
    // Clamp to valid range
    seconds = std::clamp(seconds, minTime_, maxTime_);

    int totalSecs = static_cast<int>(seconds);
    int hours = totalSecs / 3600;
    int minutes = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;

    // Block signals to prevent recursive updates
    if (isStart) {
        startHoursSpin_->blockSignals(true);
        startMinutesSpin_->blockSignals(true);
        startSecondsSpin_->blockSignals(true);

        startHoursSpin_->setValue(hours);
        startMinutesSpin_->setValue(minutes);
        startSecondsSpin_->setValue(secs);

        startHoursSpin_->blockSignals(false);
        startMinutesSpin_->blockSignals(false);
        startSecondsSpin_->blockSignals(false);
    } else {
        endHoursSpin_->blockSignals(true);
        endMinutesSpin_->blockSignals(true);
        endSecondsSpin_->blockSignals(true);

        endHoursSpin_->setValue(hours);
        endMinutesSpin_->setValue(minutes);
        endSecondsSpin_->setValue(secs);

        endHoursSpin_->blockSignals(false);
        endMinutesSpin_->blockSignals(false);
        endSecondsSpin_->blockSignals(false);
    }
}

void VideoSettingsDialog::updateDurationLabel() {
    float simDuration = getTimeInSeconds(false) - getTimeInSeconds(true);

    // Format duration nicely
    int hours = static_cast<int>(simDuration / 3600);
    int minutes = static_cast<int>((simDuration - hours * 3600) / 60);
    int seconds = static_cast<int>(simDuration) % 60;

    QString durationText;
    if (hours > 0) {
        durationText = QString("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
    } else if (minutes > 0) {
        durationText = QString("%1m %2s").arg(minutes).arg(seconds);
    } else {
        durationText = QString("%1s").arg(seconds);
    }

    durationLabel_->setText(QString("%1 (simulation time)").arg(durationText));

    // Calculate video duration
    int speedMult = speedCombo_->currentData().toInt();
    float videoDuration = simDuration / speedMult;  // in seconds
    int videoMinutes = static_cast<int>(videoDuration / 60);
    int videoSeconds = static_cast<int>(videoDuration) % 60;

    QString videoDurationText;
    if (videoMinutes > 0) {
        videoDurationText = QString("%1m %2s").arg(videoMinutes).arg(videoSeconds);
    } else {
        videoDurationText = QString("%.1f s").arg(videoDuration);
    }

    // Estimate file size (rough estimate)
    // At CRF 18 (high quality), bitrate is approximately:
    // 720p: ~5 Mbps, 1080p: ~8 Mbps, 4K: ~30 Mbps
    int resolution = resolutionCombo_->currentData().toInt();
    float bitrateMbps = 8.0f;  // Default 1080p
    if (resolution == static_cast<int>(VideoSettings::Resolution::HD_720p)) {
        bitrateMbps = 5.0f;
    } else if (resolution == static_cast<int>(VideoSettings::Resolution::UHD_4K)) {
        bitrateMbps = 30.0f;
    }

    float estimatedSizeMB = (bitrateMbps * videoDuration) / 8.0f;
    QString sizeText;
    if (estimatedSizeMB > 1024) {
        sizeText = QString("%.2f GB").arg(estimatedSizeMB / 1024.0f);
    } else {
        sizeText = QString("%.1f MB").arg(estimatedSizeMB);
    }

    estimatedSizeLabel_->setText(QString("Video duration: %1  |  Estimated size: ~%2")
                                 .arg(videoDurationText, sizeText));
}

float VideoSettingsDialog::getStartTime() const {
    return getTimeInSeconds(true);
}

float VideoSettingsDialog::getEndTime() const {
    return getTimeInSeconds(false);
}

} // namespace simvis
