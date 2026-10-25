#include "network_parser.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <zlib.h>

// We'll use a SAX-style streaming parser for memory efficiency
// pugixml doesn't support SAX, so we'll use a simple custom parser for large files

namespace simvis {

namespace {

// Simple gzip decompression helper
class GzipReader {
public:
    explicit GzipReader(const std::string& path) {
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) {
            throw std::runtime_error("Failed to open gzip file: " + path);
        }

        // Get uncompressed size estimate (for progress)
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

} // anonymous namespace

NetworkParser::Result NetworkParser::parseGzipFile(const std::string& path, ProgressCallback progress) {
    Result result;

    try {
        GzipReader reader(path);

        if (progress) {
            progress(0, reader.totalSize(), "Opening network file...");
        }

        std::string line;
        size_t nodeCount = 0;
        size_t linkCount = 0;
        size_t lastProgress = 0;

        while (!reader.eof()) {
            line = reader.readLine();
            if (line.empty()) continue;

            // Progress update every ~1MB
            if (progress && reader.bytesRead() - lastProgress > 1000000) {
                lastProgress = reader.bytesRead();
                progress(reader.bytesRead(), reader.totalSize(),
                    "Parsing network: " + std::to_string(nodeCount) + " nodes, " +
                    std::to_string(linkCount) + " links");
            }

            // Parse CRS from <attribute name="coordinateReferenceSystem" ...>EPSG:XXXXX</attribute>
            if (result.crs.empty() &&
                line.find("coordinateReferenceSystem") != std::string::npos) {
                // Value is between > and </attribute>
                auto gt = line.find('>');
                auto lt = line.find("</attribute>", gt);
                if (gt != std::string::npos && lt != std::string::npos) {
                    result.crs = line.substr(gt + 1, lt - gt - 1);
                }
            }

            // Parse node
            if (line.find("<node ") != std::string::npos) {
                std::string id = getAttr(line, "id");
                std::string xStr = getAttr(line, "x");
                std::string yStr = getAttr(line, "y");
                // Optional height. The leading space keeps a longer name that
                // ends in z from matching.
                std::string zStr = getAttr(line, " z");

                if (!id.empty() && !xStr.empty() && !yStr.empty()) {
                    double x = std::stod(xStr);
                    double y = std::stod(yStr);

                    NodeRecord rec;
                    rec.id = result.nodeIds.intern(id);
                    rec.x = static_cast<float>(x);
                    rec.y = static_cast<float>(y);
                    rec.z = zStr.empty() ? 0.0f : static_cast<float>(std::stod(zStr));

                    result.nodes.push_back(rec);
                    result.bounds.expand(x, y);
                    nodeCount++;
                }
            }
            // Parse link
            else if (line.find("<link ") != std::string::npos) {
                std::string id = getAttr(line, "id");
                std::string from = getAttr(line, "from");
                std::string to = getAttr(line, "to");
                std::string lengthStr = getAttr(line, "length");
                std::string freespeedStr = getAttr(line, "freespeed");
                std::string capacityStr = getAttr(line, "capacity");
                std::string lanesStr = getAttr(line, "permlanes");

                if (!id.empty() && !from.empty() && !to.empty()) {
                    LinkRecord rec;
                    rec.id = result.linkIds.intern(id);

                    // Map from/to node IDs
                    if (result.nodeIds.hasString(from)) {
                        rec.fromNode = result.nodeIds.getId(from);
                    } else {
                        rec.fromNode = result.nodeIds.intern(from);
                    }

                    if (result.nodeIds.hasString(to)) {
                        rec.toNode = result.nodeIds.getId(to);
                    } else {
                        rec.toNode = result.nodeIds.intern(to);
                    }

                    rec.length = lengthStr.empty() ? 0.0f : static_cast<float>(std::stod(lengthStr));
                    rec.freespeed = freespeedStr.empty() ? 13.89f : static_cast<float>(std::stod(freespeedStr));
                    rec.capacity = capacityStr.empty() ? 1000.0f : static_cast<float>(std::stod(capacityStr));
                    rec.lanes = lanesStr.empty() ? 1 : static_cast<uint8_t>(std::stod(lanesStr));

                    // Get road type from nested attribute (simplified - may need multi-line parsing)
                    rec.roadType = static_cast<uint8_t>(RoadType::Unknown);
                    rec.padding = 0;

                    result.links.push_back(rec);
                    linkCount++;
                }
            }
            // Check for road type in attribute line
            else if (line.find("name=\"type\"") != std::string::npos && !result.links.empty()) {
                // Extract type value - format: <attribute name="type" class="...">value</attribute>
                size_t start = line.find('>');
                size_t end = line.find('<', start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    std::string typeVal = line.substr(start + 1, end - start - 1);
                    result.links.back().roadType = static_cast<uint8_t>(parseRoadType(typeVal));
                }
            }
        }

        if (progress) {
            progress(reader.totalSize(), reader.totalSize(),
                "Completed: " + std::to_string(nodeCount) + " nodes, " +
                std::to_string(linkCount) + " links");
        }

        result.success = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }

    return result;
}

NetworkParser::Result NetworkParser::parseStream(std::istream& stream, size_t totalSize, ProgressCallback progress) {
    // For now, read entire stream and parse
    std::stringstream buffer;
    buffer << stream.rdbuf();
    std::string content = buffer.str();

    Result result;
    // Similar parsing logic as above but from string
    // (simplified implementation - in production would use proper streaming)

    result.success = false;
    result.errorMessage = "Stream parsing not yet implemented - use parseGzipFile";
    return result;
}

bool NetworkParser::writeBinary(const std::string& path, const Result& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    // Write header
    NetworkFileHeader header;
    header.magic = NETWORK_FILE_MAGIC;
    header.version = FILE_VERSION;
    header.nodeCount = static_cast<uint32_t>(data.nodes.size());
    header.linkCount = static_cast<uint32_t>(data.links.size());
    header.bounds = data.bounds;

    // Calculate offsets
    size_t currentOffset = sizeof(NetworkFileHeader);

    header.nodeOffset = currentOffset;
    currentOffset += data.nodes.size() * sizeof(NodeRecord);

    header.linkOffset = currentOffset;
    currentOffset += data.links.size() * sizeof(LinkRecord);

    header.stringTableOffset = currentOffset;

    // Write header (we'll update stringTableSize after writing strings)
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write nodes
    out.write(reinterpret_cast<const char*>(data.nodes.data()),
              data.nodes.size() * sizeof(NodeRecord));

    // Write links
    out.write(reinterpret_cast<const char*>(data.links.data()),
              data.links.size() * sizeof(LinkRecord));

    // Write string tables
    size_t stringStart = out.tellp();
    data.nodeIds.writeTo(out);
    data.linkIds.writeTo(out);
    size_t stringEnd = out.tellp();

    // Update header with string table size
    header.stringTableSize = stringEnd - stringStart;
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    return out.good();
}

NetworkFileHeader NetworkParser::readBinaryHeader(const std::string& path) {
    NetworkFileHeader header{};

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return header;
    }

    in.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != NETWORK_FILE_MAGIC || header.version != FILE_VERSION) {
        header = {};  // Invalid file
    }

    return header;
}

} // namespace simvis
