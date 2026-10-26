// The Sign and Link tools (the Python app's signature_dialog, place_signature, link_dialog,
// new_link / edit_link / delete_link / follow_link).

#include "app/MainWindow.h"

#include "core/Document.h"
#include "ui/Sounds.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <optional>

namespace {

// handwriting-like fonts (those installed are offered) and the inks, as the Python app's
const char *kScriptFonts[] = {"Segoe Script", "Lucida Handwriting", "Ink Free", "Brush Script MT",
                              "Freestyle Script", "Mistral", "Bradley Hand ITC", "Kristen ITC", "Segoe Print",
                              "Gabriola", "Comic Sans MS"};
const std::pair<const char *, QRgb> kInks[] = {{"Black", qRgb(0, 0, 0)}, {"Blue", qRgb(0, 51, 204)}, {"Dark blue", qRgb(0, 0, 128)}};

QString signatureOverride;  // (the self-tests: a signature of their own, not the user's)
QString sessionSignature;   // (the one picked this time: the Sign tool places it - next time, the default again)

// the signatures made, kept (each its own picture) beside the settings; the default one - placed when Sign is clicked -
// copied to signature.png too, where the Python app keeps its one
QString signatureFolder()
{
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(base) + QStringLiteral("/Master PDF/signatures"));  // (as the files listed are named)
}

QString defaultSignature()
{
    const QString name = theme::settings().value(QStringLiteral("signature_default")).toString();
    const QString path = signatureFolder() + QLatin1Char('/') + name;
    return !name.isEmpty() && QFileInfo::exists(path) ? path : QString();
}

void setDefaultSignature(const QString &path)
{
    theme::saveSetting(QStringLiteral("signature_default"), path.isEmpty() ? QString() : QFileInfo(path).fileName());
    QFile::remove(MainWindow::signaturePath());
    if (!path.isEmpty())
        QFile::copy(path, MainWindow::signaturePath());
}

QString keepSignature(const QImage &im)
{
    QDir().mkpath(signatureFolder());
    const QString path = signatureFolder() + QStringLiteral("/signature-%1.png")
                                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
    return im.save(path, "PNG") ? path : QString();
}

// the signatures kept, the newest first (the one signature of before: kept, the default)
QStringList savedSignatures()
{
    QStringList found;
    for (const QFileInfo &f : QDir(signatureFolder()).entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time))
        found << f.absoluteFilePath();
    if (found.isEmpty() && QFileInfo::exists(MainWindow::signaturePath())) {
        const QString kept = keepSignature(QImage(MainWindow::signaturePath()));
        if (!kept.isEmpty()) {
            theme::saveSetting(QStringLiteral("signature_default"), QFileInfo(kept).fileName());
            found << kept;
        }
    }
    return found;
}

// a stroke as a smooth curve through the mouse's points, k x bigger (its points each a whole pixel: lines from one to the
// next made stairs of it) - the points close together left out, a curve from the middle of each piece to the next's
QPainterPath smoothStroke(const std::vector<QPointF> &s, double k)
{
    std::vector<QPointF> pts;
    for (const QPointF &q : s)
        if (pts.empty() || QLineF(pts.back(), q * k).length() >= 3 * k)
            pts.push_back(q * k);
    if (s.size() > 1 && pts.back() != s.back() * k)
        pts.push_back(s.back() * k);
    QPainterPath path(pts.front());
    for (size_t i = 1; i + 1 < pts.size(); ++i)
        path.quadTo(pts[i], (pts[i] + pts[i + 1]) / 2);
    if (pts.size() > 1)
        path.lineTo(pts.back());
    return path;
}

// a signature's picture cut to its ink (what's seen and darker than paper), a little round it
QImage inkOnly(const QImage &in)
{
    const QImage im = in.convertToFormat(QImage::Format_ARGB32);
    int left = im.width(), top = im.height(), right = -1, bottom = -1;
    for (int y = 0; y < im.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        for (int x = 0; x < im.width(); ++x)
            if (qAlpha(row[x]) > 40 && qGray(row[x]) < 230) {
                left = std::min(left, x), right = std::max(right, x);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
    }
    if (right < left)
        return im;
    const int pad = std::max(2, (right - left) / 40);
    return im.copy(QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-pad, -pad, pad, pad) & im.rect());
}

