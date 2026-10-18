// MainWindow's pictures (step 6): the Edit tool's picture picked up - moved, resized, turned,
// layered, replaced, saved, deleted - the Image tool, Add picture, and form fields deleted.

#include "core/Ocr.h"
#include "app/ImageEditor.h"
#include "ui/FlowLayout.h"
#include "app/MainWindow.h"

#include "core/Reader.h"
#include "ui/Classic.h"
#include "ui/Sounds.h"

#include "core/Document.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QMap>
#include <QPainter>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

const char *kPictureFiles = "Pictures (*.png *.jpg *.jpeg *.jpe *.jfif *.bmp *.gif *.tif *.tiff *.webp *.ico);;All files (*)";

}  // namespace

void MainWindow::buildPictureProps(QHBoxLayout *pr, QWidget *props)
{
    pictureGroup_ = new QWidget(props);
    auto *l = new QHBoxLayout(pictureGroup_);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(1);
    auto add = [&](const QString &icon, const QString &tip, const QString &label, std::function<void()> fn) {
        auto *b = new ToolButton(icon, tip, {}, false, label, pictureGroup_);
        b->setWhiten(label.isEmpty() ? 16 : 0);  // (the new ones; Replace / Save as / Delete as they were)
        connect(b, &ToolButton::clicked, this, fn);
        l->addWidget(b);
        return b;
    };
    // (with a list: the button does the first thing, its arrow shows them all)
    auto addList = [&](const QString &icon, const QString &tip, const QString &which, std::function<void()> fn) {
        auto *b = new ToolButton(icon, tip, {}, true, {}, pictureGroup_);
        b->setWhiten(16);
        connect(b, &ToolButton::clicked, this, fn);
        connect(b, &ToolButton::arrowClicked, this, [this, which] { pictureMenu(which); });
        l->addWidget(b);
        buttons_.insert(QStringLiteral("pic_") + which, b);
    };
    add(QStringLiteral("flip_v"), tr("Flip vertical"), {}, [this] { flipPicture(false); });
    add(QStringLiteral("flip_h"), tr("Flip horizontal"), {}, [this] { flipPicture(true); });
    buttons_.insert(QStringLiteral("pic_crop"), add(QStringLiteral("crop"), tr("Crop image (drag its edges; Enter or a click outside puts it in, Esc leaves it)"), {},
                                                     [this] { cropPicture(); }));
    addList(QStringLiteral("obj_align"), tr("Align on the page"), QStringLiteral("align"), [this] { alignPicture(QStringLiteral("left")); });
    l->addWidget(new ToolSeparator(pictureGroup_));
    add(QStringLiteral("rotate_ccw"), tr("Rotate counterclockwise"), {}, [this] { turnPicture(-90); });
    add(QStringLiteral("rotate_cw"), tr("Rotate clockwise"), {}, [this] { turnPicture(90); });
    addList(QStringLiteral("arrange"), tr("Arrange"), QStringLiteral("arrange"), [this] { stepPicture(true); });
    l->addWidget(new ToolSeparator(pictureGroup_));
    buttons_.insert(QStringLiteral("pic_ocr"),
                    add(QStringLiteral("text_tool"), tr("Convert its text to real, editable text (OCR)"), tr("Text (OCR)"),
                        [this] { convertPictureText(); }));
    buttons_.insert(QStringLiteral("pic_editor"),
                    add(QStringLiteral("brush"), tr("Edit image: crop, rotate, select, rub out, paint (as Photoshop)"), tr("Edit image"),
                        [this] { editImage(); }));
    addList(QStringLiteral("pen"), tr("Edit using another program (Paint...): saved there, it's put back here"), QStringLiteral("edit"),
            [this] { editPictureWith(theme::settings().value("edit_using").toString()); });
    l->addWidget(new ToolSeparator(pictureGroup_));
    buttons_.insert(QStringLiteral("pic_replace"), add(QStringLiteral("image"), tr("Replace picture"), tr("Replace..."), [this] { replacePicture(); }));
    buttons_.insert(QStringLiteral("pic_save"), add(QStringLiteral("save"), tr("Save picture"), tr("Save as..."), [this] { savePicture(); }));
    add(QStringLiteral("eraser"), tr("Delete picture"), tr("Delete"), [this] { deletePicture(); });
    flowInGroups(pictureGroup_);
    pr->addWidget(pictureGroup_);
}

// ---- Acrobat's Objects panel ----

void MainWindow::transformPicked(const QTransform &move, const char *what)
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic || !doc->editable())
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    QString error;
    const bool ok = doc->transformPicture(page, p, move, what, &error);
    pictureChangedDone(ok, page, move.mapRect(p.rect), error);
}

void MainWindow::flipPicture(bool across)
{
    const Picture *pic = pickedPicture();
    if (!pic)
        return;
    const QPointF c = pic->rect.center();
    transformPicked(QTransform::fromTranslate(-c.x(), -c.y()) * QTransform::fromScale(across ? -1 : 1, across ? 1 : -1) *
                        QTransform::fromTranslate(c.x(), c.y()),
                    across ? "flip horizontal" : "flip vertical");
}

void MainWindow::turnPicture(double degrees)
{
    const Picture *pic = pickedPicture();
    if (!pic)
        return;
    const QPointF c = pic->rect.center();
    QTransform r;
    r.rotate(degrees);
    transformPicked(QTransform::fromTranslate(-c.x(), -c.y()) * r * QTransform::fromTranslate(c.x(), c.y()),
                    degrees > 0 ? "rotate clockwise" : "rotate counterclockwise");
}

void MainWindow::alignPicture(const QString &where)
{
    // on the page: its left / middle / right, top / middle / bottom (a picture on its own: to the page)
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const QSizeF s = doc->pageSize(view_->pickedPicturePage());
    const QRectF r = pic->rect;
    double dx = 0, dy = 0;
    if (where == "left")
        dx = -r.left();
    else if (where == "center")
        dx = (s.width() - r.width()) / 2 - r.left();
    else if (where == "right")
        dx = s.width() - r.right();
    else if (where == "top")
        dy = -r.top();
    else if (where == "middle")
        dy = (s.height() - r.height()) / 2 - r.top();
    else if (where == "bottom")
        dy = s.height() - r.bottom();
    if (std::abs(dx) < 0.01 && std::abs(dy) < 0.01)
        return;
    transformPicked(QTransform::fromTranslate(dx, dy), "align picture");
}

