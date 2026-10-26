// Convert (Acrobat's Export PDF): the PDF as another kind of file - Word, Excel, PowerPoint, pictures, web pages, Rich
// Text, PostScript, text, XML. Its kinds grouped as Acrobat's, each with its settings; written on a thread of its own.

#include "app/MainWindow.h"

#include "core/Document.h"
#include "core/Export.h"
#include "core/Ocr.h"
#include "ui/Classic.h"
#include "ui/Sounds.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <memory>
#include <thread>

using convert::Format;
using convert::Options;

namespace {

// ---- each format's settings, kept between times ----

QString keyOf(Format f)
{
    switch (f) {
    case Format::Docx: return QStringLiteral("docx");
    case Format::Doc: return QStringLiteral("doc");
    case Format::Xlsx: return QStringLiteral("xlsx");
    case Format::XmlSpreadsheet: return QStringLiteral("xmlss");
    case Format::Pptx: return QStringLiteral("pptx");
    case Format::Jpeg: return QStringLiteral("jpeg");
    case Format::Jpeg2000: return QStringLiteral("jp2");
    case Format::Tiff: return QStringLiteral("tiff");
    case Format::Png: return QStringLiteral("png");
    case Format::HtmlSingle:
    case Format::HtmlMulti: return QStringLiteral("html");
    case Format::Rtf: return QStringLiteral("rtf");
    case Format::Eps: return QStringLiteral("eps");
    case Format::Ps: return QStringLiteral("ps");
    case Format::TextAccessible: return QStringLiteral("texta");
    case Format::TextPlain: return QStringLiteral("text");
    case Format::Xml: return QStringLiteral("xml");
    }
    return QStringLiteral("other");
}

QChar firstOf(const QString &s, QChar otherwise)
{
    return s.isEmpty() ? otherwise : s.front();
}

QJsonObject toJson(const Options &o)
{
    return QJsonObject{{"flowing", o.flowing},
                       {"comments", o.comments},
                       {"images", o.images},
                       {"recognize", o.recognize},
                       {"languages", o.languages},
                       {"sheets", o.sheets},
                       {"regional", o.regionalNumbers},
                       {"decimal", QString(o.decimal)},
                       {"thousands", QString(o.thousands)},
                       {"dpi", o.dpi},
                       {"colour", o.colour},
                       {"quality", o.quality},
                       {"progressive", o.progressive},
                       {"tiffCompression", o.tiffCompression},
                       {"tiffOneFile", o.tiffOneFile},
                       {"allImages", o.allImages},
                       {"htmlNav", o.htmlNav},
                       {"htmlSplit", o.htmlSplit},
                       {"binary", o.binary},
                       {"encoding", o.encoding},
                       {"bookmarks", o.bookmarks},
                       {"xmlImages", o.xmlImages},
                       {"subfolder", o.subfolder},
                       {"folder", o.folder},
                       {"prefix", o.prefix},
                       {"imageFormat", o.imageFormat},
                       {"downsample", o.downsample}};
}

Options fromJson(const QJsonObject &j)
{
    Options o;
    o.flowing = j.value("flowing").toBool(o.flowing);
    o.comments = j.value("comments").toBool(o.comments);
    o.images = j.value("images").toBool(o.images);
    o.recognize = j.value("recognize").toBool(o.recognize);
    o.languages = j.value("languages").toString(o.languages);
    o.sheets = j.value("sheets").toInt(o.sheets);
    o.regionalNumbers = j.value("regional").toBool(o.regionalNumbers);
    o.decimal = firstOf(j.value("decimal").toString(), o.decimal);
    o.thousands = firstOf(j.value("thousands").toString(), o.thousands);
    o.dpi = j.value("dpi").toInt(o.dpi);
    o.colour = j.value("colour").toInt(o.colour);
    o.quality = j.value("quality").toInt(o.quality);
    o.progressive = j.value("progressive").toBool(o.progressive);
    o.tiffCompression = j.value("tiffCompression").toInt(o.tiffCompression);
    o.tiffOneFile = j.value("tiffOneFile").toBool(o.tiffOneFile);
    o.allImages = j.value("allImages").toBool(o.allImages);
    o.htmlNav = j.value("htmlNav").toInt(o.htmlNav);
    o.htmlSplit = j.value("htmlSplit").toInt(o.htmlSplit);
    o.binary = j.value("binary").toBool(o.binary);
    o.encoding = j.value("encoding").toString(o.encoding);
    o.bookmarks = j.value("bookmarks").toBool(o.bookmarks);
    o.xmlImages = j.value("xmlImages").toBool(o.xmlImages);
    o.subfolder = j.value("subfolder").toBool(o.subfolder);
    o.folder = j.value("folder").toString(o.folder);
    o.prefix = j.value("prefix").toString(o.prefix);
    o.imageFormat = j.value("imageFormat").toString(o.imageFormat);
    o.downsample = j.value("downsample").toInt(o.downsample);
    return o;
}

Options optionsFor(Format f)
{
    const QJsonObject all = theme::settings().value("convert").toObject();
    Options o = fromJson(all.value(keyOf(f)).toObject());
    if (!all.contains(keyOf(f)) && f == Format::Png)
        o.dpi = 150;
    return o;
}

void keep(Format f, const Options &o)
{
    QJsonObject all = theme::settings().value("convert").toObject();
    all.insert(keyOf(f), toJson(o));
    theme::saveSetting("convert", all);
}

// ---- the settings boxes (Acrobat's, as far as each is honoured) ----

QGroupBox *box(const QString &title, QWidget *parent)
{
    auto *g = new QGroupBox(QStringLiteral(" %1 ").arg(title), parent);
    g->setFont(theme::font());
    return g;
}

// (the languages scanned pages are read in)
QComboBox *languagesBox(QWidget *parent, const QString &now)
{
    auto *c = new QComboBox(parent);
    c->addItem(QObject::tr("Arabic and English"), QStringLiteral("ara+eng"));
    c->addItem(QObject::tr("English"), QStringLiteral("eng"));
    c->addItem(QObject::tr("Arabic"), QStringLiteral("ara"));
    c->setCurrentIndex(std::max(0, c->findData(now)));
    return c;
}

QComboBox *resolutionBox(QWidget *parent, int dpi)
{
    auto *c = new QComboBox(parent);
    c->addItem(QObject::tr("Determine Automatically (150 pixels/inch)"), 150);
    for (int d : {72, 96, 150, 300, 600})
        c->addItem(QObject::tr("%1 pixels/inch").arg(d), d);
    c->setCurrentIndex(std::max(0, c->findData(dpi, Qt::UserRole, Qt::MatchExactly) > 0 && dpi != 150 ? c->findData(dpi) : 0));
    return c;
}

// Settings, OK / Restore Defaults / Cancel round a page of choices: true if OK
bool settingsBox(QWidget *owner, const QString &title, const std::function<void(QVBoxLayout *, QWidget *)> &fill,
                 const std::function<void()> &defaults, const std::function<void()> &take)
{
    ClassicDialog dlg(owner, title);
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *page = new QVBoxLayout(body);
    page->setContentsMargins(12, 10, 12, 10);
    page->setSpacing(8);
    fill(page, body);
    auto *row = new QHBoxLayout;
    row->addStretch();
    auto *ok = new ClassicButton(QObject::tr("OK"), body);
    auto *def = new ClassicButton(QObject::tr("Restore Defaults"), body);
    auto *cancel = new ClassicButton(QObject::tr("Cancel"), body);
    for (ClassicButton *b : {ok, def, cancel}) {
        b->setMinimumWidth(80);
        row->addWidget(b);
    }
    ok->setDefault(true);
    page->addLayout(row);
    bool accepted = false;
    QObject::connect(ok, &QPushButton::clicked, &dlg, [&] {
        take();
        accepted = true;
        dlg.close();
    });
    QObject::connect(def, &QPushButton::clicked, &dlg, defaults);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    dlg.exec();
    return accepted;
}

QString titleOf(Format f)
{
    switch (f) {
    case Format::Docx: return QObject::tr("Save As DOCX Settings");
    case Format::Doc: return QObject::tr("Save As DOC Settings");
    case Format::Xlsx: return QObject::tr("Save As XLSX Settings");
    case Format::XmlSpreadsheet: return QObject::tr("Save As XML Spreadsheet Settings");
    case Format::Pptx: return QObject::tr("Save As PPTX Settings");
    case Format::Jpeg: return QObject::tr("Save As JPEG Settings");
    case Format::Jpeg2000: return QObject::tr("Save As JPEG 2000 Settings");
    case Format::Tiff: return QObject::tr("Save As TIFF Settings");
    case Format::Png: return QObject::tr("Save As PNG Settings");
    case Format::HtmlSingle:
    case Format::HtmlMulti: return QObject::tr("HTML Advanced Settings");
    case Format::Rtf: return QObject::tr("Save As RTF Settings");
    case Format::Eps: return QObject::tr("Save As EPS Settings");
    case Format::Ps: return QObject::tr("Save As PostScript Settings");
    case Format::TextAccessible: return QObject::tr("Save As Text (Accessible) Settings");
    case Format::TextPlain: return QObject::tr("Save As Text (Plain) Settings");
    case Format::Xml: return QObject::tr("Save As XML 1.0 Settings");
    }
    return QObject::tr("Settings");
}

void settingsFor(QWidget *owner, Format f, Options &o)
{
    Options d;  // (the defaults)
    const QString title = titleOf(f);
    switch (f) {
    case Format::Docx:
    case Format::Doc:
    case Format::Rtf:
    case Format::Pptx: {
        const bool slides = f == Format::Pptx;
        QRadioButton *flow = nullptr, *layout = nullptr;
        QCheckBox *comments = nullptr, *images = nullptr, *recognize = nullptr;
        QComboBox *langs = nullptr;
        auto show = [&](const Options &v) {
            if (flow) {
                flow->setChecked(v.flowing);
                layout->setChecked(!v.flowing);
                images->setChecked(v.images);
            }
            comments->setChecked(v.comments);
            recognize->setChecked(v.recognize);
            langs->setCurrentIndex(std::max(0, langs->findData(v.languages)));
        };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            if (!slides) {
                auto *g = box(QObject::tr("Layout Settings"), body);
                auto *l = new QHBoxLayout(g);
                flow = new QRadioButton(QObject::tr("Retain Flowing Text"), g);
                layout = new QRadioButton(QObject::tr("Retain Page Layout"), g);
                l->addWidget(flow);
                l->addWidget(layout);
                l->addStretch();
                page->addWidget(g);
            }
            auto *c = box(QObject::tr("Comments Settings"), body);
            comments = new QCheckBox(QObject::tr("Include Comments"), c);
            (new QVBoxLayout(c))->addWidget(comments);
            page->addWidget(c);
            if (!slides) {
                auto *g = box(QObject::tr("Image Settings"), body);
                images = new QCheckBox(QObject::tr("Include Images"), g);
                (new QVBoxLayout(g))->addWidget(images);
                page->addWidget(g);
            }
            auto *t = box(QObject::tr("Text Recognition Settings"), body);
            auto *tl = new QHBoxLayout(t);
            recognize = new QCheckBox(QObject::tr("Recognize text if needed"), t);
            langs = languagesBox(t, o.languages);
            tl->addWidget(recognize);
            tl->addSpacing(12);
            tl->addWidget(new QLabel(QObject::tr("Language:"), t));
            tl->addWidget(langs);
            page->addWidget(t);
            show(o);
        }, [&] { show(d); }, [&] {
            if (flow) {
                o.flowing = flow->isChecked();
                o.images = images->isChecked();
            }
            o.comments = comments->isChecked();
            o.recognize = recognize->isChecked();
            o.languages = langs->currentData().toString();
        });
        return;
    }
    case Format::Xlsx:
    case Format::XmlSpreadsheet: {
        QRadioButton *one = nullptr, *tables = nullptr, *pages = nullptr, *regional = nullptr, *custom = nullptr;
        QComboBox *dec = nullptr, *grp = nullptr, *langs = nullptr;
        QCheckBox *recognize = nullptr;
        auto show = [&](const Options &v) {
            one->setChecked(v.sheets == 0);
            tables->setChecked(v.sheets == 1);
            pages->setChecked(v.sheets == 2);
            regional->setChecked(v.regionalNumbers);
            custom->setChecked(!v.regionalNumbers);
            dec->setCurrentText(QString(v.decimal));
            grp->setCurrentText(QString(v.thousands));
            recognize->setChecked(v.recognize);
            langs->setCurrentIndex(std::max(0, langs->findData(v.languages)));
        };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            auto *g = box(QObject::tr("Excel Workbook Settings"), body);
            auto *l = new QVBoxLayout(g);
            one = new QRadioButton(QObject::tr("Create single Worksheet for the Document"), g);
            tables = new QRadioButton(QObject::tr("Create Worksheet for each Table"), g);
            pages = new QRadioButton(QObject::tr("Create Worksheet for each Page"), g);
            for (QRadioButton *r : {one, tables, pages})
                l->addWidget(r);
            page->addWidget(g);
            auto *n = box(QObject::tr("Numeric Settings"), body);
            auto *nl = new QGridLayout(n);
            regional = new QRadioButton(QObject::tr("Detect decimal and thousands separators using regional settings"), n);
            custom = new QRadioButton(QObject::tr("Treat the following as decimal and thousands separators"), n);
            auto *group = new QButtonGroup(n);
            group->addButton(regional);
            group->addButton(custom);
            dec = new QComboBox(n);
            grp = new QComboBox(n);
            dec->addItems({QStringLiteral("."), QStringLiteral(",")});
            grp->addItems({QStringLiteral(","), QStringLiteral("."), QStringLiteral(" "), QStringLiteral("'")});
            nl->addWidget(regional, 0, 0, 1, 2);
            nl->addWidget(custom, 1, 0, 1, 2);
            nl->addWidget(new QLabel(QObject::tr("    Decimal Symbol"), n), 2, 0);
            nl->addWidget(dec, 2, 1);
            nl->addWidget(new QLabel(QObject::tr("    Thousands Separator"), n), 3, 0);
            nl->addWidget(grp, 3, 1);
            QObject::connect(custom, &QRadioButton::toggled, n, [=](bool on) {
                dec->setEnabled(on);
                grp->setEnabled(on);
            });
            page->addWidget(n);
            auto *t = box(QObject::tr("Text Recognition Settings"), body);
            auto *tl = new QHBoxLayout(t);
            recognize = new QCheckBox(QObject::tr("Recognize text if needed"), t);
            langs = languagesBox(t, o.languages);
            tl->addWidget(recognize);
            tl->addSpacing(12);
            tl->addWidget(new QLabel(QObject::tr("Language:"), t));
            tl->addWidget(langs);
            page->addWidget(t);
            show(o);
            dec->setEnabled(custom->isChecked());
            grp->setEnabled(custom->isChecked());
        }, [&] { show(d); }, [&] {
            o.sheets = one->isChecked() ? 0 : tables->isChecked() ? 1 : 2;
            o.regionalNumbers = regional->isChecked();
            o.decimal = firstOf(dec->currentText(), QLatin1Char('.'));
            o.thousands = firstOf(grp->currentText(), QLatin1Char(','));
            o.recognize = recognize->isChecked();
            o.languages = langs->currentData().toString();
        });
        return;
    }
    case Format::Jpeg:
    case Format::Jpeg2000:
    case Format::Tiff:
    case Format::Png: {
        QComboBox *quality = nullptr, *scan = nullptr, *compression = nullptr, *colour = nullptr, *res = nullptr;
        QCheckBox *oneFile = nullptr;
        auto show = [&](const Options &v) {
            if (quality)
                quality->setCurrentIndex(std::max(0, quality->findData(v.quality)));
            if (scan)
                scan->setCurrentIndex(v.progressive ? 1 : 0);
            if (compression) {
                compression->setCurrentIndex(std::clamp(v.tiffCompression, 0, 2));
                oneFile->setChecked(v.tiffOneFile);
            }
            colour->setCurrentIndex(std::max(0, colour->findData(v.colour)));
            res->setCurrentIndex(std::max(0, v.dpi == 150 ? 0 : res->findData(v.dpi)));
        };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            if (f != Format::Png) {
                auto *g = box(QObject::tr("File Settings"), body);
                auto *l = new QGridLayout(g);
                int r = 0;
                if (f == Format::Jpeg || f == Format::Jpeg2000) {
                    quality = new QComboBox(g);
                    const QString what = f == Format::Jpeg ? QStringLiteral("JPEG") : QStringLiteral("JPEG 2000");
                    quality->addItem(QObject::tr("%1 (Quality : Minimum)").arg(what), 25);
                    quality->addItem(QObject::tr("%1 (Quality : Low)").arg(what), 45);
                    quality->addItem(QObject::tr("%1 (Quality : Medium)").arg(what), 70);
                    quality->addItem(QObject::tr("%1 (Quality : High)").arg(what), 85);
                    quality->addItem(QObject::tr("%1 (Quality : Maximum)").arg(what), f == Format::Jpeg ? 95 : 100);
                    l->addWidget(new QLabel(QObject::tr("Quality:"), g), r, 0);
                    l->addWidget(quality, r++, 1);
                }
                if (f == Format::Jpeg) {
                    scan = new QComboBox(g);
                    scan->addItems({QObject::tr("Baseline (Standard)"), QObject::tr("Progressive")});
                    l->addWidget(new QLabel(QObject::tr("Format:"), g), r, 0);
                    l->addWidget(scan, r++, 1);
                }
                if (f == Format::Tiff) {
                    compression = new QComboBox(g);
                    compression->addItems({QObject::tr("None"), QObject::tr("LZW (black and white: CCITT G4)"), QObject::tr("ZIP (black and white: CCITT G4)")});
                    l->addWidget(new QLabel(QObject::tr("Compression:"), g), r, 0);
                    l->addWidget(compression, r++, 1);
                    oneFile = new QCheckBox(QObject::tr("All the pages in one TIFF file"), g);
                    l->addWidget(oneFile, r++, 0, 1, 2);
                }
                page->addWidget(g);
            }
            auto *c = box(QObject::tr("Conversion"), body);
            auto *cl = new QGridLayout(c);
            colour = new QComboBox(c);
            colour->addItem(QObject::tr("Determine Automatically"), 0);
            colour->addItem(QObject::tr("Color"), 1);
            colour->addItem(QObject::tr("Grayscale"), 2);
            if (f != Format::Jpeg && f != Format::Jpeg2000)
                colour->addItem(QObject::tr("Monochrome"), 3);
            res = resolutionBox(c, o.dpi);
            cl->addWidget(new QLabel(QObject::tr("Colorspace:"), c), 0, 0);
            cl->addWidget(colour, 0, 1);
            cl->addWidget(new QLabel(QObject::tr("Resolution:"), c), 1, 0);
            cl->addWidget(res, 1, 1);
            page->addWidget(c);
            show(o);
        }, [&] { show(d); }, [&] {
            if (quality)
                o.quality = quality->currentData().toInt();
            if (scan)
                o.progressive = scan->currentIndex() == 1;
            if (compression) {
                o.tiffCompression = compression->currentIndex();
                o.tiffOneFile = oneFile->isChecked();
            }
            o.colour = colour->currentData().toInt();
            o.dpi = res->currentData().toInt();
        });
        return;
    }
    case Format::HtmlSingle:
    case Format::HtmlMulti: {
        QCheckBox *images = nullptr, *comments = nullptr, *recognize = nullptr;
        QComboBox *langs = nullptr;
        auto show = [&](const Options &v) {
            images->setChecked(v.images);
            comments->setChecked(v.comments);
            recognize->setChecked(v.recognize);
            langs->setCurrentIndex(std::max(0, langs->findData(v.languages)));
        };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            auto *g = box(QObject::tr("Content"), body);
            auto *l = new QVBoxLayout(g);
            images = new QCheckBox(QObject::tr("Include Images (saved in a folder beside the page)"), g);
            comments = new QCheckBox(QObject::tr("Include Comments"), g);
            l->addWidget(images);
            l->addWidget(comments);
            page->addWidget(g);
            auto *t = box(QObject::tr("Text Recognition Settings"), body);
            auto *tl = new QHBoxLayout(t);
            recognize = new QCheckBox(QObject::tr("Recognize text if needed"), t);
            langs = languagesBox(t, o.languages);
            tl->addWidget(recognize);
            tl->addSpacing(12);
            tl->addWidget(new QLabel(QObject::tr("Language:"), t));
            tl->addWidget(langs);
            page->addWidget(t);
            show(o);
        }, [&] { show(d); }, [&] {
            o.images = images->isChecked();
            o.comments = comments->isChecked();
            o.recognize = recognize->isChecked();
            o.languages = langs->currentData().toString();
        });
        return;
    }
    case Format::Eps:
    case Format::Ps: {
        QRadioButton *ascii = nullptr, *binary = nullptr, *all = nullptr, *some = nullptr;
        QSpinBox *from = nullptr, *to = nullptr;
        QComboBox *res = nullptr, *colour = nullptr;
        auto show = [&](const Options &v) {
            ascii->setChecked(!v.binary);
            binary->setChecked(v.binary);
            all->setChecked(v.lastPage < 0);
            some->setChecked(v.lastPage >= 0);
            from->setValue(v.firstPage + 1);
            to->setValue(v.lastPage < 0 ? to->maximum() : v.lastPage + 1);
            res->setCurrentIndex(std::max(0, v.dpi == 150 ? 0 : res->findData(v.dpi)));
            colour->setCurrentIndex(v.colour == 2 ? 1 : 0);
        };
        d.dpi = 300;
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            auto *g = box(QObject::tr("Settings"), body);
            auto *l = new QGridLayout(g);
            ascii = new QRadioButton(QObject::tr("ASCII"), g);
            binary = new QRadioButton(QObject::tr("Binary"), g);
            l->addWidget(ascii, 0, 0);
            l->addWidget(binary, 1, 0);
            l->addWidget(new QLabel(QObject::tr("PostScript: Language Level 2"), g), 0, 1);
            res = resolutionBox(g, o.dpi);
            colour = new QComboBox(g);
            colour->addItem(QObject::tr("Color"), 1);
            colour->addItem(QObject::tr("Grayscale"), 2);
            l->addWidget(new QLabel(QObject::tr("Resolution:"), g), 2, 0);
            l->addWidget(res, 2, 1);
            l->addWidget(new QLabel(QObject::tr("Color:"), g), 3, 0);
            l->addWidget(colour, 3, 1);
            auto *note = new QLabel(QObject::tr("(Each page is written as a picture at the resolution chosen.)"), g);
            note->setWordWrap(true);
            l->addWidget(note, 4, 0, 1, 2);
            page->addWidget(g);
            auto *r = box(QObject::tr("Page Range"), body);
            auto *rl = new QHBoxLayout(r);
            all = new QRadioButton(QObject::tr("All"), r);
            some = new QRadioButton(QObject::tr("Pages:"), r);
            from = new QSpinBox(r);
            to = new QSpinBox(r);
            from->setRange(1, 99999);
            to->setRange(1, 99999);
            rl->addWidget(all);
            rl->addSpacing(12);
            rl->addWidget(some);
            rl->addWidget(new QLabel(QObject::tr("From:"), r));
            rl->addWidget(from);
            rl->addWidget(new QLabel(QObject::tr("To:"), r));
            rl->addWidget(to);
            rl->addStretch();
            QObject::connect(some, &QRadioButton::toggled, r, [=](bool on) {
                from->setEnabled(on);
                to->setEnabled(on);
            });
            page->addWidget(r);
            show(o);
            from->setEnabled(some->isChecked());
            to->setEnabled(some->isChecked());
        }, [&] { show(d); }, [&] {
            o.binary = binary->isChecked();
            o.firstPage = all->isChecked() ? 0 : std::min(from->value(), to->value()) - 1;
            o.lastPage = all->isChecked() ? -1 : std::max(from->value(), to->value()) - 1;
            o.dpi = res->currentData().toInt();
            o.colour = colour->currentData().toInt();
        });
        return;
    }
    case Format::TextAccessible:
    case Format::TextPlain: {
        QComboBox *enc = nullptr;
        auto show = [&](const Options &v) { enc->setCurrentIndex(std::max(0, enc->findData(v.encoding))); };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            auto *g = box(QObject::tr("Output File Settings"), body);
            auto *l = new QHBoxLayout(g);
            enc = new QComboBox(g);
            enc->addItem(QObject::tr("UTF-8 (all languages)"), QStringLiteral("UTF-8"));
            enc->addItem(QObject::tr("UTF-8 with its mark (for Notepad)"), QStringLiteral("UTF-8 BOM"));
            enc->addItem(QObject::tr("UTF-16 (all languages)"), QStringLiteral("UTF-16LE"));
            enc->addItem(QObject::tr("Windows Arabic (1256)"), QStringLiteral("windows-1256"));
            enc->addItem(QObject::tr("Windows Western (1252)"), QStringLiteral("windows-1252"));
            enc->addItem(QObject::tr("ISO Latin-1 (8859-1)"), QStringLiteral("ISO-8859-1"));
            l->addWidget(new QLabel(QObject::tr("Encoding:"), g));
            l->addWidget(enc, 1);
            page->addWidget(g);
            show(o);
        }, [&] { show(d); }, [&] { o.encoding = enc->currentData().toString(); });
        return;
    }
    case Format::Xml: {
        QComboBox *enc = nullptr, *down = nullptr;
        QCheckBox *marks = nullptr, *tags = nullptr, *images = nullptr, *sub = nullptr, *pre = nullptr, *downOn = nullptr;
        QLineEdit *folder = nullptr, *prefix = nullptr;
        QRadioButton *tif = nullptr, *jpg = nullptr, *png = nullptr;
        auto show = [&](const Options &v) {
            enc->setCurrentIndex(std::max(0, enc->findData(v.encoding)));
            marks->setChecked(v.bookmarks);
            images->setChecked(v.xmlImages);
            sub->setChecked(v.subfolder);
            folder->setText(v.folder);
            pre->setChecked(!v.prefix.isEmpty());
            prefix->setText(v.prefix);
            tif->setChecked(v.imageFormat == QLatin1String("tif"));
            jpg->setChecked(v.imageFormat == QLatin1String("jpg"));
            png->setChecked(v.imageFormat == QLatin1String("png"));
            downOn->setChecked(v.downsample > 0);
            down->setCurrentIndex(std::max(0, down->findData(v.downsample > 0 ? v.downsample : 150)));
        };
        settingsBox(owner, title, [&](QVBoxLayout *page, QWidget *body) {
            auto *g = box(QObject::tr("Output File Settings"), body);
            auto *l = new QGridLayout(g);
            enc = new QComboBox(g);
            enc->addItem(QObject::tr("UTF-8"), QStringLiteral("UTF-8"));
            enc->addItem(QObject::tr("UTF-16"), QStringLiteral("UTF-16"));
            enc->addItem(QObject::tr("ASCII (other letters as references)"), QStringLiteral("ASCII"));
            marks = new QCheckBox(QObject::tr("Generate bookmarks"), g);
            tags = new QCheckBox(QObject::tr("Generate tags for untagged files"), g);
            tags->setChecked(true);
            tags->setEnabled(false);  // (always: the paragraphs and headings found are its tags)
            l->addWidget(new QLabel(QObject::tr("Encoding:"), g), 0, 0);
            l->addWidget(enc, 0, 1);
            l->addWidget(marks, 1, 0, 1, 2);
            l->addWidget(tags, 2, 0, 1, 2);
            page->addWidget(g);
            auto *i = box(QObject::tr("Image File Settings"), body);
            auto *il = new QGridLayout(i);
            images = new QCheckBox(QObject::tr("Generate images"), i);
            sub = new QCheckBox(QObject::tr("Use sub-folder:"), i);
            folder = new QLineEdit(i);
            pre = new QCheckBox(QObject::tr("Use prefix:"), i);
            prefix = new QLineEdit(i);
            tif = new QRadioButton(QStringLiteral("TIFF"), i);
            jpg = new QRadioButton(QStringLiteral("JPG"), i);
            png = new QRadioButton(QStringLiteral("PNG"), i);
            downOn = new QCheckBox(QObject::tr("Downsample to:"), i);
            down = new QComboBox(i);
            for (int dpi : {72, 96, 150, 300})
                down->addItem(QString::number(dpi), dpi);
            il->addWidget(images, 0, 0, 1, 4);
            il->addWidget(sub, 1, 0);
            il->addWidget(folder, 1, 1, 1, 3);
            il->addWidget(pre, 2, 0);
            il->addWidget(prefix, 2, 1, 1, 3);
            il->addWidget(new QLabel(QObject::tr("Output format:"), i), 3, 0);
            il->addWidget(tif, 3, 1);
            il->addWidget(jpg, 3, 2);
            il->addWidget(png, 3, 3);
            il->addWidget(downOn, 4, 0);
            il->addWidget(down, 4, 1);
            il->addWidget(new QLabel(QObject::tr("dpi"), i), 4, 2);
            auto sync = [=] {
                const bool on = images->isChecked();
                for (QWidget *w : std::initializer_list<QWidget *>{sub, pre, tif, jpg, png, downOn})
                    w->setEnabled(on);
                folder->setEnabled(on && sub->isChecked());
                prefix->setEnabled(on && pre->isChecked());
                down->setEnabled(on && downOn->isChecked());
            };
            for (QCheckBox *c : {images, sub, pre, downOn})
                QObject::connect(c, &QCheckBox::toggled, i, sync);
            page->addWidget(i);
            show(o);
            sync();
        }, [&] { show(d); }, [&] {
            o.encoding = enc->currentData().toString();
            o.bookmarks = marks->isChecked();
            o.xmlImages = images->isChecked();
            o.subfolder = sub->isChecked();
            o.folder = folder->text().trimmed().isEmpty() ? QStringLiteral("images") : folder->text().trimmed();
            o.prefix = pre->isChecked() ? prefix->text().trimmed() : QString();
            o.imageFormat = tif->isChecked() ? QStringLiteral("tif") : png->isChecked() ? QStringLiteral("png") : QStringLiteral("jpg");
            o.downsample = downOn->isChecked() ? down->currentData().toInt() : 0;
        });
        return;
    }
    }
}

