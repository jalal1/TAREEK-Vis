#include "tile_renderer.h"
#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QStandardPaths>
#include <QSet>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace simvis {

// Vertex shader: position + texcoord
static const char* tileVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vTexCoord;
void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* tileFragmentShader = R"(
#version 330 core
in vec2 vTexCoord;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vTexCoord);
}
)";

TileRenderer::TileRenderer(QObject* parent)
    : QObject(parent)
{
    clock_.start();
}

TileRenderer::~TileRenderer() {
    cleanup();
}

bool TileRenderer::initialize() {
    if (!GLRenderer::initialize()) return false;

    shaderProgram_ = compileShader(tileVertexShader, tileFragmentShader);
    if (!shaderProgram_) return false;

    mvpLoc_ = glGetUniformLocation(shaderProgram_, "uMVP");
    texLoc_ = glGetUniformLocation(shaderProgram_, "uTex");

    // Create VAO/VBO for tile quads (will be filled per-tile during render)
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // 4 vertices * (2 pos + 2 texcoord) = 16 floats per quad
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // Network manager for tile downloads. The disk cache keeps tiles between
    // runs: without one, PreferCache has nothing to read and every start
    // downloads the whole view again. The OSM tile policy also asks clients to
    // cache.
    netManager_ = new QNetworkAccessManager(this);
    auto* diskCache = new QNetworkDiskCache(netManager_);
    diskCache->setCacheDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tiles");
    diskCache->setMaximumCacheSize(300LL * 1024 * 1024);
    netManager_->setCache(diskCache);
    // A stalled request would hold a download slot for good.
    netManager_->setTransferTimeout(15000);
    connect(netManager_, &QNetworkAccessManager::finished,
            this, &TileRenderer::onTileDownloaded);

    return true;
}

void TileRenderer::clearTiles() {
    for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
        glDeleteTextures(1, &it.value().texture);
    }
    tiles_.clear();
    pendingImages_.clear();
    failures_.clear();
    // abort() can call onTileDownloaded at once, which edits inFlight_, so
    // empty it before cancelling.
    const auto replies = std::exchange(inFlight_, {});
    for (const auto& reply : replies) {
        if (reply) {
            reply->setProperty("cancelled", true);
            reply->abort();
        }
    }
}

void TileRenderer::cleanup() {
    if (!initialized_) return;

    clearTiles();

    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (shaderProgram_) { glDeleteProgram(shaderProgram_); shaderProgram_ = 0; }

    GLRenderer::cleanup();
}

void TileRenderer::setEnabled(bool enabled) {
    enabled_ = enabled;
}

void TileRenderer::setTileSource(TileSource source) {
    if (source_ == source) return;
    source_ = source;
    ++sourceGeneration_;

    // Clear cache when switching sources
    if (initialized_) clearTiles();
}

int TileRenderer::calculateZoomLevel() const {
    // Get viewport bounds in network coordinates
    BoundingBox vb = getViewportBounds();
    if (vb.width() <= 0) return 12;

    // Convert corners to WGS84
    auto [lonMin, latMin] = crs::toWgs84(vb.minX, vb.minY, crs_);
    auto [lonMax, latMax] = crs::toWgs84(vb.maxX, vb.maxY, crs_);

    double lonSpan = std::abs(lonMax - lonMin);
    if (lonSpan <= 0) return 12;

    // At zoom z, the world is 2^z tiles wide, each covering 360/2^z degrees
    // We want roughly viewport_width/256 tiles across
    double tilesAcross = std::max(1.0, viewportWidth_ / 256.0);
    int z = static_cast<int>(std::log2(360.0 / lonSpan * tilesAcross));
    return std::clamp(z, 1, 19);
}

GLuint TileRenderer::useTile(const TileKey& key) {
    auto it = tiles_.find(key);
    if (it == tiles_.end()) return 0;
    it.value().lastUsedFrame = frame_;
    return it.value().texture;
}

