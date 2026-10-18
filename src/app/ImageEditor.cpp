#include "app/ImageEditor.h"

#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/WhatsThis.h"
#include "ui/Widgets.h"
#include "ui/Win98Style.h"

#include <QApplication>
#include <QBitmap>
#include <QBoxLayout>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFontComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSlider>
#include <QCloseEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace {

constexpr int kMaxUndo = 40;

// a number box with a word before it (and its unit after)
QWidget *labelled(QWidget *parent, const QString &label, QSpinBox **box, int lo, int hi, int value, const QString &suffix,
                  const QString &tip)
{
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(6, 0, 0, 0);
    l->setSpacing(3);
    auto *t = new QLabel(label, w);
    t->setFont(theme::font());
    l->addWidget(t);
    *box = new QSpinBox(w);
    (*box)->setRange(lo, hi);
    (*box)->setValue(value);
    (*box)->setSuffix(suffix);
    (*box)->setFont(theme::font());
    (*box)->setToolTip(tip);
    (*box)->setFixedHeight(20);
    l->addWidget(*box);
    return w;
}

// a smooth step down: 1 inside the hard part, to 0 at the edge
double falloff(double t, double hard)
{
    if (t <= hard)
        return 1;
    if (t >= 1)
        return 0;
    const double u = (t - hard) / std::max(1e-6, 1 - hard);
    return 1 - u * u * (3 - 2 * u);
}

}  // namespace

// ---- the canvas: the picture over a checkered see-through ground ----

class Canvas : public QWidget {
public:
    explicit Canvas(ImageEditor *editor, QWidget *parent) : QWidget(parent), ed_(editor)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 240);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

protected:
    void paintEvent(QPaintEvent *e) override
    {
        QPainter p(this);
        ed_->paintCanvas(p, e->rect());
    }
    void mousePressEvent(QMouseEvent *e) override { ed_->canvasPress(e); }
    void mouseMoveEvent(QMouseEvent *e) override { ed_->canvasMove(e); }
    void mouseReleaseEvent(QMouseEvent *e) override { ed_->canvasRelease(e); }
    void mouseDoubleClickEvent(QMouseEvent *e) override { ed_->canvasDoubleClick(e); }
    void wheelEvent(QWheelEvent *e) override { ed_->canvasWheel(e); }
    void resizeEvent(QResizeEvent *) override { ed_->canvasResized(); }
    void leaveEvent(QEvent *) override { ed_->canvasLeft(); }

private:
    ImageEditor *ed_;
};

// ---- the colour box: Paint's, along the bottom ----

// the two colours drawn with (the left button's over the right's), and Windows 98 Paint's 28 beside them:
// a click takes one for the left button, a right click for the right; a double click changes it
class ColorBox : public QWidget {
public:
    static constexpr int kCell = 16;
    explicit ColorBox(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(2 * kCell + 8);
        setWhatsThis(tr("The colours: click one to draw with it (the left button's), right click one for the right "
                        "button's - the big squares at the left show the two. Double click a colour to change it; click "
                        "a big square to choose its colour. X swaps them."));
        colors = {QColor(0, 0, 0),       QColor(128, 128, 128), QColor(128, 0, 0),     QColor(128, 128, 0),   QColor(0, 128, 0),
                  QColor(0, 128, 128),   QColor(0, 0, 128),     QColor(128, 0, 128),   QColor(128, 128, 64),  QColor(0, 64, 64),
                  QColor(0, 128, 255),   QColor(0, 64, 128),    QColor(128, 0, 255),   QColor(128, 64, 0),    QColor(255, 255, 255),
                  QColor(192, 192, 192), QColor(255, 0, 0),     QColor(255, 255, 0),   QColor(0, 255, 0),     QColor(0, 255, 255),
                  QColor(0, 0, 255),     QColor(255, 0, 255),   QColor(255, 255, 128), QColor(0, 255, 128),   QColor(128, 255, 255),
                  QColor(128, 128, 255), QColor(255, 0, 128),   QColor(255, 128, 64)};
    }
    QColor fg = Qt::black, bg = Qt::white;
    std::vector<QColor> colors;
    std::function<void(const QColor &, const QColor &)> chosen;
    std::function<QColor(const QColor &)> ask;  // (the colour dialog)

protected:
    QRect current() const { return QRect(4, 4, 2 * kCell, 2 * kCell); }
    QRect fgBox() const { return QRect(8, 8, 14, 14); }
    QRect bgBox() const { return QRect(16, 16, 14, 14); }
    QRect cell(int i) const { return QRect(4 + 2 * kCell + 6 + (i % 14) * kCell, 4 + (i / 14) * kCell, kCell, kCell); }
    int cellAt(QPoint at) const
    {
        for (int i = 0; i < int(colors.size()); ++i)
            if (cell(i).contains(at))
                return i;
        return -1;
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::face());
        classic::sunkenEdge(p, QRectF(current()), theme::face());
        classic::checkered(p, QRectF(current().adjusted(2, 2, -2, -2)), theme::face(), theme::light());
        classic::raisedEdge(p, QRectF(bgBox()), false, false);
        p.fillRect(bgBox().adjusted(2, 2, -2, -2), bg);
        classic::raisedEdge(p, QRectF(fgBox()), false, false);
        p.fillRect(fgBox().adjusted(2, 2, -2, -2), fg);
        for (int i = 0; i < int(colors.size()); ++i)
            classic::sunkenEdge(p, QRectF(cell(i)), colors[size_t(i)]);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const QPoint at = e->position().toPoint();
        if (fgBox().contains(at) || bgBox().contains(at)) {  // (a big square: its colour chosen)
            const bool front = fgBox().contains(at);
            const QColor c = ask ? ask(front ? fg : bg) : QColor();
            if (c.isValid() && chosen)
                front ? chosen(c, bg) : chosen(fg, c);
            return;
        }
        const int i = cellAt(at);
        if (i < 0 || !chosen)
            return;
        if (e->button() == Qt::RightButton)
            chosen(fg, colors[size_t(i)]);
        else if (e->button() == Qt::LeftButton)
            chosen(colors[size_t(i)], bg);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        const int i = cellAt(e->position().toPoint());
        if (i < 0 || !ask)
            return;
        const QColor c = ask(colors[size_t(i)]);
        if (c.isValid()) {
            colors[size_t(i)] = c;
            update();
            if (chosen)
                e->button() == Qt::RightButton ? chosen(fg, c) : chosen(c, bg);
        }
    }
};

// ---- the window ----

ImageEditor::ImageEditor(QWidget *owner, const QImage &image) : ClassicWindow(true, nullptr), owner_(owner)
{
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setTitle(tr("Edit image"));
    img_ = image.convertToFormat(QImage::Format_ARGB32);
    frame_ = QRectF(0, 0, img_.width(), img_.height());
    buildUi();
    ants_ = new QTimer(this);
    ants_->setInterval(140);
    connect(ants_, &QTimer::timeout, this, [this] {
        antsPhase_ = (antsPhase_ + 1) % 8;
        if (!outline_.isEmpty())
            canvas_->update();
    });
    ants_->start();
    setTool(Tool::Brush);
}

ImageEditor::~ImageEditor() = default;

bool ImageEditor::exec()
{
    // (most of the screen, over its window)
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    resize(std::min(1200, screen.width() * 9 / 10), std::min(860, screen.height() * 9 / 10));
    QWidget *top = owner_ ? owner_->window() : nullptr;
    const QRect over = top && top->isVisible() ? top->frameGeometry() : screen;
    move(over.left() + (over.width() - width()) / 2, std::max(screen.top(), over.top() + (over.height() - height()) / 2));
    show();
    activateWindow();
    canvas_->setFocus();
    QTimer::singleShot(0, this, [this] { fit(); });
    QEventLoop loop;
    loop_ = &loop;
    loop.exec();
    loop_ = nullptr;
    return accepted_;
}