void MainWindow::stepPicture(bool forward)
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    QString error;
    pictureChangedDone(doc->stepPicture(page, p, forward, &error), page, p.rect, error);
    say(forward ? tr("Brought forward: drawn over the next picture.") : tr("Sent backward: drawn under the picture before it."));
}

void MainWindow::cropPicture()
{
    if (view_->cropping()) {  // (the button again: put in)
        view_->finishCrop(true);
        return;
    }
    if (!view_->startCrop()) {
        say(tr("Only a picture that's upright (or turned a quarter) can be cropped."));
        return;
    }
    say(tr("Crop: drag its edges or corners - Enter, a double click or a click outside puts it in; Esc leaves it as it was."));
}

void MainWindow::pictureMenu(const QString &which)
{
    QMenu menu(this);
    if (which == "align") {
        for (const auto &[key, name] : std::vector<std::pair<QString, QString>>{{"left", tr("Align Left")}, {"center", tr("Align Center")},
                                                                                 {"right", tr("Align Right")}, {"", ""},
                                                                                 {"top", tr("Align Top")}, {"middle", tr("Align Middle")},
                                                                                 {"bottom", tr("Align Bottom")}}) {
            if (key.isEmpty()) {
                menu.addSeparator();
                continue;
            }
            const QString k = key;
            menu.addAction(name + tr(" (on the page)"), this, [this, k] { alignPicture(k); });
        }
    } else if (which == "arrange") {
        menu.addAction(tr("Bring to Front"), this, [this] { layerPicture(true); });
        menu.addAction(tr("Bring Forward"), this, [this] { stepPicture(true); });
        menu.addAction(tr("Send Backward"), this, [this] { stepPicture(false); });
        menu.addAction(tr("Send to Back"), this, [this] { layerPicture(false); });
    } else if (which == "edit") {
#ifdef Q_OS_WIN
        menu.addAction(tr("Microsoft Paint"), this, [this] { editPictureWith(QStringLiteral("mspaint")); });
#endif
        menu.addAction(tr("The picture app Windows opens it with"), this, [this] { editPictureWith(QStringLiteral("default")); });
        menu.addSeparator();
        menu.addAction(tr("Choose a program..."), this, [this] {
            const QString exe = QFileDialog::getOpenFileName(this, tr("Edit using"), QStringLiteral("C:/Program Files"),
                                                             tr("Programs (*.exe)"));
            if (!exe.isEmpty())
                editPictureWith(exe);
        });
    }
    ToolButton *b = buttons_.value(QStringLiteral("pic_") + which);
    menu.exec(b ? b->mapToGlobal(QPoint(0, b->height())) : QCursor::pos());
}