// the file's kind, for the Save box
QString filterOf(Format f)
{
    switch (f) {
    case Format::Docx: return QObject::tr("Word Document (*.docx)");
    case Format::Doc: return QObject::tr("Word 97-2003 Document (*.doc)");
    case Format::Xlsx: return QObject::tr("Excel Workbook (*.xlsx)");
    case Format::XmlSpreadsheet: return QObject::tr("XML Spreadsheet 2003 (*.xml)");
    case Format::Pptx: return QObject::tr("PowerPoint Presentation (*.pptx)");
    case Format::Jpeg: return QObject::tr("JPEG (*.jpg *.jpeg)");
    case Format::Jpeg2000: return QObject::tr("JPEG 2000 (*.jp2)");
    case Format::Tiff: return QObject::tr("TIFF (*.tif *.tiff)");
    case Format::Png: return QObject::tr("PNG (*.png)");
    case Format::HtmlSingle:
    case Format::HtmlMulti: return QObject::tr("Web Page (*.html *.htm)");
    case Format::Rtf: return QObject::tr("Rich Text Format (*.rtf)");
    case Format::Eps: return QObject::tr("Encapsulated PostScript (*.eps)");
    case Format::Ps: return QObject::tr("PostScript (*.ps)");
    case Format::TextAccessible:
    case Format::TextPlain: return QObject::tr("Text (*.txt)");
    case Format::Xml: return QObject::tr("XML 1.0 (*.xml)");
    }
    return QString();
}