// a kept signature in the list: its picture, the day it was made under it; a short line between one and the next
class SignatureItem : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {146, 96}; }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const QRect r = o.rect;
        const bool on = o.state & QStyle::State_Selected;
        const QRect pic(r.center().x() - 66, r.top() + 5, 132, 70);  // (a tall one - a stroke down, a line under it - room too)
        if (on)
            p->fillRect(pic.adjusted(-2, -2, 2, 2), o.palette.highlight());
        const double dpr = p->device()->devicePixelRatioF();
        p->drawPixmap(pic, i.data(Qt::DecorationRole).value<QIcon>().pixmap(pic.size(), dpr));
        const QString text = i.data(Qt::DisplayRole).toString();
        const QFontMetrics fm(o.font);
        const QRect line(r.center().x() - fm.horizontalAdvance(text) / 2 - 2, pic.bottom() + 4, fm.horizontalAdvance(text) + 4, fm.height());
        if (on)
            p->fillRect(line, o.palette.highlight());
        p->setFont(o.font);
        p->setPen(o.palette.color(on ? QPalette::HighlightedText : QPalette::Text));
        p->drawText(line, Qt::AlignCenter, text);
        if (i.row() + 1 < i.model()->rowCount()) {
            p->setPen(o.palette.color(QPalette::Mid));
            p->drawLine(r.left() + 24, r.bottom(), r.right() - 24, r.bottom());
        }
    }
};

// where a signature is drawn with the mouse: a white box, over a line to sign on
class SignPad : public QWidget {
public:
    static constexpr int W = 380, H = 130;
    std::vector<std::vector<QPointF>> strokes;
    QColor ink = Qt::black;
    std::function<void()> drawn;  // (signed in: a kept signature isn't the one any more)

    explicit SignPad(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(W + 4, H + 4);
        setCursor(Qt::CrossCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.setPen(QColor(0x80, 0x80, 0x80));  // (sunken: dark above and left, light below and right)
        p.drawLine(0, 0, width() - 1, 0);
        p.drawLine(0, 0, 0, height() - 1);
        p.setPen(QColor(0xFF, 0xFF, 0xFF));
        p.drawLine(0, height() - 1, width() - 1, height() - 1);
        p.drawLine(width() - 1, 0, width() - 1, height() - 1);
        p.setPen(QColor(0xB0, 0xB0, 0xB0));
        p.drawLine(22, H - 28, W - 18, H - 28);
        p.setPen(QColor(0x88, 0x88, 0x88));
        p.drawText(QPointF(16, H - 32), QStringLiteral("x"));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto &s : strokes)
            if (s.size() > 1)
                p.drawPath(smoothStroke(s, 1));
            else if (!s.empty())
                p.drawPoint(s.front());
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        strokes.push_back({e->position()});
        update();
        if (drawn)
            drawn();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!strokes.empty() && (e->buttons() & Qt::LeftButton)) {
            strokes.back().push_back(e->position());
            update();
        }
    }
};

