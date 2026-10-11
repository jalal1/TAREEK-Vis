#include "scenario_panel.h"
#include "core/logger.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <unordered_set>

namespace simvis {
namespace {

enum Column { ColId = 0, ColDirection, ColType, ColLength, ColCount };

constexpr int kInternalIdRole = Qt::UserRole;

// TAREEK puts the name in the output folder name, so it is kept to characters
// that are safe in a path on every system.
const QRegularExpression& nameRule() {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return re;
}

// A JSON string literal, with escaping done by Qt.
QString jsonString(const QString& s) {
    const QByteArray arr = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(arr.mid(1, arr.size() - 2));  // strip [ ]
}

} // namespace

ScenarioPanel::ScenarioPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* hint = new QLabel("Ctrl+click a road on the map to add it. "
                            "Both directions are added. Uncheck a row to leave "
                            "that link out of the file.", this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    table_ = new QTreeWidget(this);
    table_->setColumnCount(ColCount);
    table_->setHeaderLabels({"Link ID", "From -> To", "Road type", "Length (m)"});
    table_->setRootIsDecorated(false);
    table_->setUniformRowHeights(true);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setAlternatingRowColors(true);
    table_->header()->setStretchLastSection(false);
    table_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->setMinimumHeight(160);
    layout->addWidget(table_, 1);

    connect(table_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem*, int column) {
        if (column == ColId) updateExportState();
    });
    connect(table_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
        emit linkActivated(item->data(ColId, kInternalIdRole).toUInt());
    });
    connect(table_, &QTreeWidget::itemSelectionChanged, this, [this]() {
        removeButton_->setEnabled(!table_->selectedItems().isEmpty());
    });
    auto* deleteKey = new QShortcut(QKeySequence::Delete, table_);
    deleteKey->setContext(Qt::WidgetShortcut);
    connect(deleteKey, &QShortcut::activated, this, &ScenarioPanel::onRemoveRows);

    auto* rowButtons = new QHBoxLayout();
    countLabel_ = new QLabel(this);
    rowButtons->addWidget(countLabel_, 1);
    removeButton_ = new QPushButton("Remove", this);
    removeButton_->setToolTip("Remove the highlighted rows from the list (Delete)");
    removeButton_->setEnabled(false);
    connect(removeButton_, &QPushButton::clicked, this, &ScenarioPanel::onRemoveRows);
    rowButtons->addWidget(removeButton_);
    layout->addLayout(rowButtons);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // Only one action in Phase 1. Lane reduction joins this list later.
    actionCombo_ = new QComboBox(this);
    actionCombo_->addItem("Close these links", QStringLiteral("close"));
    form->addRow("Action:", actionCombo_);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText("e.g. hwy15_bridge_closure");
    nameEdit_->setToolTip("Letters, digits, '_' and '-' only");
    connect(nameEdit_, &QLineEdit::textChanged, this, &ScenarioPanel::updateExportState);
    form->addRow("Name:", nameEdit_);

    descriptionEdit_ = new QPlainTextEdit(this);
    descriptionEdit_->setPlaceholderText("Optional");
    descriptionEdit_->setFixedHeight(60);
    form->addRow("Description:", descriptionEdit_);

    baseLabel_ = new QLabel(this);
    baseLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    baseLabel_->setWordWrap(true);
    baseLabel_->setToolTip("The baseline run. TAREEK runs it again with these links closed.");
    form->addRow("Base run:", baseLabel_);
    layout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet("color: #d9534f;");
    layout->addWidget(statusLabel_);

    auto* buttons = new QHBoxLayout();
    clearButton_ = new QPushButton("Clear", this);
    clearButton_->setToolTip("Remove all links from the list");
    connect(clearButton_, &QPushButton::clicked, this, &ScenarioPanel::clearLinks);
    buttons->addWidget(clearButton_);
    buttons->addStretch();
    exportButton_ = new QPushButton("Export scenario file...", this);
    connect(exportButton_, &QPushButton::clicked, this, &ScenarioPanel::onExport);
    buttons->addWidget(exportButton_);
    layout->addLayout(buttons);

    setBaseExperiment(QString());
}

bool ScenarioPanel::contains(uint32_t internalId) const {
    for (int i = 0; i < table_->topLevelItemCount(); ++i) {
        if (table_->topLevelItem(i)->data(ColId, kInternalIdRole).toUInt() == internalId)
            return true;
    }
    return false;
}