// the box while it converts: what it's doing, how far - Cancel stops it
class ConvertingBox : public ClassicDialog {
public:
    explicit ConvertingBox(QWidget *owner, std::atomic<bool> *cancel) : ClassicDialog(owner, QObject::tr("Converting")), cancel_(cancel)
    {
        auto *l = new QVBoxLayout(body());
        l->setContentsMargins(12, 12, 12, 12);
        l->setSpacing(8);
        label = new QLabel(QObject::tr("Getting ready..."), body());
        label->setFont(theme::font());
        l->addWidget(label);
        bar = new ClassicProgress(body());
        l->addWidget(bar);
        auto *row = new QHBoxLayout;
        row->addStretch();
        auto *stop = new ClassicButton(QObject::tr("Cancel"), body());
        stop->setMinimumWidth(80);
        QObject::connect(stop, &QPushButton::clicked, body(), [this] { close(); });
        row->addWidget(stop);
        l->addLayout(row);
        body()->setFixedWidth(360);
    }
    QLabel *label = nullptr;
    ClassicProgress *bar = nullptr;
    bool done = false;

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (!done) {  // (Cancel, the X: it stops - and closes once it has)
            e->ignore();
            *cancel_ = true;
            label->setText(QObject::tr("Stopping..."));
            return;
        }
        ClassicDialog::closeEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape && !done) {
            close();
            return;
        }
        ClassicDialog::keyPressEvent(e);
    }

