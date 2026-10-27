#include "views_panel.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include "core/logger.h"

#include <QAbstractItemModel>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace simvis {

namespace {

constexpr int kViewRole = Qt::UserRole + 1;
constexpr int kCardHeight = 76;
constexpr QSize kThumbSize(96, 60);
const QString kFileName = QStringLiteral("tareek-views.json");

// The file of a run whose folder cannot be written: one file per folder in
// the app data folder
QString fallbackFile(const QString& folder) {
    const QByteArray hash = QCryptographicHash::hash(
        QDir(folder).absolutePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/views/" + QString::fromLatin1(hash) + ".json";
}

// The grey lines under the name: what the view holds besides the map position
QStringList detailLines(const QJsonObject& view) {
    QString first = ViewsPanel::clockText(view.value("time").toDouble());
    const QString track = view.value("track").toString();
    if (!track.isEmpty()) first += QString(" · following %1").arg(track);

    QStringList second;
    const QString heatmap = view.value("heatmap").toString();
    if (!heatmap.isEmpty()) second << QString("heatmap: %1").arg(heatmap);
    const int links = view.value("select").toArray().size();
    if (links > 0) second << QString("%1 link%2 selected").arg(links).arg(links == 1 ? "" : "s");

    QStringList lines{first};
    if (!second.isEmpty()) lines << second.join(" · ");
    return lines;
}

QPixmap thumbnailOf(const QJsonObject& view) {
    QPixmap pixmap;
    pixmap.loadFromData(QByteArray::fromBase64(view.value("thumbnail").toString().toLatin1()), "PNG");
    return pixmap;
}

QJsonArray readViews(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        if (error) *error = parseError.errorString();
        return {};
    }
    // A --scenes file has the same entries under "scenes"
    const QJsonObject root = doc.object();
    return root.contains("views") ? root.value("views").toArray()
                                  : root.value("scenes").toArray();
}

bool writeViews(const QString& path, const QJsonArray& views, QString* error) {
    // "frames": 0 makes the file a still-image --scenes file as it is
    const QJsonObject root{{"version", 1}, {"frames", 0}, {"views", views}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

// One view as a card: the thumbnail at the left, the name, and the details
// in grey. The colors are those of the cards in the info panel.
class ViewCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(option.rect.width(), kCardHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        const QColor accent(panelstyle::kAccent);
        const QRectF card = QRectF(option.rect).adjusted(2.5, 2.5, -2.5, -2.5);
        painter->setPen(selected ? accent : QColor(128, 128, 128, 64));
        painter->setBrush(selected ? QColor(30, 136, 229, 31)
                                   : QColor(128, 128, 128, hover ? 40 : 26));
        painter->drawRoundedRect(card, 6, 6);

        const QRect thumb(option.rect.left() + 8,
                          option.rect.top() + (option.rect.height() - kThumbSize.height()) / 2,
                          kThumbSize.width(), kThumbSize.height());
        const QPixmap pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
        QPainterPath clip;
        clip.addRoundedRect(thumb, 4, 4);
        painter->setClipPath(clip);
        if (pixmap.isNull()) painter->fillRect(thumb, QColor(128, 128, 128, 50));
        else painter->drawPixmap(thumb, pixmap);
        painter->setClipping(false);

        const QRect text = textRect(option.rect);
        QFont nameFont = option.font;
        nameFont.setBold(true);
        nameFont.setPointSizeF(10.5);
        painter->setFont(nameFont);
        painter->setPen(selected ? accent : option.palette.color(QPalette::Text));
        const QFontMetrics nameMetrics(nameFont);
        painter->drawText(QPoint(text.left(), text.top() + nameMetrics.ascent()),
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, text.width()));

        QFont detailFont = option.font;
        detailFont.setPointSizeF(9.0);
        painter->setFont(detailFont);
        painter->setPen(selected ? accent : QColor(Qt::gray));
        const QFontMetrics detailMetrics(detailFont);
        int y = text.top() + nameMetrics.height() + 2 + detailMetrics.ascent();
        const QJsonObject view = index.data(kViewRole).toJsonObject();
        for (const QString& line : detailLines(view)) {
            painter->drawText(QPoint(text.left(), y),
                              detailMetrics.elidedText(line, Qt::ElideRight, text.width()));
            y += detailMetrics.height();
        }
        painter->restore();
    }

    // Rename edits the name line only
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        const QRect text = textRect(option.rect);
        editor->setGeometry(text.left() - 3, text.top() - 3, text.width() + 3,
                            editor->sizeHint().height());
    }

private:
    static QRect textRect(const QRect& item) {
        return QRect(item.left() + 8 + kThumbSize.width() + 10, item.top() + 9,
                     item.width() - kThumbSize.width() - 34, item.height() - 18);
    }
};

} // anonymous namespace