void TileRenderer::drawQuad(GLuint tex, float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1) {
    // Triangle strip; (x0,y0) is bottom-left on the map, (u0,v0) the top-left
    // of the image, since tile images have their origin at the top.
    const float quad[16] = {
        x0, y0, u0, v1,  // bottom-left
        x1, y0, u1, v1,  // bottom-right
        x0, y1, u0, v0,  // top-left
        x1, y1, u1, v0,  // top-right
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void TileRenderer::render() {
    if (!enabled_ || !initialized_ || !crs_.isValid()) return;
    ++frame_;

    // Upload any pending images (GL context is current during render)
    if (!pendingImages_.isEmpty()) {
        for (auto it = pendingImages_.begin(); it != pendingImages_.end(); ++it) {
            GLuint tex = uploadTexture(it.value());
            if (tex) {
                evictOldTiles();
                tiles_.insert(it.key(), CachedTile{tex, frame_});
            }
        }
        pendingImages_.clear();
    }

    // Get viewport bounds in network coordinates
    BoundingBox vb = getViewportBounds();
    if (vb.width() <= 0 || vb.height() <= 0) return;

    // Convert viewport corners to WGS84
    auto [lonMin, latMin] = crs::toWgs84(vb.minX, vb.minY, crs_);
    auto [lonMax, latMax] = crs::toWgs84(vb.maxX, vb.maxY, crs_);

    // Ensure correct ordering
    if (lonMin > lonMax) std::swap(lonMin, lonMax);
    if (latMin > latMax) std::swap(latMin, latMax);

    // Get tile range covering the viewport, with a 1-tile margin. A very large
    // viewport (a high-resolution export) would need too many tiles at its own
    // zoom; step down until the count is sane rather than drawing nothing.
    int z = calculateZoomLevel();
    int txMin, txMax, tyMin, tyMax;
    for (;;) {
        auto [ax, ayMax] = crs::lonLatToTile(lonMin, latMin, z); // lat min -> tile y max
        auto [bx, byMin] = crs::lonLatToTile(lonMax, latMax, z);
        const int n = 1 << z;
        txMin = std::max(0, ax - 1);
        tyMin = std::max(0, byMin - 1);
        txMax = std::min(n - 1, bx + 1);
        tyMax = std::min(n - 1, ayMax + 1);
        const int tileCount = (txMax - txMin + 1) * (tyMax - tyMin + 1);
        if (tileCount <= 100 || z <= 1) break;
        --z;
    }

    // Set up GL state
    QMatrix4x4 mvp = projectionMatrix_ * viewMatrix_;

    glUseProgram(shaderProgram_);
    glUniformMatrix4fv(mvpLoc_, 1, GL_FALSE, mvp.constData());
    glUniform1i(texLoc_, 0);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);

    // Pre-compute CRS positions for tile grid edges to ensure adjacent tiles
    // share exact vertex positions (eliminates seams/black lines).
    // tileEdgeX[i] = CRS x for tile column (txMin + i), tileEdgeY[j] = CRS y for tile row (tyMin + j)
    int cols = txMax - txMin + 1;
    int rows = tyMax - tyMin + 1;
    std::vector<double> tileEdgeX(cols + 1);
    std::vector<double> tileEdgeY(rows + 1);

    int numTiles = 1 << z;
    for (int i = 0; i <= cols; ++i) {
        double lon = static_cast<double>(txMin + i) / numTiles * 360.0 - 180.0;
        // Use the center latitude of the viewport for x conversion (lon varies, lat is reference)
        double centerLat = (latMin + latMax) * 0.5;
        auto [ex, ey] = crs::fromWgs84(lon, centerLat, crs_);
        tileEdgeX[i] = ex;
    }
    for (int j = 0; j <= rows; ++j) {
        // Tile row tyMin+j: top edge of that tile row
        // In OSM, tile y increases downward (north to south), so tile row ty has:
        //   top edge at ty, bottom edge at ty+1
        double latRad = std::atan(std::sinh(crs::PI * (1.0 - 2.0 * static_cast<double>(tyMin + j) / numTiles)));
        double lat = latRad * crs::RAD2DEG;
        double centerLon = (lonMin + lonMax) * 0.5;
        auto [ex, ey] = crs::fromWgs84(centerLon, lat, crs_);
        tileEdgeY[j] = ey;
    }

    // Tile coordinates of the view center, for download order.
    auto [cxTile, cyTile] = crs::lonLatToTile((lonMin + lonMax) * 0.5,
                                              (latMin + latMax) * 0.5, z);

    std::vector<WantedTile> wanted;
    // Coarse tiles asked for this frame, so one is not queued once per child.
    QSet<TileKey> wantedCoarse;

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        int jRow = ty - tyMin;
        for (int tx = txMin; tx <= txMax; ++tx) {
            int iCol = tx - txMin;
            TileKey key{ tx, ty, z };

            // Tile edges from pre-computed grid (no per-tile CRS conversion = no seams)
            // tileEdgeY[jRow] = top (higher lat = higher northing)
            // tileEdgeY[jRow+1] = bottom (lower lat = lower northing)
            const float x0 = (float)tileEdgeX[iCol];
            const float x1 = (float)tileEdgeX[iCol + 1];
            const float yTop = (float)tileEdgeY[jRow];
            const float yBot = (float)tileEdgeY[jRow + 1];

            if (GLuint tex = useTile(key)) {
                drawQuad(tex, x0, yBot, x1, yTop, 0.0f, 0.0f, 1.0f, 1.0f);
                continue;
            }

            const double dx = tx + 0.5 - cxTile;
            const double dy = ty + 0.5 - cyTile;
            wanted.push_back({key, dx * dx + dy * dy});

            // Not here yet: stand in with the nearest coarser tile we have,
            // cropped to this tile's part of it. Blurry for a moment, but not
            // a black hole.
            bool covered = false;
            for (int d = 1; d <= 6 && z - d >= 0; ++d) {
                TileKey parent{ tx >> d, ty >> d, z - d };
                if (GLuint tex = useTile(parent)) {
                    const float s = 1.0f / static_cast<float>(1 << d);
                    const float u0 = (tx & ((1 << d) - 1)) * s;
                    const float v0 = (ty & ((1 << d) - 1)) * s;
                    drawQuad(tex, x0, yBot, x1, yTop, u0, v0, u0 + s, v0 + s);
                    covered = true;
                    break;
                }
            }
            // Nothing coarse on hand (first load, or a jump far away): ask for
            // the tile three levels up first. It is one small download that
            // covers 64 of these tiles.
            if (!covered && z >= 4) {
                TileKey coarse{ tx >> 3, ty >> 3, z - 3 };
                if (!wantedCoarse.contains(coarse)) {
                    wantedCoarse.insert(coarse);
                    wanted.push_back({coarse, -1.0});
                }
            }

            // After zooming out, the finer tiles of the last view are still
            // cached and are sharper than any parent; draw what we have.
            if (z + 1 <= 19) {
                const float xm = 0.5f * (x0 + x1);
                const float ym = 0.5f * (yTop + yBot);
                for (int j = 0; j < 2; ++j) {
                    for (int i = 0; i < 2; ++i) {
                        GLuint tex = useTile(TileKey{ 2 * tx + i, 2 * ty + j, z + 1 });
                        if (!tex) continue;
                        const float cx0 = i == 0 ? x0 : xm;
                        const float cx1 = i == 0 ? xm : x1;
                        const float cyTop = j == 0 ? yTop : ym;
                        const float cyBot = j == 0 ? ym : yBot;
                        drawQuad(tex, cx0, cyBot, cx1, cyTop, 0.0f, 0.0f, 1.0f, 1.0f);
                    }
                }
            }
        }
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glUseProgram(0);

    scheduleDownloads(wanted);
}

void TileRenderer::scheduleDownloads(std::vector<WantedTile>& wanted) {
    QSet<TileKey> wantedKeys;
    for (const auto& w : wanted) wantedKeys.insert(w.key);

    // Cancel downloads the view has moved away from, so they do not hold the
    // slots the new view needs. A tile already decoded is kept.
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        if (!wantedKeys.contains(it.key())) {
            QPointer<QNetworkReply> reply = it.value();
            it = inFlight_.erase(it);
            if (reply) {
                reply->setProperty("cancelled", true);
                reply->abort();
            }
        } else {
            ++it;
        }
    }

    std::sort(wanted.begin(), wanted.end(),
              [](const WantedTile& a, const WantedTile& b) { return a.priority < b.priority; });

    const qint64 now = clock_.elapsed();
    for (const auto& w : wanted) {
        if (inFlight_.size() >= MAX_IN_FLIGHT) break;
        if (inFlight_.contains(w.key) || pendingImages_.contains(w.key)) continue;
        auto f = failures_.constFind(w.key);
        if (f != failures_.constEnd() && now < f->retryAtMs) continue;
        startDownload(w.key);
    }
}