// letters drawn as shapes (a label saved as outlines) read - as the Python app reads them: their
// outlines drawn black on white, stood level (the turn whose rows of ink are sharpest), read (OCR),
// and written as real text in their place, turned as they were, in their colour
void MainWindow::convertDrawnText(const Picture &pic, int page)
{
    auto doc = view_->document();
    const QPointF pivot = pic.rect.center();
    const double reach = std::hypot(pic.rect.width(), pic.rect.height()) + 8;
    // (drawn turned back by degrees, zoom pixels a point, the pivot in the middle)
    auto drawn = [&](double degrees, double zoom) {
        const int side = std::max(8, int(std::ceil(reach * zoom)));
        QImage im(side, side, QImage::Format_Grayscale8);
        im.fill(255);
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing, zoom > 2);
        QTransform t;
        t.translate(side / 2.0, side / 2.0);
        t.scale(zoom, zoom);
        t.rotate(-degrees);
        t.translate(-pivot.x(), -pivot.y());
        p.setTransform(t);
        for (const Picture::Piece &pc : pic.pieces)
            p.fillPath(pc.path, Qt::black);
        return im;
    };
    auto sharpness = [](const QImage &im) {
        double sum = 0;
        for (int y = 0; y < im.height(); ++y) {
            const uchar *row = im.constScanLine(y);
            int ink = 0;
            for (int x = 0; x < im.width(); ++x)
                ink += row[x] < 128;
            sum += double(ink) * ink;
        }
        return sum;
    };
    // its turn: every way looked at, then finely
    const double small = std::clamp(120.0 / std::max(1.0, reach), 0.5, 3.0);
    double best = 0, score = -1;
    for (int a = 0; a < 180; a += 3) {
        const double s = sharpness(drawn(a, small));
        if (s > score)
            score = s, best = a;
    }
    for (double step : {1.5, 0.75, 0.375})
        for (double a : {best - step, best + step}) {
            const double s = sharpness(drawn(a, small));
            if (s > score)
                score = s, best = a;
        }
    // (then stood level exactly: the turn where its ink is least tall - its rows with ink fewest)
    auto tall = [&](double a) {
        const QImage im = drawn(a, 3);
        int rows = 0;
        for (int y = 0; y < im.height(); ++y) {
            const uchar *row = im.constScanLine(y);
            for (int x = 0; x < im.width(); ++x)
                if (row[x] < 128) {
                    ++rows;
                    break;
                }
        }
        return rows;
    };
    int least = tall(best);
    for (double step : {1.0, 0.5, 0.25})
        for (double a : {best - step, best + step}) {
            const int t = tall(a);
            if (t < least)
                least = t, best = a;
        }
    if (best > 90)
        best -= 180;
    // read both ways up: the way read surer. (Letters far bigger than a book's, Tesseract reads
    // nothing: drawn so its lines are about 36 pixels tall - its rows of ink measured, stood level)
    double lineHeight = 0;
    {
        const double z = 2;
        const QImage level = drawn(best, z);
        std::vector<int> bands;
        int start = -1;
        for (int y = 0; y <= level.height(); ++y) {
            bool ink = false;
            if (y < level.height()) {
                const uchar *row = level.constScanLine(y);
                for (int x = 0; x < level.width() && !ink; ++x)
                    ink = row[x] < 128;
            }
            if (ink && start < 0)
                start = y;
            else if (!ink && start >= 0)
                bands.push_back(y - start), start = -1;
        }
        if (!bands.empty()) {
            std::sort(bands.begin(), bands.end());
            lineHeight = bands.back() / z;  // (its tallest line, in points)
        }
    }
    const double zoom = std::clamp(lineHeight > 0 ? 36.0 / lineHeight : 4.2, 1.0, std::min(6.0, 4000.0 / reach));
    say(tr("Reading the drawn letters..."));
    QApplication::setOverrideCursor(classic::waitCursor());
    QString error;
    auto readAt = [&](double degrees, QImage *image) {
        *image = drawn(degrees, zoom);
        // (just its ink and a margin: a sea of white round a few letters, Tesseract reads less - what
        // it reads put back where it is in the whole)
        QRect ink;
        for (int y = 0; y < image->height(); ++y) {
            const uchar *row = image->constScanLine(y);
            for (int x = 0; x < image->width(); ++x)
                if (row[x] < 128)
                    ink |= QRect(x, y, 1, 1);
        }
        if (ink.isEmpty())
            return std::vector<ocr::Line>();
        const QRect part = ink.adjusted(-20, -20, 20, 20).intersected(image->rect());
        std::vector<ocr::Line> ls = ocr::read(image->copy(part), &error, true);
        const QPoint d = part.topLeft();
        for (ocr::Line &l : ls) {
            l.box.translate(d), l.baseStart += d, l.baseEnd += d;
            for (ocr::Word &w : l.words)
                w.box.translate(d);
        }
        return ls;
    };
    auto worth = [](const std::vector<ocr::Line> &ls) {
        double w = 0;
        for (const ocr::Line &l : ls)
            for (const ocr::Word &wd : l.words)
                for (const QChar c : wd.text)
                    w += c.isLetterOrNumber() ? wd.confidence : 0;
        return w;
    };
    auto sure = [](const std::vector<ocr::Line> &ls) {
        double sum = 0;
        int n = 0;
        for (const ocr::Line &l : ls)
            for (const ocr::Word &wd : l.words)
                sum += wd.confidence, ++n;
        return n ? sum / n : 0.0;
    };
    QImage im;
    std::vector<ocr::Line> lines = readAt(best, &im);
    if (error.isEmpty()) {
        QImage other;
        const std::vector<ocr::Line> flipped = readAt(best + 180, &other);
        if (sure(flipped) * worth(flipped) > sure(lines) * worth(lines))
            lines = flipped, im = other, best += 180;
    }
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        classicMessage(this, tr("Master PDF"), error);
        return;
    }
    // (its colour: most of its shapes')
    QMap<QRgb, int> colours;
    for (const Picture::Piece &pc : pic.pieces)
        ++colours[pc.fill.rgb()];
    QRgb colour = qRgb(0, 0, 0);
    int most = 0;
    for (auto it = colours.cbegin(); it != colours.cend(); ++it)
        if (it.value() > most)
            most = it.value(), colour = it.key();
    // (a point of the picture, straight - as if it weren't turned)
    const QPointF middle(im.width() / 2.0, im.height() / 2.0);
    auto straight = [&](QPointF at) { return (at - middle) / zoom + pivot; };
    // (clean letters: every word read is kept - signs too, "&")
    std::vector<Document::OcrLine> out;
    double inkShare = 0;
    int counted = 0;
    for (const ocr::Line &l : lines) {
        QStringList kept;
        QRect box;
        for (const ocr::Word &w : l.words) {
            if (w.confidence < 30 || w.text.trimmed().isEmpty())
                continue;
            kept << w.text;
            box |= w.box;
            const QRect wb = w.box.intersected(im.rect());
            int ink = 0;
            for (int y = wb.top(); y <= wb.bottom(); ++y) {
                const uchar *row = im.constScanLine(y);
                for (int x = wb.left(); x <= wb.right(); ++x)
                    ink += row[x] < 128;
            }
            inkShare += double(ink) / std::max(1, wb.width() * wb.height());
            ++counted;
        }
        if (kept.isEmpty())
            continue;
        Document::OcrLine o;
        o.text = kept.join(QLatin1Char(' '));
        // (from its first word kept to its last)
        const QPointF b0 = l.baseStart, b1 = l.baseEnd;
        const double left = box.left(), right = box.right() + 1;
        const double baseY = b1.x() != b0.x() ? b0.y() + (b1.y() - b0.y()) * (left - b0.x()) / (b1.x() - b0.x()) : b0.y();
        o.baseline = straight(QPointF(left, baseY));
        o.size = std::max(2.0, std::max(1.0, baseY - l.box.top()) / 0.72 / zoom);
        o.width = (right - left) / zoom;
        o.color = QColor::fromRgb(colour);
        out.push_back(o);
    }
    if (out.empty()) {
        classicMessage(this, tr("Master PDF"), tr("No text was found in this drawing."));
        return;
    }
    for (Document::OcrLine &o : out)  // (a label's letters: all bold, or none)
        o.bold = counted > 0 && inkShare / counted > 0.34;
    // the pictures drawn over the letters, with a copy of them in their pixels (PowerPoint saves a shape
    // with what's under its corners): those pixels filled from round them - as if the letters weren't
    // there. (Upright, uncropped pictures - put back in the same square)
    std::vector<std::pair<Picture, QImage>> cleaned;
    for (const Picture &p : view_->picturesOf(page)) {
        if (p.vector || p.order <= pic.order || !p.crop.isNull() || p.whole.size() != 4 || !p.rect.intersects(pic.rect))
            continue;
        const QPolygonF quad(QVector<QPointF>(p.whole.begin(), p.whole.end()));
        if (std::abs(quad[0].y() - quad[1].y()) > 0.01 || std::abs(quad[0].x() - quad[3].x()) > 0.01)
            continue;
        QImage px = doc->pictureImage(p).convertToFormat(QImage::Format_ARGB32);
        if (px.isNull())
            continue;
        // (the letters' ink, a little bolder, in the picture's pixels)
        QTransform toPage;
        if (!QTransform::quadToQuad(QPolygonF(QRectF(0, 0, px.width(), px.height())), quad, toPage))
            continue;
        QImage mask(px.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        {
            QPainter m(&mask);
            m.setRenderHint(QPainter::Antialiasing);
            m.setTransform(toPage.inverted());
            QPen pen(Qt::white, 0);
            pen.setWidthF(2.5 / std::max(0.01, std::sqrt(std::abs(toPage.inverted().determinant()))));
            m.setPen(pen);
            m.setBrush(Qt::white);
            for (const Picture::Piece &pc : pic.pieces)
                m.drawPath(pc.path);
        }
        std::vector<char> hole(size_t(px.width()) * size_t(px.height()), 0);
        int holes = 0;
        for (int y = 0; y < px.height(); ++y) {
            const uchar *row = mask.constScanLine(y);
            for (int x = 0; x < px.width(); ++x)
                if (row[x] > 0)
                    hole[size_t(y) * size_t(px.width()) + size_t(x)] = 1, ++holes;
        }
        if (!holes)
            continue;
        // (filled from the edge in: each pixel the mean of its filled neighbours - the colours round it)
        const int w = px.width(), h = px.height();
        for (bool more = true; more && holes > 0;) {
            more = false;
            std::vector<std::pair<int, QRgb>> now;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!hole[size_t(y) * size_t(w) + size_t(x)])
                        continue;
                    int r = 0, g = 0, b = 0, a = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int u = x + dx, v = y + dy;
                            if ((dx || dy) && u >= 0 && v >= 0 && u < w && v < h && !hole[size_t(v) * size_t(w) + size_t(u)]) {
                                const QRgb c = px.pixel(u, v);
                                r += qRed(c), g += qGreen(c), b += qBlue(c), a += qAlpha(c), ++n;
                            }
                        }
                    if (n)
                        now.push_back({y * w + x, qRgba(r / n, g / n, b / n, a / n)});
                }
            for (const auto &[i, c] : now) {
                px.setPixel(i % w, i / w, c);
                hole[size_t(i)] = 0;
                --holes;
                more = true;
            }
        }
        cleaned.push_back({p, px});
    }
    const bool ok = doc->drawingToText(page, pic, best, pivot, out, cleaned, &error);
    annotChanged(ok, tr("convert the drawn letters to text"), error);
    if (ok)
        say(tr("%n line(s) of drawn letters made real text.", nullptr, int(out.size())));
}

