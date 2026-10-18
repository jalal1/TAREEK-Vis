#include "help_window.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSet>
#include <QSplitter>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace simvis {

namespace {

const QString kRoot = QStringLiteral(":/help/");

constexpr int kPageMargin = 16;

constexpr int kFileRole = Qt::UserRole;

QString readPage(const QString& file) {
    QFile f(kRoot + file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString fileOf(const QUrl& url) {
    return url.fileName();
}

} // namespace

HelpWindow::HelpWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle("TAREEK-Vis Help");
    // Wide enough to show the screenshots at full size, if the screen permits
    const QRect screen = (parent ? parent->screen() : QGuiApplication::primaryScreen())
                             ->availableGeometry();
    resize(std::min(1240, screen.width() * 9 / 10), std::min(860, screen.height() * 9 / 10));

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    // Left: search and contents
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(8, 8, 4, 8);
    leftLayout->setSpacing(6);

    searchEdit_ = new QLineEdit(left);
    searchEdit_->setPlaceholderText("Search the help");
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->addAction(ribbonIcon("search"), QLineEdit::LeadingPosition);
    leftLayout->addWidget(searchEdit_);

    tree_ = new QTreeWidget(left);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(14);
    // The page on the right keeps its highlight when the tree has no focus
    tree_->setStyleSheet(QString(
        "QTreeWidget { border: none; }"
        "QTreeWidget::item { padding: 2px 0; }"
        "QTreeWidget::item:selected { background: #D6E8FA; color: %1; }")
        .arg(panelstyle::kAccent));
    leftLayout->addWidget(tree_, 1);

    // Right: the navigation buttons over the page
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(4, 8, 8, 8);
    rightLayout->setSpacing(6);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(2);
    auto makeButton = [right, buttons](const QIcon& icon, const QString& tip) {
        auto* button = new QToolButton(right);
        button->setIcon(icon);
        button->setIconSize(QSize(18, 18));
        button->setAutoRaise(true);
        button->setToolTip(tip);
        buttons->addWidget(button);
        return button;
    };
    backButton_ = makeButton(style()->standardIcon(QStyle::SP_ArrowBack), "Back");
    forwardButton_ = makeButton(style()->standardIcon(QStyle::SP_ArrowForward), "Forward");
    auto* homeButton = makeButton(ribbonIcon("book-open"), "Contents");
    buttons->addStretch(1);
    rightLayout->addLayout(buttons);

    browser_ = new QTextBrowser(right);
    browser_->setOpenExternalLinks(true);
    browser_->setFrameShape(QFrame::NoFrame);
    QFont font = browser_->font();
    font.setPointSizeF(10.5);
    browser_->setFont(font);
    rightLayout->addWidget(browser_, 1);

    // Images fit the width of the page, so it does not scroll to the side.
    // A click on an image shows it at full size.
    browser_->viewport()->setMouseTracking(true);
    browser_->viewport()->installEventFilter(this);
    resizeTimer_ = new QTimer(this);
    resizeTimer_->setSingleShot(true);
    resizeTimer_->setInterval(150);
    connect(resizeTimer_, &QTimer::timeout, this, &HelpWindow::polishPage);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, width() - 260});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);

    backButton_->setEnabled(false);
    forwardButton_->setEnabled(false);
    connect(backButton_, &QToolButton::clicked, browser_, &QTextBrowser::backward);
    connect(forwardButton_, &QToolButton::clicked, browser_, &QTextBrowser::forward);
    connect(homeButton, &QToolButton::clicked, this, [this]() { showPage("index.md"); });
    connect(browser_, &QTextBrowser::backwardAvailable, backButton_, &QToolButton::setEnabled);
    connect(browser_, &QTextBrowser::forwardAvailable, forwardButton_, &QToolButton::setEnabled);
    // The signal comes before source() has the new page, so use its argument
    connect(browser_, &QTextBrowser::sourceChanged, this, [this](const QUrl& url) {
        currentPage_ = fileOf(url);
        polishPage();
        selectCurrentPage();
    });

    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const QString file = item->data(0, kFileRole).toString();
        if (file.isEmpty()) return;
        showPage(file);
        // From a search, go to the first place that has the first word
        const QString words = searchEdit_->text().trimmed();
        if (!words.isEmpty()) {
            browser_->find(words.split(QLatin1Char(' '), Qt::SkipEmptyParts).first());
        }
    });
    connect(searchEdit_, &QLineEdit::textChanged, this, &HelpWindow::search);

    loadContents();
    showContents();
}

