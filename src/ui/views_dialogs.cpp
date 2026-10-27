#include "views_dialogs.h"
#include "views_panel.h"
#include "ribbon/ribbon.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace simvis {

namespace {

// More views than this from one Save Over Time is most likely a typing error
constexpr int kMaxViewsOverTime = 100;

// "6:00", "06:00" or "26:30" in seconds, or -1
double parseClock(const QString& text) {
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{1,2}):(\\d{2})\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch() || m.captured(2).toInt() > 59) return -1.0;
    return m.captured(1).toInt() * 3600.0 + m.captured(2).toInt() * 60.0;
}

} // anonymous namespace

CaptureViewsDialog::CaptureViewsDialog(const QStringList& names, const QString& folder,
                                       const QSize& mapSize, QWidget* parent)
    : QDialog(parent), mapSize_(mapSize) {
    setWindowTitle("Capture Views");
    auto* layout = new QVBoxLayout(this);

    auto* listHeader = new QHBoxLayout();
    listHeader->addWidget(new QLabel("Views to capture:", this), 1);
    auto* allButton = new QPushButton("All", this);
    auto* noneButton = new QPushButton("None", this);
    listHeader->addWidget(allButton);
    listHeader->addWidget(noneButton);
    layout->addLayout(listHeader);

    list_ = new QListWidget(this);
    for (const QString& name : names) {
        auto* item = new QListWidgetItem(ribbonIcon("bookmark"), name, list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    list_->setMinimumHeight(160);
    layout->addWidget(list_, 1);
    connect(list_, &QListWidget::itemChanged, this, &CaptureViewsDialog::updateState);
    auto checkAll = [this](Qt::CheckState state) {
        for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(state);
    };
    connect(allButton, &QPushButton::clicked, this, [checkAll]() { checkAll(Qt::Checked); });
    connect(noneButton, &QPushButton::clicked, this, [checkAll]() { checkAll(Qt::Unchecked); });

    QSettings settings;
    auto* form = new QFormLayout();

    // The choices of Export > PNG
    scaleCombo_ = new QComboBox(this);
    scaleCombo_->addItem("1x (screen resolution)", 1);
    scaleCombo_->addItem("2x (recommended)", 2);
    scaleCombo_->addItem("4x (high quality)", 4);
    scaleCombo_->addItem(QString::fromUtf8("8x (very high — large file)"), 8);
    const int lastScale = scaleCombo_->findData(settings.value("views/captureScale", 2).toInt());
    scaleCombo_->setCurrentIndex(lastScale >= 0 ? lastScale : 1);
    connect(scaleCombo_, &QComboBox::currentIndexChanged, this, &CaptureViewsDialog::updateState);
    form->addRow("Resolution:", scaleCombo_);

    sizeLabel_ = new QLabel(this);
    sizeLabel_->setStyleSheet("color: gray;");
    form->addRow(QString(), sizeLabel_);

    auto* folderRow = new QHBoxLayout();
    folderEdit_ = new QLineEdit(QDir::toNativeSeparators(folder), this);
    connect(folderEdit_, &QLineEdit::textChanged, this, &CaptureViewsDialog::updateState);
    folderRow->addWidget(folderEdit_, 1);
    auto* browse = new QToolButton(this);
    browse->setIcon(ribbonIcon("folder-open"));
    browse->setToolTip("Select the folder");
    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, "Folder for the images",
                                                              folderEdit_->text());
        if (!dir.isEmpty()) folderEdit_->setText(QDir::toNativeSeparators(dir));
    });
    folderRow->addWidget(browse);
    form->addRow("Folder:", folderRow);
    layout->addLayout(form);

    auto* note = new QLabel("Each view is saved as <name>.png. An image with the same "
                            "name is replaced.", this);
    note->setWordWrap(true);
    note->setStyleSheet("color: gray;");
    layout->addWidget(note);

    openFolderBox_ = new QCheckBox("Open the folder when done", this);
    openFolderBox_->setChecked(settings.value("views/openFolder", true).toBool());
    layout->addWidget(openFolderBox_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    captureButton_ = buttons->addButton("Capture", QDialogButtonBox::AcceptRole);
    captureButton_->setIcon(ribbonIcon("images"));
    captureButton_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(420, 460);
    updateState();
}

QList<int> CaptureViewsDialog::rows() const {
    QList<int> rows;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->checkState() == Qt::Checked) rows << i;
    }
    return rows;
}

int CaptureViewsDialog::scale() const {
    return scaleCombo_->currentData().toInt();
}

QString CaptureViewsDialog::folder() const {
    return QDir::fromNativeSeparators(folderEdit_->text().trimmed());
}

bool CaptureViewsDialog::openFolderWhenDone() const {
    return openFolderBox_->isChecked();
}

void CaptureViewsDialog::accept() {
    QSettings settings;
    settings.setValue("views/captureScale", scale());
    settings.setValue("views/openFolder", openFolderWhenDone());
    QDialog::accept();
}

