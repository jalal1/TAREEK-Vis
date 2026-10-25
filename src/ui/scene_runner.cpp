#include "scene_runner.h"
#include "main_window.h"
#include "core/logger.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cstdio>

namespace simvis {

namespace {

void fail(const QString& message) {
    fprintf(stderr, "scenes: %s\n", qUtf8Printable(message));
    fflush(stderr);
}

// Median, 95th percentile, mean and maximum of a list of times, in ms
QJsonObject summary(std::vector<double> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (double v : values) sum += v;
    auto at = [&](double q) {
        return values[std::min(values.size() - 1,
                               static_cast<size_t>(q * (values.size() - 1) + 0.5))];
    };
    return QJsonObject{
        {"median", at(0.5)},
        {"p95", at(0.95)},
        {"mean", sum / values.size()},
        {"max", values.back()},
    };
}

} // anonymous namespace

SceneRunner::SceneRunner(MainWindow* window, const QString& scenesFile,
                         const QString& runName, const QString& outDir,
                         QObject* parent)
    : QObject(parent), window_(window), scenesFile_(scenesFile),
      runName_(runName), outDir_(outDir) {}

bool SceneRunner::prepare() {
    QFile file(scenesFile_);
    if (!file.open(QIODevice::ReadOnly)) {
        fail("cannot read " + scenesFile_);
        return false;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (!doc.isObject()) {
        fail(scenesFile_ + ": " + error.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    frames_ = root.value("frames").toInt(frames_);
    step_ = root.value("step_s").toDouble(step_);

    for (const QJsonValue& v : root.value("scenes").toArray()) {
        if (v.toObject().value("run").toString() == runName_) scenes_.append(v);
    }
    if (scenes_.isEmpty()) {
        fail("no scene for run " + runName_ + " in " + scenesFile_);
        return false;
    }

    // A fixed map size, so that every image and every frame time is for
    // the same number of pixels
    const QJsonArray size = root.value("map_size").toArray();
    const int w = size.size() == 2 ? size[0].toInt() : 1280;
    const int h = size.size() == 2 ? size[1].toInt() : 800;
    window_->mapWidget()->setFixedSize(w, h);

    QDir().mkpath(outDir_);
    window_->setCaptureMode(true);
    connect(window_, &MainWindow::runLoaded, this, [this]() {
        // Let the window paint once before the first scene
        QTimer::singleShot(500, this, &SceneRunner::runAll);
    }, Qt::SingleShotConnection);
    return true;
}

void SceneRunner::runAll() {
    for (const QJsonValue& v : scenes_) runScene(v.toObject());
    QApplication::exit(0);
}

bool SceneRunner::applyActions(const QJsonObject& actions) {
    const QList<QAction*> all = window_->findChildren<QAction*>();
    bool ok = true;
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        QAction* found = nullptr;
        for (QAction* a : all) {
            if (a->text().remove('&') == it.key()) { found = a; break; }
        }
        if (!found) {
            fail("no action named \"" + it.key() + "\"");
            ok = false;
            continue;
        }
        if (found->isCheckable()) {
            found->setChecked(it.value().toBool());
            // An action in an exclusive group acts on triggered(), not toggled()
            if (it.value().toBool() && found->actionGroup()) found->trigger();
        } else if (it.value().toBool()) {
            found->trigger();
        }
    }
    return ok;
}

void SceneRunner::runScene(const QJsonObject& scene) {
    const QString name = scene.value("name").toString();
    MapWidget* map = window_->mapWidget();

    applyActions(scene.value("actions").toObject());

    const float time = static_cast<float>(scene.value("time").toDouble());
    map->setSimulationTime(time);
    const QJsonArray center = scene.value("center").toArray();
    if (center.size() == 2) {
        map->setView(center[0].toDouble(), center[1].toDouble(),
                     scene.value("m_per_px").toDouble(10.0));
    } else {
        map->fitToNetwork();
    }
    // "track": a MATSim vehicle id. The camera follows it and the halo marks it.
    const QString track = scene.value("track").toString();
    if (!track.isEmpty()) {
        const uint32_t id = window_->vehicleIdOf(track);
        if (id == kNoVehicle) fail("no vehicle " + track);
        else map->setTrackedVehicle(id);
    }
    QApplication::processEvents();

    // The tracked vehicle's state at the scene time, for the height check
    QJsonObject tracked;
    if (map->isTrackingVehicle()) {
        if (const VehicleState* s = map->vehicleState(map->trackedVehicle())) {
            tracked = QJsonObject{{"x", s->x}, {"y", s->y}, {"z", s->z},
                                  {"vertical_rate", s->verticalRate}, {"heading", s->angle}};
        }
    }

    const QString imagePath = QDir(outDir_).absoluteFilePath(name + ".png");
    const QImage image = map->renderToImage(1);
    if (image.isNull() || !image.save(imagePath)) fail("cannot save " + imagePath);

    const MapWidget::FrameTiming t = map->measureFrames(time, frames_, step_);
    std::vector<double> total(t.updateMs.size());
    for (size_t i = 0; i < total.size(); ++i) total[i] = t.updateMs[i] + t.renderMs[i];

    const QJsonObject record{
        {"scene", name},
        {"run", runName_},
        {"time_s", time},
        {"frames", static_cast<int>(t.updateMs.size())},
        {"step_s", step_},
        {"active_vehicles_end", static_cast<qint64>(t.activeVehicles)},
        {"tracking", map->isTrackingVehicle()},
        {"tracked_state", tracked},
        {"map_px", QJsonArray{map->width(), map->height()}},
        {"update_ms", summary(t.updateMs)},
        {"render_ms", summary(t.renderMs)},
        {"frame_ms", summary(total)},
    };
    QFile out(QDir(outDir_).absoluteFilePath("frames.jsonl"));
    if (out.open(QIODevice::Append | QIODevice::Text)) {
        out.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + "\n");
    }
    LOG_INFO(QString("Scene %1 captured").arg(name));
}

} // namespace simvis
