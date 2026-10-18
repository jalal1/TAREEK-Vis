#include "ribbon.h"
#include "ui/panel_style.h"
#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace simvis {

namespace {

// Three small buttons and their spacing fill the height of one large button.
constexpr int kSmallHeight = 25;
constexpr int kRowSpacing = 2;
constexpr int kLargeHeight = 3 * kSmallHeight + 2 * kRowSpacing;
constexpr int kSmallPerColumn = 3;

const char* kCollapsedKey = "ribbon/collapsed";

QString ribbonStyleSheet() {
    return QString(
        "simvis--RibbonBar { background: #F3F4F6; }"
        "QTabBar { background: transparent; }"
        "QTabBar::tab { background: transparent; border: none; color: #37474F;"
        "  padding: 5px 14px; margin-right: 2px; }"
        "QTabBar::tab:hover:!selected { background: #E3E7EB; }"
        "QTabBar::tab:selected { color: %1; border-bottom: 2px solid %1; }"
        "#ribbonPages, simvis--RibbonPopup { background: #FFFFFF;"
        "  border-top: 1px solid #DADCE0; border-bottom: 1px solid #DADCE0; }"
        "#ribbonGroupTitle { color: #6B7280; font-size: 8pt; }"
        "#ribbonSeparator { color: #E0E3E7; }"
        "QToolButton { background: transparent; border: 1px solid transparent;"
        "  border-radius: 4px; padding: 1px 4px; color: #263238; }"
        "QToolButton:hover { background: #E8F1FB; border-color: #BBD7F5; }"
        "QToolButton:pressed { background: #CFE3F8; }"
        // A checked toggle is filled and framed in the accent color, so the
        // state is visible without hovering.
        "QToolButton:checked { background: #D6E8FA; border-color: %1; }"
        "QToolButton:disabled { color: #9AA0A6; }"
        // The arrow of a large drop-down button goes under the label
        "QToolButton[ribbonMenu=\"large\"] { padding-bottom: 8px; }"
        "QToolButton[ribbonMenu=\"large\"]::menu-indicator {"
        "  subcontrol-origin: padding; subcontrol-position: bottom center; }"
        "QToolButton[ribbonMenu=\"small\"] { padding-right: 14px; }"
    ).arg(panelstyle::kAccent);
}

// "Open &Folder..." -> "Open Folder"
QString plainText(const QAction* action) {
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    if (text.endsWith(QLatin1String("..."))) text.chop(3);
    return text;
}

} // namespace

QIcon ribbonIcon(const QString& name) {
    QIcon icon;
    for (int size : {16, 32, 64}) {
        icon.addFile(QString(":/ribbon/%1-%2.png").arg(name).arg(size),
                     QSize(size, size));
    }
    return icon;
}

// ---------------------------------------------------------------------------
// RibbonGroup
// ---------------------------------------------------------------------------

RibbonGroup::RibbonGroup(const QString& title, QWidget* shortcutOwner, QWidget* parent)
    : QWidget(parent), shortcutOwner_(shortcutOwner)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 2);
    layout->setSpacing(2);

    columns_ = new QHBoxLayout();
    columns_->setSpacing(2);
    layout->addLayout(columns_, 1);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setObjectName("ribbonGroupTitle");
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);
}

QToolButton* RibbonGroup::addLargeAction(QAction* action) {
    smallColumn_ = nullptr;  // small buttons after this start a new column
    auto* button = addButton(action, true);
    columns_->addWidget(button, 0, Qt::AlignTop);
    return button;
}

QToolButton* RibbonGroup::addSmallAction(QAction* action) {
    if (!smallColumn_ || smallCount_ == kSmallPerColumn) {
        smallColumn_ = new QVBoxLayout();
        smallColumn_->setSpacing(kRowSpacing);
        smallColumn_->addStretch(1);  // keeps the buttons at the top
        columns_->addLayout(smallColumn_);
        smallCount_ = 0;
    }
    auto* button = addButton(action, false);
    smallColumn_->insertWidget(smallCount_++, button);
    return button;
}