void CaptureViewsDialog::updateState() {
    const int count = rows().size();
    sizeLabel_->setText(QString("%1 image%2 of %3 x %4 pixels")
        .arg(count).arg(count == 1 ? "" : "s")
        .arg(mapSize_.width() * scale()).arg(mapSize_.height() * scale()));
    captureButton_->setEnabled(count > 0 && !folder().isEmpty());
}

SaveOverTimeDialog::SaveOverTimeDialog(double now, double first, double last,
                                       const QString& name, QWidget* parent)
    : QDialog(parent), first_(first), last_(last) {
    setWindowTitle("Save Over Time");
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("Save one view for each time, all at the current map "
                             "position and with the current layers.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    nameEdit_ = new QLineEdit(name, this);
    nameEdit_->selectAll();  // typing replaces the default name
    nameEdit_->setToolTip("Each view gets this name and its time, for example \"" +
                          name + " · 08:00\"");
    form->addRow("Name:", nameEdit_);

    // From the hour on the map to 12 hours later, in the span of the run
    const double from = std::max(first, std::floor(now / 3600.0) * 3600.0);
    const double to = std::min(last, from + 12 * 3600.0);
    fromEdit_ = new QLineEdit(ViewsPanel::clockText(from), this);
    toEdit_ = new QLineEdit(ViewsPanel::clockText(to), this);
    fromEdit_->setPlaceholderText("hh:mm");
    toEdit_->setPlaceholderText("hh:mm");
    form->addRow("From:", fromEdit_);
    form->addRow("To:", toEdit_);

    stepCombo_ = new QComboBox(this);
    stepCombo_->addItem("15 min", 15 * 60);
    stepCombo_->addItem("30 min", 30 * 60);
    stepCombo_->addItem("1 h", 3600);
    stepCombo_->addItem("2 h", 2 * 3600);
    stepCombo_->addItem("3 h", 3 * 3600);
    stepCombo_->setCurrentIndex(2);
    form->addRow("Every:", stepCombo_);
    layout->addLayout(form);

    previewLabel_ = new QLabel(this);
    previewLabel_->setWordWrap(true);
    layout->addWidget(previewLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    saveButton_ = buttons->addButton("Save Views", QDialogButtonBox::AcceptRole);
    saveButton_->setIcon(ribbonIcon("bookmark-plus"));
    saveButton_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(nameEdit_, &QLineEdit::textChanged, this, &SaveOverTimeDialog::updateState);
    connect(fromEdit_, &QLineEdit::textChanged, this, &SaveOverTimeDialog::updateState);
    connect(toEdit_, &QLineEdit::textChanged, this, &SaveOverTimeDialog::updateState);
    connect(stepCombo_, &QComboBox::currentIndexChanged, this, &SaveOverTimeDialog::updateState);

    resize(380, sizeHint().height());
    updateState();
}

std::vector<double> SaveOverTimeDialog::times() const {
    const double from = parseClock(fromEdit_->text());
    const double to = parseClock(toEdit_->text());
    const double step = stepCombo_->currentData().toDouble();
    if (from < 0.0 || to < from || step <= 0.0) return {};
    // The times show whole minutes, so a run that starts at 03:30:15 shows
    // "03:30". Up to a minute outside the run is the run's own first or last
    // time.
    if (from < first_ - 60.0 || to > last_ + 60.0) return {};
    std::vector<double> times;
    for (double t = from; t <= to + 0.5; t += step) times.push_back(std::clamp(t, first_, last_));
    if (static_cast<int>(times.size()) > kMaxViewsOverTime) return {};
    return times;
}

QString SaveOverTimeDialog::name() const {
    return nameEdit_->text().trimmed();
}

void SaveOverTimeDialog::updateState() {
    const double from = parseClock(fromEdit_->text());
    const double to = parseClock(toEdit_->text());
    QString problem;
    if (name().isEmpty()) problem = "Type a name.";
    else if (from < 0.0 || to < 0.0) problem = "Type the times as hh:mm, for example 06:00.";
    else if (to < from) problem = "\"To\" must not be before \"From\".";
    else if (from < first_ - 60.0 || to > last_ + 60.0)
        problem = QString("The run is from %1 to %2.")
            .arg(ViewsPanel::clockText(first_), ViewsPanel::clockText(last_));
    else if (times().empty())
        problem = QString("That is more than %1 views. Use a longer step.").arg(kMaxViewsOverTime);

    if (!problem.isEmpty()) {
        previewLabel_->setStyleSheet("color: #d9534f;");
        previewLabel_->setText(problem);
        saveButton_->setEnabled(false);
        return;
    }
    const std::vector<double> t = times();
    QString list = name() + " · " + ViewsPanel::clockText(t.front());
    if (t.size() > 2) list += ", " + ViewsPanel::clockText(t[1]);
    if (t.size() > 3) list += QString::fromUtf8(", …");
    if (t.size() > 1) list += ", " + ViewsPanel::clockText(t.back());
    previewLabel_->setStyleSheet("color: gray;");
    previewLabel_->setText(QString("%1 view%2: %3")
        .arg(t.size()).arg(t.size() == 1 ? "" : "s").arg(list));
    saveButton_->setEnabled(true);
}

} // namespace simvis
