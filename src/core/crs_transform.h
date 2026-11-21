// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <stdexcept>

namespace simvis {

// Coordinate Reference System info parsed from EPSG code
struct CRSInfo {
    int epsgCode = 0;
    int utmZone = 0;        // 1-60, or 0 if not UTM
    bool isNorth = true;
    bool isWgs84 = false;

    bool isValid() const { return epsgCode != 0; }
    bool isUtm() const { return utmZone > 0; }
};

// Parse "EPSG:26915" style string into CRSInfo
inline CRSInfo parseCRS(const std::string& crsStr) {
    CRSInfo info;

    // Extract EPSG code number
    auto pos = crsStr.find("EPSG:");
    if (pos == std::string::npos) pos = crsStr.find("epsg:");
    if (pos == std::string::npos) return info;

    int code = std::stoi(crsStr.substr(pos + 5));
    info.epsgCode = code;

    // WGS84 geographic
    if (code == 4326) {
        info.isWgs84 = true;
        return info;
    }

    // EPSG:269xx — UTM NAD83 North (North America)
    if (code >= 26901 && code <= 26960) {
        info.utmZone = code - 26900;
        info.isNorth = true;
        return info;
    }

    // EPSG:258xx — UTM ETRS89 (Europe)
    if (code >= 25801 && code <= 25860) {
        info.utmZone = code - 25800;
        info.isNorth = true;
        return info;
    }

    // EPSG:326xx — UTM WGS84 North
    if (code >= 32601 && code <= 32660) {
        info.utmZone = code - 32600;
        info.isNorth = true;
        return info;
    }

    // EPSG:327xx — UTM WGS84 South
    if (code >= 32701 && code <= 32760) {
        info.utmZone = code - 32700;
        info.isNorth = false;
        return info;
    }

    return info; // Unknown CRS
}

namespace crs {

constexpr double PI = 3.14159265358979323846;
constexpr double DEG2RAD = PI / 180.0;
constexpr double RAD2DEG = 180.0 / PI;

// WGS84 ellipsoid parameters
constexpr double WGS84_A = 6378137.0;           // semi-major axis
constexpr double WGS84_F = 1.0 / 298.257223563; // flattening
constexpr double WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F; // eccentricity squared

// UTM parameters
constexpr double UTM_K0 = 0.9996;
constexpr double UTM_FE = 500000.0;  // false easting
constexpr double UTM_FN_SOUTH = 10000000.0; // false northing for southern hemisphere

// Convert UTM (easting, northing) to WGS84 (longitude, latitude) in degrees
inline std::pair<double, double> utmToWgs84(double easting, double northing,
                                             int zone, bool isNorth) {
    double x = easting - UTM_FE;
    double y = isNorth ? northing : northing - UTM_FN_SOUTH;

    double M = y / UTM_K0;
    double mu = M / (WGS84_A * (1.0 - WGS84_E2 / 4.0 - 3.0 * WGS84_E2 * WGS84_E2 / 64.0
                     - 5.0 * WGS84_E2 * WGS84_E2 * WGS84_E2 / 256.0));

    double e1 = (1.0 - std::sqrt(1.0 - WGS84_E2)) / (1.0 + std::sqrt(1.0 - WGS84_E2));

    double phi1 = mu + (3.0 * e1 / 2.0 - 27.0 * e1 * e1 * e1 / 32.0) * std::sin(2.0 * mu)
                     + (21.0 * e1 * e1 / 16.0 - 55.0 * e1 * e1 * e1 * e1 / 32.0) * std::sin(4.0 * mu)
                     + (151.0 * e1 * e1 * e1 / 96.0) * std::sin(6.0 * mu);

    double sinPhi1 = std::sin(phi1);
    double cosPhi1 = std::cos(phi1);
    double tanPhi1 = sinPhi1 / cosPhi1;

    double N1 = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi1 * sinPhi1);
    double T1 = tanPhi1 * tanPhi1;
    double e2prime = WGS84_E2 / (1.0 - WGS84_E2);
    double C1 = e2prime * cosPhi1 * cosPhi1;
    double R1 = WGS84_A * (1.0 - WGS84_E2) /
                std::pow(1.0 - WGS84_E2 * sinPhi1 * sinPhi1, 1.5);
    double D = x / (N1 * UTM_K0);

    double lat = phi1 - (N1 * tanPhi1 / R1) * (
        D * D / 2.0
        - (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1 - 9.0 * e2prime) * D * D * D * D / 24.0
        + (61.0 + 90.0 * T1 + 298.0 * C1 + 45.0 * T1 * T1
           - 252.0 * e2prime - 3.0 * C1 * C1) * D * D * D * D * D * D / 720.0
    );

    double lon0 = (zone * 6.0 - 183.0) * DEG2RAD;
    double lon = lon0 + (
        D
        - (1.0 + 2.0 * T1 + C1) * D * D * D / 6.0
        + (5.0 - 2.0 * C1 + 28.0 * T1 - 3.0 * C1 * C1
           + 8.0 * e2prime + 24.0 * T1 * T1) * D * D * D * D * D / 120.0
    ) / cosPhi1;

