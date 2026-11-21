// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>

namespace simvis {

struct Config {
    // Memory limits
    size_t maxMemoryMB = 1024;          // Default 1 GB

    // Rendering settings
    float nodeSize = 3.0f;
    float linkWidth = 1.5f;
    float vehicleSize = 5.0f;

    // Playback settings
    double defaultPlaybackSpeed = 1.0;
    double maxPlaybackSpeed = 100.0;

    // File paths (set at runtime)
    std::string networkXmlPath;
    std::string eventsXmlPath;
    std::string networkBinPath;
    std::string cacheDirectory;

    // Derived paths
    void setInputFiles(const std::string& networkXml, const std::string& eventsXml);
    bool hasCachedBinaryFiles() const;

    // Singleton access
    static Config& instance();

private:
    Config() = default;
};

} // namespace simvis
