#include <QApplication>
#include <QSurfaceFormat>
#include <QMessageBox>
#include <QFileInfo>
#include <memory>
#include "ui/main_window.h"
#include "ui/scene_runner.h"
#include "core/logger.h"

int main(int argc, char* argv[]) {
    try {
        simvis::Logger::init();
        LOG_INFO("TAREEK-Vis starting up");

        // Set OpenGL version before creating application
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        format.setSamples(4);  // Multisampling for smoother lines
        QSurfaceFormat::setDefaultFormat(format);

        QApplication app(argc, argv);
        app.setApplicationName("TAREEK-Vis");
        app.setApplicationVersion(TAREEK_VIS_VERSION);
        app.setOrganizationName("TAREEK-Vis");

        LOG_INFO("QApplication created");

        simvis::MainWindow mainWindow;
        mainWindow.show();

        // CLI: `TAREEK-Vis <folder> --scenes FILE --run NAME --out DIR` loads
        // the folder, captures the scenes of that run and quits (SceneRunner)
        const QStringList args = app.arguments();
        const int scenesAt = args.indexOf("--scenes");
        std::unique_ptr<simvis::SceneRunner> sceneRunner;
        if (scenesAt > 0) {
            auto valueOf = [&args](const QString& key) {
                const int at = args.indexOf(key);
                return at > 0 ? args.value(at + 1) : QString();
            };
            sceneRunner = std::make_unique<simvis::SceneRunner>(
                &mainWindow, args.value(scenesAt + 1), valueOf("--run"), valueOf("--out"));
            if (args.size() < 2 || !QFileInfo(args.value(1)).isDir() || !sceneRunner->prepare())
                return 2;
            QMetaObject::invokeMethod(&mainWindow, [&mainWindow, path = args.value(1)]() {
                mainWindow.loadFolder(path);
            }, Qt::QueuedConnection);
        // CLI: `TAREEK-Vis <folder>` auto-loads network/events/transit from folder
        } else if (argc >= 3) {
            mainWindow.loadNetworkFile(argv[1]);
            mainWindow.loadEventsFile(argv[2]);
        } else if (argc == 2 && QFileInfo(QString::fromLocal8Bit(argv[1])).isDir()) {
            QMetaObject::invokeMethod(&mainWindow, [&mainWindow, path = QString::fromLocal8Bit(argv[1])]() {
                mainWindow.loadFolder(path);
            }, Qt::QueuedConnection);
        }

        int ret = app.exec();
        LOG_INFO(QString("TAREEK-Vis exiting, code %1").arg(ret));
        return ret;

    } catch (const std::exception& e) {
        qCritical() << "Unhandled exception in main():" << e.what();
        QMessageBox::critical(nullptr, "Fatal Error",
            QString("Unhandled exception:\n%1").arg(e.what()));
        return 1;
    } catch (...) {
        qCritical() << "Unknown unhandled exception in main()";
        return 1;
    }
}
