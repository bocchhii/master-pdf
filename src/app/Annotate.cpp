// MainWindow's tools and annotations (the Python app's): choosing a tool, the bar under the
// tools (colour, thickness, see-through), what the tools draw put into the PDF, the little
// toolbar over selected text or a mark, and the Annotations list.

#include "app/MainWindow.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/ScrollBox.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/TrackBar.h"
#include "ui/Widgets.h"
#include "view/PageView.h"
#include "view/ThumbnailBar.h"

#include <set>
#include <QApplication>
#include <QTreeWidgetItemIterator>
#include <QSet>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStackedWidget>
#include <QTreeWidget>

namespace {

// what each tool does, in the bar under the tools (the Python app's TOOL_HINTS)
QString hint(const QString &tool)
{
    static const std::pair<const char *, const char *> hints[] = {
        {"select", "Drag across text to select it (then copy or mark it). Choose a tool above to add or change things."},
        {"move", "Drag to move around. Click a text, note or drawing to pick it up."},
        {"edit", "Click a line of text or a picture to pick it up; click the text again to retype it."},
        {"text", "Click on the page where the text goes."},
        {"highlight", "Draw with the marker anywhere on the page (hold Shift for a straight line)."},
        {"pen", "Drag on the page to draw."},
        {"eraser", "White-out: drag a box to cover. Eraser: rub over drawings to take them out."},
        {"image", "Click where the picture goes, or drag a box for it."},
        {"shape", "Drag on the page to draw the shape."},
        {"sign", "Click where your signature goes."},
        {"link", "Drag a box over what should become a link. Right-click a link to change it."},
    };
    for (const auto &[k, h] : hints)
        if (tool == QLatin1String(k))
            return QObject::tr(h);
    return {};
}

// the floating toolbar: a raised box of tool buttons
class FloatBar : public QWidget {
public:
    explicit FloatBar(QWidget *parent) : QWidget(parent)
    {
        row = new QHBoxLayout(this);
        row->setContentsMargins(3 + 1, 3 + 1, 3 + 1, 3 + 1);
        row->setSpacing(1);
    }
    QHBoxLayout *row;
    ToolButton *add(const QString &icon, const QImage &image, const QString &tip, const QString &label,
                    std::function<void()> fn)
    {
        auto *b = new ToolButton(icon, tip, {}, false, label, this);
        if (!image.isNull())
            b->setImage(image);
        QObject::connect(b, &ToolButton::clicked, b, fn);
        row->addWidget(b);
        return b;
    }
    void separator() { row->addWidget(new ToolSeparator(this)); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        classic::raisedEdge(p, rect());
    }
};

const char *kAnnotNames[][2] = {{"FreeText", "Text"}, {"Text", "Note"}, {"Highlight", "Highlight"},
                                {"Ink", "Drawing"}, {"Square", "Rectangle"}, {"Circle", "Ellipse"},
                                {"Underline", "Underline"}, {"StrikeOut", "Strike-out"},
                                {"Squiggly", "Squiggly underline"}, {"Line", "Line"}};

}  // namespace

// ---- choosing a tool ----

void MainWindow::setTool(const QString &tool, bool toggle)
{
    QString t = tool;
    if (toggle && t == tool_)  // (its button pressed again: put down - the mouse selects text)
        t = QStringLiteral("select");
    if (readMode_)  // (Read mode: the text selected and copied, nothing else)
        t = QStringLiteral("select");
    tool_ = t;
    for (const char *k : {"move", "edit", "text", "highlight", "pen", "eraser", "image", "shape", "sign", "link"})
        buttons_[k]->setLatched(tool_ == QLatin1String(k));
    view_->setTool(toolLook());
    refreshProps();
    if (view_->document())
        say(hint(tool_));
}

PageView::ToolLook MainWindow::toolLook() const
{
    using Tool = PageView::Tool;
    PageView::ToolLook l;
    const QString t = tool_;
    l.tool = t == "move" ? Tool::Move : t == "highlight" ? Tool::Marker : t == "pen" ? Tool::Pen
             : t == "eraser" ? Tool::Eraser : t == "shape" ? Tool::Shape : t == "edit" ? Tool::Edit
             : t == "text" ? Tool::Text : t == "image" ? Tool::Image : t == "sign" ? Tool::Sign
             : t == "link" ? Tool::Link : t == "scanarea" ? Tool::Shape : Tool::Select;
    l.family = textFamily_;
    l.bold = textBold_;
    l.italic = textItalic_;
    l.variant = variant_.value(t);
    l.color = colors_.value(t, QColor(Qt::black));
    l.width = sizes_.value(t, 2);
    l.opacity = opacities_.value(t == "highlight" ? QStringLiteral("marker") : t, 100) / 100.0;
    if (t == "scanarea") {  // (Scan > An area of the page: a box dragged round it - a thin blue one, not a shape's)
        l.variant = QStringLiteral("rect");
        l.color = QColor(0, 120, 215);
        l.width = 1;
        l.opacity = 1;
    }
    return l;
}

