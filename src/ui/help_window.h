#pragma once

#include <QWidget>

class QLineEdit;
class QTimer;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace simvis {

// ============================================================================
// The user guide. The pages are the Markdown files in help/, built into the
// executable, so the guide always matches the version that shows it and works
// without a network connection. help/index.md is the table of contents: its
// "## Section" lines and "- [Title](page.md)" items make the tree at the left,
// so a new page needs only a new file and a line in index.md.
// ============================================================================

class HelpWindow : public QWidget {
    Q_OBJECT

public:
    explicit HelpWindow(QWidget* parent = nullptr);

    // Show the window with this page, for example "heatmap.md"
    void showPage(const QString& page);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // A "##" or "###" part of a page
    struct Section {
        QString heading;  // as the page shows it
        QString text;     // lower case, for the search
    };
    struct Page {
        QString file;
        QString title;
        QString text;   // lower case, for the search
        QList<Section> sections;
    };

    void loadContents();
    void showContents();
    // The pages that have all the words, most matches first, each with the
    // sections that have all the words
    void search(const QString& text);
    // Open a page at a heading, with the first word of the search selected
    // below it
    void showSection(const QString& file, const QString& heading);
    // Select the next place of the first word of the search, from the cursor
    void findSearchWord();
    void selectCurrentPage();
    void polishPage();
    // Show a picture of the help at full size, in a window of its own
    void showImage(const QString& name);

    QLineEdit* searchEdit_;
    QTreeWidget* tree_;
    QTextBrowser* browser_;
    QToolButton* backButton_;
    QToolButton* forwardButton_;
    // Fits the images again after the window changes size
    QTimer* resizeTimer_;

    // The pages in the order of the table of contents, and the sections that
    // hold them
    QList<Page> pages_;
    QList<QPair<QString, QList<int>>> sections_;
    // The file name of the page on the right
    QString currentPage_;
};

} // namespace simvis