    return { lon * RAD2DEG, lat * RAD2DEG };
}

// Convert WGS84 (longitude, latitude) in degrees to UTM (easting, northing)
inline std::pair<double, double> wgs84ToUtm(double lon, double lat,
                                             int zone, bool isNorth) {
    double latRad = lat * DEG2RAD;
    double lonRad = lon * DEG2RAD;

    double lon0 = (zone * 6.0 - 183.0) * DEG2RAD;

    double sinLat = std::sin(latRad);
    double cosLat = std::cos(latRad);
    double tanLat = sinLat / cosLat;

    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinLat * sinLat);
    double T = tanLat * tanLat;
    double e2prime = WGS84_E2 / (1.0 - WGS84_E2);
    double C = e2prime * cosLat * cosLat;
    double A = cosLat * (lonRad - lon0);

    double M = WGS84_A * (
        (1.0 - WGS84_E2 / 4.0 - 3.0 * WGS84_E2 * WGS84_E2 / 64.0
         - 5.0 * WGS84_E2 * WGS84_E2 * WGS84_E2 / 256.0) * latRad
        - (3.0 * WGS84_E2 / 8.0 + 3.0 * WGS84_E2 * WGS84_E2 / 32.0
           + 45.0 * WGS84_E2 * WGS84_E2 * WGS84_E2 / 1024.0) * std::sin(2.0 * latRad)
        + (15.0 * WGS84_E2 * WGS84_E2 / 256.0
           + 45.0 * WGS84_E2 * WGS84_E2 * WGS84_E2 / 1024.0) * std::sin(4.0 * latRad)
        - (35.0 * WGS84_E2 * WGS84_E2 * WGS84_E2 / 3072.0) * std::sin(6.0 * latRad)
    );

    double easting = UTM_FE + UTM_K0 * N * (
        A + (1.0 - T + C) * A * A * A / 6.0
        + (5.0 - 18.0 * T + T * T + 72.0 * C - 58.0 * e2prime) * A * A * A * A * A / 120.0
    );

    double northing = UTM_K0 * (M + N * tanLat * (
        A * A / 2.0
        + (5.0 - T + 9.0 * C + 4.0 * C * C) * A * A * A * A / 24.0
        + (61.0 - 58.0 * T + T * T + 600.0 * C - 330.0 * e2prime) * A * A * A * A * A * A / 720.0
    ));

    if (!isNorth) northing += UTM_FN_SOUTH;

    return { easting, northing };
}

// Convert network coordinates to WGS84 using CRS info
inline std::pair<double, double> toWgs84(double x, double y, const CRSInfo& crs) {
    if (crs.isWgs84) return { x, y }; // x=lon, y=lat
    if (crs.isUtm()) return utmToWgs84(x, y, crs.utmZone, crs.isNorth);
    return { 0.0, 0.0 }; // unsupported
}

// Convert WGS84 to network coordinates using CRS info
inline std::pair<double, double> fromWgs84(double lon, double lat, const CRSInfo& crs) {
    if (crs.isWgs84) return { lon, lat };
    if (crs.isUtm()) return wgs84ToUtm(lon, lat, crs.utmZone, crs.isNorth);
    return { 0.0, 0.0 }; // unsupported
}

// OSM Slippy map tile math
// Convert lon/lat to tile x/y at given zoom level
inline std::pair<int, int> lonLatToTile(double lon, double lat, int zoom) {
    int n = 1 << zoom;
    int x = static_cast<int>((lon + 180.0) / 360.0 * n);
    double latRad = lat * DEG2RAD;
    int y = static_cast<int>((1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / PI) / 2.0 * n);
    x = std::clamp(x, 0, n - 1);
    y = std::clamp(y, 0, n - 1);
    return { x, y };
}

// Convert lon/lat to fractional tile coordinates (for sub-tile positioning)
inline std::pair<double, double> lonLatToTileF(double lon, double lat, int zoom) {
    int n = 1 << zoom;
    double x = (lon + 180.0) / 360.0 * n;
    double latRad = lat * DEG2RAD;
    double y = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / PI) / 2.0 * n;
    return { x, y };
}

// Get WGS84 bounding box for a tile: returns (lonMin, latMin, lonMax, latMax)
inline void tileBoundsWgs84(int tileX, int tileY, int zoom,
                             double& lonMin, double& latMin,
                             double& lonMax, double& latMax) {
    int n = 1 << zoom;
    lonMin = static_cast<double>(tileX) / n * 360.0 - 180.0;
    lonMax = static_cast<double>(tileX + 1) / n * 360.0 - 180.0;

    double latRadMax = std::atan(std::sinh(PI * (1.0 - 2.0 * static_cast<double>(tileY) / n)));
    double latRadMin = std::atan(std::sinh(PI * (1.0 - 2.0 * static_cast<double>(tileY + 1) / n)));

    latMin = latRadMin * RAD2DEG;
    latMax = latRadMax * RAD2DEG;
}

} // namespace crs
} // namespace simvis
