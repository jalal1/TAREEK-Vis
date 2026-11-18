#pragma once

#include "heatmap_palette.h"
#include <array>
#include <vector>

namespace simvis {
namespace linkpalette {

// Class colors for the link color modes (docs/link-colors.md, decision 5).
// The colored links get a thin dark edge (NetworkRenderer), so that a light
// color reads on the light OSM tiles and a dark color on the dark background.

using heatmap::Rgb;

// A magnitude (capacity, free speed, lanes; later flow and density): viridis.
// It has the same properties as the heatmap ramp (perceptually uniform,
// lightness rises with the value, safe for color blindness), but other hues.
// The heatmap and the link colors can be on at the same time, and with one
// ramp for both the reader cannot tell which layer a color belongs to.
// Control points: matplotlib viridis at t = 0, 1/8, ..., 1.
inline constexpr std::array<Rgb, 9> kViridis = {{
    {0.267f, 0.005f, 0.329f},
    {0.279f, 0.175f, 0.483f},
    {0.230f, 0.322f, 0.546f},
    {0.173f, 0.449f, 0.558f},
    {0.128f, 0.567f, 0.551f},
    {0.158f, 0.684f, 0.502f},
    {0.369f, 0.789f, 0.383f},
    {0.678f, 0.864f, 0.190f},
    {0.993f, 0.906f, 0.144f},
}};

inline Rgb viridis(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(kViridis.size() - 1);
    const int lower = std::min(static_cast<int>(scaled), static_cast<int>(kViridis.size()) - 2);
    const float frac = scaled - static_cast<float>(lower);
    const Rgb& a = kViridis[static_cast<size_t>(lower)];
    const Rgb& b = kViridis[static_cast<size_t>(lower) + 1];
    return {a.r + (b.r - a.r) * frac, a.g + (b.g - a.g) * frac, a.b + (b.b - a.b) * frac};
}

// The classes of a magnitude. The ramp starts at 0.15, not 0: its dark start
// is hard to see on the dark background, and "low" must not look like
// "no data".
inline std::vector<Rgb> sequential(int classes) {
    std::vector<Rgb> colors;
    for (int i = 0; i < classes; ++i) {
        const float t = classes > 1 ? static_cast<float>(i) / static_cast<float>(classes - 1) : 1.0f;
        colors.push_back(viridis(0.15f + 0.85f * t));
    }
    return colors;
}

// Congestion (speed ratio, delay, v/c), from good to bad: ColorBrewer RdYlBu,
// 5 classes, reversed. It is safe for the common types of color blindness.
// The vehicles use the same classes.
inline constexpr std::array<Rgb, 5> kCongestion = {{
    {0x2c / 255.0f, 0x7b / 255.0f, 0xb6 / 255.0f},  // blue: free flow
    {0xab / 255.0f, 0xd9 / 255.0f, 0xe9 / 255.0f},  // light blue
    {0xff / 255.0f, 0xff / 255.0f, 0xbf / 255.0f},  // pale yellow
    {0xfd / 255.0f, 0xae / 255.0f, 0x61 / 255.0f},  // orange
    {0xd7 / 255.0f, 0x19 / 255.0f, 0x1c / 255.0f},  // red: congested
}};

// Categories (allowed modes): the Okabe-Ito colors, which stay distinct for
// the common types of color blindness. In the order of the categories.
inline constexpr std::array<Rgb, 5> kCategorical = {{
    {0x56 / 255.0f, 0xb4 / 255.0f, 0xe9 / 255.0f},  // sky blue
    {0xe6 / 255.0f, 0x9f / 255.0f, 0x00 / 255.0f},  // orange
    {0xcc / 255.0f, 0x79 / 255.0f, 0xa7 / 255.0f},  // reddish purple
    {0x00 / 255.0f, 0x9e / 255.0f, 0x73 / 255.0f},  // bluish green
    {0xf0 / 255.0f, 0xe4 / 255.0f, 0x42 / 255.0f},  // yellow
}};

// Road Type (the base map): no hue per road type, five gray levels and
// widths, from minor roads (level 0) to motorways (level 4), as in common web
// maps. The important roads stand out; the many minor roads recede. On the
// dark map and on satellite images the important roads are lighter; on the
// light tiles (OSM, topo) they are darker. Links with no road type (pt, rail)
// get the no-data style. Air links keep their own colors.
inline constexpr std::array<float, 5> kRoadGrayDark = {0.33f, 0.45f, 0.58f, 0.72f, 0.88f};
inline constexpr std::array<float, 5> kRoadGrayLight = {0.60f, 0.50f, 0.40f, 0.28f, 0.15f};
// Half width per level, screen pixels: 1.2 px (minor) to 3.2 px (motorway)
inline constexpr std::array<float, 5> kRoadHalfWidthPx = {0.6f, 0.75f, 1.0f, 1.3f, 1.6f};

inline std::vector<Rgb> roadGrays(bool lightBackground) {
    const auto& g = lightBackground ? kRoadGrayLight : kRoadGrayDark;
    std::vector<Rgb> colors;
    for (float v : g) colors.push_back({v, v, v});
    return colors;
}

// A link with no data: dim and neutral, as the base network
inline constexpr Rgb kNoData = {0.55f, 0.55f, 0.55f};
inline constexpr float kNoDataAlpha = 0.45f;

// The edge of a colored link
inline constexpr Rgb kEdge = {0.08f, 0.08f, 0.10f};
inline constexpr float kEdgeAlpha = 0.85f;

} // namespace linkpalette
} // namespace simvis