void MainWindow::toolClicked(const QString &key)
{
    static const QStringList built = {"move", "edit", "text", "highlight", "pen", "eraser", "image", "shape", "sign", "link"};
    if (key == QLatin1String("annots")) {
        showAnnotations();
        return;
    }
    if (!built.contains(key)) {
        const ToolButton *b = buttons_.value(key);
        notYet(b ? b->toolTip() : key);
        return;
    }
    setTool(key);
}

void MainWindow::toolMenu(const QString &key)
{
    // a tool's little arrow: its kinds (the one in use pressed in)
    static const std::map<QString, std::vector<std::pair<const char *, const char *>>> kinds = {
        {"eraser", {{"White-out (cover with a box)", "whiteout"}, {"Eraser (rub out drawings)", "rub"}}},
        {"shape", {{"Rectangle", "rect"}, {"Ellipse", "ellipse"}, {"Line", "line"}, {"Arrow", "arrow"}}},
        {"image", {{"Picture from a file...", "file"}, {"Picture you copied (paste)", "clipboard"}}},
        {"sign", {{"Place my signature", "place"}, {"New signature...", "new"}}},
    };
    auto it = kinds.find(key);
    if (it == kinds.end()) {
        const ToolButton *b = buttons_.value(key);
        notYet(b ? b->toolTip() : key);
        return;
    }
    ToolButton *b = buttons_.value(key);
    QMenu menu(this);
    for (const auto &[label, k] : it->second) {
        QAction *a = menu.addAction(tr(label));
        a->setCheckable(true);
        a->setChecked(variant_.value(key) == QLatin1String(k) && key != QLatin1String("sign"));
        const QString kind = QString::fromLatin1(k);
        connect(a, &QAction::triggered, this, [this, key, kind] {
            if (key == QLatin1String("sign")) {  // (Sign: the tool taken up; a new signature made first if asked)
                setTool(key, false);
                if (kind == QLatin1String("new"))
                    newSignature();
                return;
            }
            variant_[key] = kind;
            tintTools();
            setTool(key, false);
        });
    }
    menu.exec(b->mapToGlobal(QPoint(0, b->height())));
}

void MainWindow::tintTools()
{
    // the marker and the shape show the colour they draw in (and the shape its kind)
    buttons_["highlight"]->setImage(icons::tinted(icons::get(QStringLiteral("highlight_tool")), QColor(255, 255, 0),
                                                  colors_.value("highlight")));
    const QString shape = QStringLiteral("shape_") + variant_.value("shape");
    buttons_["shape"]->setImage(icons::tinted(icons::best(shape), QColor(255, 0, 0), colors_.value("shape")));
    view_->setTool(toolLook());
}

// ---- the bar under the tools ----

