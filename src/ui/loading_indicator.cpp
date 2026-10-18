#include "loading_indicator.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include <QEvent>
#include <QPainter>
#include <QPainterPath>

namespace simvis {

namespace {

constexpr int kIconSize = 28;
constexpr int kPadding = 16;
constexpr int kHeight = 64;

} // namespace

LoadingIndicator::LoadingIndicator(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();

    // The loader icon recolored to the accent, at the screen's pixel ratio
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
        update();
    });

    parent->installEventFilter(this);
}

void LoadingIndicator::start(const QString& text) {
    setText(text);
    angle_ = 0;
    timer_.start();
    show();
    raise();
}

void LoadingIndicator::setText(const QString& text) {
    text_ = text;
    place();
    update();
}

void LoadingIndicator::stop() {
    timer_.stop();
    hide();
}

void LoadingIndicator::place() {
    const int width = kPadding + kIconSize + 12 + fontMetrics().horizontalAdvance(text_) + kPadding;
    const QWidget* parent = parentWidget();
    setGeometry((parent->width() - width) / 2, (parent->height() - kHeight) / 2, width, kHeight);
}

bool LoadingIndicator::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) place();
    return QWidget::eventFilter(watched, event);
}

void LoadingIndicator::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card: white, thin gray border, rounded like the ribbon buttons
    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.fillPath(card, Qt::white);
    p.setPen(QColor("#DADCE0"));
    p.drawPath(card);

    // Turning icon
    const QPointF center(kPadding + kIconSize / 2.0, height() / 2.0);
    p.save();
    p.translate(center);
    p.rotate(angle_);
    p.drawPixmap(QPointF(-kIconSize / 2.0, -kIconSize / 2.0), spinner_);
    p.restore();

    // What is being read
    p.setPen(QColor("#37474F"));
    p.drawText(QRect(kPadding + kIconSize + 12, 0, width(), height()),
               Qt::AlignLeft | Qt::AlignVCenter, text_);
}

} // namespace simvis
