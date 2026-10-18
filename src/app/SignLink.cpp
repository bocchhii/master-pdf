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
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <optional>

namespace {

// handwriting-like fonts (those installed are offered) and the inks, as the Python app's
const char *kScriptFonts[] = {"Segoe Script", "Lucida Handwriting", "Ink Free", "Brush Script MT",
                              "Freestyle Script", "Mistral", "Bradley Hand ITC", "Kristen ITC", "Segoe Print",
                              "Gabriola", "Comic Sans MS"};
const std::pair<const char *, QRgb> kInks[] = {{"Black", qRgb(0, 0, 0)}, {"Blue", qRgb(0, 51, 204)}, {"Dark blue", qRgb(0, 0, 128)}};

QString signatureOverride;  // (the self-tests: a signature of their own, not the user's)

// where a signature is drawn with the mouse: a white box, over a line to sign on
class SignPad : public QWidget {
public:
    static constexpr int W = 380, H = 130;
    std::vector<std::vector<QPointF>> strokes;
    QColor ink = Qt::black;

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
                p.drawPolyline(s.data(), int(s.size()));
            else if (!s.empty())
                p.drawPoint(s.front());
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        strokes.push_back({e->position()});
        update();
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
        QPainterPath path(s.front() * k);
        for (size_t i = 1; i < s.size(); ++i)
            path.lineTo(s[i] * k);
        if (s.size() > 1)
            p.drawPath(path);
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

// Make a signature: drawn with the mouse, or typed in a handwriting font. Null if cancelled.
QImage MainWindow::signatureDialog()
{
    ClassicDialog dlg(this, tr("Signature"));
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *page = new QVBoxLayout(body);
    page->setContentsMargins(12, 10, 12, 10);
    auto *top = new QHBoxLayout;
    auto *drawIt = new QRadioButton(tr("Draw it"), body);
    auto *typeIt = new QRadioButton(tr("Type it"), body);
    drawIt->setChecked(true);
    top->addWidget(drawIt);
    top->addWidget(typeIt);
    top->addStretch();
    top->addWidget(new QLabel(tr("Ink:"), body));
    auto *ink = new QComboBox(body);
    for (const auto &[name, rgb] : kInks)
        ink->addItem(tr(name), QColor(rgb));
    top->addWidget(ink);
    page->addLayout(top);

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
    page->addWidget(stack);

    auto colour = [ink] { return ink->currentData().value<QColor>(); };
    auto refresh = [=] {
        pad->ink = colour();
        pad->update();
        const QImage im = typedPicture(name->text(), family->currentText(), colour());
        preview->setPixmap(im.isNull() ? QPixmap()
                                       : QPixmap::fromImage(im.scaled(SignPad::W - 20, SignPad::H - 50, Qt::KeepAspectRatio,
                                                                      Qt::SmoothTransformation)));
    };
    connect(name, &QLineEdit::textChanged, &dlg, refresh);
    connect(family, &QComboBox::currentIndexChanged, &dlg, refresh);
    connect(ink, &QComboBox::currentIndexChanged, &dlg, refresh);
    connect(drawIt, &QRadioButton::toggled, &dlg, [=](bool on) {
        stack->setCurrentIndex(on ? 0 : 1);
        if (!on)
            name->setFocus();
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
    connect(clear, &QPushButton::clicked, &dlg, [=] {
        pad->strokes.clear();
        name->clear();
        refresh();
    });
    connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    connect(ok, &QPushButton::clicked, &dlg, [&] {
        const bool drawing = drawIt->isChecked();
        const QImage im = drawing ? drawnPicture(*pad) : typedPicture(name->text(), family->currentText(), colour());
        if (im.isNull()) {
            sounds::play(sounds::Error);
            classicMessage(&dlg, tr("Master PDF"),
                           drawing ? tr("Sign in the box with the mouse first (or type your name).")
                                   : tr("Type your name first."));
            return;
        }
        result = im;
        dlg.close();
    });
    refresh();
    dlg.exec();
    return result;
}

// Make (or make again) the signature; it's kept for next time
bool MainWindow::newSignature()
{
    const QImage im = signatureDialog();
    if (im.isNull()) {
        if (!QFileInfo::exists(signaturePath()) && tool_ == QLatin1String("sign"))
            setTool(QStringLiteral("select"), false);
        return false;
    }
    QDir().mkpath(QFileInfo(signaturePath()).absolutePath());
    if (!im.save(signaturePath(), "PNG")) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("Couldn't keep the signature."));
        return false;
    }
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
    if (!QFileInfo::exists(signaturePath()) && !newSignature())
        return;
    const QImage im(signaturePath());
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