void MainWindow::buildProps(QHBoxLayout *pr, QWidget *props)
{
    auto group = [&]() {
        auto *w = new QWidget(props);
        auto *l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(1);
        pr->addWidget(w);
        return std::make_pair(w, l);
    };
    auto [cw, cl] = group();
    colorGroup_ = cw;
    colorButton_ = new ToolButton(QString(), tr("Colour"),
                                  tr("The colour of what you add next (highlights, drawings, shapes...). Click to choose another."),
                                  false, {}, cw);
    connect(colorButton_, &ToolButton::clicked, this, &MainWindow::pickColor);
    cl->addWidget(colorButton_);
    for (int n = 0; n < 2; ++n) {
        auto [w, l] = group();
        l->addWidget(new ToolSeparator(w));
        auto *title = new QLabel(n == 0 ? tr("Thickness:") : tr("Opacity:"), w);
        title->setFont(theme::font());
        l->addWidget(title);
        l->addSpacing(2);
        auto *bar = new TrackBar(w, 110);
        l->addWidget(bar);
        l->addSpacing(3);
        auto *value = new QLabel(w);
        value->setFont(theme::font());
        value->setMinimumWidth(QFontMetrics(theme::font()).horizontalAdvance(QStringLiteral("000 pt")));
        l->addWidget(value);
        if (n == 0) {
            widthGroup_ = w;
            widthTitle_ = title;
            widthBar_ = bar;
            widthLabel_ = value;
            connect(bar, &TrackBar::valueChanged, this, [this](int v) {
                widthLabel_->setText(tr("%1 pt").arg(v));
                if (!propsLoading_ && sizes_.contains(tool_)) {
                    sizes_[tool_] = v;
                    view_->setTool(toolLook());
                }
            });
        } else {
            opacityGroup_ = w;
            opacityBar_ = bar;
            opacityLabel_ = value;
            bar->setRange(10, 100, 10);
            connect(bar, &TrackBar::valueChanged, this, [this](int v) {
                opacityLabel_->setText(QStringLiteral("%1%").arg(v));
                const QString key = tool_ == "highlight" ? QStringLiteral("marker") : tool_;
                if (!propsLoading_ && opacities_.contains(key)) {
                    opacities_[key] = v;
                    view_->setTool(toolLook());
                }
            });
        }
    }
}

void MainWindow::refreshProps()
{
    // what the bar shows: the tool's colour, thickness and see-through; else a word on it
    const bool has = view_->document() != nullptr;
    QString ctx = QStringLiteral("hint");
    const bool lineUp = has && tool_ == "edit" && view_->pickedLine() >= 0;
    if (has) {
        if (tool_ == "text")
            ctx = QStringLiteral("text");
        else if (lineUp)
            ctx = QStringLiteral("line");
        else if (tool_ == "edit" && view_->pickedPicture() >= 0)
            ctx = QStringLiteral("picture");
        else if (tool_ == "highlight")
            ctx = QStringLiteral("marker");
        else if (tool_ == "pen" || tool_ == "shape")
            ctx = tool_;
        else if (tool_ == "eraser")
            ctx = variant_.value("eraser") == "whiteout" ? QStringLiteral("whiteout") : QStringLiteral("rub");
    }
    static const std::map<QString, QString> titles = {{"text", "Add text:"}, {"line", "Text:"}, {"picture", "Picture:"}, {"marker", "Marker:"}, {"pen", "Draw:"}, {"shape", "Shape:"},
                                                      {"whiteout", "White-out:"}, {"rub", "Eraser:"}};
    static const std::map<QString, QString> names = {{"select", "Select"}, {"move", "Move"}, {"edit", "Edit"}, {"sign", "Sign"}, {"link", "Link"}};
    const bool color = ctx == "marker" || ctx == "pen" || ctx == "shape" || ctx == "whiteout";
    const bool width = ctx == "marker" || ctx == "pen" || ctx == "shape" || ctx == "rub";
    const bool opacity = ctx == "marker" || ctx == "pen" || ctx == "shape";
    colorGroup_->setVisible(color);
    widthGroup_->setVisible(width);
    opacityGroup_->setVisible(opacity);
    propsHint_->setVisible(ctx == "hint");
    textGroup_->setVisible(ctx == "text" || ctx == "line");
    pictureGroup_->setVisible(ctx == "picture");
    // (a drawing picked up - lines, a box, a frame: moved, sized, turned, flipped, deleted; what's a
    // picture's own - its pixels, its crop, its place among the others, laid out with the text - not)
    bool drawing = false;
    if (ctx == "picture") {
        const int pp = view_->pickedPicturePage(), pi = view_->pickedPicture();
        const std::vector<Picture> &pics = view_->picturesOf(pp);
        drawing = pi >= 0 && pi < int(pics.size()) && pics[size_t(pi)].vector;
        const bool letters = drawing && pics[size_t(pi)].letters;  // (letters drawn as shapes: can be read)
        for (const char *k : {"pic_crop", "pic_replace", "pic_save", "pic_edit", "pic_arrange", "pic_editor", "pic_ocr"})
            if (buttons_.contains(QString::fromLatin1(k)))
                buttons_[QString::fromLatin1(k)]->setEnabled(!drawing || (letters && qstrcmp(k, "pic_ocr") == 0));
    }
    boxesGroup_->setVisible(has && tool_ == "edit");
    if (ctx == "text" || ctx == "line")
        refreshTextProps(ctx == "line");
    auto t = titles.find(ctx);
    propsTitle_->setText(!has ? QString()
                         : drawing ? tr("Drawing:")
                         : t != titles.end() ? tr(qPrintable(t->second))
                                             : tr("%1:").arg(names.count(tool_) ? names.at(tool_) : tool_));
    propsLoading_ = true;
    if (color)
        colorButton_->setImage(icons::swatch(colors_.value(tool_)));
    if (width) {
        widthTitle_->setText(ctx == "rub" ? tr("Size:") : tr("Thickness:"));
        static const std::map<QString, std::array<int, 3>> ranges = {
            {"pen", {1, 20, 1}}, {"shape", {1, 20, 1}}, {"highlight", {4, 40, 2}}, {"eraser", {4, 60, 2}}};
        const auto &r = ranges.at(tool_);
        widthBar_->setRange(r[0], r[1], r[2]);
        widthBar_->setValue(int(sizes_.value(tool_)));
        widthLabel_->setText(tr("%1 pt").arg(widthBar_->value()));
    }
    if (opacity) {
        opacityBar_->setValue(opacities_.value(tool_ == "highlight" ? QStringLiteral("marker") : tool_));
        opacityLabel_->setText(QStringLiteral("%1%").arg(opacityBar_->value()));
    }
    propsLoading_ = false;
    if (ctx == "hint")
        propsHint_->setText(has ? hint(tool_) : tr("Open a PDF to start."));
}