void MainWindow::convertPictureText()
{
    // the picture's text read (OCR, Tesseract): the picture without its letters - each word's box filled
    // with the colour round it - and real text in their place, as big, as wide, in their colour
    auto doc = view_->document();
    const Picture *picked = pickedPicture();
    if (!doc || !picked || !doc->editable())
        return;
    if (picked->vector) {  // (a drawing: only letters drawn as shapes are read)
        if (picked->letters)
            convertDrawnText(Picture(*picked), view_->pickedPicturePage());
        return;
    }
    const Picture pic = *picked;
    const int page = view_->pickedPicturePage();
    const QImage im = doc->pictureImage(pic).convertToFormat(QImage::Format_ARGB32);
    if (im.isNull()) {
        classicMessage(this, tr("Master PDF"), tr("This picture can't be read."));
        return;
    }
    const QRectF box = pic.full.isNull() ? pic.rect : pic.full;
    // (small: read bigger - about 300 dots an inch, as it's shown)
    const double ppp = im.width() / std::max(1.0, box.width());
    const double up = std::clamp(4.2 / std::max(0.01, ppp), 1.0, 4.0);
    const QImage big = up > 1.05 ? im.scaled(im.size() * up, Qt::KeepAspectRatio, Qt::SmoothTransformation) : im;
    say(tr("Reading the picture's text..."));
    QApplication::setOverrideCursor(classic::waitCursor());
    QString error;
    const std::vector<ocr::Line> lines = ocr::read(big, &error);
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        classicMessage(this, tr("Master PDF"), error);
        return;
    }
    const double k = double(big.width()) / im.width();  // (read pixels per picture pixel)
    const double kx = box.width() / im.width(), ky = box.height() / im.height();
    QImage clean = im.copy();
    auto dist = [](QRgb a, QRgb b) { return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b)); };
    auto median = [](std::vector<int> v) {
        if (v.empty())
            return 0;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    auto medianD = [](std::vector<double> v) {
        if (v.empty())
            return 0.0;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    // the words kept - each with its line's baseline (picture pixels) and the height of its letters there.
    // Signs too ("{", "//", "*", ";": code, sums) - Tesseract reads them, sure of them; only a sign it's
    // unsure of is left (a frame's edge read as "|")
    struct Kept {
        QString text;
        QRect box;               // (picture pixels)
        QPointF base0, base1;    // (its line's baseline)
        double above = 0;        // (its line's letters' height over the baseline)
    };
    std::vector<Kept> words;
    auto keep = [&](const ocr::Line &l, const ocr::Word &w, QPoint shift, double scale) {
        bool letters = false;
        for (const QChar c : w.text)
            letters = letters || c.isLetterOrNumber();
        if (w.confidence < 40 || (!letters && w.confidence < 60) || w.text == QLatin1String("|"))
            return;
        Kept o;
        o.text = w.text;
        o.box = QRectF((w.box.left() + shift.x()) / scale, (w.box.top() + shift.y()) / scale, w.box.width() / scale, w.box.height() / scale).toAlignedRect();
        o.base0 = QPointF((l.baseStart.x() + shift.x()) / scale, (l.baseStart.y() + shift.y()) / scale);
        o.base1 = QPointF((l.baseEnd.x() + shift.x()) / scale, (l.baseEnd.y() + shift.y()) / scale);
        const double baseY = o.base1.x() != o.base0.x() ? o.base0.y() + (o.base1.y() - o.base0.y()) * (o.box.left() - o.base0.x()) / (o.base1.x() - o.base0.x()) : o.base0.y();
        o.above = std::max(1.0, baseY - (l.box.top() + shift.y()) / scale);
        words.push_back(o);
    };
    for (const ocr::Line &l : lines)
        for (const ocr::Word &w : l.words)
            keep(l, w, QPoint(), k);
    // what Tesseract passed over (a lone "}" at a line's start, a sign on its own): the ink no word covers,
    // in patches of a letter's size, read again one patch at a time as a line of its own
    if (!words.empty()) {
        std::vector<int> heights;
        for (const Kept &w : words)
            heights.push_back(w.box.height());
        const int textH = std::max(4, median(heights));
        std::vector<int> br, bgc, bb;  // (the picture's background: its edge's colour)
        for (int x = 0; x < im.width(); ++x)
            for (int y : {0, im.height() - 1}) {
                const QRgb c = im.pixel(x, y);
                br.push_back(qRed(c)), bgc.push_back(qGreen(c)), bb.push_back(qBlue(c));
            }
        const QRgb paper = qRgb(median(br), median(bgc), median(bb));
        std::vector<char> ink(size_t(im.width()) * size_t(im.height()), 0);
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x)
                ink[size_t(y) * size_t(im.width()) + size_t(x)] = qAlpha(im.pixel(x, y)) > 128 && dist(im.pixel(x, y), paper) > 120;
        for (const Kept &w : words) {  // (what's read already: not again)
            const QRect r = w.box.adjusted(-3, -3, 3, 3).intersected(im.rect());
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    ink[size_t(y) * size_t(im.width()) + size_t(x)] = 0;
        }
        // (its pieces of ink - those a letter's size: not a frame's long edge, not a speck)
        std::vector<QRect> pieces;
        std::vector<char> seen(ink.size(), 0);
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x) {
                const size_t at = size_t(y) * size_t(im.width()) + size_t(x);
                if (!ink[at] || seen[at])
                    continue;
                QRect r(x, y, 1, 1);
                int count = 0;
                std::vector<QPoint> todo{{x, y}};
                seen[at] = 1;
                while (!todo.empty()) {
                    const QPoint q = todo.back();
                    todo.pop_back();
                    ++count;
                    r |= QRect(q, QSize(1, 1));
                    for (const QPoint d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
                        const QPoint n = q + d;
                        if (n.x() < 0 || n.y() < 0 || n.x() >= im.width() || n.y() >= im.height())
                            continue;
                        const size_t m = size_t(n.y()) * size_t(im.width()) + size_t(n.x());
                        if (ink[m] && !seen[m]) {
                            seen[m] = 1;
                            todo.push_back(n);
                        }
                    }
                }
                if (count >= 6 && r.height() >= textH * 0.4 && r.height() <= textH * 2.2 && r.width() <= textH * 6)
                    pieces.push_back(r);
            }
        // (pieces side by side, near: one patch)
        std::sort(pieces.begin(), pieces.end(), [](const QRect &a, const QRect &b) { return a.left() < b.left(); });
        std::vector<QRect> patches;
        for (const QRect &r : pieces) {
            bool joined = false;
            for (QRect &p : patches)
                if (std::min(p.bottom(), r.bottom()) - std::max(p.top(), r.top()) > 0.3 * textH && r.left() - p.right() < textH) {
                    p |= r;
                    joined = true;
                    break;
                }
            if (!joined)
                patches.push_back(r);
        }
        for (const QRect &p : patches) {
            const QRect inBig = QRectF(p.left() * k, p.top() * k, p.width() * k, p.height() * k).toAlignedRect().adjusted(-12, -12, 12, 12).intersected(big.rect());
            QString why;
            for (const ocr::Line &l : ocr::read(big.copy(inBig), &why, true))
                for (const ocr::Word &w : l.words)
                    keep(l, w, inBig.topLeft(), k);
        }
    }
    // each word: its background filled in where it was (the picture without its letters), its letters'
    // colour, how heavy they are
    struct Look {
        QColor color;
        double share = 0;
    };
    std::vector<Look> looks(words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const QRect wb = words[i].box.adjusted(-2, -2, 2, 2).intersected(clean.rect());
        if (wb.isEmpty())
            continue;
        std::vector<int> r, g, b, a;
        std::vector<std::pair<int, QRgb>> inks;  // (how far from the background, the colour)
        const QRect ring = wb.adjusted(-2, -2, 2, 2).intersected(im.rect());
        for (int y = ring.top(); y <= ring.bottom(); ++y)
            for (int x = ring.left(); x <= ring.right(); ++x)
                if (!wb.contains(x, y) || x == wb.left() || x == wb.right() || y == wb.top() || y == wb.bottom()) {
                    const QRgb c = im.pixel(x, y);
                    r.push_back(qRed(c)), g.push_back(qGreen(c)), b.push_back(qBlue(c)), a.push_back(qAlpha(c));
                }
        const QRgb bg = qRgba(median(r), median(g), median(b), median(a));
        int count = 0;
        for (int y = wb.top(); y <= wb.bottom(); ++y)
            for (int x = wb.left(); x <= wb.right(); ++x) {
                const QRgb c = im.pixel(x, y);
                if (dist(c, bg) > 120) {
                    inks.push_back({dist(c, bg), c});
                    ++count;
                }
                clean.setPixel(x, y, bg);
            }
        // (its letters' colour: their strokes' cores - near their strongest; a thin sign is mostly its smoothed
        // edge, paler, and a few darkest specks are the picture's noise)
        std::sort(inks.begin(), inks.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        std::vector<int> inkR, inkG, inkB;
        if (!inks.empty()) {
            const int strong = inks[inks.size() / 20].first;  // (the 95th percentile)
            for (const auto &[d, c] : inks)
                if (d >= 0.5 * strong)
                    inkR.push_back(qRed(c)), inkG.push_back(qGreen(c)), inkB.push_back(qBlue(c));
        }
        looks[i].color = inkR.empty() ? QColor(Qt::black) : QColor(median(inkR), median(inkG), median(inkB));
        looks[i].share = double(count) / std::max(1, wb.width() * wb.height());
    }
    // a fixed-width font (code - Courier's kind): each word's letters as wide as every other word's; then
    // its size from that width (a letter 0.6 of the size, as Courier New's)
    std::vector<double> advances, shares;
    for (size_t i = 0; i < words.size(); ++i) {
        const int n = int(words[i].text.size());
        if (n >= 4)
            advances.push_back(double(words[i].box.width()) / n);
        int letters = 0;
        for (const QChar c : words[i].text)
            letters += c.isLetter();
        if (letters >= 3)
            shares.push_back(looks[i].share);
    }
    bool mono = false;
    const double advance = medianD(advances);
    if (advances.size() >= 4 && advance > 0) {
        double var = 0;
        for (double a : advances)
            var += (a - advance) * (a - advance);
        mono = std::sqrt(var / double(advances.size())) / advance < 0.06;
    }
    const double usualShare = medianD(shares);  // (bold: heavier than the picture's words usually are)
    std::vector<Document::OcrLine> out;
    for (size_t i = 0; i < words.size(); ++i) {
        const Kept &w = words[i];
        Document::OcrLine o;
        o.text = w.text;
        const double left = w.box.left();
        const double baseY = w.base1.x() != w.base0.x() ? w.base0.y() + (w.base1.y() - w.base0.y()) * (left - w.base0.x()) / (w.base1.x() - w.base0.x()) : w.base0.y();
        o.baseline = QPointF(box.left() + left * kx, box.top() + baseY * ky);
        o.size = mono ? std::max(2.0, advance * kx / 0.6) : std::max(2.0, w.above / 0.72 * ky);
        o.width = w.box.width() * kx;
        o.color = looks[i].color;
        int letters = 0;
        for (const QChar c : w.text)
            letters += c.isLetter();
        o.bold = letters >= 2 && usualShare > 0 && looks[i].share > 1.25 * usualShare;
        o.mono = mono;
        out.push_back(o);
    }
    if (out.empty()) {
        classicMessage(this, tr("Master PDF"), tr("No text was found in this picture."));
        return;
    }
    const bool ok = doc->pictureToText(page, pic, clean, box, out, &error);
    annotChanged(ok, tr("convert the picture's text"), error);
    if (ok)
        say(tr("%n word(s) of the picture's text made real text.", nullptr, int(out.size())));
}

