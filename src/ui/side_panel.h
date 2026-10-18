#pragma once

#include <QWidget>

class QTabBar;
class QStackedWidget;
class QToolButton;

namespace simvis {

// ============================================================================
// The one panel at the right of the map. Each kind of content (the details of
// what was clicked, the selected links) is a page with a tab at the top, so
// the panels share one place instead of opening beside each other.
// ============================================================================

class SidePanel : public QWidget {
    Q_OBJECT

public:
    explicit SidePanel(QWidget* parent = nullptr);

    // Add a page and return its index. scrollable puts the page in a scroll
    // area, for content that has no scroll bar of its own.
    int addPage(QWidget* page, const QString& title, bool scrollable = false);

    // Show the panel with this page in front
    void showPage(int index);
    int currentPage() const;

    QSize sizeHint() const override;

signals:
    // The panel was shown or hidden, or another page came to the front
    void stateChanged();
    // The user clicked the close button
    void closeRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QTabBar* tabBar_;
    QStackedWidget* pages_;
    QToolButton* closeButton_;
};

} // namespace simvis