void ImageEditor::buildUi()
{
    auto *box = new QVBoxLayout(body());
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    buildMenus(box);
    auto button = [this](QHBoxLayout *row, const QString &icon, const QString &tip, const QString &label, std::function<void()> fn) {
        auto *b = new ToolButton(icon, tip, {}, false, label, body());
        b->setWhiten(label.isEmpty() ? 16 : 0);
        connect(b, &ToolButton::clicked, this, fn);
        row->addWidget(b);
        return b;
    };
    auto etched = [this, box] {
        auto *l = new QHBoxLayout;
        l->setContentsMargins(2, 0, 2, 0);
        l->addWidget(new EtchedLine(body()));
        box->addLayout(l);
    };
    // the toolbar
    auto *top = new QHBoxLayout;
    top->setContentsMargins(2, 2, 4, 2);
    top->setSpacing(1);
    top->addWidget(new ToolGrip(body()));
    button(top, QStringLiteral("save"), tr("Save into the PDF (Ctrl+S)"), {}, [this] { save(); });
    top->addWidget(new ToolSeparator(body()));
    button(top, QStringLiteral("undo"), tr("Undo (Ctrl+Z)"), {}, [this] { undo(); });
    button(top, QStringLiteral("redo"), tr("Redo (Ctrl+Y)"), {}, [this] { redo(); });
    top->addWidget(new ToolSeparator(body()));
    button(top, QStringLiteral("copy"), tr("Copy what's selected (Ctrl+C)"), {}, [this] { copySelection(false); });
    button(top, {}, tr("Paste - then drag it where it goes (Ctrl+V)"), tr("Paste"), [this] { paste(); });
    top->addWidget(new ToolSeparator(body()));
    button(top, QStringLiteral("rotate_ccw"), tr("Rotate left"), {}, [this] { turn(false); });
    button(top, QStringLiteral("rotate_cw"), tr("Rotate right"), {}, [this] { turn(true); });
    button(top, QStringLiteral("flip_h"), tr("Flip horizontal"), {}, [this] { flip(true); });
    button(top, QStringLiteral("flip_v"), tr("Flip vertical"), {}, [this] { flip(false); });
    top->addWidget(new ToolSeparator(body()));
    button(top, QStringLiteral("zoom_out"), tr("Zoom out (Ctrl+-)"), {}, [this] { zoomBy(0.8, QPointF(canvas_->rect().center())); });
    button(top, QStringLiteral("zoom_in"), tr("Zoom in (Ctrl++)"), {}, [this] { zoomBy(1.25, QPointF(canvas_->rect().center())); });
    button(top, {}, tr("Fit the picture in the window (Ctrl+0)"), tr("Fit"), [this] { fit(); });
    top->addWidget(new ToolSeparator(body()));
    button(top, {}, tr("Select all (Ctrl+A)"), tr("All"), [this] { selectAll(); });
    button(top, {}, tr("Deselect (Ctrl+D)"), tr("None"), [this] { deselect(); });
    button(top, QStringLiteral("eraser"), tr("Delete what's selected - see-through (Delete)"), {}, [this] { deleteSelected(); });
    button(top, QStringLiteral("crop"), tr("Crop to the selection"), {}, [this] { cropToSelection(); });
    top->addStretch(1);
    auto *saveB = new QPushButton(tr("Save"), body());
    saveB->setFont(theme::font());
    saveB->setToolTip(tr("Put the changes in the PDF (Ctrl+S)"));
    connect(saveB, &QPushButton::clicked, this, [this] { save(); });
    top->addWidget(saveB);
    top->addSpacing(4);
    auto *cancelB = new QPushButton(tr("Cancel"), body());
    cancelB->setFont(theme::font());
    connect(cancelB, &QPushButton::clicked, this, [this] { close(); });
    top->addWidget(cancelB);
    box->addLayout(top);
    etched();
    // the tool's options
    auto *opts = new QHBoxLayout;
    opts->setContentsMargins(2, 2, 4, 2);
    opts->setSpacing(0);
    opts->addWidget(new ToolGrip(body()));
    sizeRow_ = labelled(body(), tr("Size:"), &sizeBox_, 1, 1000, 30, tr(" px"), tr("How big the brush is ( [ and ] )"));
    hardRow_ = labelled(body(), tr("Hardness:"), &hardBox_, 0, 100, 50, tr(" %"), tr("Hard edged (100) or soft (0)"));
    widthRow_ = labelled(body(), tr("Line width:"), &widthBox_, 1, 200, 3, tr(" px"), tr("How thick the line or the outline is"));
    styleRow_ = new QWidget(body());
    {
        auto *l = new QHBoxLayout(styleRow_);
        l->setContentsMargins(6, 0, 0, 0);
        l->setSpacing(3);
        auto *t = new QLabel(tr("Style:"), styleRow_);
        t->setFont(theme::font());
        l->addWidget(t);
        styleBox_ = new QComboBox(styleRow_);
        styleBox_->setFont(theme::font());
        styleBox_->addItems({tr("Outline"), tr("Outline and fill"), tr("Fill")});
        styleBox_->setToolTip(tr("Outline: in the left button's colour - the fill in the right's"));
        styleBox_->setFixedHeight(20);
        l->addWidget(styleBox_);
    }
    opacityRow_ = labelled(body(), tr("Opacity:"), &opacityBox_, 1, 100, 100, tr(" %"), tr("How much it paints or rubs out in one stroke"));
    tolRow_ = labelled(body(), tr("Tolerance:"), &tolBox_, 0, 255, 32, QString(), tr("How alike the colours taken are"));
    contiguous_ = new QCheckBox(tr("Contiguous"), tolRow_);
    contiguous_->setFont(theme::font());
    contiguous_->setChecked(true);
    contiguous_->setToolTip(tr("Just the touching pixels like it - else every one in the picture"));
    tolRow_->layout()->addWidget(contiguous_);
    for (QWidget *w : {sizeRow_, hardRow_, widthRow_, styleRow_, opacityRow_, tolRow_})
        opts->addWidget(w);
    opts->addStretch(1);
    auto *optsBox = new QWidget(body());  // (as tall as a toolbar, whatever the tool shows)
    optsBox->setMinimumHeight(ToolButton::H + 4);
    optsBox->setLayout(opts);
    box->addWidget(optsBox);
    etched();
    // the tool box down the left - two by two, as Paint's - and the canvas
    auto *mid = new QHBoxLayout;
    mid->setContentsMargins(2, 2, 2, 2);
    mid->setSpacing(4);
    auto *toolBox = new QWidget(body());
    toolBox->setWhatsThis(tr("The tools: click one, then use it on the picture. Its letter picks it too."));
    auto *grid = new QGridLayout(toolBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(1);
    const std::vector<std::tuple<Tool, QString, QString>> list = {
        {Tool::RectSelect, QStringLiteral("sel_rect"), tr("Rectangle selection (M) - Shift: add, Alt: take away")},
        {Tool::EllipseSelect, QStringLiteral("sel_ellipse"), tr("Ellipse selection - Shift: add, Alt: take away")},
        {Tool::Lasso, QStringLiteral("sel_lasso"), tr("Lasso - draw round what to select (L)")},
        {Tool::Wand, QStringLiteral("wand"), tr("Magic wand - select the colour clicked (W)")},
        {Tool::Move, QStringLiteral("move"), tr("Move - drag what's selected (V)")},
        {Tool::Crop, QStringLiteral("crop"), tr("Crop - drag the frame, Enter puts it in (C)")},
        {Tool::Eraser, QStringLiteral("eraser_tool"), tr("Eraser - rub out to see-through (E)")},
        {Tool::Fill, QStringLiteral("bucket"), tr("Fill with colour - the colour clicked filled (G)")},
        {Tool::Pencil, QStringLiteral("pen"), tr("Pencil - one hard pixel (P)")},
        {Tool::Brush, QStringLiteral("brush"), tr("Brush - paint (B)")},
        {Tool::Text, QStringLiteral("text_tool"), tr("Text - click where the words go (T)")},
        {Tool::Hand, QStringLiteral("hand"), tr("Hand - drag the picture about (H, or hold Space)")},
    };
    int n = 0;
    for (const auto &[t, icon, tip] : list) {
        auto *b = new ToolButton(icon, tip, {}, false, {}, toolBox);
        b->setWhiten(16);
        const Tool tt = t;
        connect(b, &ToolButton::clicked, this, [this, tt] { setTool(tt); });
        grid->addWidget(b, n / 2, n % 2);
        toolButtons_.emplace_back(t, b);
        ++n;
    }
    auto *tools = new QVBoxLayout;
    tools->setSpacing(0);
    tools->addWidget(toolBox);
    tools->addStretch(1);
    mid->addLayout(tools);
    canvas_ = new Canvas(this, body());
    canvas_->setObjectName(QStringLiteral("canvas"));
    canvas_->setWhatsThis(tr("The picture. The wheel zooms; the middle button (or Space) drags it about."));
    mid->addWidget(canvas_, 1);
    box->addLayout(mid, 1);
    // the colour box
    colorBox_ = new ColorBox(body());
    colorBox_->chosen = [this](const QColor &fg, const QColor &bg) { setColors(fg, bg); };
    colorBox_->ask = [this](const QColor &c) { return colorDialog(this, tr("Edit Colors"), c, true); };
    auto *colors = new QHBoxLayout;
    colors->setContentsMargins(2, 0, 2, 0);
    colors->addWidget(colorBox_, 1);
    box->addLayout(colors);
    // the status bar: what to do | where the mouse is | how big | the zoom
    auto *status = new QHBoxLayout;
    status->setContentsMargins(2, 2, 2, 2);
    status->setSpacing(2);
    hintPanel_ = new StatusPanel(body());
    hintPanel_->setMinimumWidth(20);
    status->addWidget(hintPanel_, 1);
    const int w = QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0'));
    posPanel_ = new StatusPanel(body());
    posPanel_->setFixedWidth(w * 12 + 8);
    status->addWidget(posPanel_);
    sizePanel_ = new StatusPanel(body());
    sizePanel_->setFixedWidth(w * 14 + 8);
    status->addWidget(sizePanel_);
    zoomPanel_ = new StatusPanel(body());
    zoomPanel_->setFixedWidth(w * 6 + 8);
    status->addWidget(zoomPanel_);
    box->addLayout(status);
}

void ImageEditor::buildMenus(QVBoxLayout *box)
{
    menuBar_ = new QMenuBar(body());
    menuBar_->setNativeMenuBar(false);
    const QStringList titles = {tr("&File"), tr("&Edit"), tr("&View"), tr("&Image"), tr("&Adjust"), tr("&Help")};
    for (int i = 0; i < titles.size(); ++i) {
        auto *m = new QMenu(titles[i], menuBar_);
        m->setProperty(Win98Style::kIndent, 8);
        connect(m, &QMenu::aboutToShow, this, [this, m, i] {
            m->clear();
            fillMenu(m, i);
        });
        menuBar_->addMenu(m);
    }
    menuBar_->setWhatsThis(tr("The menus: File, Edit, View, Image, Adjust and Help."));
    auto *row = new QHBoxLayout;
    row->setContentsMargins(2, 1, 2, 0);
    row->addWidget(menuBar_);
    box->addLayout(row);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(2, 1, 2, 0);
    line->addWidget(new EtchedLine(body()));
    box->addLayout(line);
}

// (the menus: made each time they open, as the main window's)
void ImageEditor::fillMenu(QMenu *m, int which)
{
    const bool hasSel = !sel_.isNull();
    switch (which) {
    case 0:
        m->addAction(tr("&Save into the PDF"), this, [this] { save(); });
        m->addAction(tr("&Replace with another picture..."), this, [this] { replace(); });
        m->addAction(tr("&Export as a file..."), this, [this] { exportImage(); });
        m->addSeparator();
        m->addAction(tr("&Close"), this, [this] { close(); });
        break;
    case 1:
        m->addAction(tr("&Undo"), this, [this] { undo(); })->setEnabled(!undo_.empty());
        m->addAction(tr("&Redo"), this, [this] { redo(); })->setEnabled(!redo_.empty());
        m->addSeparator();
        m->addAction(tr("Cu&t"), this, [this] { copySelection(true); })->setEnabled(hasSel);
        m->addAction(tr("&Copy"), this, [this] { copySelection(false); });
        m->addAction(tr("&Paste"), this, [this] { paste(); })->setEnabled(!QGuiApplication::clipboard()->image().isNull());
        m->addAction(tr("&Delete what's selected"), this, [this] { deleteSelected(); });
        m->addSeparator();
        m->addAction(tr("Select &All"), this, [this] { selectAll(); });
        m->addAction(tr("&Deselect"), this, [this] { deselect(); })->setEnabled(hasSel);
        m->addAction(tr("&Invert the selection"), this, [this] { invertSelection(); });
        m->addSeparator();
        m->addAction(tr("Swap the &colours"), this, [this] { setColors(bg_, color_); });
        break;
    case 2: {
        m->addAction(tr("Zoom &in"), this, [this] { zoomBy(1.25, QPointF(canvas_->rect().center())); });
        m->addAction(tr("Zoom &out"), this, [this] { zoomBy(0.8, QPointF(canvas_->rect().center())); });
        m->addAction(tr("&Actual size"), this, [this] { zoomBy(1.0 / zoom_, QPointF(canvas_->rect().center())); });
        m->addAction(tr("&Fit in the window"), this, [this] { fit(); });
        m->addSeparator();
        QAction *g = m->addAction(tr("Pixel &grid"), this, [this] {
            grid_ = !grid_;
            canvas_->update();
        });
        g->setCheckable(true);
        g->setChecked(grid_);
        break;
    }
    case 3:
        m->addAction(tr("Rotate &left"), this, [this] { turn(false); });
        m->addAction(tr("Rotate &right"), this, [this] { turn(true); });
        m->addAction(tr("Rotate 1&80 degrees"), this, [this] { rotate180(); });
        m->addSeparator();
        m->addAction(tr("Flip &horizontal"), this, [this] { flip(true); });
        m->addAction(tr("Flip &vertical"), this, [this] { flip(false); });
        m->addSeparator();
        m->addAction(tr("Re&size..."), this, [this] { resizeDialog(); });
        m->addAction(tr("&Canvas size..."), this, [this] { canvasSizeDialog(); });
        m->addAction(tr("Crop to the selec&tion"), this, [this] { cropToSelection(); })->setEnabled(hasSel);
        m->addAction(tr("Trim see-through &edges"), this, [this] { trimEdges(); });
        break;
    case 4:
        m->addAction(tr("&Brightness / Contrast..."), this, [this] { brightnessDialog(); });
        m->addAction(tr("&Hue / Saturation..."), this, [this] { hueDialog(); });
        m->addSeparator();
        m->addAction(tr("B&lur"), this, [this] {
            static const float k[9] = {1 / 16.f, 2 / 16.f, 1 / 16.f, 2 / 16.f, 4 / 16.f, 2 / 16.f, 1 / 16.f, 2 / 16.f, 1 / 16.f};
            convolve(k);
        });
        m->addAction(tr("S&harpen"), this, [this] {
            static const float k[9] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
            convolve(k);
        });
        m->addSeparator();
        m->addAction(tr("&Negative"), this, [this] { invertColors(); });
        m->addAction(tr("&Greyscale"), this, [this] { grayscale(); });
        m->addAction(tr("&Sepia"), this, [this] { sepia(); });
        m->addAction(tr("Black and &white"), this, [this] { blackAndWhite(); });
        break;
    case 5:
        m->addAction(tr("What's &This?"), this, [this] { whatsThis::start(this); });
        break;
    default:
        break;
    }
}

void ImageEditor::setColors(QColor fg, QColor bg)  // (by value: swapped with its own two, safely)
{
    color_ = fg;
    bg_ = bg;
    colorBox_->fg = fg;
    colorBox_->bg = bg;
    colorBox_->update();
}

void ImageEditor::setTool(Tool t)
{
    if (tool_ == Tool::Crop && t != Tool::Crop)
        crop_ = QRectF();
    tool_ = t;
    if (t == Tool::Crop && crop_.isEmpty())
        crop_ = QRectF(0, 0, img_.width(), img_.height());
    refreshTools();
    canvas_->update();
}

void ImageEditor::refreshTools()
{
    for (const auto &[t, b] : toolButtons_)
        b->setLatched(t == tool_);
    const bool paints = tool_ == Tool::Brush || tool_ == Tool::Eraser;
    const bool shape = tool_ == Tool::Line || tool_ == Tool::Rectangle || tool_ == Tool::Ellipse;
    sizeRow_->setVisible(paints);
    hardRow_->setVisible(paints);
    widthRow_->setVisible(shape);
    styleRow_->setVisible(tool_ == Tool::Rectangle || tool_ == Tool::Ellipse);
    opacityRow_->setVisible(paints || shape || tool_ == Tool::Fill || tool_ == Tool::Text);
    tolRow_->setVisible(tool_ == Tool::Wand || tool_ == Tool::Fill);
    QString hint;
    switch (tool_) {
    case Tool::RectSelect:
    case Tool::EllipseSelect:
    case Tool::Lasso:
    case Tool::Wand:
        hint = tr("Shift: add to the selection - Alt: take from it - Shift+Alt: just where they meet");
        break;
    case Tool::Brush:
    case Tool::Eraser:
        hint = tr("Inside the selection only, if there is one. Right button: the right colour. [ ]: smaller / bigger");
        break;
    case Tool::Pencil:
        hint = tr("Draws one pixel wide. Right button: the right colour");
        break;
    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
        hint = tr("Drag to draw it - Shift keeps it straight, square or round. Right button: the colours swapped");
        break;
    case Tool::Text:
        hint = tr("Click where the words go");
        break;
    case Tool::Zoom:
        hint = tr("Click to zoom in there - right click to zoom out");
        break;
    case Tool::Move:
        hint = tr("Drag what's selected to move it (paste first, or select a part)");
        break;
    case Tool::Fill:
        hint = tr("Click to fill - right button: the right colour");
        break;
    case Tool::Picker:
        hint = tr("Click to take the colour - right click: for the right button");
        break;
    case Tool::Crop:
        hint = tr("Drag its edges or a new frame - Enter or a double click crops, Esc leaves it");
        break;
    case Tool::Hand:
        hint = tr("Drag the picture about - the wheel zooms");
        break;
    default:
        break;
    }
    hintPanel_->setText(hint);
    canvas_->setCursor(tool_ == Tool::Hand   ? Qt::OpenHandCursor
                       : paints              ? Qt::BlankCursor
                       : tool_ == Tool::Text ? Qt::IBeamCursor
                       : tool_ == Tool::Move ? Qt::SizeAllCursor
                                             : Qt::CrossCursor);
}

void ImageEditor::refreshStatus(QPointF at)
{
    const bool in = at.x() >= 0 && at.y() >= 0 && at.x() < img_.width() && at.y() < img_.height();
    posPanel_->setText(in ? QStringLiteral("%1, %2").arg(int(at.x())).arg(int(at.y())) : QString());
    sizePanel_->setText(tr("%1 x %2 px").arg(img_.width()).arg(img_.height()));
    zoomPanel_->setText(QStringLiteral("%1%").arg(std::lround(zoom_ * 100)));
}

// ---- Undo ----

void ImageEditor::remember()
{
    floating_ = false;  // (something else done: what was moved stays where it is)
    undo_.push_back({img_, sel_, frame_});
    if (int(undo_.size()) > kMaxUndo)
        undo_.erase(undo_.begin());
    redo_.clear();
}

void ImageEditor::undo()
{
    if (undo_.empty())
        return;
    floating_ = false;
    redo_.push_back({img_, sel_, frame_});
    const QSize was = img_.size();
    img_ = undo_.back().img;
    sel_ = undo_.back().sel;
    frame_ = undo_.back().frame;
    undo_.pop_back();
    --changes_;
    changed(img_.size() != was);
}

void ImageEditor::redo()
{
    if (redo_.empty())
        return;
    floating_ = false;
    undo_.push_back({img_, sel_, frame_});
    const QSize was = img_.size();
    img_ = redo_.back().img;
    sel_ = redo_.back().sel;
    frame_ = redo_.back().frame;
    redo_.pop_back();
    ++changes_;
    changed(img_.size() != was);
}

void ImageEditor::changed(bool refit)
{
    outlineSelection();
    if (tool_ == Tool::Crop)
        crop_ = QRectF(0, 0, img_.width(), img_.height());
    if (refit)
        fit();
    refreshStatus();
    canvas_->update();
}

// ---- the view ----

QPointF ImageEditor::toImage(QPointF v) const
{
    return (v - offset_) / zoom_;
}

QPointF ImageEditor::toView(QPointF i) const
{
    return offset_ + i * zoom_;
}

void ImageEditor::fit()
{
    const QSizeF room = QSizeF(canvas_->size()) - QSizeF(24, 24);
    zoom_ = std::min({room.width() / std::max(1, img_.width()), room.height() / std::max(1, img_.height()), 4.0});
    zoom_ = std::max(zoom_, 0.02);
    offset_ = QPointF((canvas_->width() - img_.width() * zoom_) / 2, (canvas_->height() - img_.height() * zoom_) / 2);
    refreshStatus();
    canvas_->update();
}

void ImageEditor::zoomBy(double k, QPointF at)
{
    const QPointF i = toImage(at);
    zoom_ = std::clamp(zoom_ * k, 0.02, 32.0);
    offset_ = at - i * zoom_;
    refreshStatus(i);
    canvas_->update();
}

void ImageEditor::canvasResized()
{
    if (img_.isNull())
        return;
    canvas_->update();
}

void ImageEditor::canvasLeft()
{
    mouse_ = QPointF(-1, -1);
    canvas_->update();
}

void ImageEditor::paintCanvas(QPainter &p, const QRect &area)
{
    p.fillRect(area, theme::workspace());
    const QRectF shown(offset_, QSizeF(img_.width() * zoom_, img_.height() * zoom_));
    // the see-through ground: grey and white squares
    {
        p.save();
        p.setClipRect(shown.intersected(QRectF(area)));
        const int cell = 8;
        for (int y = int(std::floor(shown.top())); y < shown.bottom(); y += cell)
            for (int x = int(std::floor(shown.left())); x < shown.right(); x += cell)
                p.fillRect(QRect(x, y, cell, cell), ((x - int(shown.left())) / cell + (y - int(shown.top())) / cell) % 2 ? QColor(204, 204, 204) : Qt::white);
        p.restore();
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 2);  // (big: its pixels as squares)
    p.drawImage(shown, img_);
    // the pixel grid (View > Pixel grid): zoomed in, a line round each pixel
    if (grid_ && zoom_ >= 6) {
        p.save();
        p.setClipRect(shown.intersected(QRectF(area)));
        p.setPen(QPen(QColor(128, 128, 128, 140), 0));
        const QPointF a = toImage(QPointF(area.topLeft())), b = toImage(QPointF(area.bottomRight()));
        for (int x = std::max(0, int(a.x())); x <= std::min(img_.width(), int(b.x()) + 1); ++x)
            p.drawLine(QPointF(toView(QPointF(x, 0)).x(), shown.top()), QPointF(toView(QPointF(x, 0)).x(), shown.bottom()));
        for (int y = std::max(0, int(a.y())); y <= std::min(img_.height(), int(b.y()) + 1); ++y)
            p.drawLine(QPointF(shown.left(), toView(QPointF(0, y)).y()), QPointF(shown.right(), toView(QPointF(0, y)).y()));
        p.restore();
    }
    // a line, rectangle or ellipse being drawn: as it will be
    if (dragging_ && (tool_ == Tool::Line || tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        const QColor other = paint_ == color_ ? bg_ : color_;
        const int style = tool_ == Tool::Line ? 0 : styleBox_->currentIndex();
        QPen pen(paint_, std::max(1.0, widthBox_->value() * zoom_), Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
        p.setPen(style == 2 ? Qt::NoPen : pen);
        p.setBrush(style == 1 ? QBrush(other) : style == 2 ? QBrush(paint_) : Qt::NoBrush);
        const QPointF a = toView(dragFrom_), b = toView(dragTo_);
        if (tool_ == Tool::Line)
            p.drawLine(a, b);
        else if (tool_ == Tool::Rectangle)
            p.drawRect(QRectF(a, b).normalized());
        else
            p.drawEllipse(QRectF(a, b).normalized());
        p.restore();
    }
    // the selection's marching ants
    if (!outline_.isEmpty()) {
        QTransform t;
        t.translate(offset_.x(), offset_.y());
        if (dragging_ && tool_ == Tool::Move)  // (the selection moving: its outline with it)
            t.translate(moved_.x() * zoom_, moved_.y() * zoom_);
        t.scale(zoom_, zoom_);
        const QPainterPath shownPath = t.map(outline_);
        p.setRenderHint(QPainter::Antialiasing, false);
        QPen white(Qt::white, 1);
        white.setCosmetic(true);
        p.setPen(white);
        p.setBrush(Qt::NoBrush);
        p.drawPath(shownPath);
        QPen black(Qt::black, 1, Qt::CustomDashLine);
        black.setCosmetic(true);
        black.setDashPattern({4, 4});
        black.setDashOffset(antsPhase_);
        p.setPen(black);
        p.drawPath(shownPath);
    }
    // a selection being drawn
    if (dragging_ && (tool_ == Tool::RectSelect || tool_ == Tool::EllipseSelect || tool_ == Tool::Lasso)) {
        QPen dots(Qt::black, 1, Qt::DashLine);
        dots.setCosmetic(true);
        p.setPen(dots);
        p.setBrush(Qt::NoBrush);
        const QRectF r = QRectF(toView(dragFrom_), toView(dragTo_)).normalized();
        if (tool_ == Tool::RectSelect)
            p.drawRect(r);
        else if (tool_ == Tool::EllipseSelect)
            p.drawEllipse(r);
        else if (lasso_.size() > 1) {
            QPolygonF poly;
            for (const QPointF &q : lasso_)
                poly << toView(q);
            p.drawPolyline(poly);
        }
    }
    // the crop's frame: what's cut off darkened
    if (tool_ == Tool::Crop && !crop_.isEmpty()) {
        const QRectF c(toView(crop_.topLeft()), toView(crop_.bottomRight()));
        QPainterPath outside;
        outside.addRect(shown);
        outside.addRect(c);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawPath(outside);
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(c);
        p.setPen(QPen(Qt::black, 1, Qt::DashLine));
        p.drawRect(c);
        const QPointF h[8] = {c.topLeft(), {c.center().x(), c.top()}, c.topRight(), {c.right(), c.center().y()},
                              c.bottomRight(), {c.center().x(), c.bottom()}, c.bottomLeft(), {c.left(), c.center().y()}};
        p.setPen(Qt::black);
        p.setBrush(Qt::white);
        for (const QPointF &q : h)
            p.drawRect(QRectF(q - QPointF(3, 3), QSizeF(6, 6)));
    }
    // the brush's ring, its size
    if ((tool_ == Tool::Brush || tool_ == Tool::Eraser) && mouse_.x() >= 0) {
        const double r = std::max(1.0, sizeBox_->value() * zoom_ / 2);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawEllipse(mouse_, r + 1, r + 1);
        p.setPen(QPen(Qt::black, 1));
        p.drawEllipse(mouse_, r, r);
        p.drawLine(mouse_ - QPointF(3, 0), mouse_ + QPointF(3, 0));
        p.drawLine(mouse_ - QPointF(0, 3), mouse_ + QPointF(0, 3));
    }
}

// ---- the mouse ----

void ImageEditor::canvasPress(QMouseEvent *e)
{
    canvas_->setFocus();
    const QPointF v = e->position();
    const QPointF at = toImage(v);
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (tool_ == Tool::Hand || spaceDown_))) {
        panning_ = true;
        panFrom_ = v;
        offsetFrom_ = offset_;
        canvas_->setCursor(Qt::ClosedHandCursor);
        return;
    }
    // (the right button: Paint's - drawn with the right colour; the zoom out, the right colour taken)
    const bool right = e->button() == Qt::RightButton;
    const bool rightWorks = tool_ == Tool::Brush || tool_ == Tool::Pencil || tool_ == Tool::Fill || tool_ == Tool::Picker ||
                            tool_ == Tool::Line || tool_ == Tool::Rectangle || tool_ == Tool::Ellipse || tool_ == Tool::Text ||
                            tool_ == Tool::Zoom;
    if (e->button() != Qt::LeftButton && !(right && rightWorks))
        return;
    pressed_ = e->button();
    paint_ = right ? bg_ : color_;
    const Qt::KeyboardModifiers m = e->modifiers();
    mode_ = (m & Qt::ShiftModifier) && (m & Qt::AltModifier) ? Mode::Intersect : (m & Qt::ShiftModifier) ? Mode::Add
                                                                               : (m & Qt::AltModifier)  ? Mode::Subtract
                                                                                                        : Mode::New;
    switch (tool_) {
    case Tool::RectSelect:
    case Tool::EllipseSelect:
        dragging_ = true;
        dragFrom_ = dragTo_ = at;
        break;
    case Tool::Lasso:
        dragging_ = true;
        lasso_ = {at};
        dragFrom_ = dragTo_ = at;
        break;
    case Tool::Wand:
        if (at.x() >= 0 && at.y() >= 0 && at.x() < img_.width() && at.y() < img_.height()) {
            remember();
            select(flood(at.toPoint(), contiguous_->isChecked()), mode_);
            ++changes_;
        }
        break;
    case Tool::Brush:
    case Tool::Pencil:
    case Tool::Eraser:
        remember();
        dragging_ = true;
        base_ = img_.copy();
        cover_ = QImage(img_.size(), QImage::Format_Grayscale8);
        cover_.fill(0);
        lastDab_ = at;
        dab(at);
        break;
    case Tool::Fill:
        if (at.x() >= 0 && at.y() >= 0 && at.x() < img_.width() && at.y() < img_.height())
            fill(at.toPoint());
        break;
    case Tool::Picker:
        if (at.x() >= 0 && at.y() >= 0 && at.x() < img_.width() && at.y() < img_.height()) {
            const QRgb c = img_.pixel(at.toPoint());
            const QColor got(qRed(c), qGreen(c), qBlue(c));
            right ? setColors(color_, got) : setColors(got, bg_);
        }
        break;
    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
        dragging_ = true;
        dragFrom_ = dragTo_ = at;
        break;
    case Tool::Text:
        textAt(at);
        break;
    case Tool::Zoom:
        zoomBy(right ? 0.5 : 2.0, v);
        break;
    case Tool::Move: {
        if (sel_.isNull()) {
            hintPanel_->setText(tr("Select a part first (or paste one) - then drag it"));
            break;
        }
        if (floating_) {  // (moved before: moved on - nothing lifted again, nothing left behind)
            undo_.push_back({img_, sel_, frame_});
            redo_.clear();
        } else {  // (what's selected lifted off the picture: where it was, see-through)
            remember();
            lifted_ = QImage(img_.size(), QImage::Format_ARGB32);
            lifted_.fill(Qt::transparent);
            under_ = img_.copy();
            for (int y = 0; y < img_.height(); ++y) {
                const QRgb *o = reinterpret_cast<const QRgb *>(img_.constScanLine(y));
                QRgb *l = reinterpret_cast<QRgb *>(lifted_.scanLine(y));
                QRgb *u = reinterpret_cast<QRgb *>(under_.scanLine(y));
                for (int x = 0; x < img_.width(); ++x) {
                    const int k = selected(x, y);
                    if (!k)
                        continue;
                    l[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(o[x]) * k / 255);
                    u[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(o[x]) * (255 - k) / 255);
                }
            }
            floating_ = true;
        }
        selWas_ = sel_;
        moved_ = QPoint();
        dragging_ = true;
        dragFrom_ = dragTo_ = at;
        break;
    }
    case Tool::Crop:
        cropHandle_ = cropHandleAt(v);
        if (cropHandle_ == 8 && crop_ == QRectF(0, 0, img_.width(), img_.height()))
            cropHandle_ = -1;  // (the frame round all of it, as it starts: a drag in it draws one)
        cropWas_ = crop_;
        dragging_ = true;
        dragFrom_ = dragTo_ = at;
        if (cropHandle_ < 0)  // (outside the frame: a new one)
            crop_ = QRectF(at, QSizeF(0, 0));
        break;
    default:
        break;
    }
}

void ImageEditor::canvasMove(QMouseEvent *e)
{
    const QPointF v = e->position();
    const QPointF at = toImage(v);
    mouse_ = v;
    refreshStatus(at);
    if (panning_) {
        offset_ = offsetFrom_ + (v - panFrom_);
        canvas_->update();
        return;
    }
    if (tool_ == Tool::Crop && !dragging_) {
        static const Qt::CursorShape shapes[9] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                  Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                  Qt::SizeAllCursor};
        const int h = cropHandleAt(v);
        canvas_->setCursor(h >= 0 ? shapes[h] : Qt::CrossCursor);
    }
    if (!dragging_) {
        if (tool_ == Tool::Brush || tool_ == Tool::Eraser)
            canvas_->update();
        return;
    }
    dragTo_ = (e->modifiers() & Qt::ShiftModifier) && (tool_ == Tool::Line || tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)
                  ? constrained(dragFrom_, at)
                  : at;
    switch (tool_) {
    case Tool::Move: {  // (what's lifted, over the picture under it, where it's dragged to)
        const QPoint d = (at - dragFrom_).toPoint();
        if (d == moved_)
            return;
        moved_ = d;
        img_ = under_.copy();
        QPainter p(&img_);
        p.drawImage(moved_, lifted_);
        break;
    }
    case Tool::Lasso:
        lasso_.push_back(at);
        break;
    case Tool::Brush:
    case Tool::Pencil:
    case Tool::Eraser:
        strokeTo(at);
        break;
    case Tool::Crop: {
        const QRectF bounds(0, 0, img_.width(), img_.height());
        const QPointF d = at - dragFrom_;
        QRectF c = cropWas_;
        if (cropHandle_ < 0) {
            c = QRectF(dragFrom_, at).normalized();
        } else if (cropHandle_ == 8) {
            c.translate(d);
            c.moveLeft(std::clamp(c.left(), 0.0, bounds.width() - c.width()));
            c.moveTop(std::clamp(c.top(), 0.0, bounds.height() - c.height()));
        } else {
            const int h = cropHandle_;
            if (h == 0 || h == 6 || h == 7)
                c.setLeft(std::min(c.right() - 1, c.left() + d.x()));
            if (h == 2 || h == 3 || h == 4)
                c.setRight(std::max(c.left() + 1, c.right() + d.x()));
            if (h == 0 || h == 1 || h == 2)
                c.setTop(std::min(c.bottom() - 1, c.top() + d.y()));
            if (h == 4 || h == 5 || h == 6)
                c.setBottom(std::max(c.top() + 1, c.bottom() + d.y()));
        }
        crop_ = c.intersected(bounds);
        break;
    }
    default:
        break;
    }
    canvas_->update();
}