void MainWindow::pickColor()
{
    if (!colors_.contains(tool_))
        return;
    const QColor c = colorDialog(this, tr("Color"), colors_.value(tool_));
    if (!c.isValid())
        return;
    colors_[tool_] = c;
    tintTools();
    refreshProps();
}

// ---- what the tools draw, put into the PDF ----

void MainWindow::annotChanged(bool ok, const QString &what, const QString &error)
{
    if (!ok && !error.isEmpty()) {
        afterChange(-1, true);
        classicMessage(this, tr("Master PDF"), tr("Couldn't %1:\n%2").arg(what, error));
        return;
    }
    if (ok)
        afterChange(-1, true);
}

void MainWindow::connectTools()
{
    connect(view_, &PageView::inkDrawn, this, [this](int page, const std::vector<QPointF> &pts) {
        auto doc = view_->document();
        if (!doc || pts.empty())
            return;
        QString error;
        const bool marker = tool_ == "highlight";
        const QString key = marker ? QStringLiteral("highlight") : QStringLiteral("pen");
        const bool ok = doc->addInk(page, pts, colors_.value(key), sizes_.value(key),
                                    opacities_.value(marker ? QStringLiteral("marker") : key) / 100.0, marker, &error);
        annotChanged(ok, marker ? tr("draw with the marker") : tr("draw"), error);
    });
    connect(view_, &PageView::boxDrawn, this, [this](int page, QPointF a, QPointF b) {
        auto doc = view_->document();
        if (!doc)
            return;
        QString error;
        bool ok;
        if (tool_ == "scanarea") {  // (Scan > An area of the page: just what's in the box read)
            setTool(QStringLiteral("select"), false);
            const QRectF area = QRectF(a, b).normalized();
            if (area.width() < 6 || area.height() < 6) {
                say(tr("The box was too small to scan - choose Scan > An area of the page again and drag a bigger one."));
                return;
            }
            startScan({page}, area);
            return;
        }
        if (tool_ == "image") {
            addPicture(page, a, QRectF(a, b).normalized());
            return;
        }
        if (tool_ == "link") {
            newLink(page, QRectF(a, b).normalized());
            return;
        }
        if (tool_ == "eraser") {
            ok = doc->whiteOut(page, QRectF(a, b).normalized(), colors_.value("eraser"), &error);
            annotChanged(ok, tr("white out"), error);
        } else {
            ok = doc->addShape(page, variant_.value("shape"), a, b, colors_.value("shape"), sizes_.value("shape"),
                               opacities_.value("shape") / 100.0, &error);
            annotChanged(ok, tr("draw the shape"), error);
        }
    });
    connect(view_, &PageView::rubbed, this, [this](int page, const std::vector<QPointF> &path) {
        auto doc = view_->document();
        if (!doc)
            return;
        QString error;
        const bool ok = doc->rubOut(page, path, sizes_.value("eraser") / 2, &error);
        annotChanged(ok, tr("erase"), error);
    });
    connect(view_, &PageView::annotMoved, this, [this](int page, int id, QPointF by) {
        auto doc = view_->document();
        if (!doc)
            return;
        QString error;
        const bool ok = doc->moveAnnot(page, id, by, &error);
        annotChanged(ok, tr("move it"), error);
        if (ok)
            view_->pick(page, id);  // (still picked up, where it went)
    });
    connect(view_, &PageView::picked, this, [this](int page, int id) {
        if (!id) {  // (put down: its toolbar goes with it)
            if (view_->floatBar() && view_->floatBar()->property("mark").toBool())
                view_->setFloatBar(nullptr, -1, {});
            return;
        }
        if (annotationsShowing())
            selectInList(page, id);
    });
    connect(view_, &PageView::markClicked, this, &MainWindow::showMarkBar);
    connect(view_, &PageView::selectionMade, this, &MainWindow::showTextBar);
    connect(view_, &PageView::selectionChanged, this, [this](bool any) {
        if (!any && view_->floatBar() && view_->floatBar()->property("text").toBool())
            view_->setFloatBar(nullptr, -1, {});
    });
    connect(view_, &PageView::selectionMenu, this, [this](QPoint at) {
        QMenu menu(this);
        menu.addAction(tr("Copy"), view_, &PageView::copySelection);
        if (readMode_) {  // (Read mode: copied, nothing else)
            menu.exec(at);
            return;
        }
        menu.addSeparator();
        menu.addAction(tr("Highlight"), this, [this] { markText(QStringLiteral("Highlight")); });
        menu.addAction(tr("Strike out"), this, [this] { markText(QStringLiteral("StrikeOut")); });
        menu.addAction(tr("Underline"), this, [this] { markText(QStringLiteral("Underline")); });
        menu.addAction(tr("Squiggly underline"), this, [this] { markText(QStringLiteral("Squiggly")); });
        menu.addSeparator();
        menu.addAction(tr("Edit text"), this, [this] { notYet(tr("Edit text")); });
        menu.exec(at);
    });
    connect(view_, &PageView::annotEdit, this, &MainWindow::editAnnot);
    connect(view_, &PageView::textBoxDone, this,
            [this](int page, int id, QPointF at, const QString &text, double size, const QColor &color) {
                auto doc = view_->document();
                if (!doc)
                    return;
                if (!id && tool_ == "text") {
                    addRealText(page, at, text);
                    return;
                }
                QString error;
                const bool ok = doc->setTextBox(page, id, at, text, size, color, &error);
                annotChanged(ok, tr("change the text"), error);
            });
    connect(view_, &PageView::deletePicked, this, &MainWindow::deletePicked);
    connect(view_, &PageView::escaped, this, [this] { setTool(QStringLiteral("select"), false); });
    connectTextTools();
    connectPictureTools();
    connectSignLink();
}

