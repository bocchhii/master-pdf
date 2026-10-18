#pragma once

#include "ui/ClassicWindow.h"

#include <QImage>
#include <QPainterPath>
#include <QTransform>

#include <functional>
#include <tuple>
#include <vector>

class QCheckBox;
class QComboBox;
class QEventLoop;
class QMenu;
class QMenuBar;
class StatusPanel;
class QLabel;
class QSpinBox;
class QTimer;
class ToolButton;

// The picture editor (Edit image...): a picture of the PDF opened in a window of its own, laid out as
// Windows 98's Paint - its menus, its toolbar, its tool box down the left, its colour box along the
// bottom, its status bar - and changed as in Photoshop: its canvas cropped, turned, flipped, resized,
// made bigger or trimmed; parts of it selected (rectangle, ellipse, lasso, magic wand - added to, taken
// from, turned inside out), moved, cut, copied, pasted; drawn on with a pencil or a brush, lines,
// rectangles, ellipses, words; rubbed out to see-through, filled; its colours adjusted (brightness and
// contrast, hue and saturation, blurred, sharpened, sepia, black and white, negative, grey) - then put
// back in the PDF where it was (Save)
class ImageEditor : public ClassicWindow {
    Q_OBJECT
public:
    ImageEditor(QWidget *owner, const QImage &image);
    ~ImageEditor() override;
    bool exec();  // shown over its window until it's closed: true if its changes are to be put in
    QImage image() const { return img_; }
    // where it shows, in the picture's own pixels as it was opened (as shown on the page): cropped - the
    // part kept; turned - turned round its middle; resized - as it was
    QRectF frame() const { return frame_; }

    enum class Tool { Hand, RectSelect, EllipseSelect, Lasso, Wand, Brush, Eraser, Fill, Picker, Crop,
                      Pencil, Line, Rectangle, Ellipse, Text, Zoom, Move };
    void setTool(Tool t);

    // (the canvas's: drawn, and the mouse)
    void paintCanvas(class QPainter &p, const QRect &area);
    void canvasPress(class QMouseEvent *e);
    void canvasMove(class QMouseEvent *e);
    void canvasRelease(class QMouseEvent *e);
    void canvasDoubleClick(class QMouseEvent *e);
    void canvasWheel(class QWheelEvent *e);
    void canvasResized();
    void canvasLeft();

protected:
    void keyPressEvent(class QKeyEvent *e) override;
    void keyReleaseEvent(class QKeyEvent *e) override;
    void closeEvent(class QCloseEvent *e) override;

private:
    struct State {
        QImage img, sel;
        QRectF frame;
    };
    enum class Mode { New, Add, Subtract, Intersect };

    void buildUi();
    void buildMenus(class QVBoxLayout *box);
    void fillMenu(QMenu *m, int which);
    void setColors(QColor fg, QColor bg);  // (the colour box's: the two colours drawn with)
    void refreshTools();
    void refreshStatus(QPointF imagePoint = QPointF(-1, -1));
    void remember();  // (what it is now: one step for Undo)
    void undo();
    void redo();
    void changed(bool fit = false);

    QPointF toImage(QPointF view) const;
    QPointF toView(QPointF image) const;
    void fit();
    void zoomBy(double k, QPointF at);

    // the selection: 0..255 a pixel (null: none - everything)
    void select(const QImage &mask, Mode mode);
    QImage maskOf(const QPainterPath &path) const;
    void selectAll();
    void deselect();
    void invertSelection();
    void outlineSelection();
    int selected(int x, int y) const { return sel_.isNull() ? 255 : sel_.constScanLine(y)[x]; }
    QImage flood(QPoint seed, bool contiguous) const;  // (the pixels like the one clicked)

    // the brush and the eraser: dabs laid down along the stroke, the stroke's cover kept (as
    // Photoshop's: a stroke over itself doesn't build up)
    void dab(QPointF at);
    void strokeTo(QPointF at);
    void paintStroke(const QRect &area);