void TileRenderer::startDownload(const TileKey& key) {
    QNetworkRequest request;
    request.setUrl(QUrl(tileUrl(key)));
    // The OSM tile usage policy requires a User-Agent that identifies the app
    request.setRawHeader("User-Agent",
                         "TAREEK-Vis/" TAREEK_VIS_VERSION " (+https://github.com/jalal1/TAREEK-Vis)");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferCache);

    QNetworkReply* reply = netManager_->get(request);
    reply->setProperty("tileX", key.x);
    reply->setProperty("tileY", key.y);
    reply->setProperty("tileZ", key.z);
    reply->setProperty("tileGen", sourceGeneration_);
    inFlight_.insert(key, reply);
}

void TileRenderer::onTileDownloaded(QNetworkReply* reply) {
    reply->deleteLater();

    TileKey key;
    key.x = reply->property("tileX").toInt();
    key.y = reply->property("tileY").toInt();
    key.z = reply->property("tileZ").toInt();

    // Only forget the slot if it is still this reply's: a cancelled request
    // may finish after a new one for the same tile has started.
    auto it = inFlight_.find(key);
    if (it != inFlight_.end() && it.value() == reply) inFlight_.erase(it);

    // A tile of the source we just switched away from.
    if (reply->property("tileGen").toInt() != sourceGeneration_) return;

    // We cancelled it because it left the view. Qt reports a transfer
    // time-out with the same error; that one counts as a failure below.
    if (reply->error() == QNetworkReply::OperationCanceledError &&
        reply->property("cancelled").toBool()) {
        emit tileLoaded();  // let the next frame fill the freed slot
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Tile download failed:" << reply->url() << reply->errorString();
        // Wait 2 s, then 4, 8 ... up to a minute before asking again.
        Failure& f = failures_[key];
        f.attempts = std::min(f.attempts + 1, 5);
        f.retryAtMs = clock_.elapsed() + (1000LL << f.attempts);
        emit tileLoaded();
        return;
    }

    QImage image;
    if (!image.loadFromData(reply->readAll())) {
        qDebug() << "Failed to decode tile image:" << reply->url();
        Failure& f = failures_[key];
        f.attempts = 5;
        f.retryAtMs = clock_.elapsed() + 60000;
        return;
    }
    failures_.remove(key);

    // Store image for GL upload during next render() when context is current
    // Do NOT mirror - the UV coords already account for OpenGL's bottom-up convention
    pendingImages_.insert(key, image.convertToFormat(QImage::Format_RGBA8888));
    emit tileLoaded();
}

