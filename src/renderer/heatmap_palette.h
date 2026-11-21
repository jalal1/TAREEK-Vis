// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace simvis {
namespace heatmap {

// Sequential color ramp for network density.
//
// Density is a magnitude, so the ramp is one hue family running light to dark
// in a single direction. It is NOT categorical: neighbouring steps are meant to
// look similar, because neighbouring values are similar.
//
// The map background is dark, so the ramp is inverted from the usual
// light-to-dark: low density is a dim blue that recedes into the base network,
// and high density is a bright yellow that stands out. Lightness rises
// monotonically with density, which is the property that makes the ramp
// readable, including for colorblind viewers. Hue moves blue -> magenta ->
// orange -> yellow, so the steps also separate by hue and never rely on hue
// alone.
//
// This is the "inferno" ordering, which is perceptually uniform: equal steps in
// density look like equal steps in color.
struct Rgb { float r, g, b; };

// Ramp control points, dark to bright.
inline constexpr std::array<Rgb, 8> kRampStops = {{
    {0.020f, 0.028f, 0.140f},  // near-black blue: effectively no density
    {0.176f, 0.043f, 0.373f},  // deep indigo
    {0.373f, 0.075f, 0.431f},  // purple
    {0.573f, 0.145f, 0.396f},  // magenta
    {0.757f, 0.239f, 0.290f},  // red
    {0.890f, 0.412f, 0.145f},  // orange
    {0.961f, 0.643f, 0.071f},  // amber
    {0.988f, 0.906f, 0.145f},  // yellow: peak density
}};

// Map a normalized density in [0, 1] to a color on the ramp.
inline Rgb sample(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(kRampStops.size() - 1);
    const int lower = static_cast<int>(scaled);
    const int upper = std::min(lower + 1, static_cast<int>(kRampStops.size()) - 1);
    const float frac = scaled - static_cast<float>(lower);

    const Rgb& a = kRampStops[static_cast<size_t>(lower)];
    const Rgb& b = kRampStops[static_cast<size_t>(upper)];
    return {a.r + (b.r - a.r) * frac,
            a.g + (b.g - a.g) * frac,
            a.b + (b.b - a.b) * frac};
}

// Density spans orders of magnitude: a few peaks tower over a long tail, so a
// linear scale paints almost the whole network with the darkest step and hides
// the structure. A logarithmic transform spreads the low end out.
//
// Returns a value in [0, 1] for a density, given the maximum in the map.
inline float normalize(float value, float maxValue) {
    if (!(maxValue > 0.0f) || !(value > 0.0f)) return 0.0f;
    // log1p keeps zero at zero and needs no epsilon. The scale factor sets how
    // much the low end is stretched; 1000 suits densities normalized by point
    // count, which land well below 1.
    constexpr float kScale = 1000.0f;
    const float num = std::log1p(kScale * (value / maxValue));
    const float den = std::log1p(kScale);
    return std::clamp(num / den, 0.0f, 1.0f);
}

} // namespace heatmap
} // namespace simvis
