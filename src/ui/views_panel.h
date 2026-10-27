#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace simvis {

// "Views" page of the side panel: the saved views of the run. A view keeps
// the time, the map position, the layer toggles, the selected links, the
// followed vehicle and the heatmap, so the user can come back to it with a
// double-click.
//
// The list is kept in tareek-views.json beside the network file. A view has
// the keys of a scene of a --scenes file, so the same file can also be
// captured from the command line. If the run folder cannot be written, the
// file goes to the app data folder.
class ViewsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ViewsPanel(QWidget* parent = nullptr);

    // Read the views of the run in this folder. An empty folder = no run.
    void setRunFolder(const QString& folder);

    int count() const;
    // Add a view at the end of the list and select it
    void addView(const QJsonObject& view);
    // Replace the state of a view. The name stays.
    void replaceView(int row, const QJsonObject& view);
    QJsonObject view(int row) const;
    // Next free default name, "View 1", "View 2", ...
    QString nextName() const;

    // "08:05" as on the timeline, or "26:30" for a time after midnight of
    // the next day
    static QString clockText(double seconds);

    // Add the views of a views file at the end of the list
    bool importFrom(const QString& path, QString* error);
    bool exportTo(const QString& path, QString* error) const;

signals:
    // Double-click or Enter on a view
    void viewActivated(const QJsonObject& view);
    // The "Save View" button or the context menu
    void saveRequested();
    void updateRequested(int row);
    // Views were added or removed. Carries the number of views.
    void countChanged(int count);
    // A message for the status bar
    void message(const QString& text);

private:
    void addItem(const QJsonObject& view);
    void showContextMenu(const QPoint& pos);
    void removeSelected();
    void duplicate(int row);
    void updateHint();
    // Write the list to the file. Every change calls this.
    void save();
    QJsonArray allViews() const;

    QListWidget* list_;
    QLabel* hint_;
    QPushButton* saveButton_;
    QString folder_;
    QString file_;
    // True while the list is filled from a file, so that it is not saved back
    bool loading_ = false;
};

} // namespace simvis