private:
    std::atomic<bool> *cancel_;
};

// the PDF's first page, as the Home page shows it: on a sheet of paper - a thin dark line round it, its shadow along its
// right and bottom - drawn in the screen's own pixels (sharp at 125%)
QPixmap preview(Document *doc, QSize size, double dpr)
{
    QImage out(size * dpr, QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    if (!doc || doc->pageCount() < 1)
        return QPixmap::fromImage(out);
    const QSizeF page = doc->pageSize(0);
    const QSizeF room = QSizeF(size - QSize(10, 10)) * dpr;
    const double scale = std::min(room.width() / std::max(1.0, page.width()), room.height() / std::max(1.0, page.height()));
    QImage im = doc->render(0, scale, QRect(0, 0, std::max(1, int(page.width() * scale)), std::max(1, int(page.height() * scale))));
    if (im.isNull())
        return QPixmap::fromImage(out);
    if (im.hasAlphaChannel()) {  // (see-through parts: on white paper)
        QImage paper(im.size(), QImage::Format_ARGB32_Premultiplied);
        paper.fill(Qt::white);
        QPainter(&paper).drawImage(0, 0, im);
        im = paper;
    }
    QPainter p(&out);
    const QPointF at((size.width() - im.width() / dpr) / 2, (size.height() - im.height() / dpr) / 2);
    const QRectF sheet(at, im.size() / dpr);
    {
        classic::Px x(p);
        const QRect d = x.dev(sheet);
        x.fill(QRect(d.left() + 2 * x.k, d.bottom() + 1 + x.k, d.width(), 2 * x.k), theme::shadow());
        x.fill(QRect(d.right() + 1 + x.k, d.top() + 2 * x.k, 2 * x.k, d.height()), theme::shadow());
        x.bevel(d.adjusted(-x.k, -x.k, x.k, x.k), theme::c("#404040", theme::Edge), theme::c("#404040", theme::Edge), x.k);
    }
    p.drawImage(sheet, im);
    p.end();
    return QPixmap::fromImage(out);
}

}  // namespace

