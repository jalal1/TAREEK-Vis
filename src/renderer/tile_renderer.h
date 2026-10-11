#pragma once

#include "gl_renderer.h"
#include "core/crs_transform.h"
#include <QObject>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QPointer>
#include <vector>

namespace simvis {

enum class TileSource { OpenStreetMap, Satellite, Topo };

struct TileKey {
    int x, y, z;
    bool operator==(const TileKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

inline size_t qHash(const TileKey& k, size_t seed = 0) {
    return ::qHash(k.x, seed) ^ ::qHash(k.y, seed + 1) ^ ::qHash(k.z, seed + 2);
}

// Renders slippy map tiles (OSM, satellite, etc.) as OpenGL textured quads.
//
// A tile that is not downloaded yet is drawn from a coarser tile that is (or
// from finer ones, after zooming out), so the map never shows black holes while
// it loads. Downloads go nearest-the-center first, requests for tiles that left
// the view are cancelled, and responses are kept in a disk cache between runs.
class TileRenderer : public QObject, public GLRenderer {
    Q_OBJECT

public:
    explicit TileRenderer(QObject* parent = nullptr);
    ~TileRenderer() override;

    bool initialize() override;
    void cleanup() override;
    void render();

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    void setTileSource(TileSource source);
    TileSource tileSource() const { return source_; }

    void setCRS(const CRSInfo& crs) { crs_ = crs; }
    const CRSInfo& crsInfo() const { return crs_; }

signals:
    void tileLoaded();  // emitted when a tile finishes downloading (triggers repaint)

private slots:
    void onTileDownloaded(QNetworkReply* reply);

private:
    struct CachedTile {
        GLuint texture = 0;
        uint64_t lastUsedFrame = 0;
    };

    // A tile the current view needs but does not have, and how far its center
    // is from the view center (smaller = sooner).
    struct WantedTile {
        TileKey key;
        double priority;
    };

    // Texture for a tile, marked as used this frame. 0 if not cached.
    GLuint useTile(const TileKey& key);
    void drawQuad(GLuint tex, float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1);

    // Start downloads for the most urgent wanted tiles and cancel the ones the
    // view no longer needs.
    void scheduleDownloads(std::vector<WantedTile>& wanted);
    void startDownload(const TileKey& key);

    GLuint uploadTexture(const QImage& image);
    void evictOldTiles();
    void clearTiles();
    QString tileUrl(const TileKey& key) const;
    int calculateZoomLevel() const;

    bool enabled_ = false;
    TileSource source_ = TileSource::OpenStreetMap;
    // Raised on every source change; a reply from an older source is dropped.
    int sourceGeneration_ = 0;
    CRSInfo crs_;

    // GL resources
    GLuint shaderProgram_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLint mvpLoc_ = -1;
    GLint texLoc_ = -1;

    // Texture cache, evicted least-recently-drawn first. ~256 KB of GPU memory
    // per tile.
    QHash<TileKey, CachedTile> tiles_;
    uint64_t frame_ = 0;
    static constexpr int MAX_CACHED_TILES = 400;

    // Downloads in flight, and images waiting for GL upload in render().
    QHash<TileKey, QPointer<QNetworkReply>> inFlight_;
    QHash<TileKey, QImage> pendingImages_;
    static constexpr int MAX_IN_FLIGHT = 6;

    // A failed tile is not asked for again until its time comes, so a server
    // that is down or rate-limiting is not hit on every frame.
    struct Failure { qint64 retryAtMs; int attempts; };
    QHash<TileKey, Failure> failures_;
    QElapsedTimer clock_;

    QNetworkAccessManager* netManager_ = nullptr;
};

} // namespace simvis