QToolButton* RibbonGroup::addButton(QAction* action, bool large) {
    // The tooltip names the shortcut, so users learn the keys from the
    // ribbon. It is set on the action, which a menu does not show.
    QString tip = plainText(action);
    if (!action->shortcut().isEmpty()) {
        tip += QString(" (%1)").arg(
            action->shortcut().toString(QKeySequence::NativeText));
    }
    action->setToolTip(tip);

    // A shortcut works only while a widget that holds its action is visible.
    // A button on a hidden tab is not, so the always-visible owner holds the
    // action too.
    if (!action->shortcut().isEmpty()) {
        shortcutOwner_->addAction(action);
    }

    auto* button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    if (action->menu()) {
        button->setPopupMode(QToolButton::InstantPopup);
        button->setProperty("ribbonMenu", large ? "large" : "small");
    }
    if (large) {
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(32, 32));
        button->setFixedHeight(kLargeHeight);
        button->setMinimumWidth(52);
    } else {
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(16, 16));
        button->setFixedHeight(kSmallHeight);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    connect(button, &QToolButton::triggered, this, &RibbonGroup::actionTriggered);
    return button;
}

// ---------------------------------------------------------------------------
// RibbonPage
// ---------------------------------------------------------------------------

RibbonPage::RibbonPage(QWidget* shortcutOwner, QWidget* parent)
    : QWidget(parent), shortcutOwner_(shortcutOwner)
{
    groups_ = new QHBoxLayout(this);
    groups_->setContentsMargins(4, 0, 4, 0);
    groups_->setSpacing(0);
    groups_->addStretch(1);
}

RibbonGroup* RibbonPage::addGroup(const QString& title) {
    auto* group = new RibbonGroup(title, shortcutOwner_, this);
    auto* separator = new QFrame(this);
    separator->setObjectName("ribbonSeparator");
    separator->setFrameShape(QFrame::VLine);

    // Insert before the trailing stretch
    const int at = groups_->count() - 1;
    groups_->insertWidget(at, group);
    groups_->insertWidget(at + 1, separator);

    connect(group, &RibbonGroup::actionTriggered, this, &RibbonPage::actionTriggered);
    return group;
}

// ---------------------------------------------------------------------------
// RibbonPopup
// ---------------------------------------------------------------------------

RibbonPopup::RibbonPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup)
{
    // A click outside closes the popup and goes no further. RibbonBar acts
    // on clicks on its own tabs; a click on the map only closes the popup.
    setAttribute(Qt::WA_NoMouseReplay);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
}

void RibbonPopup::mousePressEvent(QMouseEvent* event) {
    if (!rect().contains(event->position().toPoint())) {
        emit pressedOutside(event->globalPosition().toPoint());
    }
    QFrame::mousePressEvent(event);  // closes the popup on an outside press
}

// ---------------------------------------------------------------------------
// RibbonBar
// ---------------------------------------------------------------------------