GLuint TileRenderer::uploadTexture(const QImage& image) {
    // Called from render() where GL context is current
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

void TileRenderer::evictOldTiles() {
    // Drop the tile drawn longest ago. Tiles drawn in the last frame are on
    // screen now and are never dropped; the cache grows past its limit rather
    // than make a visible tile flash.
    while (tiles_.size() >= MAX_CACHED_TILES) {
        auto oldest = tiles_.end();
        for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
            if (oldest == tiles_.end() || it.value().lastUsedFrame < oldest.value().lastUsedFrame)
                oldest = it;
        }
        if (oldest == tiles_.end() || oldest.value().lastUsedFrame + 1 >= frame_) return;
        glDeleteTextures(1, &oldest.value().texture);
        tiles_.erase(oldest);
    }
}

QString TileRenderer::tileUrl(const TileKey& key) const {
    switch (source_) {
    case TileSource::OpenStreetMap:
        // OSM Standard tiles: free, no API key. Carto now serves an
        // "API key required" placeholder to clients without a key.
        return QString("https://tile.openstreetmap.org/%1/%2/%3.png")
            .arg(key.z).arg(key.x).arg(key.y);
    case TileSource::Satellite:
        return QString("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%1/%2/%3")
            .arg(key.z).arg(key.y).arg(key.x);
    case TileSource::Topo:
        return QString("https://tile.opentopomap.org/%1/%2/%3.png")
            .arg(key.z).arg(key.x).arg(key.y);
    }
    return {};
}

} // namespace simvis