void ImageEditor::canvasRelease(QMouseEvent *e)
{
    if (panning_) {
        panning_ = false;
        refreshTools();
        return;
    }
    if (!dragging_ || e->button() != pressed_)
        return;
    dragging_ = false;
    switch (tool_) {
    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
        if (QLineF(dragFrom_, dragTo_).length() >= 0.5)
            drawShape(dragFrom_, dragTo_);
        break;
    case Tool::Move: {  // (put down there: it, and the selection, moved for good)
        auto shifted = [](const QImage &im, QPoint d) {
            QImage out(im.size(), im.format());
            out.fill(0);
            for (int y = 0; y < im.height(); ++y) {
                const int ty = y + d.y();
                if (ty < 0 || ty >= im.height())
                    continue;
                const int bpp = im.depth() / 8;
                const uchar *from = im.constScanLine(y);
                uchar *to = out.scanLine(ty);
                const int x0 = std::max(0, -d.x()), x1 = std::min(im.width(), im.width() - d.x());
                if (x1 > x0)
                    std::memcpy(to + (x0 + d.x()) * bpp, from + x0 * bpp, size_t((x1 - x0) * bpp));
            }
            return out;
        };
        lifted_ = shifted(lifted_, moved_);
        sel_ = shifted(selWas_, moved_);
        moved_ = QPoint();
        ++changes_;
        changed();
        break;
    }
    case Tool::RectSelect:
    case Tool::EllipseSelect: {
        const QRectF r = QRectF(dragFrom_, dragTo_).normalized();
        remember();
        if (r.width() < 1 && r.height() < 1) {  // (a click: the selection put down)
            if (mode_ == Mode::New)
                deselect();
            break;
        }
        QPainterPath path;
        if (tool_ == Tool::RectSelect)
            path.addRect(r);
        else
            path.addEllipse(r);
        select(maskOf(path), mode_);
        ++changes_;
        break;
    }
    case Tool::Lasso: {
        if (lasso_.size() < 3)
            break;
        remember();
        QPainterPath path;
        path.addPolygon(QPolygonF(QList<QPointF>(lasso_.begin(), lasso_.end())));
        path.closeSubpath();
        select(maskOf(path), mode_);
        lasso_.clear();
        ++changes_;
        break;
    }
    case Tool::Brush:
    case Tool::Pencil:
    case Tool::Eraser:
        base_ = QImage();
        cover_ = QImage();
        ++changes_;
        break;
    default:
        break;
    }
    canvas_->update();
}

