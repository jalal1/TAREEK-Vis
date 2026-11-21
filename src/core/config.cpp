// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include <filesystem>

namespace simvis {

namespace fs = std::filesystem;

void Config::setInputFiles(const std::string& networkXml, const std::string& eventsXml) {
    networkXmlPath = networkXml;
    eventsXmlPath = eventsXml;

    // Derive binary file paths (same directory, .bin extension)
    fs::path networkPath(networkXml);
    fs::path eventsPath(eventsXml);

    // Remove .gz if present, then change to .bin
    std::string networkStem = networkPath.stem().string();
    std::string eventsStem = eventsPath.stem().string();

    if (networkStem.ends_with(".xml")) {
        networkStem = networkStem.substr(0, networkStem.length() - 4);
    }
    if (eventsStem.ends_with(".xml")) {
        eventsStem = eventsStem.substr(0, eventsStem.length() - 4);
    }

    fs::path cacheDir = networkPath.parent_path() / ".simvis_cache";
    cacheDirectory = cacheDir.string();

    networkBinPath = (cacheDir / (networkStem + ".bin")).string();
}

bool Config::hasCachedBinaryFiles() const {
    return fs::exists(networkBinPath);
}

Config& Config::instance() {
    static Config config;
    return config;
}

} // namespace simvis
