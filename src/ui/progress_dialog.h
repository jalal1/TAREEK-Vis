#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;

namespace simvis {

// ============================================================================
// Progress of preprocessing, as a card in the style of the loading card over
// the map: a turning loader icon, the stages of the work with a mark for each
// (to do, under way, done), a thin bar, the current message and the time
// used. On success it closes by itself (accepted); on failure it stays open
// with the error until the user closes it (rejected).
// ============================================================================

class ProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProgressDialog(QWidget* parent = nullptr);
    ~ProgressDialog() override;

    void setTitle(const QString& title);
    void setProgress(int percent, const QString& message);
    void setCompleted(bool success, const QString& message);

signals:
    void cancelRequested();

public slots:
    // Esc or the window close button cancel the work instead of hiding the
    // dialog while it runs
    void reject() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    // The window has no title bar, so the card itself moves it
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    // Show which stage the percent falls in
    void showStage(int percent);
    void cancel();

    QLabel* iconLabel_;
    QLabel* titleLabel_;
    QLabel* messageLabel_;
    QLabel* percentLabel_;
    QLabel* elapsedLabel_;
    QProgressBar* progressBar_;
    QPushButton* button_;

    // One row per stage: its mark and its name
    struct Stage {
        int firstPercent;  // the stage starts at this percent
        QLabel* mark;
        QLabel* name;
    };
    std::vector<Stage> stages_;

    QPixmap spinner_;  // loader icon in the accent color
    int angle_ = 0;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool running_ = true;
    QPoint dragOffset_;
};

} // namespace simvis