void ImageEditor::canvasDoubleClick(QMouseEvent *e)
{
    if (tool_ == Tool::Crop && cropHandleAt(e->position()) == 8)
        applyCrop();
}

void ImageEditor::canvasWheel(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps != 0)
        zoomBy(std::pow(1.2, steps), e->position());
}

int ImageEditor::cropHandleAt(QPointF v) const
{
    if (crop_.isEmpty())
        return -1;
    const QRectF c(toView(crop_.topLeft()), toView(crop_.bottomRight()));
    const QPointF h[8] = {c.topLeft(), {c.center().x(), c.top()}, c.topRight(), {c.right(), c.center().y()},
                          c.bottomRight(), {c.center().x(), c.bottom()}, c.bottomLeft(), {c.left(), c.center().y()}};
    for (int k = 0; k < 8; ++k)
        if (std::abs(v.x() - h[k].x()) <= 6 && std::abs(v.y() - h[k].y()) <= 6)
            return k;
    return c.contains(v) ? 8 : -1;
}

// ---- the selection ----

QImage ImageEditor::maskOf(const QPainterPath &path) const
{
    QImage m(img_.size(), QImage::Format_Grayscale8);
    m.fill(0);
    QPainter p(&m);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawPath(path);
    return m;
}

void ImageEditor::select(const QImage &mask, Mode mode)
{
    QImage out;
    if (mode == Mode::New || (sel_.isNull() && mode == Mode::Add)) {
        out = mask;
    } else if (sel_.isNull()) {  // (none yet: taking away / where they meet, from all of it)
        out = QImage(img_.size(), QImage::Format_Grayscale8);
        out.fill(255);
    } else {
        out = sel_.copy();
    }
    if (mode != Mode::New && !(sel_.isNull() && mode == Mode::Add)) {
        for (int y = 0; y < out.height(); ++y) {
            uchar *o = out.scanLine(y);
            const uchar *m = mask.constScanLine(y);
            for (int x = 0; x < out.width(); ++x)
                o[x] = mode == Mode::Add ? std::max(o[x], m[x]) : mode == Mode::Subtract ? uchar(o[x] * (255 - m[x]) / 255) : std::min(o[x], m[x]);
        }
    }
    bool any = false;
    for (int y = 0; y < out.height() && !any; ++y) {
        const uchar *o = out.constScanLine(y);
        for (int x = 0; x < out.width() && !any; ++x)
            any = o[x] > 0;
    }
    sel_ = any ? out : QImage();
    outlineSelection();
    refreshStatus();
    canvas_->update();
}

