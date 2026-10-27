#pragma once

#include <QWidget>
#include <QFrame>
#include <QIcon>
#include <QElapsedTimer>

class QAction;
class QTabBar;
class QStackedWidget;
class QHBoxLayout;
class QVBoxLayout;
class QToolButton;

namespace simvis {

// Icon from the ribbon resource file (:/ribbon/<name>-<size>.png), with the
// 16, 32 and 64 px files so each button size gets a sharp image.
QIcon ribbonIcon(const QString& name);

// ============================================================================
// A ribbon in place of a menu bar, as in Word: a row of tabs (with the search
// box at the right), and one page of named groups for each tab.
//
// The buttons show actions that the window already owns
// (QToolButton::setDefaultAction). The checked state, the enabled state, the
// action groups and the slots stay in the actions, so the ribbon holds no
// state of its own. An action that has a menu (QMenu::menuAction) becomes a
// drop-down button that shows that same menu, so code that rebuilds the menu
// still works.
//
// Double-click a tab (or press Ctrl+F1) to collapse the ribbon to the tab
// row. A click on a tab then shows its page over the map until the next click.
// ============================================================================

class RibbonGroup : public QWidget {
    Q_OBJECT

public:
    // shortcutOwner holds the actions that have a shortcut (see addButton)
    RibbonGroup(const QString& title, QWidget* shortcutOwner, QWidget* parent);

    // Icon above the text, the full height of the group
    QToolButton* addLargeAction(QAction* action);
    // Icon beside the text. Up to three small buttons share one column.
    QToolButton* addSmallAction(QAction* action);

signals:
    // A button or one of its menu entries was used
    void actionTriggered();

private:
    QToolButton* addButton(QAction* action, bool large);

    QWidget* shortcutOwner_;
    QHBoxLayout* columns_;
    QVBoxLayout* smallColumn_ = nullptr;  // column that is being filled
    int smallCount_ = 0;
};

class RibbonPage : public QWidget {
    Q_OBJECT

public:
    RibbonPage(QWidget* shortcutOwner, QWidget* parent);

    // Groups are added from left to right
    RibbonGroup* addGroup(const QString& title);

signals:
    void actionTriggered();

private:
    QWidget* shortcutOwner_;
    QHBoxLayout* groups_;
};

// Holds the pages while the ribbon is collapsed. It reports a mouse press
// outside itself, so that a click on a tab can switch pages or expand the
// ribbon instead of only closing the popup.
class RibbonPopup : public QFrame {
    Q_OBJECT

public:
    explicit RibbonPopup(QWidget* parent = nullptr);

signals:
    void pressedOutside(const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
};

class RibbonBar : public QWidget {
    Q_OBJECT

public:
    explicit RibbonBar(QWidget* parent = nullptr);

    RibbonPage* addTab(const QString& title);

    // Widget in the tab row, at the right (the search box)
    void setSearchWidget(QWidget* widget);

    // The title of the tab in front, for help on that tab
    QString currentTabTitle() const;

    // Show or hide a tab, for example a tab that only some runs need
    void setTabVisible(const QString& title, bool visible);

    bool isCollapsed() const { return collapsed_; }
    // Checkable action for Ctrl+F1 and the button at the right of the tabs
    QAction* collapseAction() const { return collapseAction_; }

public slots:
    void setCollapsed(bool collapsed);

private:
    void onTabClicked(int index);
    void onPopupPressedOutside(const QPoint& globalPos);
    void showPopup();
    void closePopup();

    QHBoxLayout* tabRow_;
    QTabBar* tabBar_;
    QToolButton* collapseButton_;
    QStackedWidget* pages_;
    RibbonPopup* popup_;
    QAction* collapseAction_;
    bool collapsed_ = false;
    // QTabBar reports a double-click as a click too. After a double-click
    // that collapses the ribbon, that click must not open the popup.
    bool ignoreNextClick_ = false;
    // When the popup opened, to tell a double-click on its tab from two
    // single clicks
    QElapsedTimer popupShown_;
};

} // namespace simvis