ViewsPanel::ViewsPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    hint_ = new QLabel(this);
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color: gray;");
    layout->addWidget(hint_);

    list_ = new QListWidget(this);
    list_->setItemDelegate(new ViewCardDelegate(list_));
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setMouseTracking(true);  // hover color of the cards
    list_->setFrameShape(QFrame::NoFrame);
    list_->setStyleSheet("QListWidget { background: transparent; }");
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(list_, 1);

    // Double-click and Enter both send itemActivated
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit viewActivated(item->data(kViewRole).toJsonObject());
    });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (loading_) return;
        QJsonObject view = item->data(kViewRole).toJsonObject();
        const QString name = item->text().trimmed();
        loading_ = true;
        if (name.isEmpty()) {
            item->setText(view.value("name").toString());
        } else {
            view["name"] = name;
            item->setText(name);
            item->setData(kViewRole, view);
        }
        loading_ = false;
        save();
    });
    // A drag can arrive as a move or as an insert and a remove
    connect(list_->model(), &QAbstractItemModel::rowsMoved, this, &ViewsPanel::save);
    connect(list_->model(), &QAbstractItemModel::rowsRemoved, this, &ViewsPanel::save);
    connect(list_, &QWidget::customContextMenuRequested, this, &ViewsPanel::showContextMenu);

    auto* deleteKey = new QShortcut(QKeySequence::Delete, list_);
    deleteKey->setContext(Qt::WidgetShortcut);
    connect(deleteKey, &QShortcut::activated, this, &ViewsPanel::removeSelected);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    saveButton_ = new QPushButton(ribbonIcon("bookmark-plus"), "Save View", this);
    saveButton_->setToolTip("Save the current view (Ctrl+B)");
    connect(saveButton_, &QPushButton::clicked, this, &ViewsPanel::saveRequested);
    buttons->addWidget(saveButton_);
    layout->addLayout(buttons);

    setRunFolder(QString());
}

void ViewsPanel::setRunFolder(const QString& folder) {
    folder_ = folder;
    loading_ = true;
    list_->clear();
    file_.clear();
    if (!folder.isEmpty()) {
        const QString inRun = QDir(folder).absoluteFilePath(kFileName);
        const QString fallback = fallbackFile(folder);
        file_ = QFileInfo::exists(inRun) || !QFileInfo::exists(fallback) ? inRun : fallback;
        if (QFileInfo::exists(file_)) {
            QString error;
            const QJsonArray views = readViews(file_, &error);
            if (!error.isEmpty()) LOG_ERROR(QString("Views: cannot read %1: %2").arg(file_, error));
            for (const QJsonValue& v : views) {
                if (v.isObject()) addItem(v.toObject());
            }
            LOG_INFO(QString("Views: %1 from %2").arg(views.size()).arg(file_));
        }
    }
    loading_ = false;
    saveButton_->setEnabled(!folder.isEmpty());
    updateHint();
    emit countChanged(count());
}

int ViewsPanel::count() const {
    return list_->count();
}

void ViewsPanel::addView(const QJsonObject& view) {
    loading_ = true;
    addItem(view);
    loading_ = false;
    // Only the new view is selected, also after several are added at once
    list_->setCurrentRow(list_->count() - 1, QItemSelectionModel::ClearAndSelect);
    list_->scrollToBottom();
    save();
    updateHint();
    emit countChanged(count());
}

void ViewsPanel::replaceView(int row, const QJsonObject& view) {
    QListWidgetItem* item = list_->item(row);
    if (!item) return;
    QJsonObject kept = view;
    kept["name"] = item->text();
    loading_ = true;
    item->setData(kViewRole, kept);
    item->setData(Qt::DecorationRole, thumbnailOf(kept));
    loading_ = false;
    save();
}

QJsonObject ViewsPanel::view(int row) const {
    const QListWidgetItem* item = list_->item(row);
    return item ? item->data(kViewRole).toJsonObject() : QJsonObject();
}

QString ViewsPanel::clockText(double seconds) {
    const int minutes = static_cast<int>(seconds / 60.0);
    return QString("%1:%2").arg(minutes / 60, 2, 10, QChar('0'))
                           .arg(minutes % 60, 2, 10, QChar('0'));
}

QString ViewsPanel::nextName() const {
    QStringList names;
    for (int i = 0; i < list_->count(); ++i) names << list_->item(i)->text();
    int n = list_->count() + 1;
    while (names.contains(QString("View %1").arg(n))) ++n;
    return QString("View %1").arg(n);
}

