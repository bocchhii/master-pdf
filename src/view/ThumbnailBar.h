#pragma once

#include <QAbstractScrollArea>
#include <QImage>
#include <QSet>
#include <QThreadPool>

#include <memory>
#include <set>
#include <vector>

class Document;

// The page thumbnails down the left side, like the Python app's (and Acrobat's): click one to
// go to that page; the page you're on is framed. Between the pages (and before the first,
// after the last) a small raised + on an etched line: what can be put in there. Only the
// thumbnails in sight are drawn (on a thread).
class ThumbnailBar : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ThumbnailBar(QWidget *parent = nullptr);
    ~ThumbnailBar() override;

    void setDocument(std::shared_ptr<Document> doc);
    void documentChanged(bool keep = false);  // the pages changed: drawn again (keep: the same pages)
    // The pages the page commands work on: the selected ones, or the page you're on
    std::vector<int> selection() const;
    void select(const std::vector<int> &pages);
    void setCurrent(int page);  // framed, and scrolled into sight
    int current() const { return current_; }
    QRect cellRect(int page) const;  // (in the viewport)
    QRect plusRect(int n) const;     // the + before page n (n = the count: after the last)

signals:
    void pageClicked(int page);
    void plusClicked(int n, QPoint below);  // (where its list goes, on the screen)
    void movePages(const std::vector<int> &pages, int to);  // dragged before page `to`
    void menuRequested(QPoint globalPos);  // right-click: the page commands
    void deleteRequested();  // the Delete key

public slots:
    void plusClosed();  // its list closed: the + pops back up

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void layoutCells();
    int thumbWidth() const;
    int plusY(int n) const;
    void request(int page, QSize size);
    int cellAt(QPoint viewportPos) const;
    // dragging pages into another order, the others making room as you go
    void startDrag();
    std::vector<int> dragOrder() const;
    void slide();
    void paintDrag(QPainter &p);

    std::shared_ptr<Document> doc_;
    std::vector<QRect> cells_;  // each page's picture box, in content coordinates
    std::vector<QImage> images_;
    std::vector<QSize> drawnSize_;  // the size each picture was drawn for
    QSet<int> pending_;
    int serial_ = 0;
    int current_ = -1;
    int plusDown_ = -1;
    int contentH_ = 0;
    std::set<int> selected_;
    int lastClick_ = -1;
    int pressPage_ = -1;
    QPoint pressPos_;  // (content coordinates)
    struct Drag {
        std::vector<int> moving, rest;
        int k = 0;            // the moving pages go before rest[k]
        QPoint grab;          // where the page was held, from its top left
        int y = 0;            // the mouse, in content coordinates
        std::vector<double> shown, from, to;  // each page's top now, and while sliding
        int frame = 0;
        QImage ghost;
    };
    std::unique_ptr<Drag> drag_;
    class QTimer *slideTimer_ = nullptr;
    QThreadPool pool_;
};
