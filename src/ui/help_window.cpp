#include "help_window.h"
#include "panel_style.h"
#include "ribbon/ribbon.h"
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
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
constexpr int kHeadingRole = Qt::UserRole + 1;

QString readPage(const QString& file) {
    QFile f(kRoot + file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// The words of Markdown text as the page shows them, in lower case: without
// the targets of links, so that a link to counts.md is not a match for
// "counts"
QString searchText(const QString& markdown) {
    static const QRegularExpression linkTarget(QStringLiteral("\\]\\([^)]*\\)"));
    QString text = markdown.toLower();
    text.replace(linkTarget, QStringLiteral("]"));
    return text;
}

// The "##" and "###" parts of a page: the heading and the search text. The
// text before the first heading is in no part.
QList<QPair<QString, QString>> sectionsOf(const QString& markdown) {
    QList<QPair<QString, QString>> sections;
    QString body;
    bool inCode = false;
    auto close = [&]() {
        if (!sections.isEmpty()) sections.last().second = searchText(body);
        body.clear();
    };
    for (const QString& line : markdown.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("```"))) inCode = !inCode;
        if (!inCode && (line.startsWith(QLatin1String("## ")) ||
                        line.startsWith(QLatin1String("### ")))) {
            close();
            QString heading = line.mid(line.indexOf(QLatin1Char(' ')) + 1).trimmed();
            heading.remove(QLatin1Char('*'));
            heading.remove(QLatin1Char('`'));
            sections.append({heading, QString()});
        }
        body += line + QLatin1Char('\n');
    }
    close();
    return sections;
}

int countOf(const QString& text, const QStringList& words) {
    int count = 0;
    for (const QString& word : words) count += text.count(word);
    return count;
}

bool hasAll(const QString& text, const QStringList& words) {
    return std::all_of(words.begin(), words.end(),
                       [&text](const QString& word) { return text.contains(word); });
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
        const QString heading = item->data(0, kHeadingRole).toString();
        if (!heading.isEmpty()) {
            showSection(file, heading);
            return;
        }
        showPage(file);
        // From a search, go to the first place that has the first word
        browser_->moveCursor(QTextCursor::Start);
        findSearchWord();
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

    pages_.append({"index.md", "Contents", searchText(readPage("index.md")), {}});

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
        const QString markdown = readPage(file);
        Page page{file, match.captured(1), searchText(markdown), {}};
        for (const auto& [heading, text] : sectionsOf(markdown)) {
            page.sections.append({heading, text});
        }
        pages_.append(page);
    }
}

void HelpWindow::showContents() {
    tree_->clear();
    tree_->setColumnCount(1);  // the search shows the matches in a second column
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
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

    // A page matches when it has all of the words. A page with the words in
    // its title comes first, then the page with the most matches.
    struct Hit {
        int page;
        int count;
        bool inTitle;
    };
    std::vector<Hit> hits;
    for (int i = 0; i < pages_.size(); ++i) {
        const Page& page = pages_[i];
        if (!hasAll(page.text, words)) continue;
        hits.push_back({i, countOf(page.text, words), hasAll(page.title.toLower(), words)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.inTitle != b.inTitle) return a.inTitle;
        return a.count > b.count;
    });

    tree_->clear();
    tree_->setColumnCount(2);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto* summary = new QTreeWidgetItem(tree_, {hits.empty()
        ? QString("No pages found for \"%1\"").arg(text.trimmed())
        : QString("%1 page%2 for \"%3\"").arg(hits.size()).arg(hits.size() == 1 ? "" : "s")
              .arg(text.trimmed())});
    summary->setFlags(Qt::NoItemFlags);
    QFont italic = summary->font(0);
    italic.setItalic(true);
    summary->setFont(0, italic);

    for (const Hit& hit : hits) {
        const Page& page = pages_[hit.page];
        auto* item = new QTreeWidgetItem(tree_, {page.title, QString::number(hit.count)});
        item->setData(0, kFileRole, page.file);
        QFont bold = item->font(0);
        bold.setBold(true);
        item->setFont(0, bold);
        item->setForeground(1, QColor(Qt::gray));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, QString("%1 match%2 on this page")
            .arg(hit.count).arg(hit.count == 1 ? "" : "es"));
        for (const Section& section : page.sections) {
            if (!hasAll(section.text, words)) continue;
            auto* child = new QTreeWidgetItem(item, {section.heading});
            child->setData(0, kFileRole, page.file);
            child->setData(0, kHeadingRole, section.heading);
        }
    }
    tree_->expandAll();
    selectCurrentPage();
}

void HelpWindow::showSection(const QString& file, const QString& heading) {
    showPage(file);
    QTextDocument* doc = browser_->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().headingLevel() == 0 || block.text().trimmed() != heading)
            continue;
        browser_->setTextCursor(QTextCursor(block));
        // The heading at the top, and the first match below it selected
        browser_->verticalScrollBar()->setValue(
            static_cast<int>(doc->documentLayout()->blockBoundingRect(block).top()));
        findSearchWord();
        return;
    }
}

void HelpWindow::findSearchWord() {
    const QStringList words = searchEdit_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!words.isEmpty()) browser_->find(words.first());
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