// the strokes made 3 x bigger than drawn (smooth when placed on the page), cut to the ink
QImage drawnPicture(const SignPad &pad)
{
    if (pad.strokes.empty())
        return {};
    const double k = 3;
    QRectF ink;
    for (const auto &s : pad.strokes)
        for (const QPointF &q : s)
            ink |= QRectF(q * k, QSizeF(0.01, 0.01));
    const QRectF box = ink.adjusted(-12, -12, 12, 12);
    QImage im(box.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-box.topLeft());
    p.setPen(QPen(pad.ink, 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const auto &s : pad.strokes) {
        if (s.size() > 1)
            p.drawPath(smoothStroke(s, k));
        p.setBrush(pad.ink);
        p.drawEllipse(s.front() * k, 3, 3);
        p.drawEllipse(s.back() * k, 3, 3);
        p.setBrush(Qt::NoBrush);
    }
    return im;
}

// a name written in a handwriting font, big (120 px), on a see-through background
QImage typedPicture(const QString &name, const QString &family, QColor ink)
{
    const QString text = name.trimmed();
    if (text.isEmpty())
        return {};
    QFont f(family);
    f.setPixelSize(120);
    const QRect r = QFontMetrics(f).boundingRect(text);
    QImage im(r.width() + 20, r.height() + 20, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QPointF(10 - r.left(), 10 - r.top()), text);
    return im;
}

// a signature photographed or scanned: its paper made see-through - each pixel by how much darker it is than the paper
// round it (a photo's shadow, its uneven light, isn't ink) - cut to the ink; the ink the colour chosen, or its own
QImage pictureSignature(const QImage &in, QColor ink, bool own)
{
    if (in.isNull())
        return {};
    QImage src(in.size(), QImage::Format_RGB32);  // (see-through parts: on white paper)
    src.fill(Qt::white);
    {
        QPainter p(&src);
        p.drawImage(0, 0, in);
    }
    if (src.width() > 1600 || src.height() > 1600)
        src = src.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int w = src.width(), h = src.height();
    const QImage grey = src.convertToFormat(QImage::Format_Grayscale8);
    // (the paper: the picture blurred a great deal - a signature's thin strokes hardly darken it)
    const QImage paper = grey.scaled(std::max(1, w / 24), std::max(1, h / 24), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(w, h, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QRect inked;
    for (int y = 0; y < h; ++y) {
        const uchar *g = grey.constScanLine(y), *pp = paper.constScanLine(y);
        const QRgb *c = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int dark = int(pp[x]) - int(g[x]);
            const int alpha = std::clamp((dark - 18) * 255 / 50, 0, 255);
            if (alpha == 0)
                continue;
            const QColor col = own ? QColor(c[x]) : ink;
            o[x] = qRgba(col.red(), col.green(), col.blue(), alpha);
            if (alpha > 60)
                inked |= QRect(x, y, 1, 1);
        }
    }
    if (inked.width() < 4 || inked.height() < 4)
        return {};
    return out.copy(inked.adjusted(-8, -8, 8, 8) & out.rect());
}

}  // namespace


QString MainWindow::signaturePath()
{
    // (beside the settings, where the Python app keeps it too: one signature for both)
    if (!signatureOverride.isEmpty())
        return signatureOverride;
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath();
    return base + QStringLiteral("/Master PDF/signature.png");
}

void MainWindow::setSignaturePath(const QString &path)
{
    signatureOverride = path;
}

// Make a signature: drawn with the mouse, typed in a handwriting font, or from a picture of it - or pick one made before
// (*saved: its file). Null if cancelled.
QImage MainWindow::signatureDialog(QString *saved)
{
    ClassicDialog dlg(this, tr("Signature"));
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *page = new QVBoxLayout(body);
    page->setContentsMargins(12, 10, 12, 10);
    auto *top = new QHBoxLayout;
    auto *drawIt = new QRadioButton(tr("Draw it"), body);
    auto *typeIt = new QRadioButton(tr("Type it"), body);
    auto *fromPic = new QRadioButton(tr("From a picture"), body);
    drawIt->setChecked(true);
    top->addWidget(drawIt);
    top->addWidget(typeIt);
    top->addWidget(fromPic);
    top->addStretch();
    top->addWidget(new QLabel(tr("Ink:"), body));
    auto *ink = new QComboBox(body);
    for (const auto &[name, rgb] : kInks)
        ink->addItem(tr(name), QColor(rgb));
    top->addWidget(ink);
    auto *middle = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    left->addLayout(top);
    middle->addLayout(left, 1);
    page->addLayout(middle);

    auto *stack = new QStackedWidget(body);
    auto *pad = new SignPad(stack);
    stack->addWidget(pad);
    auto *typeBox = new QWidget(stack);
    auto *tl = new QVBoxLayout(typeBox);
    tl->setContentsMargins(0, 0, 0, 0);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Name:"), typeBox));
    auto *name = new QLineEdit(typeBox);
    name->setMinimumWidth(170);
    row->addWidget(name);
    auto *family = new QComboBox(typeBox);
    const QStringList installed = QFontDatabase::families();
    for (const char *f : kScriptFonts)
        if (installed.contains(QString::fromLatin1(f)))
            family->addItem(QString::fromLatin1(f));
    if (family->count() == 0)
        family->addItem(QStringLiteral("Times New Roman"));
    row->addWidget(family);
    tl->addLayout(row);
    auto *preview = new QLabel(typeBox);
    preview->setFixedSize(SignPad::W + 4, SignPad::H - 26);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("background: white; border: 2px inset #808080;"));
    tl->addWidget(preview);
    stack->addWidget(typeBox);
    // (... or a picture of it - a photo, a scan: its paper taken away)
    auto *picBox = new QWidget(stack);
    auto *pl = new QVBoxLayout(picBox);
    pl->setContentsMargins(0, 0, 0, 0);
    auto *picRow = new QHBoxLayout;
    auto *choose = new ClassicButton(tr("Choose picture..."), picBox);
    picRow->addWidget(choose);
    auto *picName = new QLabel(tr("(a photo or a scan of your signature, on plain paper)"), picBox);
    picRow->addWidget(picName, 1);
    pl->addLayout(picRow);
    auto *ownColours = new QCheckBox(tr("Keep its own colours (not the ink above)"), picBox);
    pl->addWidget(ownColours);
    auto *picPreview = new QLabel(picBox);
    picPreview->setFixedSize(SignPad::W + 4, SignPad::H - 26);
    picPreview->setAlignment(Qt::AlignCenter);
    picPreview->setStyleSheet(QStringLiteral("background: white; border: 2px inset #808080;"));
    pl->addWidget(picPreview);
    stack->addWidget(picBox);
    left->addWidget(stack);
    left->addStretch();
    QImage picture;  // (the picture chosen, as it is)

    // (right: the signatures made before - one picked is used; deleted; made the default, placed when Sign is clicked)
    auto *mine = new QGroupBox(tr("My signatures"), body);
    auto *ml = new QVBoxLayout(mine);
    ml->setContentsMargins(6, 4, 6, 6);
    auto *list = new QListWidget(mine);
    list->setItemDelegate(new SignatureItem(list));
    list->setUniformItemSizes(true);
    list->setIconSize(QSize(132, 70));
    list->setFixedWidth(146 + 2 * list->frameWidth() + list->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ml->addWidget(list, 1);
    auto *mineButtons = new QHBoxLayout;
    auto *makeDefault = new ClassicButton(tr("Set as default"), mine);
    auto *remove = new ClassicButton(tr("Delete"), mine);
    mineButtons->addWidget(makeDefault, 1);
    mineButtons->addWidget(remove);
    ml->addLayout(mineButtons);
    middle->addWidget(mine);
    const bool keeping = signatureOverride.isEmpty();  // (the self-tests: the user's own left alone)
    mine->setVisible(keeping);
    auto picked = [list] {
        return list->currentItem() && list->currentItem()->isSelected() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    };
    auto buttonsFor = [=] {
        const QString p = picked();
        makeDefault->setEnabled(!p.isEmpty() && p != defaultSignature());
        remove->setEnabled(!p.isEmpty());
    };
    auto fill = [=](const QString &pick) {
        list->clear();
        const QString def = defaultSignature();
        const QSize icon = list->iconSize();
        const double dpr = list->devicePixelRatioF();
        for (const QString &path : keeping ? savedSignatures() : QStringList()) {
            const QImage im(path);
            QPixmap thumb(icon * dpr);
            thumb.setDevicePixelRatio(dpr);
            thumb.fill(Qt::white);
            if (!im.isNull()) {  // (cut to its ink - its empty edges made it tiny - and made smaller smoothly, in the screen's pixels)
                const QImage ink = inkOnly(im);
                QImage small = ink.scaled(((QSizeF(icon) - QSizeF(8, 6)) * dpr).toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                small.setDevicePixelRatio(dpr);
                const QSizeF s = QSizeF(small.size()) / dpr;
                QPainter p(&thumb);
                p.drawImage(QPointF((icon.width() - s.width()) / 2, (icon.height() - s.height()) / 2), small);
            }
            const QString made = QLocale().toString(QFileInfo(path).lastModified().date(), QStringLiteral("d MMM yyyy"));
            auto *it = new QListWidgetItem(QIcon(thumb), path == def ? tr("%1 (default)").arg(made) : made, list);
            it->setData(Qt::UserRole, path);
            if (path == def)
                it->setToolTip(tr("The default: placed when you click with Sign"));
            if (path == pick)
                list->setCurrentItem(it);
        }
        buttonsFor();
    };
    fill(QFileInfo::exists(sessionSignature) ? sessionSignature : defaultSignature());
    connect(list, &QListWidget::itemSelectionChanged, &dlg, buttonsFor);
    connect(makeDefault, &QPushButton::clicked, &dlg, [=] {
        const QString p = picked();
        if (p.isEmpty())
            return;
        setDefaultSignature(p);
        fill(p);
    });
    connect(remove, &QPushButton::clicked, &dlg, [=, &dlg] {
        const QString p = picked();
        if (p.isEmpty() ||
            classicMessage(&dlg, tr("Delete signature"), tr("Delete this signature?"), {tr("Delete"), tr("Cancel")}) != tr("Delete"))
            return;
        if (p == defaultSignature())
            setDefaultSignature(QString());
        if (p == sessionSignature)
            sessionSignature.clear();
        QFile::remove(p);
        fill(QString());
    });
    auto unpick = [list] { list->clearSelection(); };  // (a new one made: the one picked isn't used)
    pad->drawn = unpick;

    auto colour = [ink] { return ink->currentData().value<QColor>(); };
    auto refresh = [=, &picture] {
        pad->ink = colour();
        pad->update();
        // (made smaller in the screen's own pixels - at 125% a picture of the box's size was stretched, blurred)
        auto shown = [](const QImage &im, QSize room, QLabel *on) {
            if (im.isNull())
                return QPixmap();
            const double dpr = on->devicePixelRatioF();
            QPixmap px = QPixmap::fromImage(im.scaled(room * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            px.setDevicePixelRatio(dpr);
            return px;
        };
        preview->setPixmap(shown(typedPicture(name->text(), family->currentText(), colour()), QSize(SignPad::W - 20, SignPad::H - 50), preview));
        const QImage sig = pictureSignature(picture, colour(), ownColours->isChecked());
        picPreview->setPixmap(shown(sig, QSize(SignPad::W - 20, SignPad::H - 40), picPreview));
        if (!picture.isNull() && sig.isNull())
            picPreview->setText(tr("No signature found in it - it needs dark ink on lighter paper."));
    };
    connect(choose, &QPushButton::clicked, &dlg, [=, &dlg, &picture] {
        const QString file = QFileDialog::getOpenFileName(&dlg, tr("A picture of your signature"),
                                                          QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                          tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp)"));
        if (file.isEmpty())
            return;
        const QImage im(file);
        if (im.isNull()) {
            sounds::play(sounds::Error);
            classicMessage(&dlg, tr("Master PDF"), tr("%1 can't be read as a picture.").arg(QFileInfo(file).fileName()));
            return;
        }
        picture = im;
        picName->setText(QFileInfo(file).fileName());
        unpick();
        refresh();
    });
    connect(ownColours, &QCheckBox::toggled, &dlg, refresh);
    connect(name, &QLineEdit::textChanged, &dlg, refresh);
    connect(name, &QLineEdit::textEdited, &dlg, unpick);
    connect(family, &QComboBox::currentIndexChanged, &dlg, refresh);
    connect(ink, &QComboBox::currentIndexChanged, &dlg, refresh);
    connect(drawIt, &QRadioButton::toggled, &dlg, [=](bool on) {
        if (on)
            stack->setCurrentIndex(0);
    });
    connect(typeIt, &QRadioButton::toggled, &dlg, [=](bool on) {
        if (on) {
            stack->setCurrentIndex(1);
            name->setFocus();
        }
    });
    connect(fromPic, &QRadioButton::toggled, &dlg, [=](bool on) {
        if (on)
            stack->setCurrentIndex(2);
    });

    auto *buttons = new QHBoxLayout;
    auto *clear = new ClassicButton(tr("Clear"), body);
    buttons->addWidget(clear);
    buttons->addStretch();
    auto *ok = new ClassicButton(tr("OK"), body);
    auto *cancel = new ClassicButton(tr("Cancel"), body);
    ok->setMinimumWidth(75);
    cancel->setMinimumWidth(75);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    page->addLayout(buttons);
    QImage result;
    connect(clear, &QPushButton::clicked, &dlg, [=, &picture] {
        unpick();
        pad->strokes.clear();
        name->clear();
        picture = QImage();
        picName->setText(tr("(a photo or a scan of your signature, on plain paper)"));
        refresh();
    });
    connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    connect(ok, &QPushButton::clicked, &dlg, [&] {
        if (const QString p = picked(); !p.isEmpty()) {  // (one made before: that one)
            result = QImage(p);
            if (saved)
                *saved = p;
            dlg.close();
            return;
        }
        const bool drawing = drawIt->isChecked(), fromPicture = fromPic->isChecked();
        const QImage im = drawing       ? drawnPicture(*pad)
                          : fromPicture ? pictureSignature(picture, colour(), ownColours->isChecked())
                                        : typedPicture(name->text(), family->currentText(), colour());
        if (im.isNull()) {
            sounds::play(sounds::Error);
            classicMessage(&dlg, tr("Master PDF"),
                           drawing       ? tr("Sign in the box with the mouse first (or type your name).")
                           : fromPicture ? (picture.isNull() ? tr("Choose a picture of your signature first.")
                                                             : tr("No signature was found in the picture: it needs dark ink on "
                                                                  "lighter paper."))
                                         : tr("Type your name first."));
            return;
        }
        result = im;
        dlg.close();
    });
    connect(list, &QListWidget::itemDoubleClicked, ok, &QPushButton::click);
    refresh();
    dlg.exec();
    return result;
}

// Make (or make again) the signature; it's kept for next time
bool MainWindow::newSignature()
{
    QString saved;
    const QImage im = signatureDialog(&saved);
    if (im.isNull()) {
        if (!QFileInfo::exists(signaturePath()) && !QFileInfo::exists(sessionSignature) && tool_ == QLatin1String("sign"))
            setTool(QStringLiteral("select"), false);
        return false;
    }
    if (!signatureOverride.isEmpty()) {  // (the self-tests: their own file)
        QDir().mkpath(QFileInfo(signaturePath()).absolutePath());
        saved = im.save(signaturePath(), "PNG") ? signaturePath() : QString();
    } else if (saved.isEmpty()) {  // (a new one: kept with the others - the default, if there's none)
        saved = keepSignature(im);
        if (!saved.isEmpty() && defaultSignature().isEmpty())
            setDefaultSignature(saved);
    }
    if (saved.isEmpty()) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("Couldn't keep the signature."));
        return false;
    }
    sessionSignature = saved;
    say(tr("Signature ready. Click where it goes."));
    return true;
}

// the signature put on the page where it was clicked (140 points wide), then picked up with
// the Edit tool - to move and resize
void MainWindow::placeSignature(int page, QPointF at)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    // (the one picked this time; else the default)
    auto current = [] { return signatureOverride.isEmpty() && QFileInfo::exists(sessionSignature) ? sessionSignature : signaturePath(); };
    if (!QFileInfo::exists(current()) && !newSignature())
        return;
    const QImage im(current());
    if (im.isNull()) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("Couldn't read the signature."));
        return;
    }
    const double w = 140;
    const QRectF where(at, QSizeF(w, w * im.height() / std::max(1, im.width())));
    QString error;
    const bool ok = doc->placePicture(page, QString(), im.convertToFormat(QImage::Format_ARGB32), where, true, nullptr, &error);
    if (ok)
        setTool(QStringLiteral("edit"), false);
    pictureChangedDone(ok, page, where, error);
    if (ok)
        say(tr("Signed. Drag it to move it, a corner to resize it."));
}