void ImageEditor::selectAll()
{
    remember();
    sel_ = QImage(img_.size(), QImage::Format_Grayscale8);
    sel_.fill(255);
    ++changes_;
    changed();
}

void ImageEditor::deselect()
{
    if (sel_.isNull())
        return;
    sel_ = QImage();
    changed();
}

void ImageEditor::invertSelection()
{
    remember();
    if (sel_.isNull()) {  // (nothing selected: all of it, turned inside out - nothing)
        ++changes_;
        changed();
        return;
    }
    QImage m = sel_.copy();
    for (int y = 0; y < m.height(); ++y) {
        uchar *o = m.scanLine(y);
        for (int x = 0; x < m.width(); ++x)
            o[x] = uchar(255 - o[x]);
    }
    select(m, Mode::New);
    ++changes_;
}

void ImageEditor::outlineSelection()
{
    outline_ = QPainterPath();
    if (sel_.isNull())
        return;
    // (its pixels half selected or more, as a region - their outline)
    QImage mono(sel_.size(), QImage::Format_Mono);
    mono.setColorCount(2);
    mono.setColor(0, qRgb(255, 255, 255));
    mono.setColor(1, qRgb(0, 0, 0));
    mono.fill(0);
    for (int y = 0; y < sel_.height(); ++y) {
        const uchar *s = sel_.constScanLine(y);
        for (int x = 0; x < sel_.width(); ++x)
            if (s[x] >= 128)
                mono.setPixel(x, y, 1);
    }
    const QRegion region(QBitmap::fromImage(mono));
    QPainterPath path;
    path.addRegion(region);
    outline_ = path.simplified();
}

QImage ImageEditor::flood(QPoint seed, bool contiguous) const
{
    QImage m(img_.size(), QImage::Format_Grayscale8);
    m.fill(0);
    const int tol = tolBox_->value();
    const QRgb c0 = img_.pixel(seed);
    auto like = [&](QRgb c) {
        return std::abs(qRed(c) - qRed(c0)) <= tol && std::abs(qGreen(c) - qGreen(c0)) <= tol && std::abs(qBlue(c) - qBlue(c0)) <= tol &&
               std::abs(qAlpha(c) - qAlpha(c0)) <= tol;
    };
    const int w = img_.width(), h = img_.height();
    if (!contiguous) {
        for (int y = 0; y < h; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img_.constScanLine(y));
            uchar *o = m.scanLine(y);
            for (int x = 0; x < w; ++x)
                if (like(line[x]))
                    o[x] = 255;
        }
        return m;
    }
    std::vector<int> stack{seed.y() * w + seed.x()};
    m.scanLine(seed.y())[seed.x()] = 255;
    while (!stack.empty()) {
        const int k = stack.back();
        stack.pop_back();
        const int x = k % w, y = k / w;
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int i = 0; i < 4; ++i) {
            if (nx[i] < 0 || ny[i] < 0 || nx[i] >= w || ny[i] >= h)
                continue;
            uchar &o = m.scanLine(ny[i])[nx[i]];
            if (o)
                continue;
            if (like(img_.pixel(nx[i], ny[i]))) {
                o = 255;
                stack.push_back(ny[i] * w + nx[i]);
            }
        }
    }
    return m;
}

// ---- painting ----

void ImageEditor::dab(QPointF at)
{
    const bool pencil = tool_ == Tool::Pencil;
    const double r = pencil ? 0.5 : std::max(0.5, sizeBox_->value() / 2.0);
    const double hard = pencil ? 1.0 : hardBox_->value() / 100.0;
    const QRect area = QRectF(at.x() - r - 1, at.y() - r - 1, 2 * r + 2, 2 * r + 2).toAlignedRect().intersected(cover_.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        uchar *c = cover_.scanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            const double d = std::hypot(x + 0.5 - at.x(), y + 0.5 - at.y()) / r;
            const int v = int(std::lround(255 * falloff(d, hard)));
            if (v > c[x])
                c[x] = uchar(v);
        }
    }
    paintStroke(area);
}

void ImageEditor::strokeTo(QPointF at)
{
    const double spacing = tool_ == Tool::Pencil ? 0.5 : std::max(1.0, sizeBox_->value() * 0.15);
    const QPointF d = at - lastDab_;
    const double len = std::hypot(d.x(), d.y());
    if (len < spacing)
        return;
    const int n = int(len / spacing);
    for (int k = 1; k <= n; ++k)
        dab(lastDab_ + d * (k * spacing / len));
    lastDab_ = lastDab_ + d * (n * spacing / len);
}

void ImageEditor::paintStroke(const QRect &area)
{
    // the stroke's cover x its opacity x the selection there, over the picture as it was at its start
    const double opacity = tool_ == Tool::Pencil ? 1.0 : opacityBox_->value() / 100.0;
    const bool erase = tool_ == Tool::Eraser;
    const int cr = paint_.red(), cg = paint_.green(), cb = paint_.blue();
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb *b = reinterpret_cast<const QRgb *>(base_.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        const uchar *cv = cover_.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            const double c = cv[x] / 255.0 * opacity * selected(x, y) / 255.0;
            if (c <= 0) {
                o[x] = b[x];
                continue;
            }
            const QRgb was = b[x];
            if (erase) {
                o[x] = qRgba(qRed(was), qGreen(was), qBlue(was), int(std::lround(qAlpha(was) * (1 - c))));
            } else {
                const double ba = qAlpha(was) / 255.0, a = c + ba * (1 - c);
                auto mix = [&](int src, int dst) { return a > 0 ? int(std::lround((src * c + dst * ba * (1 - c)) / a)) : 0; };
                o[x] = qRgba(mix(cr, qRed(was)), mix(cg, qGreen(was)), mix(cb, qBlue(was)), int(std::lround(a * 255)));
            }
        }
    }
    canvas_->update(QRectF(toView(area.topLeft()), toView(area.bottomRight() + QPoint(1, 1))).toAlignedRect().adjusted(-2, -2, 2, 2));
}