bool ViewsPanel::importFrom(const QString& path, QString* error) {
    QString readError;
    const QJsonArray views = readViews(path, &readError);
    if (!readError.isEmpty() || views.isEmpty()) {
        if (error) *error = readError.isEmpty() ? QString("The file has no views.") : readError;
        return false;
    }
    loading_ = true;
    for (const QJsonValue& v : views) {
        if (v.isObject()) addItem(v.toObject());
    }
    loading_ = false;
    save();
    updateHint();
    emit countChanged(count());
    return true;
}

bool ViewsPanel::exportTo(const QString& path, QString* error) const {
    return writeViews(path, allViews(), error);
}

void ViewsPanel::addItem(const QJsonObject& view) {
    auto* item = new QListWidgetItem(view.value("name").toString(), list_);
    item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
    item->setData(kViewRole, view);
    item->setData(Qt::DecorationRole, thumbnailOf(view));
    item->setToolTip("Double-click to go to this view. F2 to rename.");
}

void ViewsPanel::showContextMenu(const QPoint& pos) {
    QListWidgetItem* item = list_->itemAt(pos);
    QMenu menu(this);
    if (item) {
        const int row = list_->row(item);
        QAction* go = menu.addAction(ribbonIcon("bookmark"), "Go to View");
        QFont bold = go->font();
        bold.setBold(true);
        go->setFont(bold);
        connect(go, &QAction::triggered, this, [this, item]() {
            emit viewActivated(item->data(kViewRole).toJsonObject());
        });
        menu.addAction(ribbonIcon("refresh-cw"), "Update from Current View", this,
                       [this, row]() { emit updateRequested(row); });
        menu.addSeparator();
        menu.addAction("Rename", QKeySequence(Qt::Key_F2), this,
                       [this, item]() { list_->editItem(item); });
        menu.addAction(ribbonIcon("copy"), "Duplicate", this, [this, row]() { duplicate(row); });
        menu.addAction(ribbonIcon("x"), "Delete", QKeySequence::Delete, this,
                       &ViewsPanel::removeSelected);
    } else {
        menu.addAction(ribbonIcon("bookmark-plus"), "Save Current View", this,
                       &ViewsPanel::saveRequested)->setEnabled(!folder_.isEmpty());
    }
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

void ViewsPanel::removeSelected() {
    const QList<QListWidgetItem*> items = list_->selectedItems();
    if (items.isEmpty()) return;
    for (QListWidgetItem* item : items) delete item;
    save();
    updateHint();
    emit countChanged(count());
}

void ViewsPanel::duplicate(int row) {
    QJsonObject copy = view(row);
    if (copy.isEmpty()) return;
    copy["name"] = copy.value("name").toString() + " (copy)";
    loading_ = true;
    addItem(copy);
    // Beside the original, not at the end
    list_->insertItem(row + 1, list_->takeItem(list_->count() - 1));
    loading_ = false;
    list_->setCurrentRow(row + 1, QItemSelectionModel::ClearAndSelect);
    save();
    updateHint();
    emit countChanged(count());
}

void ViewsPanel::updateHint() {
    if (folder_.isEmpty()) {
        hint_->setText("Open a run to save views of it.");
    } else if (list_->count() == 0) {
        hint_->setText("No saved views yet.\n\nA view keeps the time, the map "
                       "position, the layers, the selected links and the heatmap. "
                       "Press Ctrl+B or click Save View to save the current view.");
    } else {
        hint_->setText("Double-click a view to go back to it. "
                       "Drag a view to change the order.");
    }
}

void ViewsPanel::save() {
    if (loading_ || folder_.isEmpty()) return;
    // No file for a run that never had a view
    if (list_->count() == 0 && !QFileInfo::exists(file_)) return;

    QString error;
    if (writeViews(file_, allViews(), &error)) return;

    // A read-only run folder: keep the views in the app data folder
    const QString fallback = fallbackFile(folder_);
    if (file_ != fallback) {
        QDir().mkpath(QFileInfo(fallback).absolutePath());
        if (writeViews(fallback, allViews(), &error)) {
            LOG_INFO(QString("Views: %1 is read-only, saved to %2").arg(file_, fallback));
            file_ = fallback;
            emit message("The run folder is read-only. Views are saved in " +
                         QDir::toNativeSeparators(fallback));
            return;
        }
    }
    LOG_ERROR(QString("Views: cannot save %1: %2").arg(file_, error));
    emit message("Cannot save the views: " + error);
}

QJsonArray ViewsPanel::allViews() const {
    QJsonArray views;
    for (int i = 0; i < list_->count(); ++i) {
        views.append(list_->item(i)->data(kViewRole).toJsonObject());
    }
    return views;
}

} // namespace simvis
