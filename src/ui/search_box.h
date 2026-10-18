#pragma once

#include <QLineEdit>
#include <QResizeEvent>

namespace simvis {

// ============================================================================
// The search box in the ribbon's tab row. When there is room it is as wide as
// its long placeholder text. In a small window it gets narrower and shows the
// short placeholder instead, so the text is never cut. Widths come from the
// current font and style, so they hold for every Windows style and scale.
// ============================================================================

class SearchBox : public QLineEdit {
public:
    SearchBox(const QString& longHint, const QString& shortHint, QWidget* parent = nullptr)
        : QLineEdit(parent), longHint_(longHint), shortHint_(shortHint) {
        setClearButtonEnabled(true);
        // Preferred, not Expanding: the box takes its hint width and leaves
        // any extra room to the space beside the tabs
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setPlaceholderText(longHint_);
    }

    QSize sizeHint() const override {
        return QSize(widthFor(longHint_), QLineEdit::sizeHint().height());
    }

    QSize minimumSizeHint() const override {
        return QSize(widthFor(shortHint_), QLineEdit::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLineEdit::resizeEvent(event);
        setPlaceholderText(width() >= widthFor(longHint_) ? longHint_ : shortHint_);
    }

private:
    // Width that shows the text in full: the text plus what the style adds
    // around it (frame, padding, clear button). QLineEdit's own hint is 17
    // 'x' wide plus that extra.
    int widthFor(const QString& text) const {
        ensurePolished();
        const QFontMetrics fm(font());
        const int styleExtra = QLineEdit::sizeHint().width()
                               - 17 * fm.horizontalAdvance(QLatin1Char('x'));
        return styleExtra + fm.horizontalAdvance(text) + 8;
    }

    QString longHint_;
    QString shortHint_;
};

} // namespace simvis