void ImageEditor::fill(QPoint at)
{
    const QImage region = flood(at, contiguous_->isChecked());
    remember();
    const double opacity = opacityBox_->value() / 100.0;
    for (int y = 0; y < img_.height(); ++y) {
        const uchar *m = region.constScanLine(y);
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        for (int x = 0; x < img_.width(); ++x) {
            if (!m[x])
                continue;
            const double c = opacity * selected(x, y) / 255.0;
            if (c <= 0)
                continue;
            const QRgb was = o[x];
            const double ba = qAlpha(was) / 255.0, a = c + ba * (1 - c);
            auto mix = [&](int src, int dst) { return a > 0 ? int(std::lround((src * c + dst * ba * (1 - c)) / a)) : 0; };
            o[x] = qRgba(mix(paint_.red(), qRed(was)), mix(paint_.green(), qGreen(was)), mix(paint_.blue(), qBlue(was)), int(std::lround(a * 255)));
        }
    }
    ++changes_;
    changed();
}

void ImageEditor::pick(QPoint at)
{
    const QRgb c = img_.pixel(at);
    setColors(QColor(qRed(c), qGreen(c), qBlue(c)), bg_);
}

void ImageEditor::deleteSelected()
{
    // what's selected made see-through (all of it, if nothing is: asked first)
    if (sel_.isNull() && classicMessage(this, tr("Edit image"), tr("Nothing is selected - make the whole picture see-through?"),
                                        {tr("Yes"), tr("No")}) != tr("Yes"))
        return;
    remember();
    for (int y = 0; y < img_.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        for (int x = 0; x < img_.width(); ++x) {
            const int s = selected(x, y);
            if (s)
                o[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(o[x]) * (255 - s) / 255);
        }
    }
    ++changes_;
    changed();
}

// ---- the canvas: cropped, turned, flipped, resized, replaced ----

void ImageEditor::cropTo(const QRect &rIn)
{
    const QRect r = rIn.intersected(img_.rect());
    if (r.isEmpty() || r == img_.rect())
        return;
    remember();
    // (the part kept, where it shows)
    const double sx = frame_.width() / img_.width(), sy = frame_.height() / img_.height();
    frame_ = QRectF(frame_.left() + r.x() * sx, frame_.top() + r.y() * sy, r.width() * sx, r.height() * sy);
    img_ = img_.copy(r);
    if (!sel_.isNull())
        sel_ = sel_.copy(r);
    ++changes_;
    changed(true);
}

void ImageEditor::cropToSelection()
{
    if (sel_.isNull()) {
        classicMessage(this, tr("Edit image"), tr("Select the part to keep first (or use the Crop tool)."));
        return;
    }
    int x0 = img_.width(), y0 = img_.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < sel_.height(); ++y) {
        const uchar *s = sel_.constScanLine(y);
        for (int x = 0; x < sel_.width(); ++x)
            if (s[x]) {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
    }
    if (x1 >= x0 && y1 >= y0)
        cropTo(QRect(QPoint(x0, y0), QPoint(x1, y1)));
}

void ImageEditor::applyCrop()
{
    if (crop_.isEmpty())
        return;
    const QRect r = crop_.toAlignedRect();
    crop_ = QRectF();
    cropTo(r);
    crop_ = QRectF(0, 0, img_.width(), img_.height());
    canvas_->update();
}

void ImageEditor::turn(bool clockwise)
{
    remember();
    const int w = img_.width(), h = img_.height();
    QTransform t;
    t.rotate(clockwise ? 90 : -90);
    img_ = img_.transformed(t);
    if (!sel_.isNull())
        sel_ = sel_.transformed(t);
    // (where it shows: turned round its middle)
    const QPointF c = frame_.center();
    frame_ = QRectF(c.x() - frame_.height() / 2, c.y() - frame_.width() / 2, frame_.height(), frame_.width());
    (void)w;
    (void)h;
    ++changes_;
    changed(true);
}

void ImageEditor::flip(bool across)
{
    remember();
    img_ = img_.flipped(across ? Qt::Horizontal : Qt::Vertical);
    if (!sel_.isNull())
        sel_ = sel_.flipped(across ? Qt::Horizontal : Qt::Vertical);
    ++changes_;
    changed();
}

void ImageEditor::resizeDialog()
{
    ClassicDialog d(this, tr("Resize"));
    auto *l = new QVBoxLayout(d.body());
    l->setContentsMargins(10, 10, 10, 10);
    QSpinBox *wBox = nullptr, *hBox = nullptr;
    l->addWidget(labelled(d.body(), tr("Width:"), &wBox, 1, 20000, img_.width(), tr(" px"), {}));
    l->addWidget(labelled(d.body(), tr("Height:"), &hBox, 1, 20000, img_.height(), tr(" px"), {}));
    auto *keep = new QCheckBox(tr("Keep its proportions"), d.body());
    keep->setFont(theme::font());
    keep->setChecked(true);
    l->addWidget(keep);
    const double ratio = double(img_.width()) / std::max(1, img_.height());
    bool busy = false;
    QObject::connect(wBox, qOverload<int>(&QSpinBox::valueChanged), &d, [&](int v) {
        if (busy || !keep->isChecked())
            return;
        busy = true;
        hBox->setValue(std::max(1, int(std::lround(v / ratio))));
        busy = false;
    });
    QObject::connect(hBox, qOverload<int>(&QSpinBox::valueChanged), &d, [&](int v) {
        if (busy || !keep->isChecked())
            return;
        busy = true;
        wBox->setValue(std::max(1, int(std::lround(v * ratio))));
        busy = false;
    });
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    bool ok = false;
    auto *okB = new QPushButton(tr("OK"), d.body());
    auto *noB = new QPushButton(tr("Cancel"), d.body());
    QObject::connect(okB, &QPushButton::clicked, &d, [&] {
        ok = true;
        d.close();
    });
    QObject::connect(noB, &QPushButton::clicked, &d, [&] { d.close(); });
    row->addWidget(okB);
    row->addWidget(noB);
    l->addLayout(row);
    d.exec();
    if (!ok || (wBox->value() == img_.width() && hBox->value() == img_.height()))
        return;
    remember();
    const double kx = double(wBox->value()) / img_.width(), ky = double(hBox->value()) / img_.height();
    img_ = img_.scaled(wBox->value(), hBox->value(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (!sel_.isNull())
        sel_ = sel_.scaled(img_.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    (void)kx;  // (where it shows: as it was - just more or fewer pixels)
    (void)ky;
    ++changes_;
    changed(true);
}

void ImageEditor::replace()
{
    QString filters = tr("Pictures") + QStringLiteral(" (");
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        filters += QStringLiteral("*.") + QString::fromLatin1(f) + QLatin1Char(' ');
    filters += QLatin1Char(')');
    const QString path = QFileDialog::getOpenFileName(this, tr("Replace the picture with"), QString(), filters);
    if (path.isEmpty())
        return;
    QImage im(path);
    if (im.isNull()) {
        classicMessage(this, tr("Edit image"), tr("That file can't be read as a picture."));
        return;
    }
    remember();
    // (in the place the picture had: as big as fits in it, its shape kept, in the middle)
    const double s = std::min(frame_.width() / im.width(), frame_.height() / im.height());
    const QSizeF size(im.width() * s, im.height() * s);
    frame_ = QRectF(frame_.center() - QPointF(size.width() / 2, size.height() / 2), size);
    img_ = im.convertToFormat(QImage::Format_ARGB32);
    sel_ = QImage();
    ++changes_;
    changed(true);
}

void ImageEditor::invertColors()
{
    remember();
    for (int y = 0; y < img_.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        for (int x = 0; x < img_.width(); ++x) {
            const double c = selected(x, y) / 255.0;
            if (c <= 0)
                continue;
            const QRgb was = o[x];
            auto mix = [&](int v) { return int(std::lround((255 - v) * c + v * (1 - c))); };
            o[x] = qRgba(mix(qRed(was)), mix(qGreen(was)), mix(qBlue(was)), qAlpha(was));
        }
    }
    ++changes_;
    changed();
}

void ImageEditor::grayscale()
{
    remember();
    for (int y = 0; y < img_.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        for (int x = 0; x < img_.width(); ++x) {
            const double c = selected(x, y) / 255.0;
            if (c <= 0)
                continue;
            const QRgb was = o[x];
            const int g = qGray(was);
            auto mix = [&](int v) { return int(std::lround(g * c + v * (1 - c))); };
            o[x] = qRgba(mix(qRed(was)), mix(qGreen(was)), mix(qBlue(was)), qAlpha(was));
        }
    }
    ++changes_;
    changed();
}

// ---- keys, closing ----

void ImageEditor::keyPressEvent(QKeyEvent *e)
{
    // (Ctrl + the key itself - an Arabic keyboard's letters aside - as the main window's)
    const quint32 vk = e->nativeVirtualKey();
    const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
    auto is = [&](char c, Qt::Key k) { return vk == quint32(c) || e->key() == k; };
    if (ctrl) {
        if (is('Z', Qt::Key_Z))
            shift ? redo() : undo();
        else if (is('Y', Qt::Key_Y))
            redo();
        else if (is('A', Qt::Key_A))
            selectAll();
        else if (is('D', Qt::Key_D))
            deselect();
        else if (is('I', Qt::Key_I) && shift)
            invertSelection();
        else if (is('S', Qt::Key_S))
            save();
        else if (is('C', Qt::Key_C))
            copySelection(false);
        else if (is('X', Qt::Key_X))
            copySelection(true);
        else if (is('V', Qt::Key_V))
            paste();
        else if (e->key() == Qt::Key_1)
            zoomBy(1.0 / zoom_, QPointF(canvas_->rect().center()));
        else if (e->key() == Qt::Key_0)
            fit();
        else if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal)
            zoomBy(1.25, QPointF(canvas_->rect().center()));
        else if (e->key() == Qt::Key_Minus)
            zoomBy(0.8, QPointF(canvas_->rect().center()));
        return;
    }
    switch (e->key()) {
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) {
            spaceDown_ = true;
            canvas_->setCursor(Qt::OpenHandCursor);
        }
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelected();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (tool_ == Tool::Crop)
            applyCrop();
        return;
    case Qt::Key_Escape:
        if (tool_ == Tool::Crop && crop_ != QRectF(0, 0, img_.width(), img_.height())) {
            crop_ = QRectF(0, 0, img_.width(), img_.height());
            canvas_->update();
        } else if (!sel_.isNull()) {
            deselect();
        } else {
            close();
        }
        return;
    case Qt::Key_BracketLeft:
        sizeBox_->setValue(std::max(1, int(sizeBox_->value() * 0.8)));
        canvas_->update();
        return;
    case Qt::Key_BracketRight:
        sizeBox_->setValue(std::max(sizeBox_->value() + 1, int(sizeBox_->value() * 1.25)));
        canvas_->update();
        return;
    default:
        break;
    }
    if (vk == quint32('X') || e->key() == Qt::Key_X) {  // (the two colours swapped)
        setColors(bg_, color_);
        return;
    }
    // (the tools by their letters - the key itself)
    static const std::pair<char, Tool> letters[] = {{'H', Tool::Hand},  {'M', Tool::RectSelect}, {'L', Tool::Lasso},  {'W', Tool::Wand},
                                                    {'B', Tool::Brush}, {'E', Tool::Eraser},     {'G', Tool::Fill},   {'C', Tool::Crop},
                                                    {'P', Tool::Pencil}, {'T', Tool::Text},      {'V', Tool::Move}};
    for (const auto &[c, t] : letters)
        if (vk == quint32(c) || e->key() == Qt::Key_A + (c - 'A')) {
            setTool(t);
            return;
        }
    ClassicWindow::keyPressEvent(e);
}

void ImageEditor::keyReleaseEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceDown_ = false;
        refreshTools();
        return;
    }
    ClassicWindow::keyReleaseEvent(e);
}

