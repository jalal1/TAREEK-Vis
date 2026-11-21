// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include "analysis/link_time_table.h"
#include <string>
#include <vector>

namespace simvis {
namespace linkmetrics {

// ============================================================================
// The link color modes (roadmap item 9). This is the one definition of each
// mode: its name, unit, formula, scale rule and palette. The renderer, the
// legend, the ribbon, the info panel chart and the help page read from here.
// The file has no Qt GUI and no OpenGL, so tests and tareek-bench can use it.
// See docs/link-colors.md for the reasons behind the classes.
// ============================================================================

enum class Mode : uint8_t {
    RoadType,       // the default: 5 road levels in grays (no legend on the map)
    // Network attributes
    Capacity,
    FreeSpeed,
    Lanes,
    AllowedModes,
    // Simulation results (they follow the timeline)
    Flow,
    SpeedRatio,
    Delay,
    Density,
    VolumeCapacity,
    Queue,
};

// How the classes get their colors (see renderer/link_palette.h)
enum class Palette : uint8_t {
    RoadType,       // grays per road level, set by the background map
    Sequential,     // a magnitude: one ramp, dim to bright
    Congestion,     // good (blue) to bad (red)
    Categorical,    // a few distinct colors, no order
};

// How the class limits are found
enum class Scale : uint8_t {
    Fixed,          // the limits of the metric
    RobustMax,      // 5 equal classes up to the 98th percentile over the day,
                    // rounded to a round number, fixed for the whole run
};

// The value of a link that has no data. It is drawn thin and neutral, not
// with the color of the lowest class.
constexpr float kNoData = -3.0e38f;
inline bool isNoData(float value) { return !(value > -1.0e38f); }

struct Metric {
    Mode mode;
    const char* key;         // stable name for scenes and views, e.g. "capacity"
    const char* name;        // shown in the menu and the legend, e.g. "Capacity"
    const char* unit;        // e.g. "veh/h"; "" for a category
    bool timeDependent;      // a simulation result that follows the timeline
    Palette palette;
    Scale scale;
    // Scale::Fixed: class i holds the values v with limits[i-1] < v <= limits[i].
    // There is one class more than limits. Empty for RobustMax.
    std::vector<float> limits;
    std::vector<std::string> labels;  // one per class, for the legend
    const char* noDataLabel;  // legend text for the kNoData links; "" = not shown
    // True when class 0 is the worst class (for example speed / free speed).
    // The worst links are drawn last, so they are on top.
    bool lowIsWorst;
    // A result scaled from the simulated sample to the full population
    // (RunFactors::scale: flow, density, v/c, delayed vehicles)
    bool sampleScaled;
    const char* description;  // one sentence: what the value is
};

// Every mode, in menu order
const std::vector<Metric>& metrics();
const Metric& metric(Mode mode);
// nullptr when no mode has this key
const Metric* metricByKey(const std::string& key);

// The classes of a metric: its fixed limits, or (Scale::RobustMax) 5 classes
// up to a round number at or above robustMax
struct Classes {
    std::vector<float> limits;
    std::vector<std::string> labels;
};
Classes classesFor(const Metric& metric, float robustMax);

// The class of a value: 0 .. limits.size(), or -1 for kNoData
int classOf(const std::vector<float>& limits, float value);

// The per-link values of an attribute mode, in the unit of the metric,
// indexed like `links`. kNoData where the link has no value. capPeriodS is the
// capacity period of the network (NetworkIndex::capPeriodS); with 0 the
// capacity stays in the unit of the file, per an unknown period.
std::vector<float> attributeValues(Mode mode, const std::vector<LinkRecord>& links,
                                   uint32_t capPeriodS);

// Road level of a road type, for Mode::RoadType: 0 minor (residential,
// service, other), 1 tertiary, 2 secondary, 3 primary and trunk, 4 motorway.
// kNoData for an unknown road type (pt and rail links) and for air links.
float roadLevel(uint8_t roadType);

// Category of the allowed modes (LINK_MODE_* bits), for Mode::AllowedModes:
// 0 car, 1 bus or pt only, 2 rail or train, 3 tram or light rail, 4 other.
// kNoData when the link has no modes attribute.
float modeCategory(uint8_t modeBits);

// What the result metrics need from the run
struct RunFactors {
    // Multiplier from the simulated sample to the full population, as the
    // user entered it for the run (10 for a 10 % sample). 0 = not known: the
    // values stay those of the sample.
    double scale = 0.0;
    std::string scaleSource;   // for the legend
    // From the network. 0 = not known: the capacity is taken as per hour.
    uint32_t capPeriodS = 0;
};

// The value of a result metric for one link, from its sums over a window of
// windowS seconds. kNoData when the sums have no vehicle, or when the metric
// needs a vehicle that entered in the window (speed, delay).
float resultValue(Mode mode, const LinkSums& sums, const LinkRecord& link,
                  double windowS, const RunFactors& factors);

// resultValue() for every link over the bins [b0, b1) of the table, indexed
// like `links`. kNoData where there is no value.
void resultValues(Mode mode, const LinkTimeTable& table, const std::vector<LinkRecord>& links,
                  uint32_t b0, uint32_t b1, const RunFactors& factors, std::vector<float>& out);

// The 98th percentile of a result metric over all (link, bin) values of the
// table: the top of a Scale::RobustMax scale. 0 when there is no value.
float robustMax(Mode mode, const LinkTimeTable& table, const std::vector<LinkRecord>& links,
                const RunFactors& factors);

} // namespace linkmetrics
} // namespace simvis
