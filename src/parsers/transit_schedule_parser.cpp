// SPDX-License-Identifier: GPL-3.0-or-later

#include "transit_schedule_parser.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <zlib.h>

namespace simvis {

namespace {

// Simple gzip reader (same approach as NetworkParser)
class GzipReader {
public:
    explicit GzipReader(const std::string& path) {
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) {
            throw std::runtime_error("Failed to open gzip file: " + path);
        }
        // Estimate total size for progress
        gzFile temp = gzopen(path.c_str(), "rb");
        if (temp) {
            char buffer[65536];
            while (gzread(temp, buffer, sizeof(buffer)) > 0) {
                totalSize_ += sizeof(buffer);
            }
            gzclose(temp);
            gzrewind(file_);
        }
    }

    ~GzipReader() {
        if (file_) gzclose(file_);
    }

    std::string readLine() {
        char buffer[4096];
        if (gzgets(file_, buffer, sizeof(buffer))) {
            bytesRead_ += strlen(buffer);
            return std::string(buffer);
        }
        return "";
    }

    bool eof() const { return gzeof(file_); }
    size_t bytesRead() const { return bytesRead_; }
    size_t totalSize() const { return totalSize_; }

private:
    gzFile file_ = nullptr;
    size_t bytesRead_ = 0;
    size_t totalSize_ = 0;
};

// Plain file line reader
class PlainReader {
public:
    explicit PlainReader(const std::string& path) : stream_(path) {
        if (!stream_) {
            throw std::runtime_error("Failed to open file: " + path);
        }
        // Get file size
        stream_.seekg(0, std::ios::end);
        totalSize_ = static_cast<size_t>(stream_.tellg());
        stream_.seekg(0, std::ios::beg);
    }

    std::string readLine() {
        std::string line;
        if (std::getline(stream_, line)) {
            bytesRead_ += line.size() + 1;
            return line;
        }
        return "";
    }