// ---- links ----

namespace {

// where a link goes: a web page (uri) or a page of this PDF (from 0); none if cancelled
std::optional<std::pair<QString, int>> linkDialog(QWidget *owner, int pages, const QString &uri, int page)
{
    ClassicDialog dlg(owner, QObject::tr("Link"));
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *l = new QVBoxLayout(body);
    l->setContentsMargins(12, 10, 12, 10);
    auto *box = new QGroupBox(QObject::tr(" The link goes to "), body);
    auto *g = new QGridLayout(box);
    auto *web = new QRadioButton(QObject::tr("A web page:"), box);
    auto *inPdf = new QRadioButton(QObject::tr("A page of this PDF:"), box);
    (page >= 0 ? inPdf : web)->setChecked(true);
    auto *address = new QLineEdit(uri.isEmpty() ? QStringLiteral("https://") : uri, box);
    address->setMinimumWidth(240);
    auto *number = new QLineEdit(QString::number(std::max(0, page) + 1), box);
    number->setFixedWidth(50);
    auto *row = new QHBoxLayout;
    row->addWidget(number);
    row->addWidget(new QLabel(QObject::tr("of %1").arg(pages), box));
    row->addStretch();
    g->addWidget(web, 0, 0);
    g->addWidget(address, 0, 1);
    g->addWidget(inPdf, 1, 0);
    g->addLayout(row, 1, 1);
    l->addWidget(box);
    // (typing in a box picks its kind)
    QObject::connect(address, &QLineEdit::textEdited, &dlg, [web] { web->setChecked(true); });
    QObject::connect(number, &QLineEdit::textEdited, &dlg, [inPdf] { inPdf->setChecked(true); });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *ok = new ClassicButton(QObject::tr("OK"), body);
    auto *cancel = new ClassicButton(QObject::tr("Cancel"), body);
    ok->setMinimumWidth(75);
    cancel->setMinimumWidth(75);
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    l->addLayout(buttons);
    std::optional<std::pair<QString, int>> result;
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    QObject::connect(ok, &QPushButton::clicked, &dlg, [&] {
        if (web->isChecked()) {
            QString text = address->text().trimmed();
            if (text.isEmpty() || text == QLatin1String("https://")) {
                sounds::play(sounds::Error);
                classicMessage(&dlg, QObject::tr("Master PDF"), QObject::tr("Type the web page's address."));
                return;
            }
            static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][\\w+.-]*:"));
            if (!scheme.match(text).hasMatch())
                text = QStringLiteral("https://") + text;
            result = std::make_pair(text, -1);
        } else {
            bool good = false;
            const int n = number->text().trimmed().toInt(&good);
            if (!good || n < 1 || n > pages) {
                sounds::play(sounds::Error);
                classicMessage(&dlg, QObject::tr("Master PDF"), QObject::tr("Type a page number from 1 to %1.").arg(pages));
                return;
            }
            result = std::make_pair(QString(), n - 1);
        }
        dlg.close();
    });
    QObject::connect(address, &QLineEdit::returnPressed, ok, &QPushButton::click);
    QObject::connect(number, &QLineEdit::returnPressed, ok, &QPushButton::click);
    (page >= 0 ? number : address)->setFocus();
    dlg.exec();
    return result;
}

}  // namespace