void MainWindow::deletePicked()
{
    auto doc = view_->document();
    const int page = view_->pickedPage(), id = view_->pickedId();
    if (!doc || !id)
        return;
    view_->pick(-1, 0);
    QString error;
    annotChanged(doc->deleteAnnot(page, id, &error), tr("delete it"), error);
}

void MainWindow::editAnnot(int page, int id)
{
    // double-click on an annotation: change its text (a text box), or its note
    auto doc = view_->document();
    if (!doc)
        return;
    for (const Annot &a : doc->annots(page)) {
        if (a.id != id)
            continue;
        if (a.kind == "FreeText") {
            view_->pick(-1, 0);
            view_->openTextBox(page, id, a.rect.topLeft(), a.contents, a.fontSize,
                               a.textColor.isValid() ? a.textColor : QColor(Qt::black));
        } else if (a.kind == "Text") {
            const QString now = textDialog(this, tr("Sticky note"), tr("The note says:"), a.contents);
            if (!now.isNull() && now != a.contents) {
                QString error;
                annotChanged(doc->setNoteText(page, id, now, &error), tr("change the note"), error);
            }
        } else {
            say(tr("Drag it to move it, or press Delete to remove it."));
        }
        return;
    }
}

// ---- text marks: the selected text highlighted, struck out, underlined ----

