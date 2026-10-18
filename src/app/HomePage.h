#pragma once

#include <QHash>
#include <QImage>
#include <QWidget>

class QListWidget;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class ToolButton;

// The Home tab - the first thing shown - laid out as Windows 98's Explorer: its toolbar (Open, Star,
// Remove, the views); at the left, Recent and Starred in a pane of their own; at the right, in one white
// box, Explorer's web view panel (the list's name over its coloured line; the PDF picked: its first page,
// size and dates) beside the PDFs - Details, or Thumbnails. Double click (or Enter) opens one in a tab of
// its own.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);

    void refresh();  // (the lists read again: a PDF opened, starred...)
    // (the PDFs opened lately - newest first - and when; starred ones)
    static QStringList recentPaths();
    static void noteOpened(const QString &path);  // (on top of Recent, opened now)
    static QStringList starredPaths();
    static bool starred(const QString &path);
    static void setStarred(const QString &path, bool on);

    // (for the self-test)
    QTreeWidget *list() const { return list_; }
    QListWidget *grid() const { return grid_; }
    int section() const { return section_; }
    void showSection(int which);  // 0: Recent, 1: Starred
    QString summary() const { return summary_; }  // (the status bar's second panel: "4.21MB")

signals:
    void openRequested(const QString &path);
    void openDialogRequested();  // (Open, the panel's "Open a PDF..." link)
    void createRequested();      // (Create: a PDF made from files)
    void scanRequested();        // (Scan: a file opened and scanned)
    void message(const QString &text);  // (for the status bar: "5 object(s)")

protected:
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void fill();
    void setGrid(bool on);
    QStringList picked() const;  // (the PDFs selected, in the view shown)
    void pickedChanged();
    void starPicked();
    void removePicked();
    void menuFor(QPoint at);
    void thumbnailFor(const QString &path);
    QImage thumbnail(const QString &path, QSize size) const;

    QTreeWidget *nav_ = nullptr;
    QSplitter *split_ = nullptr;
    class InfoPanel *info_ = nullptr;
    class BigButton *openButton_ = nullptr, *starButton_ = nullptr, *removeButton_ = nullptr;
    class BigButton *viewsButton_ = nullptr, *clearButton_ = nullptr;
    QWidget *clearSep_ = nullptr;  // (the line before Clear Recent: hidden with it)
    QString summary_;
    QStackedWidget *views_ = nullptr;
    QTreeWidget *list_ = nullptr;
    QListWidget *grid_ = nullptr;
    int section_ = 0;
    bool gridOn_ = false;
    QHash<QString, QImage> thumbs_;  // (each PDF's first page, by its path and when it was saved)
    QHash<QString, bool> asked_;     // (being drawn)
};
