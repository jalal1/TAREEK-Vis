// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include <functional>
#include <string>
#include <istream>

namespace simvis {

// Progress callback: (current bytes read, total bytes, message)
using ProgressCallback = std::function<void(size_t, size_t, const std::string&)>;

class NetworkParser {
public:
    struct Result {
        std::vector<NodeRecord> nodes;
        std::vector<LinkRecord> links;
        StringInterner nodeIds;
        StringInterner linkIds;
        BoundingBox bounds;
        std::string crs;  // e.g. "EPSG:26915" from coordinateReferenceSystem attribute
        uint32_t capPeriodS = 0;  // <links capperiod="01:00:00">, s; 0 = none
        bool success = false;
        std::string errorMessage;
    };

    // Parse from gzipped XML file
    static Result parseGzipFile(const std::string& path, ProgressCallback progress = nullptr);

    // Parse from already decompressed stream
    static Result parseStream(std::istream& stream, size_t totalSize = 0, ProgressCallback progress = nullptr);

    // Write binary format
    static bool writeBinary(const std::string& path, const Result& data);

    // Read binary format (header only for quick access)
    static NetworkFileHeader readBinaryHeader(const std::string& path);
};

} // namespace simvis
