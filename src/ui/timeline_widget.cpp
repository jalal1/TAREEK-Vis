#include "timeline_widget.h"

namespace simvis {

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    // Play/Pause and the speed are on the ribbon (Home > Playback, Space), so
    // the row holds only the time and the slider.
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(10);

    // Current time label
    timeLabel_ = new QLabel("00:00:00", this);
    timeLabel_->setMinimumWidth(70);
    timeLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Timeline slider
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setMinimum(0);
    slider_->setMaximum(86400);  // Seconds in a day
    slider_->setValue(0);
    slider_->setTracking(true);
    connect(slider_, &QSlider::valueChanged, this, &TimelineWidget::onSliderMoved);
    connect(slider_, &QSlider::sliderPressed, this, &TimelineWidget::onSliderPressed);
    connect(slider_, &QSlider::sliderReleased, this, &TimelineWidget::onSliderReleased);

    // Duration label
    durationLabel_ = new QLabel("/ 24:00:00", this);
    durationLabel_->setMinimumWidth(80);

    // Layout
    layout->addWidget(timeLabel_);
    layout->addWidget(slider_, 1);
    layout->addWidget(durationLabel_);

    setLayout(layout);
}

TimelineWidget::~TimelineWidget() = default;

void TimelineWidget::setTimeRange(float minTime, float maxTime) {
    minTime_ = minTime;
    maxTime_ = maxTime;

    slider_->setMinimum(static_cast<int>(minTime));
    slider_->setMaximum(static_cast<int>(maxTime));

    durationLabel_->setText("/ " + formatTime(maxTime));
    updateTimeLabel();
}

void TimelineWidget::setCurrentTime(float time) {
    currentTime_ = time;

    // Block signals to prevent feedback loop
    slider_->blockSignals(true);
    slider_->setValue(static_cast<int>(time));
    slider_->blockSignals(false);

    updateTimeLabel();
}

float TimelineWidget::currentTime() const {
    return currentTime_;
}

double TimelineWidget::playbackSpeed() const {
    return playbackSpeed_;
}

QList<double> TimelineWidget::playbackSpeeds() const {
    return {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0};
}

void TimelineWidget::setPlaybackSpeed(double speed) {
    if (speed == playbackSpeed_ || !playbackSpeeds().contains(speed)) return;
    playbackSpeed_ = speed;
    emit playbackSpeedChanged(speed);
}

void TimelineWidget::setPlaying(bool playing) {
    isPlaying_ = playing;
    emit playingChanged(playing);
}

void TimelineWidget::togglePlayPause() {
    setPlaying(!isPlaying_);
}

void TimelineWidget::onSliderMoved(int value) {
    currentTime_ = static_cast<float>(value);
    updateTimeLabel();
    // Only emit timeChanged if not dragging (i.e., during programmatic updates or arrow key navigation)
    // This prevents expensive event processing during slider drag operations
    if (!isDragging_) {
        emit timeChanged(currentTime_);
    }
}

void TimelineWidget::onSliderPressed() {
    // Pause playback while dragging
    wasPlayingBeforeDrag_ = isPlaying_;
    isDragging_ = true;
    if (isPlaying_) {
        setPlaying(false);
    }
}

void TimelineWidget::onSliderReleased() {
    isDragging_ = false;
    // Emit timeChanged now that dragging is complete - this triggers the expensive event processing
    emit timeChanged(currentTime_);
    // Resume playback if it was playing before
    if (wasPlayingBeforeDrag_) {
        setPlaying(true);
    }
}

void TimelineWidget::updateTimeLabel() {
    timeLabel_->setText(formatTime(currentTime_));
}

QString TimelineWidget::formatTime(float seconds) const {
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

} // namespace simvis
