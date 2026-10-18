#include "progress_dialog.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace simvis {

namespace {

constexpr int kIconSize = 28;
constexpr int kMargin = 18;   // room around the card for its shadow
constexpr int kRadius = 10;

const char* kText = "#263238";
const char* kMuted = "#6B7280";
const char* kBorder = "#DADCE0";
const char* kError = "#C62828";

// Stage marks: an empty ring, a filled accent ring, an accent disc with a tick
QString markStyle(const QString& state) {
    if (state == "done")
        return QString("background: %1; color: white; border-radius: 9px;"
                       " font-weight: bold; font-size: 9pt;").arg(panelstyle::kAccent);
    if (state == "active")
        return QString("background: %1; border: 2px solid %2; border-radius: 9px;")
            .arg(panelstyle::kAccentBgTint, panelstyle::kAccent);
    return QString("background: transparent; border: 2px solid %1; border-radius: 9px;")
        .arg(kBorder);
}

// "Parsing events: 3921790 events" -> "Parsing events: 3,921,790 events"
QString groupDigits(QString text) {
    static const QRegularExpression number("\\d{4,}");
    QRegularExpressionMatchIterator it = number.globalMatch(text);
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.prepend(it.next());
    for (const QRegularExpressionMatch& m : matches) {
        text.replace(m.capturedStart(), m.capturedLength(),
                     QLocale().toString(m.captured().toLongLong()));
    }
    return text;
}

} // namespace

ProgressDialog::ProgressDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
{
    setWindowTitle("Preprocessing");
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(440 + 2 * kMargin);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kMargin + 22, kMargin + 20, kMargin + 22, kMargin + 18);
    layout->setSpacing(0);

    // Header: turning loader and the title, a line of explanation below
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    iconLabel_ = new QLabel(this);
    iconLabel_->setFixedSize(kIconSize, kIconSize);
    header->addWidget(iconLabel_, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(2);
    titleLabel_ = new QLabel("Preparing the run", this);
    titleLabel_->setStyleSheet(QString("color: %1; font-size: 13pt; font-weight: 600;").arg(kText));
    auto* subtitle = new QLabel("This is done once. Later opens read the cache.", this);
    subtitle->setStyleSheet(QString("color: %1;").arg(kMuted));
    titles->addWidget(titleLabel_);
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);
    layout->addLayout(header);
    layout->addSpacing(18);

    // Stages, at the percents the preprocessor reports them
    const std::pair<int, const char*> stageNames[] = {
        {0, "Read the network"},
        {50, "Read the events"},
        {90, "Read the transit schedule"},
        {97, "Write the cache"},
    };
    for (const auto& [firstPercent, name] : stageNames) {
        auto* row = new QHBoxLayout();
        row->setSpacing(10);
        auto* mark = new QLabel(this);
        mark->setFixedSize(18, 18);
        mark->setAlignment(Qt::AlignCenter);
        auto* label = new QLabel(name, this);
        row->addWidget(mark);
        row->addWidget(label, 1);
        layout->addLayout(row);
        layout->addSpacing(8);
        stages_.push_back({firstPercent, mark, label});
    }
    layout->addSpacing(10);

    // Thin bar, the message and the percent under it
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setTextVisible(false);
    progressBar_->setFixedHeight(6);
    progressBar_->setStyleSheet(QString(
        "QProgressBar { background: #E8EAED; border: none; border-radius: 3px; }"
        "QProgressBar::chunk { background: %1; border-radius: 3px; }")
        .arg(panelstyle::kAccent));
    layout->addWidget(progressBar_);
    layout->addSpacing(8);

    auto* status = new QHBoxLayout();
    messageLabel_ = new QLabel("Starting...", this);
    messageLabel_->setStyleSheet(QString("color: %1;").arg(kMuted));
    messageLabel_->setMinimumWidth(1);  // a long message must not widen the card
    percentLabel_ = new QLabel("0%", this);
    percentLabel_->setStyleSheet(QString("color: %1; font-weight: 600;").arg(kText));
    status->addWidget(messageLabel_, 1);
    status->addWidget(percentLabel_);
    layout->addLayout(status);
    layout->addSpacing(18);

    // Footer: time used, and Cancel (Close after a failure)
    auto* footer = new QHBoxLayout();
    elapsedLabel_ = new QLabel(this);
    elapsedLabel_->setStyleSheet(QString("color: %1;").arg(kMuted));
    button_ = new QPushButton("Cancel", this);
    button_->setCursor(Qt::PointingHandCursor);
    button_->setStyleSheet(QString(
        "QPushButton { color: %1; background: white; border: 1px solid %2;"
        "  border-radius: 6px; padding: 5px 16px; }"
        "QPushButton:hover { background: %3; border-color: %4; }"
        "QPushButton:disabled { color: %5; }")
        .arg(kText, kBorder, panelstyle::kAccentBgTint, panelstyle::kAccent, kMuted));
    connect(button_, &QPushButton::clicked, this, [this]() {
        if (running_) cancel();
        else QDialog::reject();
    });
    footer->addWidget(elapsedLabel_, 1);
    footer->addWidget(button_);
    layout->addLayout(footer);

    // The loader icon recolored to the accent, as on the loading card
    const qreal dpr = devicePixelRatioF();
    const QPixmap icon = ribbonIcon("loader-circle").pixmap(QSize(kIconSize, kIconSize), dpr);
    spinner_ = QPixmap(icon.size());
    spinner_.setDevicePixelRatio(icon.devicePixelRatio());
    spinner_.fill(Qt::transparent);
    QPainter p(&spinner_);
    p.drawPixmap(0, 0, icon);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(spinner_.rect(), QColor(panelstyle::kAccent));
    p.end();

    timer_.setInterval(30);
    connect(&timer_, &QTimer::timeout, this, [this]() {
        angle_ = (angle_ + 12) % 360;
        QPixmap frame(spinner_.size());
        frame.setDevicePixelRatio(spinner_.devicePixelRatio());
        frame.fill(Qt::transparent);
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(kIconSize / 2.0, kIconSize / 2.0);
        painter.rotate(angle_);
        painter.drawPixmap(-kIconSize / 2, -kIconSize / 2, spinner_);
        painter.end();
        iconLabel_->setPixmap(frame);

        const qint64 seconds = elapsed_.elapsed() / 1000;
        elapsedLabel_->setText(QString("%1:%2 elapsed")
            .arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0')));
    });
    elapsed_.start();
    timer_.start();

    showStage(0);
}

