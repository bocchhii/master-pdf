// MainWindow's text tools (step 5): the Edit tool's line picked up - retyped, moved, made
// bigger, restyled from the bar under the tools - and Add text, written into the page itself
// in the font, size and colour chosen (like the Python app's).

#include "ui/FlowLayout.h"
#include "app/MainWindow.h"

#include "core/Document.h"
#include "core/Fonts.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <set>
#include <QTimer>
#include <QTextEdit>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>

namespace {

// a picture beside a box (the spacing boxes'): drawn as the buttons draw theirs - sharp at 125%,
// its black white in the dark themes
class IconMark : public QWidget {
public:
    IconMark(const QString &icon, QWidget *parent) : QWidget(parent), icon_(icon) { setFixedSize(18, ToolButton::H); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        icons::draw(p, QRectF((width() - 16) / 2, (height() - 16) / 2, 16, 16), icon_, isEnabled(), 16);
    }

private:
    QString icon_;
};

const QStringList kSizes = {"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "36", "48"};

// the colour button's picture: the A with a bar of the colour under it
QImage colorIcon(const QColor &c)
{
    QImage im = icons::get(QStringLiteral("fmt_color")).convertToFormat(QImage::Format_ARGB32);
    for (int y = 12; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            if (qAlpha(im.pixel(x, y)))
                im.setPixelColor(x, y, c);
    return im;
}

}  // namespace

void MainWindow::buildTextProps(QHBoxLayout *pr, QWidget *props)
{
    textGroup_ = new QWidget(props);
    auto *l = new QHBoxLayout(textGroup_);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(1);
    fontBox_ = new QComboBox(textGroup_);
    fontBox_->setFont(theme::font());
    fontBox_->setFixedSize(QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0')) * 17 + 26, 20);
    fontBox_->setMaxVisibleItems(20);
    fontBox_->setWhatsThis(tr("The font of the text you add - or of the line picked up with the Edit tool."));
    connect(fontBox_, &QComboBox::textActivated, this, [this](const QString &family) {
        if (propsLoading_)
            return;
        if (view_->lineEditorOpen()) {  // (the line being typed: its letters selected)
            view_->editorSet(QStringLiteral("family"), family);
            return;
        }
        if (tool_ == "edit" && view_->pickedLine() >= 0) {
            Document::LineLook look;
            look.family = family;
            restyleLine(look);
        } else {
            textFamily_ = family;
            view_->setTool(toolLook());
        }
    });
    l->addWidget(fontBox_);
    l->addSpacing(2);
    sizeBox_ = new QComboBox(textGroup_);
    sizeBox_->setEditable(true);
    sizeBox_->setInsertPolicy(QComboBox::NoInsert);
    sizeBox_->setFont(theme::font());
    sizeBox_->addItems(kSizes);
    sizeBox_->setMaxVisibleItems(kSizes.size());
    sizeBox_->setFixedSize(QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0')) * 4 + 26, 20);
    auto sizeChosen = [this](const QString &t) {
        if (propsLoading_ || !oncePerKey())
            return;
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok || v <= 0 || v > 200)
            return;
        if (view_->lineEditorOpen()) {
            view_->editorSet(QStringLiteral("size"), v);
            return;
        }
        if (tool_ == "edit" && view_->pickedLine() >= 0) {
            Document::LineLook look;
            look.size = v;
            restyleLine(look);
        } else {
            sizes_["text"] = v;
            view_->setTool(toolLook());
        }
    };
    connect(sizeBox_, &QComboBox::textActivated, this, sizeChosen);
    connect(sizeBox_->lineEdit(), &QLineEdit::returnPressed, this, [this, sizeChosen] { sizeChosen(sizeBox_->currentText()); });
    l->addWidget(sizeBox_);
    l->addSpacing(2);
    auto button = [&](const QString &key, const QString &icon, const QString &tip, std::function<void()> fn) {
        auto *b = new ToolButton(icon, tip, {}, false, {}, textGroup_);
        b->setWhiten(16);
        connect(b, &ToolButton::clicked, this, fn);
        l->addWidget(b);
        buttons_.insert(key, b);
        return b;
    };
    auto step = [this](int d) {
        // one size bigger or smaller (of the list's sizes)
        if (view_->lineEditorOpen()) {  // (each letter selected, from its own size)
            view_->editorSet(QStringLiteral("step"), d, kSizes);
            return;
        }
        const double now = tool_ == "edit" && view_->pickedLine() >= 0
                               ? view_->linesOf(view_->pickedLinePage())[size_t(view_->pickedLine())].mainRun().size
                               : sizes_.value("text");
        double to = now;
        if (d > 0) {
            for (const QString &s : kSizes)
                if (s.toDouble() > now + 0.05) {
                    to = s.toDouble();
                    break;
                }
        } else {
            for (int k = int(kSizes.size()) - 1; k >= 0; --k)
                if (kSizes[k].toDouble() < now - 0.05) {
                    to = kSizes[k].toDouble();
                    break;
                }
        }
        if (to == now)
            return;
        if (tool_ == "edit" && view_->pickedLine() >= 0) {
            Document::LineLook look;
            look.size = to;
            restyleLine(look);
        } else {
            sizes_["text"] = to;
            refreshProps();
            view_->setTool(toolLook());
        }
    };
    button("grow", "fmt_grow", tr("Bigger text"), [step] { step(1); });
    button("shrink", "fmt_shrink", tr("Smaller text"), [step] { step(-1); });
    l->addWidget(new ToolSeparator(textGroup_));
    for (const char *k : {"bold", "italic", "underline", "strike"}) {
        const QString key = QString::fromLatin1(k);
        static const std::map<QString, QString> tips = {
            {"bold", "Bold"}, {"italic", "Italic"}, {"underline", "Underline"}, {"strike", "Strikethrough"}};
        button("fmt_" + key, "fmt_" + key, tr(qPrintable(tips.at(key))), [this, key] { toggleStyle(key); });
    }
    for (int rise : {1, -1})  // superscript, subscript (on the letters selected while typing; else all of it)
        button(rise > 0 ? "fmt_super" : "fmt_sub", rise > 0 ? "fmt_super" : "fmt_sub", rise > 0 ? tr("Superscript") : tr("Subscript"),
               [this, rise] {
                   Document::LineLook look;
                   const bool on = view_->lineEditorOpen() && view_->editorHas(rise > 0 ? QStringLiteral("super") : QStringLiteral("sub"));
                   look.rise = on ? 0 : rise;
                   formatText(look);
               });
    l->addWidget(new ToolSeparator(textGroup_));
    auto *color = button("text_color", "fmt_color", tr("Text colour"), [this] {
        if (view_->lineEditorOpen()) {
            const QColor c = colorDialog(this, tr("Color"), Qt::black);
            if (c.isValid())
                view_->editorSet(QStringLiteral("color"), c);
            return;
        }
        const QColor now = tool_ == "edit" && view_->pickedLine() >= 0
                               ? QColor::fromRgb(view_->linesOf(view_->pickedLinePage())[size_t(view_->pickedLine())].mainRun().color)
                               : colors_.value("text");
        const QColor c = colorDialog(this, tr("Color"), now);
        if (!c.isValid())
            return;
        if (tool_ == "edit" && view_->pickedLine() >= 0) {
            Document::LineLook look;
            look.color = c;
            restyleLine(look);
        } else {
            colors_["text"] = c;
            refreshProps();
            view_->setTool(toolLook());
        }
    });
    color->setWhiten(12);
    l->addWidget(new ToolSeparator(textGroup_));
    for (const char *k : {"left", "center", "right", "justify"}) {
        const QString kind = QString::fromLatin1(k);
        static const std::map<QString, QString> tips = {
            {"left", "Align left"}, {"center", "Centre"}, {"right", "Align right"}, {"justify", "Justify"}};
        button("align_" + kind, "align_" + kind, tr(qPrintable(tips.at(kind))), [this, kind] {
            if (view_->lineEditorOpen() || (tool_ == "edit" && view_->pickedLine() >= 0)) {  // (the block picked up)
                Document::LineLook look;
                look.align = kind;
                formatText(look);
                return;
            }
            textAlign_ = kind;
            refreshProps();
        });
    }
    // lists: bullets / numbers (each with its list of signs)
    for (bool numbers : {false, true}) {
        auto *b = new ToolButton(numbers ? QStringLiteral("list_number") : QStringLiteral("list_bullet"),
                                 numbers ? tr("Numbered list") : tr("Bulleted list"), {}, true, {}, textGroup_);
        b->setWhiten(16);
        connect(b, &ToolButton::clicked, this, [this, numbers] {
            Document::LineLook look;
            look.list = numbers ? QStringLiteral("number:") + listNumber_ : QStringLiteral("bullet:") + listBullet_;
            formatText(look);
        });
        connect(b, &ToolButton::arrowClicked, this, [this, numbers] { listMenu(numbers); });
        l->addWidget(b);
        buttons_.insert(numbers ? QStringLiteral("list_number") : QStringLiteral("list_bullet"), b);
    }
    dirSep_ = new ToolSeparator(textGroup_);
    l->addWidget(dirSep_);
    for (const char *k : {"ltr", "rtl"}) {
        const QString kind = QString::fromLatin1(k);
        button("dir_" + kind, "dir_" + kind, kind == "ltr" ? tr("Left-to-right text") : tr("Right-to-left text"),
               [this, kind] {
                   textDir_ = textDir_ == kind ? QString() : kind;
                   if (textDir_ == "rtl" && textAlign_ == "left")
                       textAlign_ = QStringLiteral("right");
                   refreshProps();
               });
    }
    // spacing: lines, paragraphs - the letters' width and the room after each
    formatSep_ = new ToolSeparator(textGroup_);
    l->addWidget(formatSep_);
    auto spin = [&](const QString &icon, const QString &tip, const QStringList &values, int chars, QComboBox **box,
                    std::function<void(double)> apply) {
        auto *pic = new IconMark(icon, textGroup_);
        pic->setToolTip(tip);
        l->addWidget(pic);
        auto *c = new QComboBox(textGroup_);
        c->setEditable(true);
        c->setFont(theme::font());
        c->addItems(values);
        c->setMaxVisibleItems(9);  // (every one's list as tall: 9 to each)
        c->setInsertPolicy(QComboBox::NoInsert);  // (a value typed in isn't added to its list - it grew with each one)
        c->setToolTip(tip);
        c->setFixedSize(QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0')) * chars + 22, 20);
        formatOnly_.push_back(pic);
        formatOnly_.push_back(c);
        auto chosen = [this, c, apply](const QString &t) {
            if (propsLoading_ || !oncePerKey())
                return;
            bool ok = false;
            const double v = QString(t).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
            if (ok)
                apply(v);
        };
        connect(c, &QComboBox::textActivated, this, chosen);
        connect(c->lineEdit(), &QLineEdit::returnPressed, this, [c, chosen] { chosen(c->currentText()); });
        l->addWidget(c);
        l->addSpacing(2);
        *box = c;
    };
    spin(QStringLiteral("spacing_line"), tr("Line spacing (lines)"), {"0.8", "1.0", "1.15", "1.2", "1.5", "1.75", "2.0", "2.5", "3.0"}, 4,
         &lineSpacingBox_, [this](double v) {
             if (v <= 0.3 || v > 10)
                 return;
             Document::LineLook look;
             look.lineSpacing = v;
             formatText(look);
         });
    spin(QStringLiteral("spacing_para"), tr("Paragraph spacing (points)"), {"0", "2", "4", "6", "8", "10", "12", "18", "24"}, 4,
         &paraSpacingBox_, [this](double v) {
             if (v < 0 || v > 200)
                 return;
             Document::LineLook look;
             look.paraSpacing = v;
             formatText(look);
         });
    spin(QStringLiteral("text_scale"), tr("Horizontal scaling (%)"), {"50", "75", "80", "90", "100", "110", "125", "150", "200"}, 4,
         &scaleBox_, [this](double v) {
             if (v < 10 || v > 400)
                 return;
             Document::LineLook look;
             look.scale = v;
             formatText(look);
         });
    spin(QStringLiteral("text_track"), tr("Character spacing (1/1000 em)"), {"-100", "-50", "-25", "0", "10", "25", "50", "100", "200"}, 4,
         &trackBox_, [this](double v) {
             if (v < -500 || v > 2000)
                 return;
             Document::LineLook look;
             look.track = v;
             formatText(look);
         });
    // (a narrow window: its groups under each other - the rows under the first start under the
    // bold / italic... buttons)
    if (FlowLayout *flow = flowInGroups(textGroup_))
        flow->setIndentUnder(buttons_["fmt_bold"]->parentWidget());
    pr->addWidget(textGroup_);
}

// the bullets' / numbers' list: which sign - or the list taken off
void MainWindow::listMenu(bool numbers)
{
    QMenu menu(this);
    const QStringList forms = numbers ? QStringList{"1.", "1)", "a.", "A.", "i.", "I."}
                                      : QStringList{"\u2022", "\u25E6", "\u25AA", "\u2013", "\u27A2", "\u2713"};
    const QString now = numbers ? listNumber_ : listBullet_;
    for (const QString &f : forms) {
        QAction *a = menu.addAction(numbers ? QStringLiteral("%1  %2").arg(f, f == "1." || f == "1)" ? QString(f).replace(QLatin1Char('1'), QLatin1Char('2')) : QString())
                                            : QStringLiteral("%1   %1   %1").arg(f));
        if (numbers) {  // (each: how the first three read)
            QStringList three;
            for (int n = 1; n <= 3; ++n) {
                const QChar k = f.at(0), end = f.at(f.size() - 1);
                const char *romans[] = {"i", "ii", "iii"};
                three << (k == 'a' ? QString(QChar('a' + n - 1)) : k == 'A' ? QString(QChar('A' + n - 1))
                          : k == 'i' ? QString::fromLatin1(romans[n - 1]) : k == 'I' ? QString::fromLatin1(romans[n - 1]).toUpper()
                                                                               : QString::number(n)) + end;
            }
            a->setText(three.join(QStringLiteral("   ")));
        }
        a->setCheckable(true);
        a->setChecked(f == now);
        connect(a, &QAction::triggered, this, [this, numbers, f] {
            (numbers ? listNumber_ : listBullet_) = f;
            Document::LineLook look;
            look.list = (numbers ? QStringLiteral("number:") : QStringLiteral("bullet:")) + f;
            formatText(look);
        });
    }
    menu.addSeparator();
    menu.addAction(tr("None (take the list off)"), this, [this] {
        Document::LineLook look;
        look.list = QStringLiteral("none");
        formatText(look);
    });
    ToolButton *b = buttons_.value(numbers ? QStringLiteral("list_number") : QStringLiteral("list_bullet"));
    menu.exec(b ? b->mapToGlobal(QPoint(0, b->height())) : QCursor::pos());
}

// a change from the Format controls: on the letters selected while typing (letter-by-letter
// things: superscript, width, spacing) - else on the line / block picked up (its alignment,
// spacing, list too: what's typed put in first)
void MainWindow::formatText(const Document::LineLook &lookIn)
{
    Document::LineLook look = lookIn;
    const bool letters = look.rise > -9 || look.scale > 0 || look.track > -1e8;
    if (view_->lineEditorOpen()) {
        if (letters) {
            if (look.rise > -9)
                view_->editorSet(QStringLiteral("rise"), look.rise);
            if (look.scale > 0)
                view_->editorSet(QStringLiteral("scale"), look.scale);
            if (look.track > -1e8)
                view_->editorSet(QStringLiteral("track"), look.track);
            refreshProps();
            return;
        }
        // (alignment, spacing, a list: for the paragraphs the caret or the selection is in - as Acrobat,
        // Word - not the whole block)
        look.paras = view_->editorParagraphs();
        view_->commitEditor();
    }
    if (tool_ != "edit" || view_->pickedLine() < 0) {
        say(tr("Pick up a line or a block of text with the Edit tool first."));
        return;
    }
    restyleLine(look);
}

bool MainWindow::oncePerKey()
{
    if (chosenNow_)
        return false;
    chosenNow_ = true;
    QTimer::singleShot(0, this, [this] { chosenNow_ = false; });
    return true;
}

void MainWindow::refreshTextProps(bool line)
{
    // what the text buttons show: the line picked up (Edit) - or how added text will look
    propsLoading_ = true;
    if (fontBox_->count() == 0)
        fontBox_->addItems(fonts::families());
    QString family = textFamily_;
    double size = sizes_.value("text");
    QColor color = colors_.value("text");
    bool bold = textBold_, italic = textItalic_, underline = textUnderline_, strike = textStrike_;
    // (the line picked up still there - after an Undo the page's lines are read again)
    line = line && view_->pickedLinePage() >= 0 && view_->pickedLine() >= 0 &&
           view_->pickedLine() < int(view_->linesOf(view_->pickedLinePage()).size());
    if (line) {
        const TextLine &l = view_->linesOf(view_->pickedLinePage())[size_t(view_->pickedLine())];
        const TextRun &r = l.mainRun();
        const fonts::Found f = fonts::findFont(r.font, r.flags);
        family = f.family;
        size = r.size;
        color = QColor::fromRgb(r.color);
        bold = f.bold;
        italic = f.italic;
        underline = strike = false;
        for (const Annot &a : view_->document()->annots(view_->pickedLinePage()))
            for (const QRectF &p : a.pieces)
                if (p.intersects(l.rect.adjusted(0, l.rect.height() * 0.3, 0, -l.rect.height() * 0.3))) {
                    underline = underline || a.kind == "Underline";
                    strike = strike || a.kind == "StrikeOut";
                }
    }
    if (view_->lineEditorOpen()) {  // (being typed: the look where the caret is - as Acrobat's bar)
        const QString fam = view_->editorValue(QStringLiteral("family")).toString();
        if (!fam.isEmpty())
            family = fam;
        if (view_->editorValue(QStringLiteral("size")).toDouble() > 0)
            size = view_->editorValue(QStringLiteral("size")).toDouble();
        bold = view_->editorValue(QStringLiteral("bold")).toBool();
        italic = view_->editorValue(QStringLiteral("italic")).toBool();
        underline = view_->editorValue(QStringLiteral("underline")).toBool();
        strike = view_->editorValue(QStringLiteral("strike")).toBool();
        color = view_->editorValue(QStringLiteral("color")).value<QColor>();
    }
    fontBox_->setCurrentText(family);
    fontBox_->setEditText(family);
    const int idx = fontBox_->findText(family, Qt::MatchFixedString);
    if (idx >= 0)
        fontBox_->setCurrentIndex(idx);
    sizeBox_->setEditText(QString::number(size, 'g', 4));
    buttons_["fmt_bold"]->setLatched(bold);
    buttons_["fmt_italic"]->setLatched(italic);
    buttons_["fmt_underline"]->setLatched(underline);
    buttons_["fmt_strike"]->setLatched(strike);
    buttons_["text_color"]->setImage(colorIcon(color));
    // (the alignment button of how it's aligned now: the text picked up - its paragraph's - or added text's)
    QString aligned = textAlign_;
    if (line) {
        const int page = view_->pickedLinePage(), at = view_->pickedLine();
        const std::vector<TextLine> &all = view_->linesOf(page);
        const int f = pagetext::frameOf(view_->framesOf(page), at);
        std::vector<int> para{at};
        QRectF box = all[size_t(at)].rect;
        const pagetext::TextFrame *fr = f >= 0 ? &view_->framesOf(page)[size_t(f)] : nullptr;
        if (fr) {
            box = fr->rect;
            for (const std::vector<int> &p : fr->paras)
                if (std::find(p.begin(), p.end(), at) != p.end())
                    para = p;
            // (being typed: the paragraph the caret's in - one to each of the editor's)
            const QTextEdit *ed = view_->lineEditor();
            if (ed && fr->paras.size() > 1 && ed->document()->blockCount() == int(fr->paras.size()))
                para = fr->paras[size_t(ed->textCursor().blockNumber())];
        }
        // (a line on its own, in a block of its own: left - or right, if it's right to left)
        aligned = pagetext::alignOf(all, para, box.left(), box.right());
        if (para.size() == 1 && std::abs(all[size_t(para.front())].rect.width() - box.width()) <= 2)
            aligned = all[size_t(para.front())].rtl ? "right" : "left";
        // (a paragraph of one line justified looks just as one aligned left: as its block's longer
        // paragraphs are, if they're justified - a heading over justified text)
        if (fr && para.size() == 1 && aligned == QLatin1String("left")) {
            int justified = 0, others = 0;
            for (const std::vector<int> &p : fr->paras)
                if (p.size() > 1)
                    (pagetext::alignOf(all, p, box.left(), box.right()) == QLatin1String("justify") ? justified : others)++;
            if (justified > 0 && others == 0)
                aligned = QStringLiteral("justify");
        }
    }
    for (const char *k : {"left", "center", "right", "justify"})
        buttons_[QStringLiteral("align_") + k]->setLatched(aligned == QLatin1String(k));
    // (Format: the picked block's spacing as it is; the letters' width / spacing: the letters
    // selected while typing - else as the font has them)
    const bool picked = line || view_->lineEditorOpen();
    for (QWidget *w : std::initializer_list<QWidget *>{lineSpacingBox_, paraSpacingBox_, scaleBox_, trackBox_, buttons_["list_bullet"],
                                                      buttons_["list_number"], buttons_["fmt_super"], buttons_["fmt_sub"]})
        w->setEnabled(picked);
    double lineSpacing = 1.2, paraSpacing = 0;
    if (line) {
        const int page = view_->pickedLinePage();
        const std::vector<TextLine> &all = view_->linesOf(page);
        const int f = pagetext::frameOf(view_->framesOf(page), view_->pickedLine());
        if (f >= 0) {
            const pagetext::TextFrame &fr = view_->framesOf(page)[size_t(f)];
            // (each step over its own line's size - a heading's 16 points aren't the body's 12; a gap
            // between paragraphs less the line step of the paragraph under it)
            std::vector<double> ratios, extras;
            auto ok = [&](const std::vector<int> &p) {
                for (int k : p)
                    if (k < 0 || k >= int(all.size()))
                        return false;
                return !p.empty();
            };
            for (const std::vector<int> &p : fr.paras)
                if (ok(p))
                    for (size_t m = 1; m < p.size(); ++m)
                        ratios.push_back((all[size_t(p[m])].baseline - all[size_t(p[m - 1])].baseline) /
                                         std::max(0.1, all[size_t(p[m])].mainRun().size));
            auto median = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            if (!ratios.empty())
                lineSpacing = median(ratios);
            for (size_t n = 1; n < fr.paras.size(); ++n) {
                if (!ok(fr.paras[n]) || !ok(fr.paras[n - 1]))
                    continue;
                const std::vector<int> &p = fr.paras[n];
                const double step = p.size() > 1 ? all[size_t(p[1])].baseline - all[size_t(p[0])].baseline
                                                 : lineSpacing * all[size_t(p[0])].mainRun().size;
                extras.push_back(all[size_t(p.front())].baseline - all[size_t(fr.paras[n - 1].back())].baseline - step);
            }
            if (!extras.empty())
                paraSpacing = std::max(0.0, median(extras));
        }
    }
    lineSpacingBox_->setEditText(QString::number(lineSpacing, 'f', 2));
    paraSpacingBox_->setEditText(QString::number(std::round(paraSpacing * 10) / 10, 'g', 4));
    const bool typing = view_->lineEditorOpen();
    // (the letters' width and spacing: the caret's while typing - else the picked text's, as the page has them)
    double scale = 100, track = 0;
    if (typing) {
        scale = view_->editorValue(QStringLiteral("scale")).toDouble();
        track = view_->editorValue(QStringLiteral("track")).toDouble();
    } else if (line) {
        const TextRun &r = view_->linesOf(view_->pickedLinePage())[size_t(view_->pickedLine())].mainRun();
        scale = r.scale;
        track = r.track;
    }
    // (read off the page, a pair kerned here and there: within a few of one of its list's values - that one)
    auto near = [](QComboBox *box, double v, double by) {
        for (int k = 0; k < box->count(); ++k)
            if (std::abs(box->itemText(k).toDouble() - v) <= by)
                return box->itemText(k).toDouble();
        return std::round(v);
    };
    scaleBox_->setEditText(QString::number(typing ? std::round(scale) : near(scaleBox_, scale, 2), 'g', 4));
    trackBox_->setEditText(QString::number(typing ? std::round(track) : near(trackBox_, track, 5), 'g', 5));
    buttons_["fmt_super"]->setLatched(typing && view_->editorHas(QStringLiteral("super")));
    buttons_["fmt_sub"]->setLatched(typing && view_->editorHas(QStringLiteral("sub")));
    // (the text's direction: for text added; the Format panel's spacing, lists, raised letters: for the
    // text picked up - each shown only where it does something, the bar fits a 125% screen)
    for (const char *k : {"ltr", "rtl"}) {
        buttons_[QStringLiteral("dir_") + k]->setLatched(!line && textDir_ == QLatin1String(k));
        buttons_[QStringLiteral("dir_") + k]->setVisible(!line && !view_->lineEditorOpen());
    }
    dirSep_->setVisible(!line && !view_->lineEditorOpen());
    const bool format = line || view_->lineEditorOpen();
    for (QWidget *w : formatOnly_)
        w->setVisible(format);
    formatSep_->setVisible(format);
    for (const char *k : {"fmt_super", "fmt_sub", "list_bullet", "list_number"})
        buttons_[QString::fromLatin1(k)]->setVisible(format);
    propsLoading_ = false;
}

void MainWindow::toggleStyle(const QString &key)
{
    if (view_->lineEditorOpen()) {  // (the letters selected in the line being typed - or all of it: on,
        view_->editorSet(key, !view_->editorHas(key));  // unless they all have it already)
        return;
    }
    if (tool_ == "edit" && view_->pickedLine() >= 0) {
        const int page = view_->pickedLinePage();
        const TextLine l = view_->linesOf(page)[size_t(view_->pickedLine())];
        std::vector<TextLine> block;  // (the box picked up: all its lines)
        for (int k : view_->frameLines(page, view_->pickedLine()))
            block.push_back(view_->linesOf(page)[size_t(k)]);
        if (block.empty())
            block.push_back(l);
        if (key == "bold" || key == "italic") {
            // (on, unless every line has it already - as Acrobat: a heading bold over plain text: all bold)
            bool all = true;
            for (const TextLine &b : block) {
                const fonts::Found f = fonts::findFont(b.mainRun().font, b.mainRun().flags);
                all = all && (key == "bold" ? f.bold : f.italic);
            }
            Document::LineLook look;
            if (key == "bold")
                look.bold = all ? 0 : 1;
            else
                look.italic = all ? 0 : 1;
            restyleLine(look);
            return;
        }
        // underline / strikethrough: a mark over the line, on or off
        auto doc = view_->document();
        // (over every line of the box: off if they all have it, else on where it's missing)
        const QString kind = key == "underline" ? QStringLiteral("Underline") : QStringLiteral("StrikeOut");
        std::set<int> found;
        std::vector<QRectF> missing;
        for (const TextLine &b : block) {
            const QRectF mid = b.rect.adjusted(0, b.rect.height() * 0.3, 0, -b.rect.height() * 0.3);
            bool has = false;
            for (const Annot &a : doc->annots(page))
                if (a.kind == kind)
                    for (const QRectF &p : a.pieces)
                        if (p.intersects(mid)) {
                            found.insert(a.id);
                            has = true;
                        }
            if (!has)
                missing.push_back(b.rect);
        }
        QString error;
        bool ok = true;
        if (missing.empty()) {
            doc->beginChanges(key == "underline" ? "underline" : "strikethrough");
            for (int id : found)
                ok = ok && doc->deleteAnnot(page, id, &error);
            doc->endChanges();
        } else {
            ok = doc->addMarkup(page, kind, missing, QColor::fromRgb(l.mainRun().color), 1, &error);
        }
        const int index = view_->pickedLine();
        annotChanged(ok, tr("change the text"), error);
        view_->pickLine(page, index);
        refreshProps();
        return;
    }
    bool *flag = key == "bold" ? &textBold_ : key == "italic" ? &textItalic_ : key == "underline" ? &textUnderline_ : &textStrike_;
    *flag = !*flag;
    refreshProps();
    view_->setTool(toolLook());
}

// a block of paragraphs (the frame the line's in) retyped - its paragraphs one to a line of text
// (null: as they are) - and / or in another look
void MainWindow::retypeBlock(int page, int index, const QString &text, const Document::LineLook &look)
{
    auto doc = view_->document();
    const std::vector<TextLine> lines = view_->linesOf(page);
    if (!doc || index < 0 || index >= int(lines.size()))
        return;
    const std::vector<pagetext::TextFrame> &fs = view_->framesOf(page);
    const int f = pagetext::frameOf(fs, index);
    pagetext::TextFrame frame;
    if (f >= 0) {
        frame = fs[size_t(f)];
    } else {  // (a line of its own: a block of one line)
        frame.lines = {index};
        frame.paras = {{index}};
    }
    // (alignment, spacing, a list for the paragraphs typed in: just those written again - the others as
    // they are, those after them moved with them; as Acrobat)
    if (!look.paras.empty() && text.isNull()) {
        const int from = std::clamp(*std::min_element(look.paras.begin(), look.paras.end()), 0, int(frame.paras.size()) - 1);
        const int to = std::clamp(*std::max_element(look.paras.begin(), look.paras.end()) + 1, from + 1, int(frame.paras.size()));
        Document::LineLook some = look;
        some.paras.clear();
        QString error;
        const bool ok = retypeSome(page, frame, from, to, nullptr, some, &error);
        lineChanged(ok, page, lines[size_t(frame.paras[size_t(from)].front())].rect, error);
        return;
    }
    // (its lines, top down, and its paragraphs by their places among them)
    std::vector<TextLine> all;
    std::vector<std::vector<int>> paras;
    QStringList now;
    for (const std::vector<int> &para : frame.paras) {
        paras.emplace_back();
        QStringList words;
        for (int k : para) {
            paras.back().push_back(int(all.size()));
            all.push_back(lines[size_t(k)]);
            words << lines[size_t(k)].text;
        }
        now << words.join(QLatin1Char(' '));
    }
    QString error;
    const bool ok = doc->retypeFrame(page, all, paras, text.isNull() ? now.join(QLatin1Char('\n')) : text, look, &error);
    lineChanged(ok, page, lines[size_t(frame.lines.front())].rect, error);
}

// the line picked up, in another look (its font, size, colour, bold, italic)
void MainWindow::restyleLine(const Document::LineLook &look)
{
    const int page = view_->pickedLinePage(), index = view_->pickedLine();
    auto doc = view_->document();
    if (!doc || index < 0)
        return;
    // (a block of paragraphs: all of it; a line's alignment, spacing, list, box: as a block of one line)
    const bool blockWise = !look.align.isEmpty() || look.lineSpacing > 0 || look.paraSpacing >= 0 || !look.list.isEmpty() ||
                           look.boxLeft > -1e8 || look.boxRight > -1e8;
    if (view_->frameLines(page, index).size() > 1 || blockWise) {
        retypeBlock(page, index, QString(), look);
        return;
    }
    const TextLine line = view_->linesOf(page)[size_t(index)];
    QString error;
    const bool ok = doc->retypeLine(page, line, line.text, look, &error);
    lineChanged(ok, page, line.rect, error);
}

// after a line changed: drawn again, and the line (where it is now) still picked up
void MainWindow::lineChanged(bool ok, int page, const QRectF &near, const QString &error)
{
    annotChanged(ok, tr("change the text"), error);
    if (!ok)
        return;
    const std::vector<TextLine> &lines = view_->linesOf(page);
    int best = -1;
    double far = 1e9;
    for (int n = 0; n < int(lines.size()); ++n) {
        const QRectF &r = lines[size_t(n)].rect;
        const double d = std::abs(r.center().y() - near.center().y()) * 3 + std::abs(r.left() - near.left());
        if (d < far && r.intersects(near.adjusted(-40, -10, 40, 10))) {
            far = d;
            best = n;
        }
    }
    view_->pickLine(page, best);
    refreshProps();
}

void MainWindow::connectTextTools()
{
    connect(view_, &PageView::linePicked, this, [this] { refreshProps(); });
    connect(view_, &PageView::editorCaretMoved, this, [this] { refreshProps(); });
    // the text bar keeps the line being typed open (its buttons restyle the letters selected)
    view_->setEditorKeeper([this](QWidget *w) { return w && textGroup_ && (w == textGroup_ || textGroup_->isAncestorOf(w)); });
    connect(view_, &PageView::lineRestyled, this, [this](int page, int index, const std::vector<Document::StyledRun> &runs) {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        QString error;
        lineChanged(doc->retypeRuns(page, lines[size_t(index)], runs, &error), page, lines[size_t(index)].rect, error);
    });
    connect(view_, &PageView::lineEdited, this, [this](int page, int index, const QString &text) {
        auto doc = view_->document();
        if (!doc)
            return;
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (index < 0 || index >= int(lines.size()))
            return;
        // a line of a paragraph: the paragraph's words flowed again
        const std::vector<int> para = pagetext::paragraph(lines, index);
        QString error;
        bool ok;
        if (para.size() > 1) {
            std::vector<TextLine> p;
            int at = 0;
            for (size_t k = 0; k < para.size(); ++k) {
                p.push_back(lines[size_t(para[k])]);
                if (para[k] == index)
                    at = int(k);
            }
            ok = doc->retypeParagraph(page, p, at, text, {}, &error);
        } else {
            ok = doc->retypeLine(page, lines[size_t(index)], text, {}, &error);
        }
        lineChanged(ok, page, lines[size_t(index)].rect, error);
    });
    connect(view_, &PageView::lineTurned, this, [this](int page, int index, double degrees) {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        QString error;
        const std::vector<int> block = view_->frameLines(page, index);
        if (block.size() > 1) {  // (a block: each of its lines turned round its middle - one change)
            QRectF box;
            for (int k : block)
                box |= lines[size_t(k)].rect;
            std::vector<TextLine> all;
            for (int k : block)
                all.push_back(lines[size_t(k)]);
            annotChanged(doc->turnLines(page, all, degrees, box.center(), &error), tr("turn the text"), error);
            return;
        }
        lineChanged(doc->turnLine(page, lines[size_t(index)], degrees, &error), page, lines[size_t(index)].rect, error);
    });
    connect(view_, &PageView::frameEdited, this, [this](int page, int index, const QString &text) {
        retypeBlock(page, index, text, {});
    });
    connect(view_, &PageView::frameRestyled, this, [this](int page, int index, const std::vector<std::vector<Document::StyledRun>> &parasIn, int from, int keep) {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        const std::vector<pagetext::TextFrame> &fs = view_->framesOf(page);
        const int f = pagetext::frameOf(fs, index);
        if (f < 0)
            return;
        const pagetext::TextFrame frame = fs[size_t(f)];
        // (just its changed paragraphs - those above them as they are, as Acrobat; those after them
        // moved up or down as much as they grew or shrank, not written again: a stand-in font a
        // little wider would wrap lines that fitted)
        from = std::clamp(from, 0, std::max(0, int(frame.paras.size()) - 1));
        const int oldEnd = int(frame.paras.size()) - keep, newEnd = int(parasIn.size()) - keep;
        if (keep <= 0 || oldEnd <= from || newEnd <= from)
            keep = 0;
        const int oldTo = keep > 0 ? oldEnd : int(frame.paras.size()), newTo = keep > 0 ? newEnd : int(parasIn.size());
        const std::vector<std::vector<Document::StyledRun>> paras(parasIn.begin() + std::min<ptrdiff_t>(from, ptrdiff_t(parasIn.size())),
                                                                  parasIn.begin() + newTo);
        if (from >= oldTo || paras.empty())
            return;
        QString error;
        const bool ok = retypeSome(page, frame, from, oldTo, &paras, Document::LineLook(), &error);
        lineChanged(ok, page, lines[size_t(frame.paras[size_t(from)].front())].rect, error);
    });
    connect(view_, &PageView::lineMoved, this, [this](int page, int index, QPointF by) {
        lineMovedBy(page, index, by);
    });
    connect(view_, &PageView::groupMoved, this, &MainWindow::groupMovedBy);
    connect(view_, &PageView::groupDelete, this, &MainWindow::groupDeleted);
    connect(view_, &PageView::groupChanged, this, [this](int count) {
        if (count > 1)
            say(tr("%1 picked together: drag one of them to move them all. Ctrl or Shift + click picks more - or puts one "
                   "down; Esc puts them all down.").arg(count));
    });
    connect(view_, &PageView::textResized, this, [this](int page, int index, double left, double right, double k) {
        // its box resized (as Acrobat's): its sides - the text wrapped in them again; its top /
        // bottom - the text bigger or smaller
        auto doc = view_->document();
        if (!doc || index < 0 || index >= int(view_->linesOf(page).size()))
            return;
        const TextLine line = view_->linesOf(page)[size_t(index)];
        if (std::abs(k - 1) > 0.001 && view_->frameLines(page, index).size() == 1) {
            QString error;
            lineChanged(doc->scaleLine(page, line, k, false, &error), page, line.rect, error);
            return;
        }
        Document::LineLook look;
        if (std::abs(k - 1) > 0.001)
            look.size = line.mainRun().size * k;
        look.boxLeft = left;
        look.boxRight = right;
        retypeBlock(page, index, QString(), look);
    });
    connect(view_, &PageView::lineScaled, this, [this](int page, int index, double k, bool fromRight) {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        QString error;
        const bool ok = doc->scaleLine(page, lines[size_t(index)], k, fromRight, &error);
        lineChanged(ok, page, lines[size_t(index)].rect, error);
    });
    connect(view_, &PageView::lineDelete, this, [this](int page, int index) {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        const std::vector<int> block = view_->frameLines(page, index);
        view_->pickLine(-1, -1);
        QString error;
        if (block.size() > 1) {  // (a block of paragraphs: all of it)
            std::vector<TextLine> all;
            for (int k : block)
                all.push_back(lines[size_t(k)]);
            annotChanged(doc->deleteLines(page, all, &error), tr("delete the text"), error);
            return;
        }
        annotChanged(doc->deleteLine(page, lines[size_t(index)], &error), tr("delete the text"), error);
    });
}

bool MainWindow::retypeSome(int page, const pagetext::TextFrame &frame, int from, int oldTo,
                            const std::vector<std::vector<Document::StyledRun>> *styled, const Document::LineLook &lookIn, QString *error)
{
    auto doc = view_->document();
    const std::vector<TextLine> lines = view_->linesOf(page);
    std::vector<TextLine> all, after;
    std::vector<std::vector<int>> parts;
    QStringList texts;
    for (int n = from; n < oldTo; ++n) {
        parts.emplace_back();
        QStringList words;
        for (int k : frame.paras[size_t(n)]) {
            parts.back().push_back(int(all.size()));
            all.push_back(lines[size_t(k)]);
            words << lines[size_t(k)].text;
        }
        texts << words.join(QLatin1Char(' '));
    }
    for (int n = oldTo; n < int(frame.paras.size()); ++n)
        for (int k : frame.paras[size_t(n)])
            after.push_back(lines[size_t(k)]);
    if (!doc || all.empty())
        return false;
    {
        double oldLast = 0;
        for (const TextLine &l : all)
            oldLast = std::max(oldLast, l.baseline);
        // (the block's usual gap between paragraphs - a paragraph made with Enter gets it)
        std::vector<double> paraSteps;
        for (size_t n = 1; n < frame.paras.size(); ++n)
            if (!frame.paras[n].empty() && !frame.paras[n - 1].empty())
                paraSteps.push_back(lines[size_t(frame.paras[n].front())].baseline - lines[size_t(frame.paras[n - 1].back())].baseline);
        std::sort(paraSteps.begin(), paraSteps.end());
        const double paraStep = paraSteps.empty() ? 0 : paraSteps[paraSteps.size() / 2];
        doc->beginChanges(styled ? "edit text" : "change text");
        bool ok;
        if (styled) {
            ok = doc->retypeFrameRuns(page, all, parts, *styled, error, frame.rect.left(), frame.rect.right(), paraStep);
        } else {
            Document::LineLook look = lookIn;  // (in the block's sides, its usual paragraph gap)
            look.boxLeft = look.boxLeft > -1e8 ? look.boxLeft : frame.rect.left();
            look.boxRight = look.boxRight > -1e8 ? look.boxRight : frame.rect.right();
            look.paraGapWas = paraStep;
            ok = doc->retypeFrame(page, all, parts, texts.join(QLatin1Char('\n')), look, error);
        }
        if (ok && !after.empty()) {
            // (where the written ones end now: the lines there from the first written one down, but those
            // that were there before and weren't written again - the ones after, other text. A written
            // line the same as it was - its last, "growth." - counts: it's where they end)
            const std::vector<TextLine> now = doc->lines(page);
            auto same = [](const TextLine &a, const TextLine &b) {
                return a.text == b.text && (a.rect.topLeft() - b.rect.topLeft()).manhattanLength() < 0.5;
            };
            auto hadElsewhere = [&](const TextLine &l) {
                for (const TextLine &o : lines)
                    if (same(o, l) && std::none_of(all.begin(), all.end(), [&](const TextLine &a) { return same(a, o); }))
                        return true;
                return false;
            };
            double newLast = -1e9;
            for (const TextLine &l : now)
                if (!hadElsewhere(l) && l.rect.bottom() > all.front().rect.top() - 1 && l.rect.center().x() > frame.rect.left() - 2 &&
                    l.rect.center().x() < frame.rect.right() + 2)
                    newLast = std::max(newLast, l.baseline);
            const double by = newLast > -1e8 ? newLast - oldLast : 0;
            if (std::abs(by) > 0.3) {
                std::vector<TextLine> moving;  // (as they are on the page now)
                for (const TextLine &a : after)
                    for (const TextLine &l : now)
                        if (l.text == a.text && (l.rect.topLeft() - a.rect.topLeft()).manhattanLength() < 0.5) {
                            moving.push_back(l);
                            break;
                        }
                ok = doc->moveLines(page, moving, QPointF(0, by), error);
            }
        }
        doc->endChanges();
        return ok;
    }
}

void MainWindow::lineMovedBy(int page, int index, QPointF by)
{
    {
        auto doc = view_->document();
        const std::vector<TextLine> lines = view_->linesOf(page);
        if (!doc || index < 0 || index >= int(lines.size()))
            return;
        QString error;
        const std::vector<int> block = view_->frameLines(page, index);
        if (block.size() > 1) {  // (a block of paragraphs: all of it)
            std::vector<TextLine> all;
            for (int k : block)
                all.push_back(lines[size_t(k)]);
            lineChanged(doc->moveLines(page, all, by, &error), page, lines[size_t(index)].rect.translated(by), error);
            return;
        }
        const bool ok = doc->moveLine(page, lines[size_t(index)], by, &error);
        lineChanged(ok, page, lines[size_t(index)].rect.translated(by), error);
    }
}

void MainWindow::groupMovedBy(int page, const std::vector<int> &lineIndexes, const std::vector<int> &picIndexes, QPointF by)
{
    auto doc = view_->document();
    if (!doc)
        return;
    const std::vector<TextLine> lines = view_->linesOf(page);
    const std::vector<Picture> pics = view_->picturesOf(page);
    // what moves, as it is now - and where each will be (to pick them together again there)
    std::vector<TextLine> texts;
    std::vector<QRectF> textsThere, picsThere;
    for (int i : lineIndexes) {
        if (i < 0 || i >= int(lines.size()))
            continue;
        for (int k : view_->frameLines(page, i))  // (a block of paragraphs: all of it)
            texts.push_back(lines[size_t(k)]);
        textsThere.push_back(view_->groupBox(page, {false, i}).translated(by));
    }
    std::vector<Picture> moving;
    for (int i : picIndexes)
        if (i >= 0 && i < int(pics.size())) {
            moving.push_back(pics[size_t(i)]);
            picsThere.push_back(view_->groupBox(page, {true, i}).translated(by));
        }
    QString error;
    bool ok = true;
    doc->beginChanges("move objects");
    if (!texts.empty())
        ok = doc->moveLines(page, texts, by, &error);
    for (const Picture &was : moving) {
        if (!ok)
            break;
        // (found again: the page's drawing has changed since - the text moved, the pictures before it)
        const std::vector<Picture> now = doc->pictures(page);
        const Picture *same = nullptr;
        double far = 2;  // (points)
        for (const Picture &p : now) {
            const double d = std::abs(p.rect.left() - was.rect.left()) + std::abs(p.rect.top() - was.rect.top()) +
                             std::abs(p.rect.width() - was.rect.width()) + std::abs(p.rect.height() - was.rect.height());
            if (p.vector == was.vector && p.xref == was.xref && d < far) {
                far = d;
                same = &p;
            }
        }
        if (!same) {
            ok = false;
            error = tr("A picture wasn't where it was.");
            break;
        }
        ok = doc->transformPicture(page, *same, QTransform::fromTranslate(by.x(), by.y()), "move objects", &error);
    }
    doc->endChanges();
    annotChanged(ok, tr("move them"), error);
    if (ok) {
        view_->pickGroup(page, textsThere, picsThere);
        say(tr("Moved together. Drag one of them to move them again; Esc puts them down."));
    }
}

void MainWindow::groupDeleted(int page, const std::vector<int> &lineIndexes, const std::vector<int> &picIndexes)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    const std::vector<TextLine> lines = view_->linesOf(page);
    const std::vector<Picture> pics = view_->picturesOf(page);
    std::vector<TextLine> texts;
    for (int i : lineIndexes)
        if (i >= 0 && i < int(lines.size()))
            for (int k : view_->frameLines(page, i))  // (a block of paragraphs: all of it)
                texts.push_back(lines[size_t(k)]);
    std::vector<Picture> going;
    for (int i : picIndexes)
        if (i >= 0 && i < int(pics.size()))
            going.push_back(pics[size_t(i)]);
    QString error;
    bool ok = true;
    doc->beginChanges("delete objects");
    if (!texts.empty())
        ok = doc->deleteLines(page, texts, &error);
    for (const Picture &was : going) {
        if (!ok)
            break;
        // (found again: the page's drawing has changed since - the text taken out, the pictures before it)
        const std::vector<Picture> now = doc->pictures(page);
        const Picture *same = nullptr;
        double far = 2;  // (points)
        for (const Picture &p : now) {
            const double d = std::abs(p.rect.left() - was.rect.left()) + std::abs(p.rect.top() - was.rect.top()) +
                             std::abs(p.rect.width() - was.rect.width()) + std::abs(p.rect.height() - was.rect.height());
            if (p.vector == was.vector && p.xref == was.xref && d < far) {
                far = d;
                same = &p;
            }
        }
        if (same)
            ok = doc->deletePicture(page, *same, &error);
    }
    doc->endChanges();
    annotChanged(ok, tr("delete them"), error);
    if (ok)
        say(tr("Deleted %1 things. Ctrl+Z puts them back.").arg(lineIndexes.size() + picIndexes.size()));
}

// Add text: written into the page itself (and marked, if underline / strikethrough are on)
void MainWindow::addRealText(int page, QPointF top, const QString &text)
{
    auto doc = view_->document();
    if (!doc || text.trimmed().isEmpty())
        return;
    QString error;
    const std::vector<QRectF> boxes =
        doc->addText(page, top, text, textFamily_, sizes_.value("text"), colors_.value("text"), textBold_, textItalic_,
                     textAlign_, textDir_, &error);
    bool ok = !boxes.empty();
    if (ok && textUnderline_)
        doc->addMarkup(page, QStringLiteral("Underline"), boxes, colors_.value("text"), 1, &error);
    if (ok && textStrike_)
        doc->addMarkup(page, QStringLiteral("StrikeOut"), boxes, colors_.value("text"), 1, &error);
    annotChanged(ok, tr("add text"), error);
}

void MainWindow::fontsChanged()
{
    // (My fonts changed: the font list made again)
    fontBox_->clear();
    refreshProps();
}