void MainWindow::editImage()
{
    // the picture opened in the picture editor; saved there, put back where it was - cropped, turned,
    // resized: where its part shows now
    auto doc = view_->document();
    const Picture *picked = pickedPicture();
    if (!doc || !picked || picked->vector || !doc->editable())
        return;
    const Picture pic = *picked;
    const int page = view_->pickedPicturePage();
    const QImage im = doc->pictureImage(pic);
    if (im.isNull()) {
        classicMessage(this, tr("Master PDF"), tr("This picture can't be edited."));
        return;
    }
    ImageEditor editor(this, im);
    if (!editor.exec())
        return;
    const QImage result = editor.image();
    const QRectF box0 = pic.full.isNull() ? pic.rect : pic.full;
    const QRectF part = editor.frame();
    const double kx = box0.width() / std::max(1, im.width()), ky = box0.height() / std::max(1, im.height());
    const QRectF box(box0.left() + part.left() * kx, box0.top() + part.top() * ky, part.width() * kx, part.height() * ky);
    QString error;
    const bool ok = doc->placePicture(page, QString(), result, box, false, &pic, &error);
    pictureChangedDone(ok, page, box, error);
    if (ok)
        say(tr("The picture's changes put in."));
}

void MainWindow::editPictureWith(const QString &programIn)
{
    // the picture saved as a file and opened in the program; each time it's saved there, it's put
    // back in its place here (as Acrobat's Edit Using)
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic || !doc->editable())
        return;