void MainWindow::connectSignLink()
{
    connect(view_, &PageView::signClicked, this, &MainWindow::placeSignature);
    connect(view_, &PageView::linkClicked, this, &MainWindow::followLink);
    connect(view_, &PageView::linkMenu, this, [this](int page, int index, QPoint global) {
        QMenu menu(this);
        menu.addAction(tr("Change link..."), this, [this, page, index] { editLink(page, index); });
        menu.addAction(tr("Go there"), this, [this, page, index] { followLink(page, index); });
        menu.addSeparator();
        menu.addAction(tr("Delete link"), this, [this, page, index] { deleteLink(page, index); });
        menu.exec(global);
    });
}

// a link clicked: a web page (asked first), or a page of this PDF
void MainWindow::followLink(int page, int index)
{
    const std::vector<PdfLink> &links = view_->linksOf(page);
    if (index < 0 || index >= int(links.size()))
        return;
    const PdfLink l = links[size_t(index)];
    if (!l.uri.isEmpty()) {
        if (classicMessage(this, tr("Master PDF"), tr("Open this web page?\n%1").arg(l.uri), {tr("Open"), tr("Cancel")}) ==
            tr("Open"))
            QDesktopServices::openUrl(QUrl(l.uri));
    } else if (l.page >= 0 && l.page < view_->document()->pageCount()) {
        view_->goToPage(l.page);
    }
}

void MainWindow::newLink(int page, const QRectF &rect)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    const auto choice = linkDialog(this, doc->pageCount(), QString(), -1);
    if (!choice)
        return;
    QString error;
    annotChanged(doc->addLink(page, rect, choice->first, choice->second, &error), tr("add the link"), error);
}

void MainWindow::editLink(int page, int index)
{
    auto doc = view_->document();
    const std::vector<PdfLink> &links = view_->linksOf(page);
    if (!doc || index < 0 || index >= int(links.size()))
        return;
    const PdfLink l = links[size_t(index)];
    const auto choice = linkDialog(this, doc->pageCount(), l.uri, l.uri.isEmpty() ? l.page : -1);
    if (!choice)
        return;
    QString error;
    annotChanged(doc->changeLink(page, l, choice->first, choice->second, &error), tr("change the link"), error);
}

void MainWindow::deleteLink(int page, int index)
{
    auto doc = view_->document();
    const std::vector<PdfLink> &links = view_->linksOf(page);
    if (!doc || index < 0 || index >= int(links.size()))
        return;
    const PdfLink l = links[size_t(index)];
    QString error;
    annotChanged(doc->deleteLink(page, l, &error), tr("delete the link"), error);
}
