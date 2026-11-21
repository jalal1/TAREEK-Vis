// SPDX-License-Identifier: GPL-3.0-or-later

#include "link_metrics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace simvis {
namespace linkmetrics {

namespace {

// Capacity at or above this is a "no limit" value. MATSim networks made from
// OSM give pt and rail links 9999 veh/h.
constexpr float kNoLimitCapacity = 9999.0f;

std::vector<Metric> makeMetrics() {
    std::vector<Metric> m;
    // The base map: 5 road levels (roadLevel), drawn in grays with width
    m.push_back({Mode::RoadType, "road_type", "Road Type", "", false, Palette::RoadType,
                 Scale::Fixed, {0.5f, 1.5f, 2.5f, 3.5f},
                 {"Residential, service, other", "Tertiary", "Secondary", "Primary and trunk",
                  "Motorway"},
                 "No road type (pt, rail)", false, false,
                 "The road type of the link (the base map)."});

    // --- Network attributes. Classes from the Twin Cities network
    // (docs/link-colors.md, section 1): 82 % of the links have 600 veh/h.
    m.push_back({Mode::Capacity, "capacity", "Capacity", "veh/h", false, Palette::Sequential,
                 Scale::Fixed, {600.0f, 1200.0f, 2000.0f, 4000.0f},
                 {"600 or less", "601 to 1,200", "1,201 to 2,000", "2,001 to 4,000",
                  "More than 4,000"},
                 "No limit (9,999 or more)", false, false,
                 "The capacity of the link, per hour."});
    m.push_back({Mode::FreeSpeed, "free_speed", "Free Speed", "km/h", false, Palette::Sequential,
                 Scale::Fixed, {35.0f, 55.0f, 75.0f, 100.0f, 130.0f},
                 {"35 or less", "36 to 55", "56 to 75", "76 to 100", "101 to 130",
                  "More than 130"},
                 "", false, false, "The free speed of the link."});
    m.push_back({Mode::Lanes, "lanes", "Lanes", "", false, Palette::Sequential,
                 Scale::Fixed, {1.0f, 2.0f, 3.0f, 4.0f},
                 {"1 or less", "1.5 to 2", "2.5 to 3", "3.5 to 4", "More than 4"},
                 "", false, false, "The number of lanes of the link."});
    m.push_back({Mode::AllowedModes, "allowed_modes", "Allowed Modes", "", false,
                 Palette::Categorical, Scale::Fixed, {0.5f, 1.5f, 2.5f, 3.5f},
                 {"Car (and other modes)", "Bus or pt only", "Rail or train",
                  "Tram or light rail", "Other modes only"},
                 "No modes in the network file", false, false,
                 "The modes that can use the link."});

    // --- Simulation results. They follow the timeline (decision 3: the value
    // of the bin that holds the current time) or a fixed window.
    m.push_back({Mode::Flow, "flow", "Flow", "veh/h", true, Palette::Sequential,
                 Scale::RobustMax, {}, {}, "No vehicles", false, true,
                 "Vehicles that enter the link, per hour."});
    m.push_back({Mode::SpeedRatio, "speed_ratio", "Speed / Free Speed", "", true,
                 Palette::Congestion, Scale::Fixed, {0.25f, 0.5f, 0.75f, 0.9f},
                 {"25 % or less", "26 to 50 %", "51 to 75 %", "76 to 90 %", "More than 90 %"},
                 "No vehicles entered", true, false,
                 "Free-flow time / mean travel time of the vehicles that entered."});
    m.push_back({Mode::Delay, "delay", "Delay", "s per vehicle", true, Palette::Congestion,
                 Scale::RobustMax, {}, {}, "No vehicles entered", false, false,
                 "Mean travel time minus the free-flow time, of the vehicles that entered."});
    m.push_back({Mode::Density, "density", "Density", "veh/km/lane", true, Palette::Sequential,
                 Scale::RobustMax, {}, {}, "No vehicles", false, true,
                 "Mean number of vehicles on the link, per km and lane."});
    m.push_back({Mode::VolumeCapacity, "volume_capacity", "Volume / Capacity", "", true,
                 Palette::Congestion, Scale::Fixed, {0.5f, 0.75f, 0.9f, 1.0f},
                 {"0.50 or less", "0.51 to 0.75", "0.76 to 0.90", "0.91 to 1.00",
                  "More than 1.00"},
                 "No vehicles, or no capacity limit", false, true,
                 "Flow / capacity per hour."});
    // A point-queue (vertical queue) estimate: the vehicle-seconds after the
    // free-flow time, divided by the period. MATSim events have no speeds or
    // queue positions, so this is not a queue length.
    m.push_back({Mode::Queue, "delayed_vehicles", "Delayed Vehicles", "veh", true,
                 Palette::Congestion, Scale::RobustMax, {}, {}, "No vehicles", false, true,
                 "Mean number of vehicles on the link that are past their free-flow time "
                 "(an estimate from the events, not a queue length)."});
    return m;
}

// A round number at or above v: 1, 2, 2.5 or 5 times a power of 10
double roundUp(double v) {
    if (!(v > 0.0)) return 1.0;
    const double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double f : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (f * p >= v * 0.9999) return f * p;
    return 10.0 * p;
}

std::string formatNumber(double v) {
    char buf[64];
    if (v < 10.0 && std::abs(v - std::round(v)) > 1e-6) {
        // Two decimals below 1 (0.25), one below 10 (2.5)
        std::snprintf(buf, sizeof buf, v < 1.0 ? "%.2f" : "%.1f", v);
        std::string s = buf;
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
        return s;
    }
    // Whole number with thousands separators
    std::snprintf(buf, sizeof buf, "%.0f", v);
    std::string s = buf, out;
    const int n = static_cast<int>(s.size());
    for (int i = 0; i < n; ++i) {
        out += s[i];
        if ((n - 1 - i) % 3 == 0 && i < n - 1) out += ',';
    }
    return out;
}

double perHour(const LinkRecord& link, const RunFactors& f) {
    return f.capPeriodS > 0 ? link.capacity * 3600.0 / f.capPeriodS : link.capacity;
}

} // namespace

const std::vector<Metric>& metrics() {
    static const std::vector<Metric> all = makeMetrics();
    return all;
}

const Metric& metric(Mode mode) {
    for (const Metric& m : metrics())
        if (m.mode == mode) return m;
    return metrics().front();
}

const Metric* metricByKey(const std::string& key) {
    for (const Metric& m : metrics())
        if (key == m.key) return &m;
    return nullptr;
}

Classes classesFor(const Metric& metric, float robustMax) {
    if (metric.scale == Scale::Fixed) return {metric.limits, metric.labels};
    Classes c;
    const double top = roundUp(robustMax);
    const double step = top / 5.0;
    for (int i = 1; i <= 4; ++i) c.limits.push_back(static_cast<float>(step * i));
    c.labels.push_back(formatNumber(step) + " or less");
    for (int i = 1; i <= 3; ++i)
        c.labels.push_back(formatNumber(step * i) + " to " + formatNumber(step * (i + 1)));
    c.labels.push_back("More than " + formatNumber(step * 4));
    return c;
}

int classOf(const std::vector<float>& limits, float value) {
    if (isNoData(value)) return -1;
    int c = 0;
    for (float limit : limits)
        if (value > limit) ++c;
    return c;
}

float roadLevel(uint8_t roadType) {
    switch (static_cast<RoadType>(roadType)) {
        case RoadType::Motorway: return 4.0f;
        case RoadType::Primary: return 3.0f;     // also trunk
        case RoadType::Secondary: return 2.0f;
        case RoadType::Tertiary: return 1.0f;
        case RoadType::Residential:              // also unclassified, living street
        case RoadType::Service:
        case RoadType::Other: return 0.0f;
        default: return kNoData;                 // Unknown (pt, rail) and air links
    }
}

float modeCategory(uint8_t bits) {
    if (bits == 0) return kNoData;
    if (bits & LINK_MODE_CAR) return 0.0f;
    if (bits & LINK_MODE_TRAM) return 3.0f;
    if (bits & LINK_MODE_RAIL) return 2.0f;
    if (bits & (LINK_MODE_BUS | LINK_MODE_PT)) return 1.0f;
    return 4.0f;
}

std::vector<float> attributeValues(Mode mode, const std::vector<LinkRecord>& links,
                                   uint32_t capPeriodS) {
    std::vector<float> values(links.size(), kNoData);
    // Capacity per hour. Without a period, the value of the file stays.
    const float hourFactor = capPeriodS > 0 ? 3600.0f / static_cast<float>(capPeriodS) : 1.0f;
    for (size_t i = 0; i < links.size(); ++i) {
        const LinkRecord& link = links[i];
        float v = kNoData;
        switch (mode) {
            case Mode::RoadType:
                v = roadLevel(link.roadType);
                break;
            case Mode::Capacity:
                if (std::isfinite(link.capacity) && link.capacity < kNoLimitCapacity)
                    v = link.capacity * hourFactor;
                break;
            case Mode::FreeSpeed:
                if (std::isfinite(link.freespeed) && link.freespeed > 0.0f)
                    v = link.freespeed * 3.6f;
                break;
            case Mode::Lanes:
                v = link.lanes();
                break;
            case Mode::AllowedModes:
                v = modeCategory(link.modes);
                break;
            default:
                break;
        }
        values[i] = v;
    }
    return values;
}

float resultValue(Mode mode, const LinkSums& s, const LinkRecord& link,
                  double windowS, const RunFactors& f) {
    if (s.empty() || !(windowS > 0.0)) return kNoData;
    // From the sample to the full population
    const double scale = f.scale > 0.0 ? f.scale : 1.0;
    const double fft = matsimFreeFlowSeconds(link);
    switch (mode) {
        case Mode::Flow:
            return static_cast<float>(s.enters * 3600.0 / windowS * scale);
        case Mode::SpeedRatio:
            if (s.enters == 0 || !(s.travelS > 0.0) || !(fft > 0.0)) return kNoData;
            return static_cast<float>(std::min(1.0, fft * s.enters / s.travelS));
        case Mode::Delay:
            if (s.enters == 0) return kNoData;
            return static_cast<float>(std::max(0.0, s.travelS / s.enters - fft));
        case Mode::Density: {
            const double km = link.length / 1000.0;
            const double lanes = std::max(1.0f, link.lanes());
            if (!(km > 0.0)) return kNoData;
            return static_cast<float>(s.vehicleS / windowS / (km * lanes) * scale);
        }
        case Mode::VolumeCapacity: {
            if (!(link.capacity > 0.0f) || link.capacity >= kNoLimitCapacity) return kNoData;
            const double flow = s.enters * 3600.0 / windowS * scale;
            return static_cast<float>(flow / perHour(link, f));
        }
        case Mode::Queue:
            return static_cast<float>(s.queueS / windowS * scale);
        default:
            return kNoData;
    }
}

void resultValues(Mode mode, const LinkTimeTable& table, const std::vector<LinkRecord>& links,
                  uint32_t b0, uint32_t b1, const RunFactors& factors, std::vector<float>& out) {
    out.assign(links.size(), kNoData);
    b1 = std::min(b1, table.binCount());
    if (b0 >= b1) return;
    const double windowS = static_cast<double>(b1 - b0) * table.binSeconds();
    if (b1 == b0 + 1) {
        // One bin: each entry is already the sum of its link
        for (const LinkBinEntry& e : table.bin(b0)) {
            if (e.link >= links.size()) continue;
            LinkSums s;
            s.add(e);
            out[e.link] = resultValue(mode, s, links[e.link], windowS, factors);
        }
        return;
    }
    std::vector<LinkSums> sums(links.size());
    std::vector<uint8_t> seen(links.size(), 0);
    for (uint32_t b = b0; b < b1; ++b)
        for (const LinkBinEntry& e : table.bin(b))
            if (e.link < links.size()) { sums[e.link].add(e); seen[e.link] = 1; }
    for (size_t i = 0; i < links.size(); ++i)
        if (seen[i]) out[i] = resultValue(mode, sums[i], links[i], windowS, factors);
}

float robustMax(Mode mode, const LinkTimeTable& table, const std::vector<LinkRecord>& links,
                const RunFactors& factors) {
    std::vector<float> values;
    values.reserve(table.entryCount());
    const double windowS = table.binSeconds();
    for (uint32_t b = 0; b < table.binCount(); ++b) {
        for (const LinkBinEntry& e : table.bin(b)) {
            if (e.link >= links.size()) continue;
            LinkSums s;
            s.add(e);
            const float v = resultValue(mode, s, links[e.link], windowS, factors);
            if (!isNoData(v)) values.push_back(v);
        }
    }
    if (values.empty()) return 0.0f;
    const size_t k = static_cast<size_t>(0.98 * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

} // namespace linkmetrics
} // namespace simvis