#ifdef Q_OS_WIN
    const QString program = programIn.isEmpty() ? QStringLiteral("mspaint") : programIn;
#else
    const QString program = programIn.isEmpty() ? QStringLiteral("default") : programIn;  // (no Paint: the system's own app)
#endif
    const QImage im = doc->pictureImage(*pic);
    if (im.isNull()) {
        classicMessage(this, tr("Master PDF"), tr("This picture can't be edited in another program."));
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/Master PDF edits");
    QDir().mkpath(dir);
    static int n = 0;
    const QString file = dir + QStringLiteral("/picture %1.png").arg(++n);
    if (!im.save(file)) {
        classicMessage(this, tr("Master PDF"), tr("Couldn't save the picture for editing."));
        return;
    }
    editFile_ = file;
    editPage_ = view_->pickedPicturePage();
    editDoc_ = view_->document();
    editBox_ = pic->full.isNull() ? pic->rect : pic->full;
    if (!editWatch_) {
        editWatch_ = new QFileSystemWatcher(this);
        // (saved: put back once the program's done writing - a save can tell twice or more: once -
        // and watched again, as some programs save a new file)
        editSettle_ = new QTimer(this);
        editSettle_->setSingleShot(true);
        editSettle_->setInterval(700);
        connect(editWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
            if (path == editFile_)
                editSettle_->start();
        });
        connect(editSettle_, &QTimer::timeout, this, [this] {
            const QString path = editFile_;
            {
                if (!editWatch_->files().contains(path) && QFileInfo::exists(path))
                    editWatch_->addPath(path);
                auto doc = view_->document();
                if (doc && doc != editDoc_.lock()) {  // (another PDF's tab showing: put back when its own is)
                    say(tr("The picture was saved - go back to its PDF's tab and save it again there to put it back."));
                    return;
                }
                if (!doc || editPage_ < 0 || editPage_ >= doc->pageCount() || !QFileInfo::exists(path))
                    return;
                // the picture where it was put back last
                const std::vector<Picture> pics = doc->pictures(editPage_);
                const Picture *best = nullptr;
                double far = 1e18;
                for (const Picture &p : pics) {
                    const QRectF r = p.full.isNull() ? p.rect : p.full;
                    const double d = std::abs(r.center().x() - editBox_.center().x()) + std::abs(r.center().y() - editBox_.center().y()) +
                                     std::abs(r.width() - editBox_.width()) + std::abs(r.height() - editBox_.height());
                    if (d < far) {
                        far = d;
                        best = &p;
                    }
                }
                if (!best || far > 20)
                    return;
                QString error;
                const Picture was = *best;
                const bool ok = doc->placePicture(editPage_, path, QImage(), editBox_, true, &was, &error);
                pictureChangedDone(ok, editPage_, editBox_, error);
                if (ok) {
                    editBox_ = was.full.isNull() ? was.rect : was.full;
                    say(tr("The picture's changes put in (edited in another program)."));
                }
            }
        });
    }
    if (!editWatch_->files().isEmpty())
        editWatch_->removePaths(editWatch_->files());
    editWatch_->addPath(file);
    bool started = false;
    if (program == QLatin1String("default"))
        started = QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    else
        started = QProcess::startDetached(program, {QDir::toNativeSeparators(file)});
    if (!started) {
        classicMessage(this, tr("Master PDF"), tr("Couldn't start %1.").arg(QFileInfo(program).fileName()));
        return;
    }
    if (program != QLatin1String("default"))
        theme::saveSetting(QStringLiteral("edit_using"), program);
    say(tr("Editing the picture in %1 - save it there and it's put back here.")
            .arg(program == QLatin1String("mspaint") ? tr("Paint") : program == QLatin1String("default") ? tr("its picture app")
                                                                                                          : QFileInfo(program).baseName()));
}

