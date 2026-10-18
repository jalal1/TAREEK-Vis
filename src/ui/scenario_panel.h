#pragma once

#include <QWidget>
#include <QString>
#include <cstdint>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace simvis {

class ElidedLabel;

// "Selected links" panel: the road links picked with Ctrl+click for a what-if
// scenario, and the export of the scenario file TAREEK runs. The file format is
// the contract in TAREEK's DESIGN_SCENARIOS.md, section 3.
class ScenarioPanel : public QWidget {
    Q_OBJECT

public:
    struct LinkRow {
        uint32_t internalId = 0;
        QString linkId;      // MATSim link id string
        QString fromNode;    // MATSim node id strings
        QString toNode;
        QString roadType;
        double lengthM = 0.0;
    };

    explicit ScenarioPanel(QWidget* parent = nullptr);

    bool contains(uint32_t internalId) const;
    // Rows already in the list are skipped. New rows start checked.
    void addLinks(const std::vector<LinkRow>& rows);
    void removeLinks(const std::vector<uint32_t>& internalIds);
    void clearLinks();
    bool isEmpty() const;

    // Every link in the list, checked or not, in list order.
    std::vector<uint32_t> linkIds() const;

    // Folder name of the run the links come from. Empty = no run loaded.
    void setBaseExperiment(const QString& name);

signals:
    // The list changed (rows added or removed). Carries every link in it.
    void selectionChanged(const std::vector<uint32_t>& internalIds);
    // The user double-clicked a row.
    void linkActivated(uint32_t internalId);
    // Rows were added or removed. Carries the number of rows.
    void linkCountChanged(int count);

public slots:
    // Ask where to save the scenario file and write it, or explain why it
    // cannot be written yet.
    void onExport();

private slots:
    void updateExportState();
    void onRemoveRows();

private:
    // Why Export cannot run now, or an empty string when it can.
    QString exportBlocker() const;
    QStringList checkedLinkIds() const;
    QString scenarioJson() const;

    QTreeWidget* table_;
    QPushButton* removeButton_;
    QLabel* countLabel_;
    QComboBox* actionCombo_;
    QLineEdit* nameEdit_;
    QPlainTextEdit* descriptionEdit_;
    ElidedLabel* baseLabel_;
    QLabel* statusLabel_;
    QPushButton* exportButton_;
    QPushButton* clearButton_;

    QString baseExperiment_;
};

} // namespace simvis