ProgressDialog::~ProgressDialog() = default;

void ProgressDialog::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

void ProgressDialog::setProgress(int percent, const QString& message) {
    if (!running_) return;
    progressBar_->setValue(percent);
    percentLabel_->setText(QString("%1%").arg(percent));
    messageLabel_->setText(groupDigits(message));
    showStage(percent);
}

void ProgressDialog::showStage(int percent) {
    for (size_t i = 0; i < stages_.size(); ++i) {
        const bool started = percent >= stages_[i].firstPercent;
        const bool finished = i + 1 < stages_.size()
            ? percent >= stages_[i + 1].firstPercent
            : percent >= 100;
        const QString state = finished ? "done" : started ? "active" : "todo";
        stages_[i].mark->setStyleSheet(markStyle(state));
        stages_[i].mark->setText(finished ? QString::fromUtf8("✓") : QString());
        stages_[i].name->setStyleSheet(QString("color: %1;%2")
            .arg(state == "todo" ? kMuted : kText,
                 state == "active" ? " font-weight: 600;" : ""));
    }
}

void ProgressDialog::setCompleted(bool success, const QString& message) {
    running_ = false;
    timer_.stop();

    if (success) {
        progressBar_->setValue(100);
        percentLabel_->setText("100%");
        showStage(100);
        titleLabel_->setText("Run ready");
        messageLabel_->setText(message);
        iconLabel_->setPixmap(QPixmap());
        button_->setEnabled(false);
        // Long enough to see every stage ticked off
        QTimer::singleShot(700, this, &QDialog::accept);
        return;
    }

    titleLabel_->setText("Preprocessing failed");
    messageLabel_->setText(message);
    messageLabel_->setStyleSheet(QString("color: %1;").arg(kError));
    progressBar_->setStyleSheet(QString(
        "QProgressBar { background: #E8EAED; border: none; border-radius: 3px; }"
        "QProgressBar::chunk { background: %1; border-radius: 3px; }").arg(kError));
    iconLabel_->setPixmap(QPixmap());
    button_->setEnabled(true);
    button_->setText("Close");
}

void ProgressDialog::cancel() {
    emit cancelRequested();
    button_->setEnabled(false);
    button_->setText("Cancelling...");
}

void ProgressDialog::reject() {
    if (running_) {
        if (button_->isEnabled()) cancel();
        return;
    }
    QDialog::reject();
}

void ProgressDialog::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Soft shadow: rings that grow and fade, a little lower than the card.
    // Painted here, not with a graphics effect, which would blur the text.
    p.setPen(Qt::NoPen);
    for (int i = kMargin; i > 0; i -= 2) {
        const qreal alpha = 22.0 * (1.0 - qreal(i) / kMargin);
        p.setBrush(QColor(0, 0, 0, int(alpha)));
        p.drawRoundedRect(QRectF(rect()).adjusted(kMargin - i, kMargin - i + 4,
                                                  -kMargin + i, -kMargin + i + 4),
                          kRadius + i, kRadius + i);
    }

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(kMargin + 0.5, kMargin + 0.5,
                                                -kMargin - 0.5, -kMargin - 0.5),
                        kRadius, kRadius);
    p.fillPath(card, Qt::white);
    p.setPen(QColor(kBorder));
    p.drawPath(card);
}

void ProgressDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
    QDialog::mousePressEvent(event);
}

void ProgressDialog::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton)
        move(event->globalPosition().toPoint() - dragOffset_);
    QDialog::mouseMoveEvent(event);
}

} // namespace simvis