    void fill(QPoint at);
    void pick(QPoint at);
    void deleteSelected();
    void cropTo(const QRect &r);
    void cropToSelection();
    void applyCrop();
    void turn(bool clockwise);
    void flip(bool across);
    void resizeDialog();
    void replace();
    void invertColors();
    void grayscale();
    // (Adjust: each pixel changed - inside the selection, if there is one)
    QImage adjusted(const QImage &from, const std::function<QRgb(QRgb)> &fn) const;
    void adjust(const std::function<QRgb(QRgb)> &fn);
    void convolve(const float k[9]);  // (blur, sharpen: each pixel from those round it)
    void brightnessDialog();
    void hueDialog();
    bool previewDialog(const QString &title, const std::vector<std::tuple<QString, int, int>> &sliders,
                       const std::function<QImage(const QImage &, const std::vector<int> &)> &apply);
    void sepia();
    void blackAndWhite();
    void rotate180();
    void canvasSizeDialog();
    void trimEdges();
    void exportImage();
    // (the clipboard)
    void copySelection(bool cut);
    void paste();
    QRect selectionBounds() const;
    // (lines, rectangles, ellipses, words: drawn in a layer, then put over the picture)
    void drawShape(QPointF a, QPointF b);
    void putLayer(const QImage &layer);
    void textAt(QPointF at);
    QPointF constrained(QPointF from, QPointF to) const;  // (Shift: square, round, straight)
    int cropHandleAt(QPointF view) const;  // (0..7: tl, t, tr, r, br, b, bl, l; 8 inside; -1 none)
    void save();

    QWidget *owner_;
    QEventLoop *loop_ = nullptr;
    bool accepted_ = false;
    QImage img_, sel_;
    QRectF frame_;
    std::vector<State> undo_, redo_;
    int changes_ = 0;  // (since opened: asked about when closed)

    class Canvas *canvas_ = nullptr;
    double zoom_ = 1;
    QPointF offset_;  // (the picture's top left in the canvas)
    Tool tool_ = Tool::Brush;
    std::vector<std::pair<Tool, ToolButton *>> toolButtons_;
    QWidget *sizeRow_ = nullptr, *hardRow_ = nullptr, *opacityRow_ = nullptr, *tolRow_ = nullptr, *widthRow_ = nullptr,
            *styleRow_ = nullptr;
    QSpinBox *sizeBox_ = nullptr, *hardBox_ = nullptr, *opacityBox_ = nullptr, *tolBox_ = nullptr, *widthBox_ = nullptr;
    QComboBox *styleBox_ = nullptr;  // (Rectangle, Ellipse: outline, outline and fill, fill)
    QCheckBox *contiguous_ = nullptr;
    QMenuBar *menuBar_ = nullptr;
    class ColorBox *colorBox_ = nullptr;
    StatusPanel *hintPanel_ = nullptr, *posPanel_ = nullptr, *sizePanel_ = nullptr, *zoomPanel_ = nullptr;
    QColor color_ = Qt::black, bg_ = Qt::white;  // (the colours drawn with: the left button's, the right's)
    QColor paint_ = Qt::black;  // (the one this stroke uses)
    bool grid_ = false;         // (View > Pixel grid: when zoomed in)

    // the mouse
    bool panning_ = false, spaceDown_ = false;
    QPointF panFrom_, offsetFrom_;
    bool dragging_ = false;
    QPointF dragFrom_, dragTo_;  // (picture pixels)
    std::vector<QPointF> lasso_;
    Mode mode_ = Mode::New;
    QPointF mouse_{-1, -1};  // (the canvas: where the mouse is - the brush's ring)
    // a stroke being painted
    QImage base_, cover_;
    QPointF lastDab_;
    // the crop's frame (picture pixels; empty: none) and what's dragged of it
    QRectF crop_;
    int cropHandle_ = -1;
    QRectF cropWas_;
    // a selection being moved: what's lifted from it, the picture under it, how far it's gone
    QImage lifted_, under_, selWas_;
    QPoint moved_;
    bool floating_ = false;  // (lifted and not put down for good yet: moved again, nothing left behind)
    Qt::MouseButton pressed_ = Qt::NoButton;
    // the selection's outline, its marching ants
    QPainterPath outline_;
    QTimer *ants_ = nullptr;
    int antsPhase_ = 0;
};
