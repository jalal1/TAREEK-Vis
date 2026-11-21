// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QImage>
#include <memory>

namespace simvis {

// Video recording settings
struct VideoSettings {
    // Resolution presets
    enum class Resolution {
        HD_720p,     // 1280x720
        FullHD_1080p, // 1920x1080
        UHD_4K       // 3840x2160
    };

    // Frame rate options
    enum class FrameRate {
        FPS_30 = 30,
        FPS_60 = 60
    };

    // Speed multiplier - how many simulation seconds per video second
    enum class SpeedMultiplier {
        Speed_1x = 1,
        Speed_10x = 10,
        Speed_30x = 30,
        Speed_60x = 60
    };

    Resolution resolution = Resolution::FullHD_1080p;
    FrameRate frameRate = FrameRate::FPS_30;
    SpeedMultiplier speedMultiplier = SpeedMultiplier::Speed_30x;
    QString outputPath;

    // Helper methods
    int width() const;
    int height() const;
    int fps() const { return static_cast<int>(frameRate); }
    int speedMult() const { return static_cast<int>(speedMultiplier); }
};

// Manages video recording using FFmpeg
class VideoRecorder : public QObject {
    Q_OBJECT

public:
    explicit VideoRecorder(QObject* parent = nullptr);
    ~VideoRecorder() override;

    // Recording control
    bool startRecording(const VideoSettings& settings, float startTime, float endTime);
    void stopRecording();
    bool isRecording() const { return isRecording_; }

    // Frame submission (call this from render loop)
    void submitFrame(const QImage& frame, float currentSimTime);

    // Progress information
    float progress() const;
    QString statusMessage() const;
    int framesRecorded() const { return framesRecorded_; }
    int totalFrames() const { return totalFrames_; }

signals:
    void recordingStarted();
    void recordingStopped(bool success, const QString& message);
    void progressChanged(int percent);
    void errorOccurred(const QString& error);

private slots:
    void onFFmpegError(QProcess::ProcessError error);
    void onFFmpegFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onFFmpegReadyReadStdout();
    void onFFmpegReadyReadStderr();

private:
    bool startFFmpegProcess();
    void cleanupFFmpeg();

    // FFmpeg process
    std::unique_ptr<QProcess> ffmpegProcess_;

    // Recording state
    bool isRecording_ = false;
    VideoSettings settings_;
    float startTime_ = 0.0f;
    float endTime_ = 0.0f;
    float lastRecordedTime_ = 0.0f;
    int framesRecorded_ = 0;
    int totalFrames_ = 0;

    // Frame timing
    float simTimePerFrame_ = 0.0f;  // How much simulation time between frames
    float nextFrameTime_ = 0.0f;    // Next simulation time to capture a frame

    // FFmpeg output for debugging
    QString ffmpegOutput_;
};

} // namespace simvis