void MainWindow::markText(const QString &kind)
{
    auto doc = view_->document();
    const int page = view_->selectionPage();
    if (!doc || page < 0)
        return;
    const std::vector<QRectF> boxes = view_->selectionBoxes();
    const QColor color = kind == "Highlight" ? colors_.value("highlight")
                         : kind == "Underline" ? QColor("#0000FF") : QColor("#FF0000");
    std::set<int> before;
    for (const Annot &a : doc->annots(page))
        before.insert(a.id);
    QString error;
    const bool ok = doc->addMarkup(page, kind, boxes, color, opacities_.value("highlight") / 100.0, &error);
    annotChanged(ok, tr("mark the text"), error);
    if (!ok)
        return;
    // the mark just made: picked up at once, its toolbar over it (a click elsewhere puts it down)
    for (const Annot &a : doc->annots(page))
        if (a.markup() && !before.count(a.id)) {
            showMarkBar(page, a.id);
            break;
        }
}

bool MainWindow::marksOver(int page, const std::vector<QRectF> &boxes)
{
    for (const Annot &a : view_->document()->annots(page))
        if (a.markup())
            for (const QRectF &r : a.pieces)
                for (const QRectF &b : boxes)
                    if (std::min(r.bottom(), b.bottom()) - std::max(r.top(), b.top()) >= 0.5 * std::min(r.height(), b.height()) &&
                        std::min(r.right(), b.right()) - std::max(r.left(), b.left()) > 1)
                        return true;
    return false;
}

void MainWindow::showTextBar()
{
    if (readMode_)  // (Read mode: text picked to copy - no bar to mark or change it)
        return;
    // the little toolbar over the selection: copy it, mark it, edit it
    const int page = view_->selectionPage();
    if (page < 0)
        return;
    const std::vector<QRectF> boxes = view_->selectionBoxes();
    auto *bar = new FloatBar(view_->viewport());
    bar->setProperty("text", true);
    bar->add(QStringLiteral("copy"), {}, tr("Copy"), {}, [this] { view_->copySelection(); });
    bar->separator();
    bar->add(QString(), icons::tinted(icons::get(QStringLiteral("mark_highlight")), QColor(255, 214, 0), colors_.value("highlight")),
             tr("Highlight"), {}, [this] { markText(QStringLiteral("Highlight")); });
    bar->add(QStringLiteral("mark_strike"), {}, tr("Strike out"), {}, [this] { markText(QStringLiteral("StrikeOut")); });
    bar->add(QStringLiteral("mark_underline"), {}, tr("Underline"), {}, [this] { markText(QStringLiteral("Underline")); });
    bar->add(QStringLiteral("mark_squiggly"), {}, tr("Squiggly underline"), {}, [this] { markText(QStringLiteral("Squiggly")); });
    bar->separator();
    bar->add(QStringLiteral("edit"), {}, tr("Edit text"), tr("Edit text"), [this] { notYet(tr("Edit text")); });
    if (marksOver(page, boxes)) {  // (marked already: the mark can be taken off these letters)
        bar->separator();
        bar->add(QStringLiteral("eraser"), {}, tr("Remove the highlight (or other mark) from the selected letters"),
                 tr("Remove"), [this] {
                     auto doc = view_->document();
                     const int pg = view_->selectionPage();
                     if (!doc || pg < 0)
                         return;
                     QString error;
                     annotChanged(doc->cutMarks(pg, view_->selectionBoxes(), &error), tr("remove the highlight"), error);
                 });
    }
    view_->setFloatBar(bar, page, boxes);
    const int n = int(view_->selectedText().remove(QLatin1Char('\n')).size());
    say(tr("%1 letter%2 selected: copy (Ctrl + C), mark or edit %3 with the little toolbar.")
            .arg(n)
            .arg(n != 1 ? QStringLiteral("s") : QString())
            .arg(n != 1 ? tr("them") : tr("it")));
}

void MainWindow::showMarkBar(int page, int id)
{
    // a highlight (or other text mark) clicked: picked up, with its toolbar
    auto doc = view_->document();
    if (!doc)
        return;
    Annot mark;
    for (const Annot &a : doc->annots(page))
        if (a.id == id)
            mark = a;
    if (!mark.id)
        return;
    view_->pick(page, id);
    auto *bar = new FloatBar(view_->viewport());
    bar->setProperty("mark", true);
    bar->add(QStringLiteral("copy"), {}, tr("Copy its text"), {}, [this, page, mark] {
        const QString text = markedText(page, mark);
        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text);
            say(tr("Copied the marked text."));
        }
    });
    bar->separator();
    bar->add(QString(), icons::swatch(mark.color.isValid() ? mark.color : QColor(Qt::yellow)), tr("Colour"), {},
             [this, page, id, mark] {
                 const QColor c = colorDialog(this, tr("Color"), mark.color);
                 if (!c.isValid())
                     return;
                 QString error;
                 annotChanged(view_->document()->recolorAnnot(page, id, c, &error), tr("change the colour"), error);
                 showMarkBar(page, id);  // (still picked up, its toolbar back)
             });
    bar->separator();
    bar->add(QStringLiteral("eraser"), {}, tr("Remove (Delete)"), tr("Remove"), [this] { deletePicked(); });
    view_->setFloatBar(bar, page, mark.pieces);
    say(tr("Picked up: copy its text, change its colour, or remove it (Delete)."));
}

