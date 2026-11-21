// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_recorder.h"
#include "logger.h"
#include <QBuffer>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <cmath>

namespace simvis {

// ============================================================================
// VideoSettings implementation
// ============================================================================

int VideoSettings::width() const {
    switch (resolution) {
        case Resolution::HD_720p: return 1280;
        case Resolution::FullHD_1080p: return 1920;
        case Resolution::UHD_4K: return 3840;
    }
    return 1920;
}

int VideoSettings::height() const {
    switch (resolution) {
        case Resolution::HD_720p: return 720;
        case Resolution::FullHD_1080p: return 1080;
        case Resolution::UHD_4K: return 2160;
    }
    return 1080;
}

// ============================================================================
// VideoRecorder implementation
// ============================================================================

VideoRecorder::VideoRecorder(QObject* parent)
    : QObject(parent)
{
}

VideoRecorder::~VideoRecorder() {
    if (isRecording_) {
        stopRecording();
    }
}

bool VideoRecorder::startRecording(const VideoSettings& settings, float startTime, float endTime) {
    if (isRecording_) {
        emit errorOccurred("Recording already in progress");
        return false;
    }

    if (settings.outputPath.isEmpty()) {
        emit errorOccurred("Output path is empty");
        return false;
    }

    if (endTime <= startTime) {
        emit errorOccurred("End time must be greater than start time");
        return false;
    }

    // Create output directory if needed
    QFileInfo fileInfo(settings.outputPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit errorOccurred("Failed to create output directory: " + dir.absolutePath());
            return false;
        }
    }

    settings_ = settings;
    startTime_ = startTime;
    endTime_ = endTime;
    lastRecordedTime_ = startTime;
    framesRecorded_ = 0;
    ffmpegOutput_.clear();

    // Calculate frame timing
    // speedMultiplier = simulation seconds per video second
    // fps = frames per video second
    // Therefore: simTimePerFrame = speedMultiplier / fps
    simTimePerFrame_ = static_cast<float>(settings_.speedMult()) / static_cast<float>(settings_.fps());
    nextFrameTime_ = startTime_;

    // Calculate total frames needed
    float totalSimTime = endTime_ - startTime_;
    totalFrames_ = static_cast<int>(std::ceil(totalSimTime / simTimePerFrame_));

    if (!startFFmpegProcess()) {
        LOG_ERROR("Failed to start FFmpeg process");
        return false;
    }

    LOG_INFO(QString("VideoRecorder: started %1x%2 @ %3fps, speed %4x, t=[%5,%6], out=%7")
        .arg(settings_.width()).arg(settings_.height())
        .arg(settings_.fps()).arg(settings_.speedMult())
        .arg(startTime, 0, 'f', 1).arg(endTime, 0, 'f', 1)
        .arg(settings_.outputPath));

    isRecording_ = true;
    emit recordingStarted();
    emit progressChanged(0);

    return true;
}

void VideoRecorder::stopRecording() {
    if (!isRecording_) return;

    isRecording_ = false;
    bool wasRunning = false;

    // Close FFmpeg stdin to signal end of input
    if (ffmpegProcess_ && ffmpegProcess_->state() == QProcess::Running) {
        wasRunning = true;
        ffmpegProcess_->closeWriteChannel();
        // Wait for FFmpeg to finish encoding
        if (!ffmpegProcess_->waitForFinished(10000)) {
            ffmpegProcess_->kill();
            emit recordingStopped(false, "FFmpeg timeout during finalization");
            cleanupFFmpeg();
            return;
        }
    }

    cleanupFFmpeg();

    // Emit success if we were recording (manual stop or completion)
    if (wasRunning) {
        LOG_INFO(QString("VideoRecorder: stopped normally, %1 frames").arg(framesRecorded_));
        emit recordingStopped(true, QString("Recording stopped: %1 frames saved to %2")
                             .arg(framesRecorded_).arg(settings_.outputPath));
    }
}

