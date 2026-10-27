#pragma once

#include <QAbstractScrollArea>

class QTextEdit;
class QTextCharFormat;
#include <QColor>
#include <QPointer>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QThreadPool>
#include <QTransform>

#include "core/PageText.h"
#include "core/Document.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <vector>

class Document;
struct Annot;
class QPlainTextEdit;
class QLineEdit;

// Undo / Redo in text being typed - the caret kept where it was when just a look is taken back
void undoInPlace(QTextEdit *box, bool redo);

// The pages, one under the other. Each page is drawn in tiles on background threads -
// only the tiles that can be seen. While new tiles are drawn (after a zoom) the old ones,
// or a small picture of the whole page, are shown stretched, so it never waits.
class PageView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;

    void setDocument(std::shared_ptr<Document> doc);
    // The pages changed: drawn again, the view kept where it was. keep: the same pages (a
    // mark added...) - what's shown stays until the new pictures are drawn (no flicker)
    void documentChanged(bool keep = false);

    // ---- the tools ----
    enum class Tool { Select, Move, Text, Marker, Pen, Eraser, Image, Shape, Sign, Link, Edit };
    struct ToolLook {
        Tool tool = Tool::Select;
        QString variant;  // the eraser's (whiteout, rub), the shape's (rect, ellipse, line, arrow)
        QColor color = Qt::red;
        double width = 2;    // in points (the eraser's: how big it is)
        double opacity = 1;
        QString family = QStringLiteral("Arial");  // (Add text's font)
        bool bold = false, italic = false;
    };
    void setTool(const ToolLook &look);
    // Read mode: nothing changed - text picked and copied only (no annotation picked up, moved or opened)
    void setReadOnly(bool on);
    bool readOnly() const { return readOnly_; }
    const ToolLook &tool() const { return look_; }
    // the annotation picked up (id 0: none)
    void pick(int page, int id);
    int pickedPage() const { return pickPage_; }
    int pickedId() const { return pickId_; }
    int selectionPage() const { return selPage_; }
    const std::vector<QRectF> &selectionBoxes() const { return selBoxes_; }
    // a little toolbar floating over some boxes of a page (nullptr: none)
    void setFloatBar(QWidget *bar, int page, const std::vector<QRectF> &rects);
    QWidget *floatBar() const { return bar_; }
    // a text box's words typed on the page (id 0: a new one at `at`)
    void openTextBox(int page, int id, QPointF at, const QString &text, double size, const QColor &color);
    void closeTextBox(bool commit);

    // the Edit tool: the line picked up (-1: none), the page's lines as it found them
    int pickedLine() const { return lineIndex_; }
    int pickedLinePage() const { return linePage_; }
    void pickLine(int page, int index);
    const std::vector<TextLine> &linesOf(int page);
    // the frame (block of paragraphs) a line is in: its lines' indexes, top down (one: just it)
    std::vector<int> frameLines(int page, int line);
    const std::vector<pagetext::TextFrame> &framesOf(int page);
    // its words to retype where they are, letter by letter - the caret where it was clicked (at, in the
    // view; none: at its end)
    void editLine(int page, int index, QPoint at = QPoint(-1, -1));
    // the line being typed (open): its selected letters - or all of them - restyled; all: whether
    // every one of them has a look
    double pixelsPerPoint() const;  // on screen (logical pixels)
    int lineAt(int page, QPointF at, bool inBox = true);  // (the line there, page points: -1 none - inBox: inside its block's box)
    bool lineEditorOpen() const { return lineEditor_ != nullptr; }
    void commitEditor() { closeLineEditor(true); }
    QRectF pickedBox() { return pickedTextOutline().boundingRect(); }
    QPolygonF pickedOutline() { return pickedTextOutline(); }  // (its corners in the view, as shown)
    QPointF turnHandle() { return lineTurnHandle(); }  // (where its rotate handle is, in the view)  // (what's picked up: its box in the view)  // (what's typed put in, as clicking away does)
    QVariant editorValue(const QString &key) const;  // (the first letter selected's: "scale", "track")
    void styleEditor(const std::function<void(QTextCharFormat &)> &change);
    bool editorAll(const std::function<bool(const QTextCharFormat &)> &test) const;
    QTextEdit *lineEditor() const { return lineEditor_; }
    // (the text bar's: "family", "size" (points), "bold", "italic", "underline", "strike" (on/off),
    // "color" - and "step": each letter a size up (+1) or down (-1) of `sizes`)
    void editorSet(const QString &key, const QVariant &value, const QStringList &sizes = {});
    bool editorHas(const QString &key) const;  // (every letter selected has it on)
    // the block's paragraphs (from 0) the caret or the letters selected are in, while typing in a block
    // (empty: not a block)
    std::vector<int> editorParagraphs() const;
    // (the text bar: clicking there keeps the line being typed open)
    void setEditorKeeper(std::function<bool(QWidget *)> keep) { editorKeeper_ = std::move(keep); }
    // ... and the picture picked up (-1: none)
    int pickedPicture() const { return picIndex_; }
    int pickedPicturePage() const { return picPage_; }
    void pickPicture(int page, int index);
    // several things picked at once (the Edit tool, Ctrl / Shift + click): blocks of text (a line of each) and pictures,
    // on one page - dragged together
    struct GroupItem {
        bool picture = false;
        int index = -1;
        bool operator==(const GroupItem &o) const { return picture == o.picture && index == o.index; }
    };
    const std::vector<GroupItem> &group() const { return group_; }
    int groupPage() const { return groupPage_; }
    void clearGroup();
    // (picked again after a change: the text and pictures at these places - their boxes, page points)
    void pickGroup(int page, const std::vector<QRectF> &texts, const std::vector<QRectF> &pictures);
    QRectF groupBox(int page, const GroupItem &g);  // (one of them: its box, page points)
    // Crop: the picked picture's crop box, its edges and corners dragged; Enter / a click outside
    // puts it in, Esc leaves it as it was
    bool startCrop();
    bool cropping() const { return cropping_; }
    void finishCrop(bool apply);
    QRectF cropBox() const { return cropRect_; }
    void setCropBox(const QRectF &r) { cropRect_ = r; viewport()->update(); }
    // Show bounding boxes: every block of text's and picture's outline at once (the Edit tool)
    void setShowBoxes(bool on);
    bool showBoxes() const { return showBoxes_; }
    const std::vector<Picture> &picturesOf(int page);
    const std::vector<PdfLink> &linksOf(int page);  // (kept until the PDF changes)
    std::shared_ptr<Document> document() const { return doc_; }

    double zoom() const { return zoom_; }  // 1.0 = 100% (actual size)
    void setZoom(double zoom, QPoint anchor = QPoint(-1, -1));
    void zoomBy(double factor, QPoint anchor = QPoint(-1, -1));
    void fitWidth();
    void fitPage();
    enum class Fit { None, Width, Page };
    Fit fit() const { return fit_; }  // (kept fitted when the window changes size)

    int currentPage() const;
    void goToPage(int page);
    void showRect(int page, const QRectF &rect);  // scrolled so it's in view (rect in points)

    // Text selected by dragging across it (double click: a word, triple click: the line)
    QString selectedText() const { return selText_; }
    void clearSelection();
    void copySelection();  // to the clipboard
    // (code-driven: the letters between two points of a page, in points)
    void selectBetween(int page, QPointF a, QPointF b);

    // Find's results tinted on the pages: all of them, and the one you're at (current)
    void setFindHits(std::vector<FindHit> hits, int current);

    // Where a point of the view (viewport pixels) is: a page and the point on it in points,
    // or -1 if not on a page; and back
    int pageAt(QPoint pos, QPointF *at = nullptr) const;
    QPoint toView(int page, QPointF at) const;