void ImageEditor::save()
{
    accepted_ = true;
    close();
}

void ImageEditor::closeEvent(QCloseEvent *e)
{
    if (!accepted_ && changes_ > 0) {
        const QString a = classicMessage(this, tr("Edit image"), tr("Put the changes to the picture in the PDF?"), {tr("Save"), tr("Don't save"), tr("Cancel")});
        if (a == tr("Save")) {
            accepted_ = true;
        } else if (a != tr("Don't save")) {
            e->ignore();
            return;
        }
    }
    ClassicWindow::closeEvent(e);
    if (loop_)
        loop_->quit();
}

// ---- Adjust ----

QImage ImageEditor::adjusted(const QImage &from, const std::function<QRgb(QRgb)> &fn) const
{
    QImage out = from.copy();
    for (int y = 0; y < out.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const double c = (sel_.isNull() || sel_.size() != out.size() ? 255 : sel_.constScanLine(y)[x]) / 255.0;
            if (c <= 0)
                continue;
            const QRgb was = o[x], now = fn(was);
            auto mix = [&](int a, int b) { return std::clamp(int(std::lround(b * c + a * (1 - c))), 0, 255); };
            o[x] = qRgba(mix(qRed(was), qRed(now)), mix(qGreen(was), qGreen(now)), mix(qBlue(was), qBlue(now)), qAlpha(was));
        }
    }
    return out;
}

void ImageEditor::adjust(const std::function<QRgb(QRgb)> &fn)
{
    remember();
    img_ = adjusted(img_, fn);
    ++changes_;
    changed();
}

void ImageEditor::sepia()
{
    adjust([](QRgb c) {
        const double r = qRed(c), g = qGreen(c), b = qBlue(c);
        auto cl = [](double v) { return std::clamp(int(std::lround(v)), 0, 255); };
        return qRgb(cl(0.393 * r + 0.769 * g + 0.189 * b), cl(0.349 * r + 0.686 * g + 0.168 * b), cl(0.272 * r + 0.534 * g + 0.131 * b));
    });
}

void ImageEditor::blackAndWhite()
{
    adjust([](QRgb c) { return qGray(c) >= 128 ? qRgb(255, 255, 255) : qRgb(0, 0, 0); });
}

void ImageEditor::convolve(const float k[9])
{
    // (on the colours times their see-through - so see-through pixels' colours don't bleed in)
    remember();
    const QImage src = img_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out = src.copy();
    const int w = src.width(), h = src.height();
    for (int y = 0; y < h; ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double c = selected(x, y) / 255.0;
            if (c <= 0)
                continue;
            double sum[4] = {0, 0, 0, 0};
            for (int j = -1; j <= 1; ++j) {
                const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(std::clamp(y + j, 0, h - 1)));
                for (int i = -1; i <= 1; ++i) {
                    const QRgb q = line[std::clamp(x + i, 0, w - 1)];
                    const float f = k[(j + 1) * 3 + (i + 1)];
                    sum[0] += qRed(q) * f, sum[1] += qGreen(q) * f, sum[2] += qBlue(q) * f, sum[3] += qAlpha(q) * f;
                }
            }
            const QRgb was = o[x];
            const int a = std::clamp(int(std::lround(sum[3] * c + qAlpha(was) * (1 - c))), 0, 255);
            auto ch = [&](double v, int old) { return std::clamp(int(std::lround(v * c + old * (1 - c))), 0, a); };
            o[x] = qRgba(ch(sum[0], qRed(was)), ch(sum[1], qGreen(was)), ch(sum[2], qBlue(was)), a);
        }
    }
    img_ = out.convertToFormat(QImage::Format_ARGB32);
    ++changes_;
    changed();
}

// a dialog of sliders, the picture changed as they move (Cancel: as it was)
bool ImageEditor::previewDialog(const QString &title, const std::vector<std::tuple<QString, int, int>> &sliders,
                                const std::function<QImage(const QImage &, const std::vector<int> &)> &apply)
{
    const QImage before = img_;
    ClassicDialog d(this, title);
    auto *l = new QVBoxLayout(d.body());
    l->setContentsMargins(12, 10, 12, 10);
    l->setSpacing(6);
    std::vector<QSlider *> bars;
    std::vector<int> values(sliders.size(), 0);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    for (size_t i = 0; i < sliders.size(); ++i) {
        const auto &[name, lo, hi] = sliders[i];
        auto *t = new QLabel(name, d.body());
        t->setFont(theme::font());
        auto *bar = new QSlider(Qt::Horizontal, d.body());
        bar->setRange(lo, hi);
        bar->setValue(0);
        bar->setMinimumWidth(220);
        auto *v = new QLabel(QStringLiteral("0"), d.body());
        v->setFont(theme::font());
        v->setMinimumWidth(QFontMetrics(theme::font()).horizontalAdvance(QStringLiteral("-000")));
        v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(t, int(i), 0);
        grid->addWidget(bar, int(i), 1);
        grid->addWidget(v, int(i), 2);
        QObject::connect(bar, &QSlider::valueChanged, &d, [&, i, v](int n) {
            values[i] = n;
            v->setText(QString::number(n));
            img_ = apply(before, values);
            canvas_->update();
        });
        bars.push_back(bar);
    }
    l->addLayout(grid);
    auto *row = new QHBoxLayout;
    auto *resetB = new QPushButton(tr("Reset"), d.body());
    QObject::connect(resetB, &QPushButton::clicked, &d, [&] {
        for (QSlider *b : bars)
            b->setValue(0);
    });
    row->addWidget(resetB);
    row->addStretch(1);
    bool ok = false;
    auto *okB = new QPushButton(tr("OK"), d.body());
    auto *noB = new QPushButton(tr("Cancel"), d.body());
    QObject::connect(okB, &QPushButton::clicked, &d, [&] {
        ok = true;
        d.close();
    });
    QObject::connect(noB, &QPushButton::clicked, &d, [&] { d.close(); });
    row->addWidget(okB);
    row->addWidget(noB);
    l->addLayout(row);
    d.exec();
    const QImage after = img_;
    img_ = before;
    bool any = false;
    for (int v : values)
        any = any || v != 0;
    if (ok && any) {
        remember();
        img_ = after;
        ++changes_;
    }
    changed();
    return ok && any;
}

void ImageEditor::brightnessDialog()
{
    previewDialog(tr("Brightness / Contrast"), {{tr("Brightness:"), -100, 100}, {tr("Contrast:"), -100, 100}},
                  [this](const QImage &from, const std::vector<int> &v) {
                      const double add = v[0] * 2.55, f = std::pow((100.0 + v[1]) / 100.0, 2);
                      return adjusted(from, [add, f](QRgb c) {
                          auto ch = [&](int x) { return std::clamp(int(std::lround((x + add - 128) * f + 128)), 0, 255); };
                          return qRgb(ch(qRed(c)), ch(qGreen(c)), ch(qBlue(c)));
                      });
                  });
}

void ImageEditor::hueDialog()
{
    previewDialog(tr("Hue / Saturation"), {{tr("Hue:"), -180, 180}, {tr("Saturation:"), -100, 100}, {tr("Lightness:"), -100, 100}},
                  [this](const QImage &from, const std::vector<int> &v) {
                      const int dh = v[0];
                      const double ds = v[1] / 100.0, dl = v[2] / 100.0;
                      return adjusted(from, [dh, ds, dl](QRgb c) {
                          int h, s, l, a;
                          QColor(c).getHsl(&h, &s, &l, &a);
                          if (h >= 0)
                              h = ((h + dh) % 360 + 360) % 360;
                          s = std::clamp(int(std::lround(ds >= 0 ? s + (255 - s) * ds : s * (1 + ds))), 0, 255);
                          l = std::clamp(int(std::lround(dl >= 0 ? l + (255 - l) * dl : l * (1 + dl))), 0, 255);
                          return QColor::fromHsl(std::max(0, h), s, l).rgb();
                      });
                  });
}

// ---- Image ----

void ImageEditor::rotate180()
{
    remember();
    QTransform t;
    t.rotate(180);
    img_ = img_.transformed(t);
    if (!sel_.isNull())
        sel_ = sel_.transformed(t);
    ++changes_;
    changed();
}