RibbonBar::RibbonBar(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setStyleSheet(ribbonStyleSheet());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(2);

    tabRow_ = new QHBoxLayout();
    tabRow_->setContentsMargins(4, 0, 4, 0);
    tabBar_ = new QTabBar(this);
    tabBar_->setDrawBase(false);
    tabBar_->setExpanding(false);
    tabBar_->setFocusPolicy(Qt::NoFocus);
    tabRow_->addWidget(tabBar_);
    tabRow_->addStretch(1);

    collapseAction_ = new QAction("Collapse or Expand the Ribbon", this);
    collapseAction_->setCheckable(true);
    collapseAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F1));
    // The action belongs to the ribbon, which is always visible, so its
    // shortcut works on every tab.
    addAction(collapseAction_);
    connect(collapseAction_, &QAction::toggled, this, &RibbonBar::setCollapsed);

    collapseButton_ = new QToolButton(this);
    collapseButton_->setDefaultAction(collapseAction_);
    collapseButton_->setAutoRaise(true);
    collapseButton_->setIconSize(QSize(16, 16));
    tabRow_->addWidget(collapseButton_);
    layout->addLayout(tabRow_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName("ribbonPages");
    pages_->setAttribute(Qt::WA_StyledBackground);
    // setCollapsed() below puts the pages in the layout or in the popup

    popup_ = new RibbonPopup(this);

    connect(tabBar_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(tabBar_, &QTabBar::tabBarClicked, this, &RibbonBar::onTabClicked);
    connect(tabBar_, &QTabBar::tabBarDoubleClicked, this, [this]() {
        ignoreNextClick_ = !collapsed_;
        setCollapsed(!collapsed_);
    });
    connect(popup_, &RibbonPopup::pressedOutside, this, &RibbonBar::onPopupPressedOutside);

    const bool collapsed = QSettings().value(kCollapsedKey, false).toBool();
    collapsed_ = !collapsed;  // so that the call below sets every part
    setCollapsed(collapsed);
}

QString RibbonBar::currentTabTitle() const {
    return tabBar_->tabText(tabBar_->currentIndex());
}

RibbonPage* RibbonBar::addTab(const QString& title) {
    auto* page = new RibbonPage(this, pages_);
    pages_->addWidget(page);
    tabBar_->addTab(title);
    connect(page, &RibbonPage::actionTriggered, this, &RibbonBar::closePopup);
    return page;
}

void RibbonBar::setSearchWidget(QWidget* widget) {
    // At the right of the tabs, before the collapse button
    const int at = tabRow_->indexOf(collapseButton_);
    tabRow_->insertWidget(at, widget, 0, Qt::AlignVCenter);
    tabRow_->insertSpacing(at + 1, 6);
}

void RibbonBar::setCollapsed(bool collapsed) {
    if (collapsed == collapsed_) return;
    collapsed_ = collapsed;

    if (collapsed) {
        layout()->removeWidget(pages_);
        popup_->layout()->addWidget(pages_);
    } else {
        closePopup();
        popup_->layout()->removeWidget(pages_);
        static_cast<QVBoxLayout*>(layout())->addWidget(pages_);
    }
    pages_->show();

    collapseAction_->setChecked(collapsed);
    collapseAction_->setIcon(ribbonIcon(collapsed ? "chevron-down" : "chevron-up"));
    collapseAction_->setToolTip(QString("%1 the ribbon (%2)")
        .arg(collapsed ? "Expand" : "Collapse",
             collapseAction_->shortcut().toString(QKeySequence::NativeText)));

    QSettings().setValue(kCollapsedKey, collapsed);
}

void RibbonBar::onTabClicked(int index) {
    if (ignoreNextClick_) {
        ignoreNextClick_ = false;
        return;
    }
    if (!collapsed_ || index < 0) return;
    tabBar_->setCurrentIndex(index);
    showPopup();
}

void RibbonBar::onPopupPressedOutside(const QPoint& globalPos) {
    // The popup closes after this; decide what the click means first.
    if (collapseButton_->rect().contains(collapseButton_->mapFromGlobal(globalPos))) {
        QTimer::singleShot(0, this, [this]() { setCollapsed(false); });
        return;
    }
    const int index = tabBar_->tabAt(tabBar_->mapFromGlobal(globalPos));
    if (index < 0) return;
    if (index != tabBar_->currentIndex()) {
        tabBar_->setCurrentIndex(index);
        QTimer::singleShot(0, this, &RibbonBar::showPopup);
    } else if (popupShown_.isValid()
               && popupShown_.elapsed() < QApplication::doubleClickInterval()) {
        // Second click of a double-click on the open tab, as in Word
        QTimer::singleShot(0, this, [this]() { setCollapsed(false); });
    }
}

void RibbonBar::showPopup() {
    const int top = tabBar_->mapTo(this, QPoint(0, tabBar_->height())).y();
    popup_->setFixedWidth(width());
    popup_->adjustSize();
    popup_->move(mapToGlobal(QPoint(0, top)));
    popup_->show();
    popupShown_.start();
}

void RibbonBar::closePopup() {
    popup_->hide();
}

} // namespace simvis