signals:
    void pictureCropped(int page, int index, const QRectF &unit);  // (its own square's part, PDF's way up)
    void zoomChanged(double zoom);
    void currentPageChanged(int page);
    void selectionChanged(bool any);
    void message(const QString &text);
    void inkDrawn(int page, const std::vector<QPointF> &points);  // pen / marker (page points)
    void boxDrawn(int page, QPointF a, QPointF b);                 // a shape, a white-out box
    void rubbed(int page, const std::vector<QPointF> &path);       // the eraser went along it
    void annotMoved(int page, int id, QPointF by);
    void picked(int page, int id);     // an annotation picked up (id 0: put down)
    void annotEdit(int page, int id);  // double-clicked: change what it says
    void markClicked(int page, int id);  // a highlight (or other mark) clicked
    void selectionMade();              // text selected (the mouse let go): its toolbar
    void selectionMenu(QPoint globalPos);
    void deletePicked();
    void escaped();                    // Esc with nothing to let go: the tool put down
    void textBoxDone(int page, int id, QPointF at, const QString &text, double size, const QColor &color);
    void linePicked(int page, int index);  // (index -1: put down)
    void lineMoved(int page, int index, QPointF by);
    void lineScaled(int page, int index, double k, bool fromRight);
    // a line's / block's box resized: its sides (page points; -1e9: as it was) - its text wrapped in
    // them again - or its text made k times as big (top / bottom handles)
    void textResized(int page, int index, double left, double right, double k);
    void lineTurned(int page, int index, double degrees);  // (the rotate handle: clockwise as shown)
    void lineEdited(int page, int index, const QString &text);
    void lineRestyled(int page, int index, const std::vector<Document::StyledRun> &runs);  // (letters in looks of their own)
    void frameEdited(int page, int index, const QString &text);  // (a block of paragraphs retyped: one to a line)
    // (... letter by letter; from: the first paragraph changed - those before it as they were)
    // (from: its first changed paragraph; keep: how many at its end are as they were - moved, not written again)
    void frameRestyled(int page, int index, const std::vector<std::vector<Document::StyledRun>> &paras, int from, int keep);
    void lineDelete(int page, int index);
    void editorCaretMoved();  // (the caret moved in the text being typed: the text bar shows its look)
    void picturePicked(int page, int index);  // (index -1: put down)
    void groupChanged(int count);             // (things picked together: how many - 0: none)
    // (the things picked together dragged: their texts' lines and their pictures, by how far - page points)
    void groupMoved(int page, const std::vector<int> &lines, const std::vector<int> &pictures, QPointF by);
    void groupDelete(int page, const std::vector<int> &lines, const std::vector<int> &pictures);
    void pictureChanged(int page, int index, const QTransform &move, const QString &what);
    void pictureDelete(int page, int index);
    void editMenu(int page, QPointF at, QPoint globalPos);  // right-click with the Edit tool
    void imageClicked(int page, QPointF at);  // the Image tool: a click (a box: boxDrawn)
    void signClicked(int page, QPointF at);   // the Sign tool: where the signature goes
    void linkClicked(int page, int index);    // a link clicked (Select, Move): followed
    void linkMenu(int page, int index, QPoint globalPos);  // right-click on a link (Link tool)

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    bool viewportEvent(QEvent *) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    struct Tile {
        QImage image;
        double scale = 0;  // pixels per point it was drawn at
        QRect area;        // in pixels of the whole page at that scale
        int page = 0;
        int gen = 0;
        int pageGen = 0;  // (its page's own: a new one when just that page changed)
        quint64 used = 0;
    };
    // What the background threads may still draw (anything else they skip)
    struct Wanted {
        std::mutex mutex;
        QSet<quint64> keys;
    };

    void layoutPages();
    QRect pageRect(int page) const;  // in content coordinates
    QPoint scrollOffset() const;
    void requestTile(int page, int tx, int ty, double scale, const QRect &area, quint64 key);
    void requestPreview(int page);
    void tileDone(quint64 key, int serial, Tile tile);
    void previewDone(int page, int serial, QImage image);
    void evictTiles();
    void setSelection(int page, TextPick pick);
    void paintOverlays(QPainter &p, int page, const QRect &onScreen);
    void paintTool(QPainter &p);
    void selectPress(QMouseEvent *e);
    void selectMove(QMouseEvent *e);
    void selectRelease(QMouseEvent *e);
    void selectDouble(QMouseEvent *e);
    const Annot *annotAt(int page, QPointF at, bool markups, std::vector<Annot> &store) const;
    QPointF toPage(int page, QPoint viewPos) const;  // (even off the page)
    void placeBar();
    void updateCursor(QPoint pos);
    bool onWord(int page, QPointF at) const;
    void startTextBox(int page, QPointF at);
    // the picked line's / block's handles: 0..7 round it (tl, t, tr, r, br, b, bl, l), 8 its
    // rotate handle, -1 none
    int handleAt(QPoint pos);
    // a text box made taller / shorter by its top / bottom handle (this session): its box as shown -
    // the text's own box if it hasn't been
    QRectF sizedBox(int page, const QRectF &text) const;
    QRectF pickedTextBox();  // (the picked line's text's box: its block's, or its own - page points)
    QRectF draggedBox();     // (what the handle being dragged makes of its box - page points)
    QRectF shownBox(const QRectF &box, bool block);  // (a box as its outline is drawn, in the view)
    // a box's outline on the page (page points) - turned with its lines if they're turned
    QPolygonF frameOutline(int page, const pagetext::TextFrame &f);
    // the text box whose edge is under pos (in the view): one of its lines, or -1 - picked one first
    int edgeAt(QPoint pos, int *page);
    QPolygonF pickedTextOutline();  // (in the view: its box - turned: as shown)
    // a line's gaps far wider than a space (tabs: "1.9.4 -> Database", a bullet's): after which of its
    // words (0: its first), and where the word after it starts (page points)
    std::vector<std::pair<int, double>> wideGaps(int page, const TextLine &line, bool spread = false);
    int textHandle_ = -1;
    QPointF lineTurnHandle();  // (the line picked up's rotate handle, in the view; null if none)
    double lineTurnDegrees() const;  // (how far the rotate handle's been dragged round)
    int pictureAt(int page, QPointF at);
    int overPicture(int page, QPointF at);  // a picture drawn over the text there, or -1
    int linkAt(int page, QPointF at);       // the link there (its place in linksOf), or -1
    int pictureHandleAt(QPoint pos);
    QPointF pictureTurnSpot();  // (the picked picture's rotate handle, in the view)
    // 0..7 round it, 8 the rotate handle  // corners 0-3 (tl, tr, br, bl), 4 the rotate handle, -1 none
    QTransform pictureMove() const;   // what the drag does to the picture picked up
    void editPress(QMouseEvent *e);
    void closeLineEditor(bool commit);
    static quint64 tileKey(int page, int gen, int pageGen, int tx, int ty);
    std::vector<int> pageGen_;
    int pageGen(int page) const { return page >= 0 && page < int(pageGen_.size()) ? pageGen_[size_t(page)] : 0; }

    std::shared_ptr<Document> doc_;
    double zoom_ = 1.0;
    // a pinch (or Ctrl + wheel): the zoom glides to where it's going, frame by frame, not in jumps
    void glideZoom(double factor, QPoint anchor);
    class QTimer *glideTimer_ = nullptr;
    double glideTo_ = 1.0;
    QPoint glideAnchor_;
    bool gliding_ = false;
    Fit fit_ = Fit::Width;  // (a PDF opens fitted to the width)
    int gen_ = 0;  // which zoom the tiles are for: a new one at each zoom
    int serial_ = 0;  // which document: a new one at each file opened
    std::vector<QRect> pages_;
    QSize contentSize_;
    QHash<quint64, Tile> tiles_;
    QSet<quint64> pending_;
    std::vector<QImage> previews_;
    QSet<int> previewPending_;
    quint64 useCounter_ = 0;
    std::shared_ptr<Wanted> wanted_ = std::make_shared<Wanted>();
    QThreadPool pool_;
    int lastPage_ = -1;

    // the text selected
    int selPage_ = -1;
    std::vector<QRectF> selBoxes_;
    QString selText_;
    // a drag across text: where it started
    int pressPage_ = -1;
    QPointF pressAt_;
    QPoint pressPos_;
    bool dragging_ = false;
    qint64 doubleAt_ = 0;  // when the last double click was (a third click: the line)
    QPoint doublePos_;
    // Find's results
    std::vector<FindHit> hits_;
    int hitNow_ = -1;
    std::vector<bool> previewStale_;  // (shown until the new one's drawn)

    // the tools
    ToolLook look_;
    bool readOnly_ = false;
    int pickPage_ = -1, pickId_ = 0;
    enum class Press { None, Select, Pan, Move, Ink, Box, Rub, MoveLine, ScaleLine, SizeText, TurnLine, MovePic, SizePic, TurnPic, MoveGroup } press_ = Press::None;
    int toolPage_ = -1;              // the page the tool is used on
    std::vector<QPointF> stroke_;    // (page points)
    QPointF boxFrom_, boxTo_;        // (page points)
    QPoint panFrom_, panScroll_;
    QPoint moveFrom_, moveTo_;       // (viewport pixels)
    QRectF moveRect_;                // what's moved, as it was (page points)
    int markAt_ = 0;                 // a mark under the press (select tool)
    QPoint hover_{-1000, -1000};
    QPointer<QWidget> bar_;
    int barPage_ = -1;
    std::vector<QRectF> barRects_;
    QPlainTextEdit *editor_ = nullptr;
    // the Edit tool
    int linesPage_ = -1, linesGen_ = -1;
    std::vector<TextLine> lines_;
    int hoverPage_ = -1, hoverIndex_ = -1;
    int linePage_ = -1, lineIndex_ = -1;
    std::map<int, std::vector<std::pair<QRectF, QRectF>>> boxSizes_;  // (each page's: the text's box -> as sized)
    bool edgeMove_ = false;  // (the press was on a box's edge: a drag moves it, a click just picks it)
    bool again_ = false;   // (the press was on the line already picked up: a click retypes it)
    bool scaleRight_ = false;
    QTextEdit *lineEditor_ = nullptr;
    // (where the box being typed in goes, in page points - so a zoom puts it back over its text:
    // its left / right, its first baseline; the zoom it was laid out at; its top in the view)
    double editorLeft_ = 0, editorRight_ = 0, editorBase_ = 0, editorPpp_ = 1;
    int editorTop_ = 0;
    std::function<void()> editorFit_;
    void showEditorCaret();  // (a key pressed in the editor, the page scrolled away from it: brought back to its caret)
    void rescaleEditor();  // (zoomed while typing: its letters, spacing and place at the new zoom)
    std::vector<Document::StyledRun> editorRuns_;  // (the line's runs as it was opened)
    bool editorFrame_ = false;  // (the editor holds a block of paragraphs)
    // while text is edited in place: the page under it drawn without its letters (as Acrobat: no
    // white box), and where
    void holeFor(int page, const std::vector<QRectF> &letters);
    QString shownFamily(const QString &pdfFont, const QString &found);
    // a turned line typed in where it is: the editor kept straight and unseen (it takes the keys,
    // the caret, the selection); its text drawn turned over the page (paintTurnedEditor), the mouse
    // turned back to it (turnedEditorMouse)
    // the eraser: the page as it'd be without the drawings it takes, shown through where it's rubbed
    // (while rubbing - and after, until the page is drawn again without what it took: no flicker)
    QImage rubClean_;
    QRectF rubRect_;  // (page points)
    int rubPage_ = -1, rubKeepGen_ = -1;
    std::vector<QPointF> rubPath_;
    bool rubKept_ = false;
    void paintRubbed(QPainter &p, const std::vector<QPointF> &path);
    bool cropping_ = false;
    QRectF cropRect_;      // (page points)
    int cropHandle_ = -1;  // 0..7: tl, t, tr, r, br, b, bl, l; 8: the box moved
    QPointF cropFrom_;
    QRectF cropWas_;
    bool showBoxes_ = false;
    bool cropMouse(QMouseEvent *e);
    QRectF cropBounds();   // (the whole picture, uncropped)
    bool editorTurned_ = false;
    double editorAngle_ = 0;
    QPointF editorPivot_;  // (page points)
    bool turnedDrag_ = false;
    QTransform editorTurn() const;  // (the view's straight place -> as shown, turned)
    bool turnedEditorMouse(QMouseEvent *e);
    void paintTurnedEditor(QPainter &p, int page);
    QImage hole_;
    QRectF holeRect_;            // (page points)
    std::vector<QRectF> holeLetters_;
    int holePage_ = -1;
    double holeScale_ = 0;
    std::vector<std::vector<Document::StyledRun>> editorParas_;  // (its paragraphs as it was opened)
    std::vector<pagetext::TextFrame> frames_;
    int framesPage_ = -1, framesGen_ = -1;
    // (the boxes Show bounding boxes draws, each page's - as Acrobat boxes its text)
    std::map<int, std::vector<pagetext::TextBox>> boxes_;
    int boxesGen_ = -1;
    const std::vector<pagetext::TextBox> &boxesOf(int page);
    int picsPage_ = -1, picsGen_ = -1;
    std::vector<Picture> pics_;
    std::map<int, std::vector<PdfLink>> links_;
    int linksGen_ = -1, linksRev_ = -1;
    int linkPress_ = -1;  // the link the mouse went down on (Select, Move): followed if it's a click
    int hoverLinkPage_ = -1, hoverLink_ = -1;
    int picPage_ = -1, picIndex_ = -1;
    int picHandle_ = -1;
    std::vector<GroupItem> group_;
    int groupPage_ = -1;
    bool groupClick(QPoint pos);  // (Ctrl / Shift + click: one more picked - or put down)
    // the arrow keys: what's picked moved a point at a press (Shift: ten) - shown at once, put in when the keys stop
    // (one step to Undo, however many presses)
    QPointF nudge_;
    class QTimer *nudgeTimer_ = nullptr;
    bool nudge(QPointF by);  // (false: nothing picked to move)
    void flushNudge();
    int groupAt(int page, QPointF at);  // (which of them is there: -1 none)
    int editorPage_ = -1, editorIndex_ = -1;
    QString editorWas_;
    std::function<bool(QWidget *)> editorKeeper_;
    struct Editing {
        int page = -1, id = 0;
        QPointF at;
        double size = 12;
        QColor color;
        QString was;
    } edit_;
};