const Picture *MainWindow::pickedPicture()
{
    const int page = view_->pickedPicturePage(), index = view_->pickedPicture();
    if (page < 0 || index < 0 || index >= int(view_->picturesOf(page).size()))
        return nullptr;
    return &view_->picturesOf(page)[size_t(index)];
}

// after a picture changed: drawn again, and the picture (where it is now) still picked up
void MainWindow::pictureChangedDone(bool ok, int page, const QRectF &near, const QString &error)
{
    annotChanged(ok, tr("change the picture"), error);
    if (!ok || near.isNull())
        return;
    const std::vector<Picture> &pics = view_->picturesOf(page);
    int best = -1;
    double far = 1e18;
    for (int n = 0; n < int(pics.size()); ++n) {
        const QRectF &r = pics[size_t(n)].rect;
        const double d = std::abs(r.center().x() - near.center().x()) + std::abs(r.center().y() - near.center().y()) +
                         std::abs(r.width() - near.width()) + std::abs(r.height() - near.height());
        if (d < far) {
            far = d;
            best = n;
        }
    }
    view_->pickPicture(page, best);
    refreshProps();
}

void MainWindow::connectPictureTools()
{
    connect(view_, &PageView::picturePicked, this, [this] {
        refreshProps();
        if (view_->pickedPicture() >= 0)
            say(tr("Picked up: drag it to move it, a corner to resize it, the round handle to turn it. "
                   "Right-click for more."));
    });
    connect(view_, &PageView::pictureChanged, this, [this](int page, int index, const QTransform &move, const QString &what) {
        auto doc = view_->document();
        const std::vector<Picture> pics = view_->picturesOf(page);
        if (!doc || index < 0 || index >= int(pics.size()))
            return;
        QString error;
        const bool ok = doc->transformPicture(page, pics[size_t(index)], move, what.toUtf8().constData(), &error);
        pictureChangedDone(ok, page, move.mapRect(pics[size_t(index)].rect), error);
    });
    connect(view_, &PageView::pictureDelete, this, [this] { deletePicture(); });
    connect(view_, &PageView::pictureCropped, this, [this](int page, int index, const QRectF &unit) {
        auto doc = view_->document();
        const std::vector<Picture> pics = view_->picturesOf(page);
        if (!doc || index < 0 || index >= int(pics.size()))
            return;
        QString error;
        const Picture p = pics[size_t(index)];
        const bool ok = doc->cropPicture(page, p, unit, &error);
        pictureChangedDone(ok, page, p.rect, error);
        if (ok)
            say(tr("Cropped."));
    });
    connect(view_, &PageView::imageClicked, this, [this](int page, QPointF at) { addPicture(page, at, QRectF()); });
    connect(view_, &PageView::editMenu, this, [this](int page, QPointF at, QPoint global) {
        auto doc = view_->document();
        if (!doc)
            return;
        QMenu menu(this);
        // form fields there (a signing app's "SIGN here" box)
        for (const FormField &f : doc->fields(page))
            if (f.rect.contains(at)) {
                const int id = f.id;
                menu.addAction(f.signature ? tr("Delete signature field") : tr("Delete form field"), this, [this, page, id] {
                    QString error;
                    annotChanged(view_->document()->deleteField(page, id, &error), tr("delete the form field"), error);
                });
                menu.addSeparator();
            }
        if (view_->pickedLine() >= 0) {
            const int line = view_->pickedLine();
            menu.addAction(tr("Edit text"), this, [this, page, line] { view_->editLine(page, line); });
            menu.addSeparator();
            menu.addAction(tr("Delete"), this, [this, page, line] { emit view_->lineDelete(page, line); });
        } else if (view_->pickedPicture() >= 0) {
            const Picture *picked = pickedPicture();
            const bool drawing = picked && picked->vector;  // (lines, boxes: no pixels of their own)
            if (drawing && picked->letters) {  // (letters drawn as shapes: they can be made text)
                menu.addAction(tr("Convert to text (OCR)"), this, [this] { convertPictureText(); });
                menu.addSeparator();
            }
            if (!drawing) {
                menu.addAction(tr("Edit image..."), this, [this] { editImage(); });
                menu.addAction(tr("Convert to text (OCR)"), this, [this] { convertPictureText(); });
                menu.addSeparator();
                menu.addAction(tr("Replace picture..."), this, [this] { replacePicture(); });
                menu.addAction(tr("Save picture as..."), this, [this] { savePicture(); });
                menu.addSeparator();
                addLayoutItems(menu.addMenu(tr("Layout Options")));
                menu.addSeparator();
                menu.addAction(tr("Bring to Front"), this, [this] { layerPicture(true); });
                menu.addAction(tr("Bring Forward"), this, [this] { stepPicture(true); });
                menu.addAction(tr("Send Backward"), this, [this] { stepPicture(false); });
                menu.addAction(tr("Send to Back"), this, [this] { layerPicture(false); });
                menu.addSeparator();
            }
            menu.addAction(tr("Flip Horizontal"), this, [this] { flipPicture(true); });
            menu.addAction(tr("Flip Vertical"), this, [this] { flipPicture(false); });
            menu.addAction(tr("Rotate Clockwise"), this, [this] { turnPicture(90); });
            menu.addAction(tr("Rotate Counterclockwise"), this, [this] { turnPicture(-90); });
            if (!drawing)
                menu.addAction(tr("Crop Image"), this, [this] { cropPicture(); });
            menu.addSeparator();
            menu.addAction(tr("Delete"), this, [this] { deletePicture(); });
        } else {
            // letters saved as drawn shapes there: read and made real text, or deleted
            for (const Drawing &d : doc->drawings(page))
                if (d.rect.adjusted(-2, -2, 2, 2).contains(at)) {
                    menu.addAction(tr("Convert to text"), this, [this, page, d] { convertDrawing(page, d); });
                    menu.addAction(tr("Delete drawing"), this, [this, page, d] {
                        QString error;
                        annotChanged(view_->document()->removeDrawing(page, d, {}, &error), tr("delete the drawing"), error);
                    });
                    menu.addSeparator();
                    break;
                }
            menu.addAction(tr("Add picture here..."), this, [this, page, at] { addPicture(page, at, QRectF()); });
        }
        menu.exec(global);
    });
}