void HelpWindow::showPage(const QString& page) {
    if (page != currentPage_) browser_->setSource(QUrl(QStringLiteral("qrc") + kRoot + page));
    show();
    raise();
    activateWindow();
}

bool HelpWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched != browser_->viewport()) return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::Resize) {
        resizeTimer_->start();
        return false;
    }
    if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonRelease)
        return false;

    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPointF onPage = mouse->position() + QPointF(browser_->horizontalScrollBar()->value(),
                                                       browser_->verticalScrollBar()->value());
    const QTextFormat format = browser_->document()->documentLayout()->formatAt(onPage);
    if (!format.isImageFormat()) return false;

    if (event->type() == QEvent::MouseMove) {
        // Handled here, or the browser sets its own cursor again
        browser_->viewport()->setCursor(Qt::PointingHandCursor);
        QToolTip::showText(mouse->globalPosition().toPoint(),
                           "Click to see the image at full size", browser_->viewport());
        return true;
    }
    if (mouse->button() == Qt::LeftButton) {
        showImage(format.toImageFormat().name());
        return true;
    }
    return false;
}

void HelpWindow::showImage(const QString& name) {
    const QPixmap pixmap(kRoot + name);
    if (pixmap.isNull()) return;

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("TAREEK-Vis Help - Image");

    auto* label = new QLabel(dialog);
    label->setPixmap(pixmap);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidget(label);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    // The image at full size; scroll bars only when the screen is too small
    const QRect screen = this->screen()->availableGeometry();
    dialog->resize(std::min(pixmap.width() + 24, screen.width() * 95 / 100),
                   std::min(pixmap.height() + 24, screen.height() * 9 / 10));
    dialog->show();
}

void HelpWindow::loadContents() {
    // index.md: "## Section" starts a section, "- [Title](page.md)" is a page
    static const QRegularExpression item(QStringLiteral("^- \\[(.+)\\]\\((.+\\.md)\\)"));

    pages_.append({"index.md", "Contents", readPage("index.md").toLower()});

    const QStringList lines = readPage("index.md").split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("## "))) {
            sections_.append({line.mid(3).trimmed(), {}});
            continue;
        }
        const QRegularExpressionMatch match = item.match(line.trimmed());
        if (!match.hasMatch() || sections_.isEmpty()) continue;
        const QString file = match.captured(2);
        sections_.last().second.append(pages_.size());
        pages_.append({file, match.captured(1), readPage(file).toLower()});
    }
}

void HelpWindow::showContents() {
    tree_->clear();
    tree_->setRootIsDecorated(false);

    auto* home = new QTreeWidgetItem(tree_, {pages_.first().title});
    home->setData(0, kFileRole, pages_.first().file);

    for (const auto& [title, pageIndices] : sections_) {
        auto* section = new QTreeWidgetItem(tree_, {title});
        section->setFlags(Qt::ItemIsEnabled);
        QFont bold = section->font(0);
        bold.setBold(true);
        section->setFont(0, bold);
        for (int index : pageIndices) {
            auto* page = new QTreeWidgetItem(section, {pages_[index].title});
            page->setData(0, kFileRole, pages_[index].file);
        }
    }
    tree_->expandAll();
    selectCurrentPage();
}

