#pragma once

#include <QHash>
#include <QImage>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

class Document;
class PageGrid;
class QLabel;
class QSpinBox;

// Organize Pages (as Acrobat's): every page of the PDF as a big picture in a grid, in place of the pages -
// picked (Ctrl, Shift: several), dragged to a new place; turned left or right, deleted, taken out to a PDF of
// their own, pages put in (blank, or from a file), the picked ones replaced by another file's, the PDF split.
// What it does is done by the window (its hooks); refresh() after the PDF changes.
class OrganizeView : public QWidget {
    Q_OBJECT
public:
    explicit OrganizeView(QWidget *parent = nullptr);

    void setDocument(std::shared_ptr<Document> doc);
    void refresh();  // (the PDF changed: its pages drawn again, the picked ones kept as they can be)
    std::vector<int> selection() const;
    void select(const std::vector<int> &pages);

    std::function<void(const std::vector<int> &, int)> rotate;  // (pages, degrees)
    std::function<void(const std::vector<int> &)> remove, extract, replace;
    std::function<void(const std::vector<int> &, int)> move;  // (pages, before which)
    std::function<void(int)> insertBlank, insertFile;           // (before which)
    std::function<void()> split, close;
    // Create PDF (Home's Create): a new PDF's pages put in order here first - named, then made (create); its Close
    // a Cancel (close: the new PDF thrown away)
    void setCreating(bool on);
    bool creating() const { return creating_; }
    std::function<void(const QString &)> create;  // (the name typed)

protected:
    void keyPressEvent(class QKeyEvent *e) override;

private:
    void fill();
    void picked();
    void drawPage(int page, const QString &key);
    int insertAt() const;  // (after the last picked - else at the end)

    std::shared_ptr<Document> doc_;
    PageGrid *grid_ = nullptr;
    QSpinBox *pageBox_ = nullptr;
    QLabel *count_ = nullptr;
    std::vector<class ToolButton *> needPick_;  // (the buttons for the picked pages: off with none)
    QHash<QString, QImage> drawn_;              // (each page's picture, by the PDF's revision and page)
    QHash<QString, bool> asked_;
    bool creating_ = false;
    QWidget *nameRow_ = nullptr;
    class QLineEdit *nameBox_ = nullptr;
    class QPushButton *createB_ = nullptr, *closeB_ = nullptr;
    QLabel *title_ = nullptr;
};