// letters drawn as shapes (a label saved as outlines) read by Windows' text reader, and
// written as real text in their place - one change, undone in one go
void MainWindow::convertDrawing(int page, const Drawing &d)
{
    auto doc = view_->document();
    if (!doc)
        return;
    say(tr("Reading the letters..."));
    QApplication::setOverrideCursor(classic::waitCursor());
    QString error;
    const auto lines = reader::read(*doc, page, d.rect, &error);
    QApplication::restoreOverrideCursor();
    if (lines.empty()) {
        sounds::play(sounds::Error);
        say(error);
        return;
    }
    annotChanged(doc->removeDrawing(page, d, lines, &error), tr("convert the drawing to text"), error);
    if (error.isEmpty()) {
        QStringList read;
        for (const auto &l : lines)
            read << l.first;
        say(tr("Converted to text: %1").arg(read.join(QStringLiteral(" / "))));
    }
}

// Layout Options (as Word's): in line with the text, the ways text can wrap round the picture,
// behind or in front of the text - the one it has checked
const char *const kLayouts[][2] = {{"inline", "In Line with Text"}, {"square", "Square"}, {"tight", "Tight"},
                                   {"through", "Through"}, {"top_bottom", "Top and Bottom"},
                                   {"behind", "Behind Text"}, {"front", "In Front of Text"}};

void MainWindow::addLayoutItems(QMenu *menu)
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const QString now = doc->pictureLayout(view_->pickedPicturePage(), *pic);
    menu->addSection(tr("In Line with Text"));
    for (const auto &[key, name] : kLayouts) {
        if (QLatin1String(key) == QLatin1String("square"))
            menu->addSection(tr("With Text Wrapping"));
        QAction *a = menu->addAction(QIcon(QPixmap::fromImage(icons::layout(QString::fromLatin1(key), false))), tr(name));
        a->setCheckable(true);
        a->setChecked(now == QLatin1String(key));
        const QString mode = QString::fromLatin1(key);
        connect(a, &QAction::triggered, this, [this, mode] { setLayout(mode); });
    }
}

void MainWindow::setLayout(const QString &mode)
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic || !doc->editable())
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    bool past = false;
    QString error;
    const bool ok = doc->setPictureLayout(page, p, mode, &past, &error);
    pictureChangedDone(ok, page, p.rect, error);
    if (!ok)
        return;
    QString name = mode;
    for (const auto &[key, n] : kLayouts)
        if (mode == QLatin1String(key))
            name = tr(n);
    say(past ? tr("Some text went past the bottom of the page - make the picture smaller or move it.")
             : tr("Picture: %1.").arg(name) + (Document::flowing(mode) ? tr(" The text flows round it.") : QString()));
}

void MainWindow::deletePicture()
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    view_->pickPicture(-1, -1);
    QString error;
    annotChanged(doc->deletePicture(page, p, &error), tr("delete the picture"), error);
}

void MainWindow::layerPicture(bool front)
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    QString error;
    pictureChangedDone(doc->layerPicture(page, p, front, &error), page, p.rect, error);
    say(front ? tr("Brought to front: drawn over everything on the page.") : tr("Sent to back: drawn under everything on the page."));
}

void MainWindow::replacePicture()
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Replace picture"),
                                                      theme::settings().value("last_folder").toString(), tr(kPictureFiles));
    if (path.isEmpty())
        return;
    const int page = view_->pickedPicturePage();
    const Picture p = *pic;
    QString error;
    const QRectF box = p.full.isNull() ? p.rect : p.full;  // (the whole picture's box, not just its shape's)
    pictureChangedDone(doc->placePicture(page, path, QImage(), box, true, &p, &error), page, box, error);
}

void MainWindow::savePicture()
{
    auto doc = view_->document();
    const Picture *pic = pickedPicture();
    if (!doc || !pic)
        return;
    const QImage im = doc->pictureImage(*pic);
    if (im.isNull()) {
        classicMessage(this, tr("Master PDF"), tr("This picture can't be saved."));
        return;
    }
    const QString base = QFileInfo(name_).completeBaseName() + tr(" (picture).png");
    const QString path = QFileDialog::getSaveFileName(this, tr("Save picture"),
                                                      QFileInfo(doc->path()).absolutePath() + QLatin1Char('/') + base,
                                                      tr("PNG picture (*.png);;JPEG picture (*.jpg)"));
    if (path.isEmpty())
        return;
    if (!im.save(path))
        classicMessage(this, tr("Master PDF"), tr("Couldn't save the picture."));
    else
        say(tr("Saved the picture as %1.").arg(QFileInfo(path).fileName()));
}

// a picture put on a page: at a point (its own size, no bigger than half the page) or in a box
void MainWindow::addPicture(int page, QPointF at, const QRectF &box)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    QImage image;  // (the picture copied - or none: the file's read as it is)
    QString path;
    if (variant_.value("image") == "clipboard") {
        image = QApplication::clipboard()->image();
        if (image.isNull()) {
            say(tr("There's no picture copied."));
            return;
        }
    } else {
        path = QFileDialog::getOpenFileName(this, tr("Add picture"), theme::settings().value("last_folder").toString(),
                                            tr(kPictureFiles));
        if (path.isEmpty())
            return;
    }
    const QImage probe = image.isNull() ? QImage(path) : image;
    if (probe.isNull()) {
        classicMessage(this, tr("Master PDF"), tr("%1 can't be read as a picture.").arg(QFileInfo(path).fileName()));
        return;
    }
    QRectF where = box.normalized();
    if (where.width() < 1 || where.height() < 1) {
        // its own size (at the resolution it says; 96 dpi if it says 72 or less)
        const double dpi = probe.dotsPerMeterX() * 0.0254 > 72 ? probe.dotsPerMeterX() * 0.0254 : 96;
        QSizeF s(probe.width() * 72 / dpi, probe.height() * 72 / dpi);
        const QSizeF pageSize = doc->pageSize(page);
        const double k = std::min({1.0, pageSize.width() * 0.5 / s.width(), pageSize.height() * 0.5 / s.height()});
        where = QRectF(at, s * k);
    }
    QString error;
    const bool ok = doc->placePicture(page, path, image, where, true, nullptr, &error);
    pictureChangedDone(ok, page, where, error);
    if (ok)
        say(tr("Added the picture."));
}