void ScenarioPanel::addLinks(const std::vector<LinkRow>& rows) {
    std::unordered_set<uint32_t> present;
    for (uint32_t id : linkIds()) present.insert(id);

    bool added = false;
    {
        // One itemChanged per row would redo the export check every time.
        const QSignalBlocker block(table_);
        for (const auto& row : rows) {
            if (!present.insert(row.internalId).second) continue;
            auto* item = new QTreeWidgetItem(table_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(ColId, Qt::Checked);
            item->setData(ColId, kInternalIdRole, row.internalId);
            item->setText(ColId, row.linkId);
            item->setText(ColDirection, QString("%1 -> %2").arg(row.fromNode, row.toNode));
            item->setText(ColType, row.roadType);
            item->setText(ColLength, QString::number(row.lengthM, 'f', 0));
            item->setTextAlignment(ColLength, Qt::AlignRight | Qt::AlignVCenter);
            added = true;
        }
    }
    if (!added) return;
    table_->scrollToItem(table_->topLevelItem(table_->topLevelItemCount() - 1));
    updateExportState();
    emit selectionChanged(linkIds());
}

void ScenarioPanel::removeLinks(const std::vector<uint32_t>& internalIds) {
    const std::unordered_set<uint32_t> drop(internalIds.begin(), internalIds.end());
    bool removed = false;
    for (int i = table_->topLevelItemCount() - 1; i >= 0; --i) {
        if (drop.count(table_->topLevelItem(i)->data(ColId, kInternalIdRole).toUInt())) {
            delete table_->takeTopLevelItem(i);
            removed = true;
        }
    }
    if (!removed) return;
    updateExportState();
    emit selectionChanged(linkIds());
}

void ScenarioPanel::clearLinks() {
    if (isEmpty()) return;
    table_->clear();
    updateExportState();
    emit selectionChanged({});
}

bool ScenarioPanel::isEmpty() const {
    return table_->topLevelItemCount() == 0;
}

std::vector<uint32_t> ScenarioPanel::linkIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(table_->topLevelItemCount());
    for (int i = 0; i < table_->topLevelItemCount(); ++i)
        ids.push_back(table_->topLevelItem(i)->data(ColId, kInternalIdRole).toUInt());
    return ids;
}

void ScenarioPanel::setBaseExperiment(const QString& name) {
    baseExperiment_ = name;
    baseLabel_->setText(name.isEmpty() ? QString("(no run loaded)") : name);
    updateExportState();
}

void ScenarioPanel::onRemoveRows() {
    std::vector<uint32_t> ids;
    for (auto* item : table_->selectedItems())
        ids.push_back(item->data(ColId, kInternalIdRole).toUInt());
    removeLinks(ids);
}

QStringList ScenarioPanel::checkedLinkIds() const {
    // Two rows never share a link, but keep the file clean if they ever do.
    QStringList ids;
    for (int i = 0; i < table_->topLevelItemCount(); ++i) {
        const auto* item = table_->topLevelItem(i);
        if (item->checkState(ColId) == Qt::Checked && !ids.contains(item->text(ColId)))
            ids << item->text(ColId);
    }
    return ids;
}

QString ScenarioPanel::exportBlocker() const {
    if (baseExperiment_.isEmpty()) return "Open a run first.";
    if (isEmpty()) return "No links selected. Ctrl+click a road on the map.";
    if (checkedLinkIds().isEmpty()) return "No row is checked. Check at least one link.";
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) return "Type a name for the scenario.";
    if (!nameRule().match(name).hasMatch())
        return "The name can have only letters, digits, '_' and '-'.";
    return QString();
}

void ScenarioPanel::updateExportState() {
    const int total = table_->topLevelItemCount();
    const int checked = static_cast<int>(checkedLinkIds().size());
    countLabel_->setText(total == 0 ? QString("No links")
                                    : QString("%1 of %2 links checked").arg(checked).arg(total));
    clearButton_->setEnabled(total > 0);

    const QString why = exportBlocker();
    exportButton_->setEnabled(why.isEmpty());
    exportButton_->setToolTip(why);
    statusLabel_->setText(why);
    statusLabel_->setVisible(!why.isEmpty());
}

QString ScenarioPanel::scenarioJson() const {
    // Written by hand rather than through QJsonObject, which sorts keys: the
    // file then reads in the same order as the format in DESIGN_SCENARIOS.md.
    QStringList ids;
    for (const QString& id : checkedLinkIds()) ids << jsonString(id);

    QString out;
    out += "{\n";
    out += QString("  \"name\": %1,\n").arg(jsonString(nameEdit_->text().trimmed()));
    out += QString("  \"description\": %1,\n")
               .arg(jsonString(descriptionEdit_->toPlainText().trimmed()));
    out += QString("  \"base_experiment\": %1,\n").arg(jsonString(baseExperiment_));
    out += "  \"closures\": [\n";
    out += QString("    { \"type\": %1, \"link_ids\": [%2] }\n")
               .arg(jsonString(actionCombo_->currentData().toString()), ids.join(", "));
    out += "  ]\n";
    out += "}\n";
    return out;
}

void ScenarioPanel::onExport() {
    const QString why = exportBlocker();
    if (!why.isEmpty()) {
        QMessageBox::warning(this, "Export scenario file", why);
        return;
    }

    QSettings settings;
    const QString lastDir = settings.value("scenarioExportDir").toString();
    const QString name = nameEdit_->text().trimmed();
    QString path = QFileDialog::getSaveFileName(
        this, "Export scenario file",
        QDir(lastDir).filePath(name + ".json"),
        "Scenario file (*.json)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
        file.write(scenarioJson().toUtf8()) < 0 || !file.commit()) {
        QMessageBox::critical(this, "Export scenario file",
                              QString("Could not save the file:\n%1").arg(path));
        return;
    }
    settings.setValue("scenarioExportDir", QFileInfo(path).absolutePath());

    const int n = static_cast<int>(checkedLinkIds().size());
    LOG_INFO(QString("Scenario file exported: %1 (%2 links)").arg(path).arg(n));
    QMessageBox::information(this, "Export scenario file",
        QString("Saved %1 link(s) to:\n%2\n\nRun it in TAREEK with:\n"
                "python run_scenario.py run --base <baseline folder> --scenario %3")
            .arg(n).arg(path, QFileInfo(path).fileName()));
}

} // namespace simvis
