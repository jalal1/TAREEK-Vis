#pragma once

#include <QWidget>
#include <QPixmap>
#include <QTimer>

namespace simvis {

// ============================================================================
// A card over the map while a run loads: a turning loader icon (the Lucide
// set of the ribbon, in the accent color) and a line that says what is being
// read. It stays centered on its parent and lets clicks through.
// ============================================================================

class LoadingIndicator : public QWidget {
    Q_OBJECT

public:
    explicit LoadingIndicator(QWidget* parent);

    // Show the card and start turning
    void start(const QString& text);
    void setText(const QString& text);
    void stop();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void place();

    QTimer timer_;
    int angle_ = 0;
    QString text_;
    QPixmap spinner_;  // loader icon in the accent color
};

} // namespace simvis
