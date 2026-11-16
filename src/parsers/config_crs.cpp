#include "config_crs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>
#include <zlib.h>

namespace simvis {

namespace {

namespace fs = std::filesystem;

std::string attrValue(const std::string& line, const std::string& name) {
    const std::string search = name + "=\"";
    size_t pos = line.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    const size_t end = line.find('"', pos);
    if (end == std::string::npos) return "";
    return line.substr(pos, end - pos);
}

bool isEpsg(const std::string& value) {
    if (value.size() < 6) return false;
    std::string prefix = value.substr(0, 5);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return prefix == "EPSG:" &&
           std::isdigit(static_cast<unsigned char>(value[5]));
}

// The CRS in one config file, or "". gzopen also reads files that are not
// compressed.
std::string crsInFile(const fs::path& path) {
    gzFile file = gzopen(path.string().c_str(), "rb");
    if (!file) return "";

    std::string module;
    std::string global, inputCrs;
    char buffer[4096];
    while (gzgets(file, buffer, sizeof(buffer))) {
        const std::string line(buffer);
        if (line.find("<module") != std::string::npos) {
            module = attrValue(line, "name");
        }
        if (line.find("<param") == std::string::npos) continue;
        const std::string name = attrValue(line, "name");
        if (module == "global" && name == "coordinateSystem")
            global = attrValue(line, "value");
        else if (module == "network" && name == "inputCRS")
            inputCrs = attrValue(line, "value");
    }
    gzclose(file);

    if (isEpsg(global)) return global;
    if (isEpsg(inputCrs)) return inputCrs;
    return "";
}

} // anonymous namespace

std::string findCrsInRunConfig(const std::string& folder, std::string* sourceFile) {
    std::error_code ec;
    const fs::path dir(folder);
    if (!fs::is_directory(dir, ec)) return "";

    std::vector<fs::path> candidates;
    for (const char* name : { "output_config.xml", "output_config.xml.gz",
                              "config.xml", "config.xml.gz" }) {
        candidates.push_back(dir / name);
    }
    // Other config files, for example output_config_reduced.xml, in name order
    std::vector<fs::path> others;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool xml = name.size() > 4 &&
            (name.compare(name.size() - 4, 4, ".xml") == 0 ||
             (name.size() > 7 && name.compare(name.size() - 7, 7, ".xml.gz") == 0));
        if (xml && name.find("config") != std::string::npos &&
            std::find(candidates.begin(), candidates.end(), entry.path()) == candidates.end()) {
            others.push_back(entry.path());
        }
    }
    std::sort(others.begin(), others.end());
    candidates.insert(candidates.end(), others.begin(), others.end());

    for (const auto& path : candidates) {
        if (!fs::is_regular_file(path, ec)) continue;
        std::string crs = crsInFile(path);
        if (!crs.empty()) {
            if (sourceFile) *sourceFile = path.string();
            return crs;
        }
    }
    return "";
}

} // namespace simvis