void MainWindow::convertDialog(const QString &givenPath)
{
    // the PDF: the one shown - or (Home) one picked from the computer
    std::shared_ptr<Document> doc;
    QString path = givenPath;
    if (path.isEmpty() && current_ >= 0)
        doc = view_->document();
    auto pick = [this]() -> QString {
        return QFileDialog::getOpenFileName(this, tr("Select a PDF to convert"), theme::settings().value("last_folder").toString(),
                                            tr("PDF files (*.pdf)"));
    };
    if (!doc && path.isEmpty()) {
        path = pick();
        if (path.isEmpty())
            return;
    }

    ClassicDialog dlg(this, tr("Convert"));
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *page = new QVBoxLayout(body);
    page->setContentsMargins(14, 12, 14, 12);
    page->setSpacing(10);
    auto *head = new QLabel(tr("Export your PDF to any format"), body);
    head->setFont(theme::font(12, true));
    page->addWidget(head);
    auto *row = new QHBoxLayout;
    row->setSpacing(14);
    // (left: the PDF)
    auto *left = new QVBoxLayout;
    auto *icon = new QLabel(body);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(112, 140);
    auto *name = new QLabel(body);
    name->setWordWrap(true);
    name->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    name->setFixedWidth(112);
    auto *select = new ClassicButton(tr("Select a File..."), body);
    left->addWidget(icon);
    left->addWidget(name);
    left->addWidget(select, 0, Qt::AlignHCenter);
    left->addStretch();
    row->addLayout(left);
    // (a PDF picked from the computer: opened now - its first page shown, then converted)
    if (!doc) {
        QString error;
        doc = Document::open(path, &error);
        if (!doc) {
            sounds::play(sounds::Error);
            classicMessage(this, tr("Convert"), tr("Couldn't open %1:\n%2").arg(QFileInfo(path).fileName(), error));
            return;
        }
    }
    auto showName = [&] {
        name->setText(doc->path().isEmpty() ? tr("(this PDF)") : QFileInfo(doc->path()).fileName());
        icon->setPixmap(preview(doc.get(), icon->size(), icon->devicePixelRatioF()));
    };
    showName();
    connect(select, &QPushButton::clicked, &dlg, [&] {
        const QString p = pick();
        if (p.isEmpty())
            return;
        QString error;
        auto picked = Document::open(p, &error);
        if (!picked) {
            sounds::play(sounds::Error);
            classicMessage(&dlg, tr("Convert"), tr("Couldn't open %1:\n%2").arg(QFileInfo(p).fileName(), error));
            return;
        }
        path = p;
        doc = picked;
        showName();
    });
    // (middle: the kinds of file; right: each kind's formats)
    auto *kinds = new QListWidget(body);
    kinds->addItems({tr("Microsoft Word"), tr("Spreadsheet"), tr("Microsoft PowerPoint"), tr("Image"), tr("HTML Web Page"), tr("More Formats")});
    kinds->setFixedWidth(150);
    kinds->setFont(theme::font(10));
    row->addWidget(kinds);
    auto *stack = new QStackedWidget(body);
    row->addWidget(stack, 1);
    page->addLayout(row);

    std::map<QRadioButton *, Format> formats;
    QButtonGroup *group = new QButtonGroup(&dlg);
    std::map<Format, Options> opts;
    auto optionsOf = [&](Format f) -> Options & {
        auto it = opts.find(f);
        if (it == opts.end())
            it = opts.emplace(f, optionsFor(f)).first;
        return it->second;
    };
    // a format's choice: its name, and its Settings button
    auto addFormat = [&](QVBoxLayout *l, QWidget *parent, const QString &label, Format f, bool settings = true) {
        auto *r = new QHBoxLayout;
        auto *b = new QRadioButton(label, parent);
        formats[b] = f;
        group->addButton(b);
        r->addWidget(b);
        if (settings) {  // (its settings: a gear beside it while it's the one chosen - as Acrobat's)
            auto *s = new ToolButton(QStringLiteral("gear"), tr("Settings"), tr("This format's settings."), false, QString(), parent);
            s->setVisible(false);
            connect(b, &QRadioButton::toggled, s, &QWidget::setVisible);
            connect(s, &ToolButton::clicked, &dlg, [&, f] { settingsFor(&dlg, f, optionsOf(f)); });
            r->addSpacing(4);
            r->addWidget(s);
        }
        r->addStretch();
        l->addLayout(r);
        return b;
    };
    auto pageOf = [&]() {
        auto *w = new QWidget(stack);
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(8);
        stack->addWidget(w);
        return std::make_pair(w, l);
    };
    {
        auto [w, l] = pageOf();
        addFormat(l, w, tr("Word Document (.docx)"), Format::Docx)->setChecked(true);
        addFormat(l, w, tr("Word 97-2003 Document (.doc)"), Format::Doc);
        l->addStretch();
    }
    {
        auto [w, l] = pageOf();
        addFormat(l, w, tr("Microsoft Excel Workbook (.xlsx)"), Format::Xlsx);
        addFormat(l, w, tr("XML Spreadsheet 2003 (.xml)"), Format::XmlSpreadsheet);
        l->addStretch();
    }
    {
        auto [w, l] = pageOf();
        addFormat(l, w, tr("PowerPoint presentation (.pptx)"), Format::Pptx);
        l->addStretch();
    }
    QCheckBox *allImages = nullptr;
    {
        auto [w, l] = pageOf();
        addFormat(l, w, QStringLiteral("JPEG"), Format::Jpeg);
        addFormat(l, w, QStringLiteral("JPEG 2000"), Format::Jpeg2000);
        addFormat(l, w, QStringLiteral("TIFF"), Format::Tiff);
        addFormat(l, w, QStringLiteral("PNG"), Format::Png);
        auto *line = new QFrame(w);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        l->addWidget(line);
        allImages = new QCheckBox(tr("Export all images"), w);
        allImages->setToolTip(tr("The PDF's own pictures, each as a file - not pictures of its pages."));
        l->addWidget(allImages);
        l->addStretch();
    }
    QCheckBox *navHeadings = nullptr, *navMarks = nullptr;
    QRadioButton *splitHeadings = nullptr, *splitMarks = nullptr;
    {
        auto [w, l] = pageOf();
        QRadioButton *single = addFormat(l, w, tr("Single HTML page"), Format::HtmlSingle, false);
        auto *navLabel = new QLabel(tr("      Add Navigation Frame"), w);
        navHeadings = new QCheckBox(tr("Headings-based"), w);
        navMarks = new QCheckBox(tr("Bookmarks-based"), w);
        l->addWidget(navLabel);
        for (QCheckBox *c : {navHeadings, navMarks}) {
            auto *r = new QHBoxLayout;
            r->addSpacing(28);
            r->addWidget(c);
            r->addStretch();
            l->addLayout(r);
        }
        QRadioButton *multi = addFormat(l, w, tr("Multiple HTML pages"), Format::HtmlMulti, false);
        splitHeadings = new QRadioButton(tr("Split by Doc Headings"), w);
        splitMarks = new QRadioButton(tr("Split by Doc Bookmarks"), w);
        auto *splits = new QButtonGroup(w);
        splits->addButton(splitHeadings);
        splits->addButton(splitMarks);
        splitHeadings->setChecked(true);
        for (QRadioButton *c : {splitHeadings, splitMarks}) {
            auto *r = new QHBoxLayout;
            r->addSpacing(28);
            r->addWidget(c);
            r->addStretch();
            l->addLayout(r);
        }
        // (a navigation frame: by the headings or by the bookmarks - one of them)
        connect(navHeadings, &QCheckBox::toggled, w, [=](bool on) { if (on) navMarks->setChecked(false); });
        connect(navMarks, &QCheckBox::toggled, w, [=](bool on) { if (on) navHeadings->setChecked(false); });
        auto sync = [=] {
            navHeadings->setEnabled(single->isChecked());
            navMarks->setEnabled(single->isChecked());
            navLabel->setEnabled(single->isChecked());
            splitHeadings->setEnabled(multi->isChecked());
            splitMarks->setEnabled(multi->isChecked());
        };
        connect(single, &QRadioButton::toggled, w, sync);
        connect(multi, &QRadioButton::toggled, w, sync);
        sync();
        auto *line = new QFrame(w);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        l->addWidget(line);
        auto *adv = new ClassicButton(tr("Advanced Settings..."), w);
        connect(adv, &QPushButton::clicked, &dlg, [&] { settingsFor(&dlg, Format::HtmlSingle, optionsOf(Format::HtmlSingle)); });
        l->addWidget(adv, 0, Qt::AlignLeft);
        l->addStretch();
        const Options &h = optionsOf(Format::HtmlSingle);
        navHeadings->setChecked(h.htmlNav == 1);
        navMarks->setChecked(h.htmlNav == 2);
        (h.htmlSplit == 1 ? splitMarks : splitHeadings)->setChecked(true);
    }
    {
        auto [w, l] = pageOf();
        addFormat(l, w, tr("Rich Text Format"), Format::Rtf);
        addFormat(l, w, tr("Encapsulated PostScript"), Format::Eps);
        addFormat(l, w, tr("PostScript"), Format::Ps);
        addFormat(l, w, tr("Text (Accessible)"), Format::TextAccessible);
        addFormat(l, w, tr("Text (Plain)"), Format::TextPlain);
        addFormat(l, w, tr("XML 1.0"), Format::Xml);
        l->addStretch();
    }
    {  // (as wide as its widest kind's choices from the start - the box doesn't grow as another kind is picked)
        int widest = 0;
        for (int i = 0; i < stack->count(); ++i)
            widest = std::max(widest, stack->widget(i)->sizeHint().width() + 30);  // (+ a gear shown beside a choice)
        stack->setFixedWidth(widest);
    }
    // (a kind picked: its formats - its first chosen, if none of its own is)
    connect(kinds, &QListWidget::currentRowChanged, &dlg, [&](int r) {
        stack->setCurrentIndex(r);
        bool mine = false;
        for (const auto &[b, f] : formats)
            mine = mine || (b->isChecked() && b->window() && stack->widget(r)->isAncestorOf(b));
        if (!mine)
            for (QRadioButton *b : stack->widget(r)->findChildren<QRadioButton *>())
                if (formats.count(b)) {
                    b->setChecked(true);
                    break;
                }
    });
    {  // (as last time)
        const QString last = theme::settings().value("convert_last").toString();
        int kind = 0;
        for (const auto &[b, f] : formats)
            if (keyOf(f) == last && f != Format::HtmlMulti) {
                for (int i = 0; i < stack->count(); ++i)
                    if (stack->widget(i)->isAncestorOf(b))
                        kind = i;
                kinds->setCurrentRow(kind);
                b->setChecked(true);
            }
        if (kinds->currentRow() < 0)
            kinds->setCurrentRow(0);
        allImages->setChecked(optionsOf(Format::Jpeg).allImages);
    }
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *go = new ClassicButton(tr("Convert"), body);
    auto *cancel = new ClassicButton(tr("Cancel"), body);
    go->setDefault(true);
    for (ClassicButton *b : {go, cancel}) {
        b->setMinimumWidth(85);
        buttons->addWidget(b);
    }
    page->addLayout(buttons);
    std::optional<Format> chosen;
    connect(go, &QPushButton::clicked, &dlg, [&] {
        for (const auto &[b, f] : formats)
            if (b->isChecked())
                chosen = f;
        dlg.close();
    });
    connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    dlg.exec();
    if (!chosen)
        return;

    // the choices kept for next time
    const Format f = *chosen;
    Options o = optionsOf(f);
    if (f == Format::HtmlSingle || f == Format::HtmlMulti) {
        o = optionsOf(Format::HtmlSingle);
        o.htmlNav = navHeadings->isChecked() ? 1 : navMarks->isChecked() ? 2 : 0;
        o.htmlSplit = splitMarks->isChecked() ? 1 : 0;
        keep(Format::HtmlSingle, o);
    } else {
        if (f == Format::Jpeg || f == Format::Jpeg2000 || f == Format::Tiff || f == Format::Png)
            o.allImages = allImages->isChecked();
        keep(f, o);
        if (f == Format::Jpeg || f == Format::Jpeg2000 || f == Format::Tiff || f == Format::Png) {  // (shared by the pictures)
            Options j = optionsOf(Format::Jpeg);
            j.allImages = o.allImages;
            keep(Format::Jpeg, j);
        }
    }
    theme::saveSetting("convert_last", keyOf(f));

    if (o.recognize && ocr::dataFolder().isEmpty())
        o.recognize = false;
    // where it goes
    const QString source = doc->path().isEmpty() ? tr("Untitled") : QFileInfo(doc->path()).completeBaseName();
    const QString folder = doc->path().isEmpty() ? theme::settings().value("last_folder").toString() : QFileInfo(doc->path()).absolutePath();
    const QString ext = convert::extension(f);
    QString out = QFileDialog::getSaveFileName(this, tr("Convert to"), QDir(folder).filePath(source + QLatin1Char('.') + ext), filterOf(f));
    if (out.isEmpty())
        return;
    if (QFileInfo(out).suffix().isEmpty())
        out += QLatin1Char('.') + ext;

    // converted on a thread of its own - the box shows how far; Cancel stops it
    std::atomic<bool> stop{false};
    ConvertingBox box(this, &stop);
    struct Shared {
        std::atomic<bool> finished{false};
        QStringList written;
        QString error;
        double done = 0;
        QString doing;
        std::mutex lock;
    };
    auto shared = std::make_shared<Shared>();
    convert::Progress pr;
    pr.cancel = &stop;
    pr.step = [shared](double d, const QString &what) {
        std::lock_guard g(shared->lock);
        shared->done = d;
        if (!what.isEmpty())
            shared->doing = what;
    };
    std::thread worker([shared, doc, f, out, o, pr] {
        QString error;
        const QStringList written = convert::run(doc.get(), f, out, o, pr, &error);
        std::lock_guard g(shared->lock);
        shared->written = written;
        shared->error = error;
        shared->finished = true;
    });
    QTimer poll;
    poll.setInterval(80);
    connect(&poll, &QTimer::timeout, &box, [&] {
        std::lock_guard g(shared->lock);
        box.bar->setValue(shared->done);
        if (!stop && !shared->doing.isEmpty())
            box.label->setText(shared->doing + QStringLiteral("..."));
        if (shared->finished) {
            box.done = true;
            box.close();
        }
    });
    poll.start();
    box.exec();
    poll.stop();
    worker.join();
    if (stop) {
        for (const QString &w : shared->written)  // (stopped: what was half written taken away)
            QFile::remove(w);
        say(tr("Converting was stopped."));
        return;
    }
    if (shared->written.isEmpty()) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Convert"), tr("It couldn't be converted:\n%1").arg(shared->error.isEmpty() ? tr("nothing was written") : shared->error));
        return;
    }
    sounds::play(sounds::Done);
    const QString first = shared->written.front();
    const QString what = shared->written.size() > 1 ? tr("%1 files were made, the first:\n%2").arg(shared->written.size()).arg(QDir::toNativeSeparators(first))
                                                    : tr("It was saved as:\n%1").arg(QDir::toNativeSeparators(first));
    const QString open = tr("Open"), show = tr("Show in folder");
    const QString answer = classicMessage(this, tr("Convert"), tr("Converted.") + QLatin1Char('\n') + what, {open, show, tr("OK")});
    if (answer == open)
        QDesktopServices::openUrl(QUrl::fromLocalFile(first));
    else if (answer == show)
        showInFolder(first);
    say(tr("Converted: %1").arg(QFileInfo(first).fileName()));
}