    bool eof() const { return stream_.eof() || !stream_.good(); }
    size_t bytesRead() const { return bytesRead_; }
    size_t totalSize() const { return totalSize_; }

private:
    std::ifstream stream_;
    size_t bytesRead_ = 0;
    size_t totalSize_ = 0;
};

// Parse a single XML attribute value
std::string getAttr(const std::string& line, const std::string& name) {
    std::string search = name + "=\"";
    size_t pos = line.find(search);
    if (pos == std::string::npos) return "";

    pos += search.length();
    size_t end = line.find('"', pos);
    if (end == std::string::npos) return "";

    return line.substr(pos, end - pos);
}

// Parse time offset string "HH:MM:SS" to milliseconds
uint32_t parseTimeOffsetMs(const std::string& timeStr) {
    if (timeStr.empty()) return 0;

    int h = 0, m = 0, s = 0;
    if (sscanf(timeStr.c_str(), "%d:%d:%d", &h, &m, &s) >= 2) {
        return static_cast<uint32_t>((h * 3600 + m * 60 + s) * 1000);
    }
    return 0;
}

// State machine for tracking XML context
enum class ParseState {
    Root,
    TransitStops,
    TransitLine,
    TransitRoute,
    RouteProfile,
    RouteLinks,
    Departures
};

template<typename Reader>
TransitScheduleParser::Result parseWithReader(Reader& reader,
                                               const StringInterner& linkIds,
                                               ProgressCallback progress) {
    TransitScheduleParser::Result result;

    ParseState state = ParseState::Root;
    TransitLine currentLine;
    TransitRoute currentRoute;
    size_t lastProgress = 0;
    size_t stopCount = 0;
    size_t lineCount = 0;

    while (!reader.eof()) {
        std::string line = reader.readLine();
        if (line.empty()) continue;

        // Progress update every ~500KB
        if (progress && reader.bytesRead() - lastProgress > 500000) {
            lastProgress = reader.bytesRead();
            progress(reader.bytesRead(), reader.totalSize(),
                "Parsing transit: " + std::to_string(stopCount) + " stops, " +
                std::to_string(lineCount) + " lines");
        }

        // ================================================================
        // Stop facilities
        // ================================================================
        if (line.find("<transitStops>") != std::string::npos ||
            line.find("<transitStops ") != std::string::npos) {
            state = ParseState::TransitStops;
            continue;
        }

        if (line.find("</transitStops>") != std::string::npos) {
            state = ParseState::Root;
            continue;
        }

        if (state == ParseState::TransitStops &&
            line.find("<stopFacility ") != std::string::npos) {
            std::string id = getAttr(line, "id");
            std::string xStr = getAttr(line, "x");
            std::string yStr = getAttr(line, "y");
            std::string name = getAttr(line, "name");
            std::string linkRef = getAttr(line, "linkRefId");

            if (!id.empty() && !xStr.empty() && !yStr.empty()) {
                TransitStop stop;
                stop.id = result.stopIds.intern(id);
                stop.x = std::stof(xStr);
                stop.y = std::stof(yStr);
                stop.name = name;
                stop.originalId = id;

                // Resolve link reference
                if (!linkRef.empty() && linkIds.hasString(linkRef)) {
                    stop.linkRefId = linkIds.getId(linkRef);
                } else {
                    stop.linkRefId = 0;
                }

                result.stopIdToIndex[stop.id] = result.stops.size();
                result.stops.push_back(std::move(stop));
                stopCount++;
            }
            continue;
        }

        // ================================================================
        // Transit lines
        // ================================================================
        if (line.find("<transitLine ") != std::string::npos) {
            state = ParseState::TransitLine;
            currentLine = TransitLine{};
            std::string id = getAttr(line, "id");
            std::string name = getAttr(line, "name");
            if (!id.empty()) {
                currentLine.id = result.lineIds.intern(id);
                currentLine.originalId = id;
                currentLine.name = name.empty() ? id : name;
                currentLine.primaryMode = TransitMode::Unknown;
            }
            continue;
        }

        if (line.find("</transitLine>") != std::string::npos) {
            // Determine primary mode from first route
            if (!currentLine.routes.empty()) {
                currentLine.primaryMode = currentLine.routes[0].mode;
            }
            result.lines.push_back(std::move(currentLine));
            lineCount++;
            state = ParseState::Root;
            continue;
        }

        // ================================================================
        // Transit routes within a line
        // ================================================================
        if (state == ParseState::TransitLine &&
            line.find("<transitRoute ") != std::string::npos) {
            state = ParseState::TransitRoute;
            currentRoute = TransitRoute{};
            std::string id = getAttr(line, "id");
            if (!id.empty()) {
                currentRoute.id = result.routeIds.intern(id);
            }
            continue;
        }

        if (line.find("</transitRoute>") != std::string::npos) {
            currentLine.routes.push_back(std::move(currentRoute));
            state = ParseState::TransitLine;
            continue;
        }

        // Transport mode
        if (state == ParseState::TransitRoute &&
            line.find("<transportMode>") != std::string::npos) {
            size_t start = line.find("<transportMode>") + 15;
            size_t end = line.find("</transportMode>");
            if (end != std::string::npos && end > start) {
                std::string modeStr = line.substr(start, end - start);
                currentRoute.mode = parseTransitMode(modeStr);
            }
            continue;
        }

        // ================================================================
        // Route profile (stops)
        // ================================================================
        if (line.find("<routeProfile>") != std::string::npos) {
            state = ParseState::RouteProfile;
            continue;
        }

        if (line.find("</routeProfile>") != std::string::npos) {
            state = ParseState::TransitRoute;
            continue;
        }

        if (state == ParseState::RouteProfile &&
            line.find("<stop ") != std::string::npos) {
            std::string refId = getAttr(line, "refId");
            std::string arrival = getAttr(line, "arrivalOffset");
            std::string departure = getAttr(line, "departureOffset");

            if (!refId.empty()) {
                TransitRouteStop routeStop;
                // Intern the stop reference if not already known
                if (result.stopIds.hasString(refId)) {
                    routeStop.stopId = result.stopIds.getId(refId);
                } else {
                    routeStop.stopId = result.stopIds.intern(refId);
                }
                routeStop.arrivalOffsetMs = parseTimeOffsetMs(arrival);
                routeStop.departureOffsetMs = parseTimeOffsetMs(departure);
                currentRoute.stops.push_back(routeStop);
            }
            continue;
        }

        // ================================================================
        // Route link sequence
        // ================================================================
        if (line.find("<route>") != std::string::npos) {
            state = ParseState::RouteLinks;
            continue;
        }

        if (line.find("</route>") != std::string::npos) {
            state = ParseState::TransitRoute;
            continue;
        }

        if (state == ParseState::RouteLinks &&
            line.find("<link ") != std::string::npos) {
            std::string refId = getAttr(line, "refId");
            if (!refId.empty() && linkIds.hasString(refId)) {
                currentRoute.linkIds.push_back(linkIds.getId(refId));
            }
            // Skip pt_ artificial links that aren't in the network
            continue;
        }

        // ================================================================
        // Departures
        // ================================================================
        if (line.find("<departures>") != std::string::npos) {
            state = ParseState::Departures;
            continue;
        }

        if (line.find("</departures>") != std::string::npos) {
            state = ParseState::TransitRoute;
            continue;
        }

        if (state == ParseState::Departures &&
            line.find("<departure ") != std::string::npos) {
            std::string depTime = getAttr(line, "departureTime");
            std::string vehicleRef = getAttr(line, "vehicleRefId");

            if (!vehicleRef.empty()) {
                TransitDeparture dep;
                dep.departureTimeMs = parseTimeOffsetMs(depTime);
                dep.vehicleRefId = vehicleRef;
                currentRoute.departures.push_back(dep);

                // Build vehicle-to-mode map
                result.vehicleToMode[vehicleRef] = currentRoute.mode;
            }
            continue;
        }
    }

    if (progress) {
        progress(reader.totalSize(), reader.totalSize(),
            "Completed: " + std::to_string(stopCount) + " stops, " +
            std::to_string(lineCount) + " lines");
    }

    result.success = true;
    return result;
}

} // anonymous namespace

TransitScheduleParser::Result TransitScheduleParser::parse(
    const std::string& xmlPath,
    const StringInterner& linkIds,
    ProgressCallback progress)
{
    Result result;
    try {
        PlainReader reader(xmlPath);
        result = parseWithReader(reader, linkIds, progress);
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }
    return result;
}

TransitScheduleParser::Result TransitScheduleParser::parseGzipFile(
    const std::string& gzipPath,
    const StringInterner& linkIds,
    ProgressCallback progress)
{
    Result result;
    try {
        GzipReader reader(gzipPath);
        result = parseWithReader(reader, linkIds, progress);
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }
    return result;
}

} // namespace simvis
