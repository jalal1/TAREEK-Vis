// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QMutex>
#include <QFile>
#include <QTextStream>

namespace simvis {

class Logger {
public:
    enum class Level { Info, Warn, Error };

    // Call once from main() before QApplication.
    // Opens a fallback log in the system temp dir until redirectLogFile() is called.
    static void init();

    // Redirect log output to .simvis_cache/simvis.log once the cache dir is known.
    static void redirectLogFile(const QString& cacheDir);

    // Thread-safe logging.
    static void log(Level level, const QString& message);

    static Logger& instance();

private:
    Logger() = default;

    void openFile(const QString& path);
    void truncateIfNeeded();

    QMutex mutex_;
    QFile file_;
    QTextStream stream_;
    bool streamOpen_ = false;
    static constexpr qint64 kMaxBytes = 2 * 1024 * 1024; // 2 MB
};

} // namespace simvis

#define LOG_INFO(msg)  simvis::Logger::log(simvis::Logger::Level::Info,  (msg))
#define LOG_WARN(msg)  simvis::Logger::log(simvis::Logger::Level::Warn,  (msg))
#define LOG_ERROR(msg) simvis::Logger::log(simvis::Logger::Level::Error, (msg))