QString MainWindow::markedText(int page, const Annot &mark)
{
    // the letters inside its pieces, line by line
    auto chars = view_->document()->text(page);
    QStringList lines;
    int line = -1;
    for (const TextChar &c : *chars) {
        bool in = false;
        for (const QRectF &r : mark.pieces)
            in = in || r.contains(c.box.center());
        if (!in)
            continue;
        if (c.line != line) {
            lines << QString();
            line = c.line;
        }
        lines.back() += QString::fromUcs4(&c.c, 1);
    }
    QStringList kept;
    for (const QString &l : lines)
        if (!l.trimmed().isEmpty())
            kept << l.trimmed();
    return kept.join(QLatin1Char('\n'));
}

// ---- the Annotations list (in the side panel) ----

void MainWindow::buildAnnotationList()
{
    annotList_ = new QTreeWidget(sideStack_);
    annotList_->setHeaderHidden(true);
    annotList_->setFont(theme::font());
    annotList_->setRootIsDecorated(true);
    annotList_->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *page = new QWidget(sideStack_);
    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(0, 0, 0, 0);
    box->addWidget(new ScrollBox(annotList_, false, page), 1);
    annotNote_ = new EngravedLabel(page);
    box->addWidget(annotNote_);
    sideStack_->addWidget(page);  // (index 2)
    auto go = [this](QTreeWidgetItem *item, bool edit) {
        auto doc = view_->document();
        if (!item || !doc)
            return;
        const int page = item->data(0, Qt::UserRole).toInt();
        const int id = item->data(0, Qt::UserRole + 1).toInt();
        if (page >= doc->pageCount())
            return;
        if (!id) {
            view_->goToPage(page);
            return;
        }
        if (tool_ != "select" && tool_ != "move")
            setTool(QStringLiteral("select"), false);
        for (const Annot &a : doc->annots(page))
            if (a.id == id)
                view_->showRect(page, a.rect);
        view_->pick(page, id);
        if (edit)
            editAnnot(page, id);
    };
    connect(annotList_, &QTreeWidget::itemClicked, this, [go](QTreeWidgetItem *it) { go(it, false); });
    connect(annotList_, &QTreeWidget::itemDoubleClicked, this, [go](QTreeWidgetItem *it) { go(it, true); });
    connect(annotList_, &QTreeWidget::customContextMenuRequested, this, [this, go](QPoint at) {
        QTreeWidgetItem *it = annotList_->itemAt(at);
        if (!it)
            return;
        annotList_->setCurrentItem(it);
        QMenu menu(this);
        menu.addAction(tr("Go to it"), this, [go, it] { go(it, false); });
        if (it->data(0, Qt::UserRole + 1).toInt()) {
            menu.addSeparator();
            menu.addAction(tr("Delete"), this, [this, go, it] {
                go(it, false);
                deletePicked();
            });
        }
        menu.exec(annotList_->viewport()->mapToGlobal(at));
    });
}

// ---- the Bookmarks list (the PDF's own table of contents, in the side panel) ----

void MainWindow::buildBookmarkList()
{
    bookmarkList_ = new QTreeWidget(sideStack_);
    bookmarkList_->setHeaderHidden(true);
    bookmarkList_->setFont(theme::font());
    bookmarkList_->setRootIsDecorated(true);
    auto *page = new QWidget(sideStack_);
    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(0, 0, 0, 0);
    box->addWidget(new ScrollBox(bookmarkList_, false, page), 1);
    bookmarkNote_ = new EngravedLabel(page);
    box->addWidget(bookmarkNote_);
    sideStack_->addWidget(page);  // (index 3)
    // a bookmark clicked: its page, scrolled to the place it marks
    connect(bookmarkList_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        auto doc = view_->document();
        if (!it || !doc)
            return;
        const int page = it->data(0, Qt::UserRole).toInt();
        if (page < 0 || page >= doc->pageCount())
            return;
        const QPointF at = it->data(0, Qt::UserRole + 1).toPointF();
        view_->goToPage(page);
        if (!at.isNull())
            view_->showRect(page, QRectF(at, QSizeF(1, 1)));
    });
}