void HelpWindow::search(const QString& text) {
    const QStringList words = text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        showContents();
        return;
    }

    // A page matches when it has all of the words
    tree_->clear();
    for (const Page& page : pages_) {
        bool all = true;
        for (const QString& word : words) {
            if (!page.text.contains(word)) {
                all = false;
                break;
            }
        }
        if (!all) continue;
        auto* item = new QTreeWidgetItem(tree_, {page.title});
        item->setData(0, kFileRole, page.file);
    }
    if (tree_->topLevelItemCount() == 0) {
        auto* none = new QTreeWidgetItem(tree_, {"No pages found"});
        none->setFlags(Qt::NoItemFlags);
    }
    selectCurrentPage();
}

void HelpWindow::selectCurrentPage() {
    const QString& current = currentPage_;
    for (const Page& page : pages_) {
        if (page.file == current) {
            setWindowTitle(QString("TAREEK-Vis Help - %1").arg(page.title));
            break;
        }
    }

    QTreeWidgetItemIterator it(tree_);
    for (; *it; ++it) {
        if ((*it)->data(0, kFileRole).toString() == current) {
            tree_->setCurrentItem(*it);
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
}

void HelpWindow::polishPage() {
    // A Markdown page has no style sheet, so the formats are set here: lines
    // around the tables, a shaded background for code, and images that fit
    QTextDocument* doc = browser_->document();
    doc->setDocumentMargin(kPageMargin);
    // Wider images are shown smaller, so that the page does not scroll to the
    // side. A smaller image goes back to full size when the window gets wider.
    const int maxImageWidth = browser_->viewport()->width() - 2 * kPageMargin - 4;

    const QColor line(QStringLiteral("#DADCE0"));
    const QColor shade(QStringLiteral("#F3F4F6"));
    const QColor title(panelstyle::kAccent);
    const QColor heading(QStringLiteral("#263238"));

    QSet<QTextTable*> tables;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextCursor blockCursor(block);
        if (QTextTable* table = blockCursor.currentTable()) tables.insert(table);

        QTextBlockFormat format = block.blockFormat();
        if (format.headingLevel() > 0) {
            format.setTopMargin(format.headingLevel() == 1 ? 0 : 14);
            format.setBottomMargin(6);
            blockCursor.setBlockFormat(format);
            blockCursor.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat chars;
            chars.setForeground(format.headingLevel() == 1 ? title : heading);
            blockCursor.mergeCharFormat(chars);
        } else if (format.hasProperty(QTextFormat::BlockCodeFence)) {
            format.setBackground(shade);
            blockCursor.setBlockFormat(format);
        }

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            QTextCharFormat chars = fragment.charFormat();
            QTextCursor range(doc);
            range.setPosition(fragment.position());
            range.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);

            if (chars.isImageFormat()) {
                QTextImageFormat image = chars.toImageFormat();
                const QImage source(kRoot + image.name());
                const int width = std::min(source.width(), std::max(maxImageWidth, 200));
                if (!source.isNull() && image.width() != width) {
                    image.setWidth(width);
                    image.setHeight(source.height() * width / double(source.width()));
                    range.setCharFormat(image);
                }
            } else if (chars.fontFixedPitch() && !format.hasProperty(QTextFormat::BlockCodeFence)) {
                QTextCharFormat code;
                code.setBackground(shade);
                range.mergeCharFormat(code);
            }
        }
    }

    // A line under each row and a shaded header row
    for (QTextTable* table : tables) {
        QTextTableFormat format = table->format();
        format.setBorder(0);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(5);
        format.setTopMargin(4);
        format.setBottomMargin(8);
        table->setFormat(format);
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setBottomBorder(1);
                cellFormat.setBottomBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cellFormat.setBottomBorderBrush(line);
                if (row == 0) cellFormat.setBackground(shade);
                cell.setFormat(cellFormat);
            }
        }
    }
    cursor.endEditBlock();
    doc->setModified(false);
}

} // namespace simvis