void ImageEditor::canvasSizeDialog()
{
    // (the picture in the middle of a bigger - or smaller - canvas; what's added see-through)
    ClassicDialog d(this, tr("Canvas size"));
    auto *l = new QVBoxLayout(d.body());
    l->setContentsMargins(10, 10, 10, 10);
    QSpinBox *wBox = nullptr, *hBox = nullptr;
    l->addWidget(labelled(d.body(), tr("Width:"), &wBox, 1, 20000, img_.width(), tr(" px"), {}));
    l->addWidget(labelled(d.body(), tr("Height:"), &hBox, 1, 20000, img_.height(), tr(" px"), {}));
    auto *note = new QLabel(tr("The picture stays in the middle; what's added is see-through."), d.body());
    note->setFont(theme::font());
    l->addWidget(note);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    bool ok = false;
    auto *okB = new QPushButton(tr("OK"), d.body());
    auto *noB = new QPushButton(tr("Cancel"), d.body());
    QObject::connect(okB, &QPushButton::clicked, &d, [&] {
        ok = true;
        d.close();
    });
    QObject::connect(noB, &QPushButton::clicked, &d, [&] { d.close(); });
    row->addWidget(okB);
    row->addWidget(noB);
    l->addLayout(row);
    d.exec();
    const int nw = wBox->value(), nh = hBox->value();
    if (!ok || (nw == img_.width() && nh == img_.height()))
        return;
    remember();
    const QPoint at((nw - img_.width()) / 2, (nh - img_.height()) / 2);
    QImage out(nw, nh, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(at, img_);
    }
    if (!sel_.isNull()) {
        QImage m(nw, nh, QImage::Format_Grayscale8);
        m.fill(0);
        for (int y = 0; y < sel_.height(); ++y)
            for (int x = 0; x < sel_.width(); ++x)
                if (const QPoint q(x + at.x(), y + at.y()); q.x() >= 0 && q.y() >= 0 && q.x() < nw && q.y() < nh)
                    m.scanLine(q.y())[q.x()] = sel_.constScanLine(y)[x];
        sel_ = m;
    }
    // (where it shows: the canvas's new edges, the picture where it was)
    const double sx = frame_.width() / img_.width(), sy = frame_.height() / img_.height();
    frame_ = QRectF(frame_.left() - at.x() * sx, frame_.top() - at.y() * sy, nw * sx, nh * sy);
    img_ = out;
    ++changes_;
    changed(true);
}

void ImageEditor::trimEdges()
{
    int x0 = img_.width(), y0 = img_.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img_.height(); ++y) {
        const QRgb *o = reinterpret_cast<const QRgb *>(img_.constScanLine(y));
        for (int x = 0; x < img_.width(); ++x)
            if (qAlpha(o[x]) > 0)
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (x1 < x0) {
        classicMessage(this, tr("Edit image"), tr("The whole picture is see-through - there's nothing to keep."));
        return;
    }
    const QRect r(QPoint(x0, y0), QPoint(x1, y1));
    if (r == img_.rect()) {
        classicMessage(this, tr("Edit image"), tr("Its edges aren't see-through - there's nothing to trim."));
        return;
    }
    cropTo(r);
}

void ImageEditor::exportImage()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export the picture"), QStringLiteral("picture.png"),
                                                tr("PNG picture (*.png);;JPEG picture (*.jpg *.jpeg);;Bitmap (*.bmp)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");
    const bool flat = !path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive);  // (no see-through: on white)
    QImage out = img_;
    if (flat) {
        out = QImage(img_.size(), QImage::Format_RGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        p.drawImage(0, 0, img_);
    }
    if (!out.save(path))
        classicMessage(this, tr("Edit image"), tr("Couldn't save %1.").arg(QFileInfo(path).fileName()));
}

// ---- the clipboard ----

QRect ImageEditor::selectionBounds() const
{
    if (sel_.isNull())
        return img_.rect();
    int x0 = img_.width(), y0 = img_.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < sel_.height(); ++y) {
        const uchar *s = sel_.constScanLine(y);
        for (int x = 0; x < sel_.width(); ++x)
            if (s[x])
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    return x1 < x0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

void ImageEditor::copySelection(bool cut)
{
    const QRect r = selectionBounds();
    if (r.isEmpty())
        return;
    QImage part = img_.copy(r);
    if (!sel_.isNull())
        for (int y = 0; y < part.height(); ++y) {
            QRgb *o = reinterpret_cast<QRgb *>(part.scanLine(y));
            for (int x = 0; x < part.width(); ++x) {
                const int k = selected(x + r.x(), y + r.y());
                o[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(o[x]) * k / 255);
            }
        }
    QGuiApplication::clipboard()->setImage(part);
    hintPanel_->setText(cut ? tr("Cut - Ctrl+V pastes it back") : tr("Copied - Ctrl+V pastes it"));
    if (!cut)
        return;
    if (sel_.isNull()) {  // (nothing selected: all of it cut - see-through)
        remember();
        img_.fill(Qt::transparent);
        ++changes_;
        changed();
    } else {
        deleteSelected();
    }
}

void ImageEditor::paste()
{
    // (put over the picture where the selection is - else in its middle - and picked up: the Move tool
    // drags it where it goes, nothing of the picture under it lost)
    QImage c = QGuiApplication::clipboard()->image();
    if (c.isNull()) {
        hintPanel_->setText(tr("There's no picture to paste - copy one first"));
        return;
    }
    c = c.convertToFormat(QImage::Format_ARGB32);
    const QRect r = selectionBounds();
    const QPoint at = !sel_.isNull() && !r.isEmpty() ? r.topLeft()
                                                     : QPoint(std::max(0, (img_.width() - c.width()) / 2), std::max(0, (img_.height() - c.height()) / 2));
    remember();
    under_ = img_.copy();
    lifted_ = QImage(img_.size(), QImage::Format_ARGB32);
    lifted_.fill(Qt::transparent);
    {
        QPainter p(&lifted_);
        p.drawImage(at, c);
    }
    QImage m(img_.size(), QImage::Format_Grayscale8);
    m.fill(0);
    for (int y = 0; y < img_.height(); ++y) {
        const QRgb *l = reinterpret_cast<const QRgb *>(lifted_.constScanLine(y));
        uchar *o = m.scanLine(y);
        for (int x = 0; x < img_.width(); ++x)
            o[x] = uchar(qAlpha(l[x]) ? 255 : 0);
    }
    {
        QPainter p(&img_);
        p.drawImage(0, 0, lifted_);
    }
    sel_ = m;
    floating_ = true;
    ++changes_;
    setTool(Tool::Move);
    changed();
}

// ---- lines, rectangles, ellipses, words ----

QPointF ImageEditor::constrained(QPointF from, QPointF to) const
{
    const QPointF d = to - from;
    if (tool_ == Tool::Line) {  // (across, down, or at 45 degrees - whichever's nearest)
        const double a = std::atan2(d.y(), d.x()), step = M_PI / 4, snapped = std::round(a / step) * step;
        const double len = std::hypot(d.x(), d.y());
        return from + QPointF(std::cos(snapped), std::sin(snapped)) * len;
    }
    const double side = std::max(std::abs(d.x()), std::abs(d.y()));  // (a square, a circle)
    return from + QPointF(d.x() < 0 ? -side : side, d.y() < 0 ? -side : side);
}

void ImageEditor::putLayer(const QImage &layer)
{
    // (a layer drawn on - premultiplied - over the picture: inside the selection, at the tool's opacity)
    const double opacity = opacityBox_->value() / 100.0;
    for (int y = 0; y < img_.height(); ++y) {
        const QRgb *l = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(img_.scanLine(y));
        for (int x = 0; x < img_.width(); ++x) {
            const int la = qAlpha(l[x]);
            if (!la)
                continue;
            const double c = la / 255.0 * opacity * selected(x, y) / 255.0;
            if (c <= 0)
                continue;
            const int sr = qRed(l[x]) * 255 / la, sg = qGreen(l[x]) * 255 / la, sb = qBlue(l[x]) * 255 / la;
            const QRgb was = o[x];
            const double ba = qAlpha(was) / 255.0, a = c + ba * (1 - c);
            auto mix = [&](int src, int dst) { return a > 0 ? std::clamp(int(std::lround((src * c + dst * ba * (1 - c)) / a)), 0, 255) : 0; };
            o[x] = qRgba(mix(sr, qRed(was)), mix(sg, qGreen(was)), mix(sb, qBlue(was)), int(std::lround(a * 255)));
        }
    }
}

void ImageEditor::drawShape(QPointF a, QPointF b)
{
    QImage layer(img_.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor other = paint_ == color_ ? bg_ : color_;
        const int style = tool_ == Tool::Line ? 0 : styleBox_->currentIndex();
        QPen pen(paint_, widthBox_->value(), Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
        p.setPen(style == 2 ? Qt::NoPen : pen);
        p.setBrush(style == 1 ? QBrush(other) : style == 2 ? QBrush(paint_) : Qt::NoBrush);
        if (tool_ == Tool::Line)
            p.drawLine(a, b);
        else if (tool_ == Tool::Rectangle)
            p.drawRect(QRectF(a, b).normalized());
        else
            p.drawEllipse(QRectF(a, b).normalized());
    }
    remember();
    putLayer(layer);
    ++changes_;
    changed();
}

void ImageEditor::textAt(QPointF at)
{
    // the words, their font, size and look - then drawn there in the colour
    static QString lastFamily = QStringLiteral("Arial");
    static int lastSize = 24;
    static bool lastBold = false, lastItalic = false;
    ClassicDialog d(this, tr("Text"));
    auto *l = new QVBoxLayout(d.body());
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(6);
    auto *words = new QPlainTextEdit(d.body());
    words->setFont(theme::font());
    words->setFixedSize(320, 80);
    l->addWidget(words);
    auto *row1 = new QHBoxLayout;
    auto *family = new QFontComboBox(d.body());
    family->setFont(theme::font());
    family->setCurrentFont(QFont(lastFamily));
    row1->addWidget(family, 1);
    QSpinBox *size = nullptr;
    row1->addWidget(labelled(d.body(), tr("Size:"), &size, 4, 1000, lastSize, tr(" px"), {}));
    l->addLayout(row1);
    auto *row2 = new QHBoxLayout;
    auto *bold = new QCheckBox(tr("Bold"), d.body());
    auto *italic = new QCheckBox(tr("Italic"), d.body());
    bold->setFont(theme::font());
    italic->setFont(theme::font());
    bold->setChecked(lastBold);
    italic->setChecked(lastItalic);
    row2->addWidget(bold);
    row2->addWidget(italic);
    row2->addStretch(1);
    l->addLayout(row2);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    bool ok = false;
    auto *okB = new QPushButton(tr("OK"), d.body());
    auto *noB = new QPushButton(tr("Cancel"), d.body());
    QObject::connect(okB, &QPushButton::clicked, &d, [&] {
        ok = true;
        d.close();
    });
    QObject::connect(noB, &QPushButton::clicked, &d, [&] { d.close(); });
    row->addWidget(okB);
    row->addWidget(noB);
    l->addLayout(row);
    QTimer::singleShot(0, words, [words] { words->setFocus(); });
    d.exec();
    const QString text = words->toPlainText();
    if (!ok || text.trimmed().isEmpty())
        return;
    lastFamily = family->currentFont().family();
    lastSize = size->value();
    lastBold = bold->isChecked();
    lastItalic = italic->isChecked();
    QFont f(lastFamily);
    f.setPixelSize(lastSize);
    f.setBold(lastBold);
    f.setItalic(lastItalic);
    QImage layer(img_.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(f);
        p.setPen(paint_);
        p.drawText(QRectF(at, QSizeF(1e6, 1e6)), Qt::AlignLeft | Qt::AlignTop, text);
    }
    remember();
    putLayer(layer);
    ++changes_;
    changed();
}
