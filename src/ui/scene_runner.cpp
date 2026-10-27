#include "scene_runner.h"
#include "main_window.h"
#include "core/logger.h"
#include <QAction>
#include <QElapsedTimer>
#include <QPainter>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
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
    scale_ = root.value("scale").toInt(scale_);

    // A views file (Views tab) has the same entries under "views". An entry
    // with no "run" is for every run.
    const QJsonArray entries = root.contains("scenes") ? root.value("scenes").toArray()
                                                       : root.value("views").toArray();
    for (const QJsonValue& v : entries) {
        const QString run = v.toObject().value("run").toString();
        if (run.isEmpty() || run == runName_) scenes_.append(v);
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
    // Still images (frames 0) time nothing, so the background workers
    // (hourly volumes, heatmaps) can run and the panels show their data
    window_->setCaptureMode(frames_ > 0);
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
            if (a->text().remove('&') == it.key() || MainWindow::toggleKey(a) == it.key()) {
                found = a;
                break;
            }
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

void SceneRunner::wait(double seconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < seconds * 1000.0) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void SceneRunner::runScene(const QJsonObject& scene) {
    const QString name = scene.value("name").toString();
    MapWidget* map = window_->mapWidget();

    applyActions(scene.value("actions").toObject());

    // "counts": {"file": path, "scale": countsScaleFactor}
    const QJsonObject counts = scene.value("counts").toObject();
    if (!counts.isEmpty() &&
        !window_->loadCounts(counts.value("file").toString(),
                             counts.value("scale").toDouble(1.0))) {
        fail("cannot load counts " + counts.value("file").toString());
    }
    // "select": MATSim link ids to add to the selected links (Ctrl+click)
    for (const QJsonValue& v : scene.value("select").toArray()) {
        if (!window_->selectLinkForCapture(v.toString())) fail("no link " + v.toString());
    }
    // "fill": {placeholder text: text}, typed into that box of the side panel
    const QJsonObject fill = scene.value("fill").toObject();
    for (auto it = fill.begin(); it != fill.end(); ++it) {
        QLineEdit* box = nullptr;
        for (QLineEdit* e : window_->sidePanel()->findChildren<QLineEdit*>()) {
            if (e->placeholderText() == it.key()) { box = e; break; }
        }
        if (box) box->setText(it.value().toString());
        else fail("no box with placeholder \"" + it.key() + "\"");
    }
    // "heatmap": an activity type
    const QString heatmap = scene.value("heatmap").toString();
    if (!heatmap.isEmpty()) window_->showHeatmapForCapture(heatmap);

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
    // "find": a link, person or vehicle id, as typed in the search box. It
    // comes after the view, because a find moves the view to what it found.
    const QString find = scene.value("find").toString();
    if (!find.isEmpty()) window_->findForCapture(find);
    QApplication::processEvents();
    // "wait_s": time for background work (heatmap, hourly volumes) to end
    wait(scene.value("wait_s").toDouble(0.0));
    // A heatmap on a large network takes minutes: wait until it is on the map
    while (window_->heatmapRunning()) wait(1.0);

    // The tracked vehicle's state at the scene time, for the height check
    QJsonObject tracked;
    if (map->isTrackingVehicle()) {
        if (const VehicleState* s = map->vehicleState(map->trackedVehicle())) {
            tracked = QJsonObject{{"x", s->x}, {"y", s->y}, {"z", s->z},
                                  {"vertical_rate", s->verticalRate}, {"heading", s->angle}};
        }
    }

    const QString imagePath = QDir(outDir_).absoluteFilePath(name + ".png");
    // "scale": the same off-screen render as Export > PNG at 2x, 4x or 8x
    const QImage image = map->renderToImage(scene.value("scale").toInt(scale_));
    if (image.isNull() || !image.save(imagePath)) fail("cannot save " + imagePath);

    // "panel": also save the side panel, at this many times its screen size
    const int panelScale = scene.value("panel").toInt(0);
    QWidget* panel = window_->sidePanel();
    if (panelScale > 0 && panel && panel->isVisible()) {
        QImage panelImage(panel->size() * panelScale, QImage::Format_ARGB32_Premultiplied);
        panelImage.setDevicePixelRatio(panelScale);
        panelImage.fill(panel->palette().window().color());
        panel->render(&panelImage);
        const QString panelPath = QDir(outDir_).absoluteFilePath(name + "-panel.png");
        if (!panelImage.save(panelPath)) fail("cannot save " + panelPath);
    }

    // "count_chart": a link with a count station. Saves the chart window that
    // a click on the station opens, at the "panel" scale (3 if not given).
    const QString countLink = scene.value("count_chart").toString();
    if (!countLink.isEmpty()) {
        QWidget* chart = window_->showCountChartForCapture(countLink);
        if (!chart) {
            fail("no count station on link " + countLink);
        } else {
            wait(1.0);
            const int s = panelScale > 0 ? panelScale : 3;
            QImage chartImage(chart->size() * s, QImage::Format_ARGB32_Premultiplied);
            chartImage.setDevicePixelRatio(s);
            chartImage.fill(chart->palette().window().color());
            chart->render(&chartImage);
            const QString chartPath = QDir(outDir_).absoluteFilePath(name + "-counts.png");
            if (!chartImage.save(chartPath)) fail("cannot save " + chartPath);
            chart->close();
        }
    }

    if (frames_ <= 0) {
        LOG_INFO(QString("Scene %1 captured").arg(name));
        return;
    }

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
