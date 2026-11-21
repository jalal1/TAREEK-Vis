// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "analysis/nkdv_network.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace simvis {

// Stores computed density maps beside the other cache files, so a map is
// computed once per scenario instead of once per session.
//
// A map takes a few seconds to compute and about 1.7 MB to store, and it never
// changes while the scenario and its parameters stay the same. All seven
// activity types of a metro-scale scenario come to under 7 MB together, which
// is small next to the .vidx beside them.
//
// One file per (activity type, parameters). The parameters are part of the file
// name, so changing the bandwidth or lixel size simply misses the cache rather
// than returning a map computed for different settings.
class HeatmapCache {
public:
    // Identifies one computed map. Everything that changes the result belongs
    // here, so that a cache hit is always a map for exactly these settings.
    struct Key {
        QString activityType;
        int lixelLength = 25;
        double bandwidth = 500.0;

        // File name for this key, without a directory.
        QString fileName() const;
    };

    explicit HeatmapCache(const QString& cacheDir) : cacheDir_(cacheDir) {}

    // Load a cached map. Returns false when there is no valid entry, including
    // when the entry is older than `sourceModified`, which is the time the
    // events cache was written: a re-preprocess invalidates every map.
    bool load(const Key& key, const QDateTime& sourceModified,
              std::vector<NkdvNetwork::Lixel>& out) const;

    // Store a computed map. Failure is not fatal: the map is still usable, it
    // simply has to be recomputed next time, so callers log and carry on.
    bool store(const Key& key, const std::vector<NkdvNetwork::Lixel>& lixels) const;

    // True when a valid entry exists, without reading the whole file.
    bool contains(const Key& key, const QDateTime& sourceModified) const;

    // Highest density in one stored map, read from its header without loading
    // the lixels. Returns 0 when there is no valid entry.
    float peakOf(const Key& key, const QDateTime& sourceModified) const;

    // Highest density across every stored map for these settings, so the
    // shared color scale does not depend on which types the user happened to
    // open. Reads headers only, so it costs one small read per cached type.
    float peakAcrossAll(const QStringList& activityTypes, int lixelLength,
                        double bandwidth,
                        const QDateTime& sourceModified) const;

private:
    QString pathFor(const Key& key) const;

    QString cacheDir_;
};

} // namespace simvis
