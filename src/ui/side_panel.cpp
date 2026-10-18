#include "side_panel.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include <QHBoxLayout>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace simvis {

namespace {

// The tab row matches the ribbon tabs, and the scroll bars are slim so they
// take little of the panel width.
QString sidePanelStyleSheet() {
    return QString(
        "simvis--SidePanel { border-left: 1px solid #DADCE0; }"
        "#sidePanelHeader { background: #F3F4F6; border-bottom: 1px solid #DADCE0; }"
        "#sidePanelHeader QTabBar::tab { background: transparent; border: none;"
        "  color: #37474F; padding: 6px 12px; }"
        "#sidePanelHeader QTabBar::tab:hover:!selected { background: #E3E7EB; }"
        "#sidePanelHeader QTabBar::tab:selected { color: %1; border-bottom: 2px solid %1; }"
        "#sidePanelClose { background: transparent; border: none; border-radius: 4px; }"
        "#sidePanelClose:hover { background: #E3E7EB; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #C4C9CF; border-radius: 4px;"
        "  min-height: 24px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background: #9AA0A6; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
    ).arg(panelstyle::kAccent);
}

} // namespace

SidePanel::SidePanel(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(300);
    setStyleSheet(sidePanelStyleSheet());

    auto* layout = new QVBoxLayout(this);
    // Room for the border at the left
    layout->setContentsMargins(1, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName("sidePanelHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 2, 4, 0);
    headerLayout->setSpacing(0);

    tabBar_ = new QTabBar(header);
    tabBar_->setDrawBase(false);
    tabBar_->setExpanding(false);
    tabBar_->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(tabBar_);
    headerLayout->addStretch(1);

    closeButton_ = new QToolButton(header);
    closeButton_->setObjectName("sidePanelClose");
    closeButton_->setIcon(ribbonIcon("x"));
    closeButton_->setIconSize(QSize(16, 16));
    closeButton_->setToolTip("Close the panel");
    connect(closeButton_, &QToolButton::clicked, this, &SidePanel::closeRequested);
    headerLayout->addWidget(closeButton_, 0, Qt::AlignVCenter);

    layout->addWidget(header);

    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_, 1);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        pages_->setCurrentIndex(index);
        emit stateChanged();
    });
}

int SidePanel::addPage(QWidget* page, const QString& title, bool scrollable) {
    QWidget* shown = page;
    if (scrollable) {
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        shown = scroll;
    }
    pages_->addWidget(shown);
    return tabBar_->addTab(title);
}

void SidePanel::showPage(int index) {
    tabBar_->setCurrentIndex(index);
    show();
}

int SidePanel::currentPage() const {
    return tabBar_->currentIndex();
}

QSize SidePanel::sizeHint() const {
    // The width the panel opens with; the splitter handle changes it
    return QSize(360, QWidget::sizeHint().height());
}

void SidePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emit stateChanged();
}

void SidePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    emit stateChanged();
}

} // namespace simvis
