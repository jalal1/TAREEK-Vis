// SPDX-License-Identifier: GPL-3.0-or-later

#include "logger.h"
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

namespace simvis {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init() {
    auto& L = instance();
    QMutexLocker lock(&L.mutex_);

    QString tempLog = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                      + "/simvis.log";
    L.openFile(tempLog);
}

void Logger::redirectLogFile(const QString& cacheDir) {
    auto& L = instance();
    QMutexLocker lock(&L.mutex_);

    QDir().mkpath(cacheDir);
    QString logPath = cacheDir + "/simvis.log";

    if (L.file_.isOpen()) {
        L.stream_.flush();
        L.file_.close();
    }
    L.openFile(logPath);
}

void Logger::log(Level level, const QString& message) {
    auto& L = instance();
    QMutexLocker lock(&L.mutex_);

    if (!L.streamOpen_) return;

    L.truncateIfNeeded();

    const char* tag = (level == Level::Info)  ? "INFO " :
                      (level == Level::Warn)  ? "WARN " : "ERROR";

    QString line = QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss]")
                   + " [" + tag + "] " + message;

    L.stream_ << line << "\n";
    L.stream_.flush();

    if (level == Level::Error)
        qWarning().noquote() << line;
    else
        qDebug().noquote() << line;
}

void Logger::openFile(const QString& path) {
    file_.setFileName(path);
    file_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    stream_.setDevice(&file_);
    streamOpen_ = file_.isOpen();
}

void Logger::truncateIfNeeded() {
    if (!file_.isOpen()) return;
    if (file_.size() < kMaxBytes) return;

    QString path = file_.fileName();
    stream_.flush();
    file_.close();

    QFile::remove(path + ".1");
    QFile::rename(path, path + ".1");

    file_.setFileName(path);
    file_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    stream_.setDevice(&file_);
    streamOpen_ = file_.isOpen();
}

} // namespace simvis
