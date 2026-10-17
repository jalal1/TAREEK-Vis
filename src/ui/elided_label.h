#pragma once

#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionButton>

namespace simvis {

// ============================================================================
// Single-line text that never makes its panel wider than the panel is.
// When the text does not fit, it is cut with "…" and the full text goes to
// the tooltip. The size hint still asks for the full text, so nothing is cut
// when there is room for it. Font, padding and DPI all come from the current
// style, so the result is the same with every Windows style and scale.
// ============================================================================

class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr,
                         Qt::TextElideMode mode = Qt::ElideRight)
        : QLabel(text, parent), fullText_(text), mode_(mode) {}

    QSize sizeHint() const override {
        ensurePolished();
        return QSize(overhead() + fontMetrics().horizontalAdvance(fullText_),
                     QLabel::sizeHint().height());
    }

    QSize minimumSizeHint() const override {
        ensurePolished();
        return QSize(overhead() + fontMetrics().horizontalAdvance(QChar(0x2026)),
                     QLabel::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        updateElision();
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updateElision();
    }

private:
    // Frame, padding and margin around the text
    int overhead() const {
        const QMargins m = contentsMargins();
        return m.left() + m.right() + 2 * margin();
    }

    void updateElision() {
        const int avail = width() - overhead();
        const QFontMetrics fm = fontMetrics();
        const bool fits = fm.horizontalAdvance(fullText_) <= avail;
        const QString shown = fits ? fullText_ : fm.elidedText(fullText_, mode_, avail);
        if (shown != text()) QLabel::setText(shown);
        setToolTip(fits ? QString() : fullText_);
    }

    QString fullText_;
    Qt::TextElideMode mode_;
};

class ElidedButton : public QPushButton {
public:
    explicit ElidedButton(const QString& text, QWidget* parent = nullptr)
        : QPushButton(text, parent), fullText_(text) {
        // Default push-button policy is Minimum, which makes the full text a
        // hard minimum width.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize minimumSizeHint() const override {
        return QSize(3 * fontMetrics().height(), QPushButton::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QPushButton::resizeEvent(event);
        updateElision();
    }

    void changeEvent(QEvent* event) override {
        QPushButton::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updateElision();
    }

private:
    void updateElision() {
        // The text area that the style (or style sheet padding) leaves free
        QStyleOptionButton opt;
        initStyleOption(&opt);
        const int avail =
            style()->subElementRect(QStyle::SE_PushButtonContents, &opt, this).width();
        const QFontMetrics fm = fontMetrics();
        const bool fits = fm.horizontalAdvance(fullText_) <= avail;
        const QString shown = fits ? fullText_
                                   : fm.elidedText(fullText_, Qt::ElideRight, avail);
        if (shown != text()) QPushButton::setText(shown);
        setToolTip(fits ? QString() : fullText_);
    }

    QString fullText_;
};

} // namespace simvis
