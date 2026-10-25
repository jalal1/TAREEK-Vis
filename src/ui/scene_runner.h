#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace simvis {

class MainWindow;

// Scene capture: `TAREEK-Vis <folder> --scenes FILE --run NAME --out DIR`.
// Loads the run, and for each scene of that run sets the time, the view and
// the ribbon toggles, saves <scene>.png, times a short playback, writes one
// JSON line per scene to DIR/frames.jsonl, and quits. It does not use the
// mouse or the keyboard, so the images and the times can be made again
// exactly. See docs/air-and-modes-plan.md, stage VIS-0.
class SceneRunner : public QObject {
    Q_OBJECT

public:
    SceneRunner(MainWindow* window, const QString& scenesFile,
                const QString& runName, const QString& outDir,
                QObject* parent = nullptr);

    // Reads the scenes file and sizes the map. False (with a message on
    // stderr) when the file cannot be read or has no scene for the run.
    bool prepare();

private:
    void runAll();
    void runScene(const QJsonObject& scene);
    // Sets each checkable ribbon action named in `actions` (the action text
    // without '&') to the given value. False if an action is not found.
    bool applyActions(const QJsonObject& actions);

    MainWindow* window_;
    QString scenesFile_;
    QString runName_;
    QString outDir_;
    QJsonArray scenes_;
    int frames_ = 300;
    double step_ = 10.0 / 60.0;  // 10x playback at 60 frames per second
};

} // namespace simvis