void VideoRecorder::submitFrame(const QImage& frame, float currentSimTime) {
    if (!isRecording_) return;
    if (!ffmpegProcess_ || ffmpegProcess_->state() != QProcess::Running) {
        stopRecording();
        emit errorOccurred("FFmpeg process is not running");
        return;
    }

    // Check if we've reached the end time
    if (currentSimTime >= endTime_) {
        stopRecording();
        emit recordingStopped(true, QString("Recording complete: %1 frames").arg(framesRecorded_));
        return;
    }

    // Check if it's time to record frame(s)
    if (currentSimTime < nextFrameTime_) {
        return; // Not time yet
    }

    // Record the frame once and convert it (avoid re-converting in loop)
    QImage scaledFrame = frame.scaled(settings_.width(), settings_.height(),
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage rgbFrame = scaledFrame.convertToFormat(QImage::Format_RGB888);
    const uchar* bits = rgbFrame.constBits();
    qint64 bytesToWrite = rgbFrame.sizeInBytes();

    // We might need to write the same frame multiple times if simulation advanced quickly
    // This ensures we don't skip frames when playback speed is high
    while (currentSimTime >= nextFrameTime_ && nextFrameTime_ < endTime_) {
        // Write raw RGB data to FFmpeg stdin
        qint64 bytesWritten = ffmpegProcess_->write(reinterpret_cast<const char*>(bits), bytesToWrite);

        if (bytesWritten != bytesToWrite) {
            stopRecording();
            emit errorOccurred(QString("Failed to write frame data: wrote %1 of %2 bytes")
                              .arg(bytesWritten).arg(bytesToWrite));
            return;
        }

        framesRecorded_++;
        lastRecordedTime_ = currentSimTime;
        nextFrameTime_ += simTimePerFrame_;

        // Update progress
        int percent = static_cast<int>((framesRecorded_ * 100) / totalFrames_);
        emit progressChanged(percent);
    }
}

float VideoRecorder::progress() const {
    if (totalFrames_ == 0) return 0.0f;
    return static_cast<float>(framesRecorded_) / static_cast<float>(totalFrames_);
}

QString VideoRecorder::statusMessage() const {
    if (!isRecording_) {
        return "Not recording";
    }
    return QString("Recording: %1 / %2 frames (%3%)")
        .arg(framesRecorded_)
        .arg(totalFrames_)
        .arg(static_cast<int>(progress() * 100));
}

bool VideoRecorder::startFFmpegProcess() {
    // Find FFmpeg executable
    QString ffmpegPath;

    // First, check if ffmpeg is bundled in the same directory as the executable
    QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    QString bundledFFmpeg = appDir + "/ffmpeg/ffmpeg.exe";
#else
    QString bundledFFmpeg = appDir + "/ffmpeg/ffmpeg";
#endif

    if (QFileInfo::exists(bundledFFmpeg)) {
        ffmpegPath = bundledFFmpeg;
    } else {
        // Fall back to system PATH
        ffmpegPath = "ffmpeg";
    }

    // Build FFmpeg command line arguments
    QStringList args;

    // Input settings
    args << "-y";  // Overwrite output file
    args << "-f" << "rawvideo";
    args << "-pixel_format" << "rgb24";
    args << "-video_size" << QString("%1x%2").arg(settings_.width()).arg(settings_.height());
    args << "-framerate" << QString::number(settings_.fps());
    args << "-i" << "-";  // Read from stdin

    // Output settings
    args << "-c:v" << "libx264";
    args << "-preset" << "medium";  // Balance between speed and compression
    args << "-crf" << "18";  // High quality (0-51, lower is better)
    args << "-pix_fmt" << "yuv420p";  // Compatibility with most players
    args << "-movflags" << "+faststart";  // Enable streaming

    // Output file
    args << settings_.outputPath;

    // Create and configure process
    ffmpegProcess_ = std::make_unique<QProcess>();

    connect(ffmpegProcess_.get(), &QProcess::errorOccurred,
            this, &VideoRecorder::onFFmpegError);
    connect(ffmpegProcess_.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VideoRecorder::onFFmpegFinished);
    connect(ffmpegProcess_.get(), &QProcess::readyReadStandardOutput,
            this, &VideoRecorder::onFFmpegReadyReadStdout);
    connect(ffmpegProcess_.get(), &QProcess::readyReadStandardError,
            this, &VideoRecorder::onFFmpegReadyReadStderr);

    // Start FFmpeg
    LOG_INFO(QString("VideoRecorder: launching FFmpeg at '%1'").arg(ffmpegPath));
    ffmpegProcess_->start(ffmpegPath, args);

    if (!ffmpegProcess_->waitForStarted(3000)) {
        QString error = QString("Failed to start FFmpeg at '%1'. Make sure FFmpeg is installed or bundled.\n"
                               "Error: %2").arg(ffmpegPath, ffmpegProcess_->errorString());
        LOG_ERROR(QString("VideoRecorder: FFmpeg failed to start at '%1': %2")
            .arg(ffmpegPath).arg(ffmpegProcess_->errorString()));
        emit errorOccurred(error);
        cleanupFFmpeg();
        return false;
    }

    return true;
}

void VideoRecorder::cleanupFFmpeg() {
    if (ffmpegProcess_) {
        disconnect(ffmpegProcess_.get(), nullptr, this, nullptr);
        if (ffmpegProcess_->state() == QProcess::Running) {
            ffmpegProcess_->kill();
            ffmpegProcess_->waitForFinished(1000);
        }
        ffmpegProcess_.reset();
    }
}

void VideoRecorder::onFFmpegError(QProcess::ProcessError error) {
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "FFmpeg failed to start. Check that ffmpeg.exe is in the application directory.";
            break;
        case QProcess::Crashed:
            errorMsg = "FFmpeg crashed during recording";
            break;
        case QProcess::Timedout:
            errorMsg = "FFmpeg timed out";
            break;
        case QProcess::WriteError:
            errorMsg = "Error writing to FFmpeg";
            break;
        case QProcess::ReadError:
            errorMsg = "Error reading from FFmpeg";
            break;
        default:
            errorMsg = "Unknown FFmpeg error";
            break;
    }

    LOG_ERROR(QString("VideoRecorder FFmpeg error: %1").arg(errorMsg));
    emit errorOccurred(errorMsg);

    if (isRecording_) {
        isRecording_ = false;
        emit recordingStopped(false, errorMsg);
    }
}