void MainWindow::fillBookmarkList(bool fresh)
{
    if (!bookmarkList_)
        return;
    // (what was open stays open when it's made again - by the titles down to it)
    auto pathOf = [](QTreeWidgetItem *it) {
        QStringList names;
        for (; it; it = it->parent())
            names.prepend(it->text(0));
        return names.join(QChar(0x1F));
    };
    QSet<QString> opened;
    if (!fresh) {
        QTreeWidgetItemIterator i(bookmarkList_);
        for (; *i; ++i)
            if ((*i)->isExpanded())
                opened.insert(pathOf(*i));
    }
    bookmarkList_->clear();
    auto doc = view_->document();
    const std::vector<Bookmark> marks = doc ? doc->bookmarks() : std::vector<Bookmark>{};
    std::vector<QTreeWidgetItem *> parents;
    for (const Bookmark &b : marks) {
        const int level = std::min(b.level, int(parents.size()));
        parents.resize(size_t(level));
        auto *item = parents.empty() ? new QTreeWidgetItem(bookmarkList_, {b.title})
                                     : new QTreeWidgetItem(parents.back(), {b.title});
        item->setData(0, Qt::UserRole, b.page);
        item->setData(0, Qt::UserRole + 1, b.at);
        item->setToolTip(0, b.title);
        parents.push_back(item);
    }
    QTreeWidgetItemIterator i(bookmarkList_);
    for (; *i; ++i)
        if (opened.contains(pathOf(*i)))
            (*i)->setExpanded(true);
    bookmarkNote_->setText(marks.empty() && doc ? tr("This PDF has no bookmarks.") : QString());
    bookmarkNote_->setVisible(marks.empty() && doc);
}

bool MainWindow::annotationsShowing() const
{
    return showThumbs_ && sideStack_->currentIndex() == 2;
}

void MainWindow::showAnnotations()
{
    // the list at the left - or, when it's already showing, back to the pages
    if (annotationsShowing()) {
        showSideView(0);
        return;
    }
    if (!showThumbs_)
        toggleThumbs();
    showSideView(2);
}

void MainWindow::fillAnnotationList()
{
    if (!annotList_)
        return;
    annotList_->clear();
    auto doc = view_->document();
    int count = 0;
    for (int i = 0; doc && i < doc->pageCount(); ++i) {
        const std::vector<Annot> list = doc->annots(i);
        if (list.empty())
            continue;
        auto *pageItem = new QTreeWidgetItem(annotList_, {tr("Page %1").arg(i + 1)});
        pageItem->setData(0, Qt::UserRole, i);
        pageItem->setData(0, Qt::UserRole + 1, 0);
        for (const Annot &a : list) {
            QString label = a.kind;
            for (const auto &n : kAnnotNames)
                if (a.kind == QLatin1String(n[0]))
                    label = tr(n[1]);
            if (a.kind == "Ink" && a.subject == "Marker")
                label = tr("Marker");
            QString says = a.contents.simplified();
            if (says.isEmpty() && a.markup())
                says = markedText(i, a).simplified();
            auto *item = new QTreeWidgetItem(pageItem, {says.isEmpty() ? label : QStringLiteral("%1: %2").arg(label, says.left(40))});
            item->setData(0, Qt::UserRole, i);
            item->setData(0, Qt::UserRole + 1, a.id);
            ++count;
        }
        pageItem->setExpanded(true);
    }
    annotNote_->setText(count || !doc ? QString()
                                      : tr("No annotations yet. Add highlights, drawings or shapes with the toolbar."));
    annotNote_->setVisible(!count && doc);
}

void MainWindow::selectInList(int page, int id)
{
    for (int n = 0; n < annotList_->topLevelItemCount(); ++n) {
        QTreeWidgetItem *p = annotList_->topLevelItem(n);
        for (int k = 0; k < p->childCount(); ++k)
            if (p->child(k)->data(0, Qt::UserRole).toInt() == page && p->child(k)->data(0, Qt::UserRole + 1).toInt() == id)
                annotList_->setCurrentItem(p->child(k));
    }
}