void VideoRecorder::onFFmpegFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (exitStatus == QProcess::CrashExit) {
        LOG_ERROR("VideoRecorder: FFmpeg crashed");
        emit recordingStopped(false, "FFmpeg crashed");
    } else if (exitCode != 0) {
        QString msg = QString("FFmpeg finished with error code %1\n%2").arg(exitCode).arg(ffmpegOutput_);
        LOG_ERROR(QString("VideoRecorder: FFmpeg exited with code %1").arg(exitCode));
        emit recordingStopped(false, msg);
    } else if (isRecording_) {
        LOG_INFO(QString("VideoRecorder: FFmpeg finished OK, output: %1").arg(settings_.outputPath));
        // Normal completion
        emit recordingStopped(true, QString("Recording saved: %1").arg(settings_.outputPath));
    }
}

void VideoRecorder::onFFmpegReadyReadStdout() {
    if (ffmpegProcess_) {
        QByteArray output = ffmpegProcess_->readAllStandardOutput();
        ffmpegOutput_.append(QString::fromUtf8(output));
    }
}

void VideoRecorder::onFFmpegReadyReadStderr() {
    if (ffmpegProcess_) {
        QByteArray output = ffmpegProcess_->readAllStandardError();
        ffmpegOutput_.append(QString::fromUtf8(output));
    }
}

} // namespace simvis
