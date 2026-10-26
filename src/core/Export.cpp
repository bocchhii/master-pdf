#include "core/Export.h"

#include "core/Document.h"
#include "core/Ocr.h"
#include "core/PageText.h"

#include <mupdf/fitz.h>

#include <leptonica/allheaders.h>

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QLocale>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringEncoder>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace convert {

namespace {

struct Cancelled {};

// ---- what a page holds: paragraphs, pictures, comments - its lines too (for slides and tables) ----

struct Span {
    QString text;
    QString family = QStringLiteral("Arial");
    double size = 11;
    bool bold = false, italic = false;
    QColor color = Qt::black;
};

struct Para {
    std::vector<Span> spans;
    bool rtl = false;
    int heading = 0;  // (1..3: a heading - by its size; 0: body text)
    QString align = QStringLiteral("left");  // left, right, center, justify
    QRectF box;       // (page points)
    QString text() const
    {
        QString t;
        for (const Span &s : spans)
            t += s.text;
        return t;
    }
};

struct Pic {
    QImage image;
    QRectF box;
};

struct Item {
    bool picture = false;
    Para para;
    Pic pic;
    double y = 0;
};

struct Note {
    QString text;
    QRectF box;
};

struct Page {
    int number = 0;
    QSizeF size;
    std::vector<Item> items;
    std::vector<Note> notes;
    std::vector<TextLine> lines;
    bool read = false;  // (its text read from its picture: a scanned page)
};

QString familyOf(const QString &pdfFont)
{
    QString f = pdfFont.section(QLatin1Char('+'), -1);
    f = f.section(QLatin1Char('-'), 0, 0).section(QLatin1Char(','), 0, 0);
    f.remove(QRegularExpression(QStringLiteral("(MT|PS|PSMT)$")));
    static const QRegularExpression camel(QStringLiteral("([a-z])([A-Z])"));
    f.replace(camel, QStringLiteral("\\1 \\2"));
    return f.isEmpty() ? QStringLiteral("Arial") : f;
}

bool arabicIn(const QString &s)
{
    for (const QChar c : s)
        if (pagetext::isRtl(c.unicode()))
            return true;
    return false;
}

Span spanOf(const TextRun &r)
{
    Span s;
    s.text = r.text;
    s.family = familyOf(r.font);
    s.size = r.size > 0 ? r.size : 11;
    s.bold = (r.flags & 1) || r.font.contains(QLatin1String("Bold"), Qt::CaseInsensitive) || (r.flags & 16);
    s.italic = (r.flags & 2) || r.font.contains(QLatin1String("Italic"), Qt::CaseInsensitive) ||
               r.font.contains(QLatin1String("Oblique"), Qt::CaseInsensitive);
    s.color = QColor::fromRgb(QRgb(0xFF000000u | r.color));
    return s;
}

void addSpan(std::vector<Span> &spans, const Span &s)
{
    if (s.text.isEmpty())
        return;
    if (!spans.empty()) {
        Span &b = spans.back();
        if (b.family == s.family && std::abs(b.size - s.size) < 0.3 && b.bold == s.bold && b.italic == s.italic && b.color == s.color) {
            b.text += s.text;
            return;
        }
    }
    spans.push_back(s);
}

// (the text reader's direction marks - unseen letters: not text)
QString unmarked(QString s)
{
    static const QRegularExpression marks(QStringLiteral("[\\x{200E}\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{061C}]"));
    return s.remove(marks).simplified();
}

// a scanned page's text, read from its picture: as lines (as the page's own would be)
std::vector<TextLine> readLines(Document *doc, int page, const Options &o)
{
    const double dpi = 300;
    const QSizeF size = doc->pageSize(page) * (dpi / 72);
    const QImage im = doc->render(page, dpi / 72, QRect(0, 0, int(size.width()), int(size.height())));
    QString error;
    std::vector<TextLine> out;
    for (const ocr::Line &l : ocr::read(im, &error, false, o.languages)) {
        const QString text = unmarked(l.text);
        if (text.isEmpty())
            continue;
        const QRectF r(l.box.x() * 72 / dpi, l.box.y() * 72 / dpi, l.box.width() * 72 / dpi, l.box.height() * 72 / dpi);
        TextLine line;
        line.rect = r;
        line.text = text;
        line.rtl = arabicIn(text);
        line.baseline = r.bottom() - r.height() * 0.2;
        TextRun run;
        run.text = text;
        run.font = QStringLiteral("Arial");
        run.size = std::max(4.0, r.height() * 0.72);
        run.box = r;
        run.origin = QPointF(r.left(), line.baseline);
        line.runs.push_back(run);
        out.push_back(line);
    }
    return out;
}

Page readPage(Document *doc, int page, const Options &o, bool pictures, bool notes)
{
    Page p;
    p.number = page;
    p.size = doc->pageSize(page);
    p.lines = doc->lines(page);
    {
        int letters = 0;
        for (const TextLine &l : p.lines)
            letters += int(l.text.trimmed().size());
        if (letters < 4 && o.recognize && !ocr::dataFolder().isEmpty()) {
            p.lines = readLines(doc, page, o);
            p.read = true;
        }
    }
    // (turned lines: written straight - their text kept)
    const std::vector<pagetext::TextFrame> frames = pagetext::frames(p.lines);
    for (const pagetext::TextFrame &f : frames)
        for (const std::vector<int> &paraLines : f.paras) {
            Para para;
            int rtl = 0;
            QRectF box;
            for (size_t n = 0; n < paraLines.size(); ++n) {
                const TextLine &l = p.lines[size_t(paraLines[n])];
                rtl += l.rtl ? 1 : -1;
                box |= l.rect;
                for (const TextRun &r : l.runs)
                    addSpan(para.spans, spanOf(r));
                if (n + 1 < paraLines.size()) {  // (a line's end: a space - or a word broken across it put together)
                    if (!para.spans.empty() && para.spans.back().text.endsWith(QLatin1Char('-')) && !l.rtl)
                        para.spans.back().text.chop(1);
                    else if (!para.spans.empty() && !para.spans.back().text.endsWith(QLatin1Char(' ')))
                        para.spans.back().text += QLatin1Char(' ');
                }
            }
            if (para.text().trimmed().isEmpty())
                continue;
            para.rtl = rtl > 0;
            para.box = box;
            // (how it's aligned: its lines' sides)
            double minL = 1e9, maxL = -1e9, minR = 1e9, maxR = -1e9;
            for (int k : paraLines) {
                const QRectF &r = p.lines[size_t(k)].rect;
                minL = std::min(minL, r.left()), maxL = std::max(maxL, r.left());
                minR = std::min(minR, r.right()), maxR = std::max(maxR, r.right());
            }
            const bool even = paraLines.size() > 2 && maxL - minL < 3 && maxR - minR < 3;
            const bool middle = std::abs(box.center().x() - p.size.width() / 2) < p.size.width() * 0.04 && box.width() < p.size.width() * 0.75;
            para.align = even ? QStringLiteral("justify") : middle && (maxL - minL > 3 || paraLines.size() == 1) ? QStringLiteral("center")
                         : para.rtl ? QStringLiteral("right") : QStringLiteral("left");
            Item it;
            it.para = para;
            it.y = box.top();
            p.items.push_back(it);
        }
    if (pictures)
        for (const Picture &pic : doc->pictures(page)) {
            if (pic.vector || pic.rect.width() < 4 || pic.rect.height() < 4)
                continue;
            if (p.read && pic.rect.width() * pic.rect.height() > 0.6 * p.size.width() * p.size.height())
                continue;  // (a scanned page's scan: its text read is what's written - the scan again would be it twice)
            QImage im = doc->pictureImage(pic);
            if (im.isNull())
                continue;
            if (im.hasAlphaChannel()) {  // (see-through parts: on white, as the page shows them)
                QImage flat(im.size(), QImage::Format_RGB32);
                flat.fill(Qt::white);
                QPainter(&flat).drawImage(0, 0, im);
                im = flat;
            }
            Item it;
            it.picture = true;
            it.pic = {im, pic.rect};
            it.y = pic.rect.top();
            p.items.push_back(it);
        }
    std::stable_sort(p.items.begin(), p.items.end(), [](const Item &a, const Item &b) { return a.y < b.y; });
    // (a list's bullet read as a paragraph of its own - "-", "•": put at the start of the paragraph beside it)
    static const QRegularExpression bullet(QStringLiteral("^\\s*[-\\x{2022}\\x{25E6}\\x{25AA}\\x{25CF}\\x{2013}*o]\\s*$"));
    for (size_t k = 0; k < p.items.size(); ++k) {
        const Item &it = p.items[k];
        if (it.picture || !bullet.match(it.para.text()).hasMatch())
            continue;
        // (its paragraph: the one whose first line is level with it - before it or after it in the reading)
        long best = -1;
        double far = it.para.box.height() * 0.8;
        for (long n = std::max(0L, long(k) - 3); n < long(p.items.size()) && n <= long(k) + 3; ++n) {
            const Item &other = p.items[size_t(n)];
            if (n == long(k) || other.picture || bullet.match(other.para.text()).hasMatch())
                continue;
            const double d = std::abs(other.para.box.top() - it.para.box.top());
            if (d < far)
                far = d, best = n;
        }
        if (best < 0)
            continue;
        Para &into = p.items[size_t(best)].para;
        Span mark = it.para.spans.front();
        mark.text = it.para.text().trimmed() + QLatin1Char(' ');
        into.spans.insert(into.spans.begin(), mark);
        into.box |= it.para.box;
        p.items.erase(p.items.begin() + long(k));
        --k;
    }
    if (notes)
        for (const Annot &a : doc->annots(page))
            if (!a.contents.trimmed().isEmpty())
                p.notes.push_back({a.contents.trimmed(), a.rect});
    return p;
}

// headings: by their size, against the body text's (most of the letters') size
void findHeadings(std::vector<Page> &pages)
{
    std::map<int, double> sizes;  // (half points -> letters)
    for (const Page &p : pages)
        for (const Item &it : p.items)
            if (!it.picture)
                for (const Span &s : it.para.spans)
                    sizes[int(std::lround(s.size * 2))] += s.text.size();
    double body = 11, most = -1;
    for (const auto &[half, n] : sizes)
        if (n > most)
            most = n, body = half / 2.0;
    for (Page &p : pages)
        for (Item &it : p.items) {
            if (it.picture)
                continue;
            double size = 0;
            bool bold = true;
            for (const Span &s : it.para.spans) {
                size = std::max(size, s.size);
                bold = bold && (s.bold || s.text.trimmed().isEmpty());
            }
            const int words = int(it.para.text().split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
            int letters = 0;
            for (const QChar c : it.para.text())
                letters += c.isLetter() ? 1 : 0;
            if (words > 25 || letters < 2)
                continue;  // (a long paragraph - or a mark, a number on its own: not a heading)
            it.para.heading = size >= body * 1.6 ? 1 : size >= body * 1.3 ? 2 : (size >= body * 1.12 || (bold && words <= 12 && size >= body - 0.1)) ? 3 : 0;
        }
}

// ---- the pages read, each in turn (progress told, a cancel answered) ----

std::vector<int> pagesOf(Document *doc, const Options &o)
{
    const int count = doc->pageCount();
    const int first = std::clamp(o.firstPage, 0, std::max(0, count - 1));
    const int last = o.lastPage < 0 ? count - 1 : std::clamp(o.lastPage, first, count - 1);
    std::vector<int> out;
    for (int p = first; p <= last; ++p)
        out.push_back(p);
    return out;
}

void check(const Progress &pr)
{
    if (pr.cancel && pr.cancel->load())
        throw Cancelled();
}

std::vector<Page> readAll(Document *doc, const Options &o, const Progress &pr, bool pictures, bool notes, double share = 0.6)
{
    std::vector<Page> pages;
    const std::vector<int> which = pagesOf(doc, o);
    for (size_t k = 0; k < which.size(); ++k) {
        check(pr);
        pr.step(share * double(k) / double(which.size()), QObject::tr("Reading page %1 of %2").arg(k + 1).arg(which.size()));
        pages.push_back(readPage(doc, which[k], o, pictures, notes));
    }
    findHeadings(pages);
    return pages;
}

// ---- small helpers ----

QString esc(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default:
            if (c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\t'))
                continue;  // (not allowed in XML)
            out += c;
        }
    }
    return out;
}

QByteArray pngOf(const QImage &im)
{
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    im.save(&buf, "PNG");
    return b;
}

QByteArray jpegOf(const QImage &im, int quality, bool progressive = false)
{
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "JPEG");
    w.setQuality(quality);
    w.setProgressiveScanWrite(progressive);
    w.write(im.hasAlphaChannel() ? im.convertToFormat(QImage::Format_RGB32) : im);
    return b;
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        if (error)
            *error = QObject::tr("%1 couldn't be written: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    return true;
}

// text in the encoding chosen (UTF-8 with no mark: as the web and XML expect; others with theirs)
QByteArray encoded(const QString &text, const QString &encoding, bool mark = true)
{
    if (encoding.compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) == 0)
        return text.toUtf8();
    if (encoding.startsWith(QLatin1String("UTF-8"), Qt::CaseInsensitive))  // (UTF-8 with its mark)
        return QByteArray("\xEF\xBB\xBF") + text.toUtf8();
    QStringEncoder enc(encoding.toLatin1().constData(), mark ? QStringEncoder::Flag::WriteBom : QStringEncoder::Flag::Default);
    if (!enc.isValid())
        return text.toUtf8();
    QByteArray out = enc(text);
    return out;
}

// a picture as the format chosen: its colour (greyscale, black and white)
QImage coloured(const QImage &im, int colour)
{
    if (colour == 2)
        return im.convertToFormat(QImage::Format_Grayscale8);
    if (colour == 3)
        return im.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither);
    return im.convertToFormat(QImage::Format_RGB32);
}

// a picture to leptonica (TIFF, JPEG 2000): 32-bit RGB, 8-bit grey, 1-bit black and white
PIX *pixOf(const QImage &in)
{
    if (in.format() == QImage::Format_Mono || in.format() == QImage::Format_MonoLSB) {
        const QImage im = in.convertToFormat(QImage::Format_Mono);
        PIX *pix = pixCreate(im.width(), im.height(), 1);
        for (int y = 0; y < im.height(); ++y) {
            const uchar *src = im.constScanLine(y);
            l_uint32 *row = pixGetData(pix) + y * pixGetWpl(pix);
            (void)src;
            for (int x = 0; x < im.width(); ++x) {
                const bool black = qGray(im.pixel(x, y)) < 128;
                if (black)
                    SET_DATA_BIT(row, x);
            }
        }
        return pix;
    }
    if (in.format() == QImage::Format_Grayscale8) {
        PIX *pix = pixCreate(in.width(), in.height(), 8);
        for (int y = 0; y < in.height(); ++y) {
            const uchar *src = in.constScanLine(y);
            l_uint32 *row = pixGetData(pix) + y * pixGetWpl(pix);
            for (int x = 0; x < in.width(); ++x)
                SET_DATA_BYTE(row, x, src[x]);
        }
        return pix;
    }
    const QImage im = in.convertToFormat(QImage::Format_RGB32);
    PIX *pix = pixCreate(im.width(), im.height(), 32);
    for (int y = 0; y < im.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        l_uint32 *row = pixGetData(pix) + y * pixGetWpl(pix);
        for (int x = 0; x < im.width(); ++x)
            composeRGBPixel(qRed(src[x]), qGreen(src[x]), qBlue(src[x]), row + x);
    }
    return pix;
}

QByteArray tiffOf(const QImage &im, int compression, int dpi)
{
    PIX *pix = pixOf(im);
    pixSetResolution(pix, dpi, dpi);
    const int depth = pixGetDepth(pix);
    const int comp = depth == 1 ? (compression == 0 ? IFF_TIFF : IFF_TIFF_G4)
                                : compression == 0 ? IFF_TIFF : compression == 2 ? IFF_TIFF_ZIP : IFF_TIFF_LZW;
    l_uint8 *data = nullptr;
    size_t size = 0;
    QByteArray out;
    if (pixWriteMemTiff(&data, &size, pix, comp) == 0 && data)
        out = QByteArray(reinterpret_cast<const char *>(data), qsizetype(size));
    lept_free(data);
    pixDestroy(&pix);
    return out;
}

QByteArray jp2Of(const QImage &im, int quality)
{
    PIX *pix = pixOf(im.format() == QImage::Format_Mono ? im.convertToFormat(QImage::Format_Grayscale8) : im);
    l_uint8 *data = nullptr;
    size_t size = 0;
    QByteArray out;
    // (leptonica's quality: a signal-to-noise ratio, 27..45 - 0: lossless)
    const int snr = quality >= 98 ? 0 : 27 + (std::clamp(quality, 0, 97) * 18) / 97;
    if (pixWriteMemJp2k(&data, &size, pix, snr, 5, 0, 0) == 0 && data)
        out = QByteArray(reinterpret_cast<const char *>(data), qsizetype(size));
    lept_free(data);
    pixDestroy(&pix);
    return out;
}

// a page as a picture, at dpi
QImage pagePicture(Document *doc, int page, int dpi)
{
    const QSizeF size = doc->pageSize(page) * (dpi / 72.0);
    QImage im = doc->render(page, dpi / 72.0, QRect(0, 0, std::max(1, int(size.width())), std::max(1, int(size.height()))));
    if (im.hasAlphaChannel()) {
        QImage flat(im.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter(&flat).drawImage(0, 0, im);
        im = flat;
    }
    return im;
}

// a zip file (Office's .docx, .xlsx, .pptx are): its parts
class Zip {
public:
    explicit Zip(const QString &path)
    {
        ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx_) {
            zip_ = fz_new_zip_writer(ctx_, path.toUtf8().constData());
        }
        fz_catch(ctx_) {
            error_ = QString::fromUtf8(fz_caught_message(ctx_));
        }
    }
    ~Zip()
    {
        if (zip_)
            fz_drop_zip_writer(ctx_, zip_);
        fz_drop_context(ctx_);
    }
    bool ok() const { return zip_ != nullptr && error_.isEmpty(); }
    QString error() const { return error_; }
    void add(const QString &name, const QByteArray &data, bool compress = true)
    {
        if (!ok())
            return;
        fz_buffer *b = nullptr;
        fz_var(b);
        fz_try(ctx_) {
            b = fz_new_buffer_from_copied_data(ctx_, reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()));
            fz_write_zip_entry(ctx_, zip_, name.toUtf8().constData(), b, compress ? 1 : 0);
        }
        fz_always(ctx_) {
            fz_drop_buffer(ctx_, b);
        }
        fz_catch(ctx_) {
            error_ = QString::fromUtf8(fz_caught_message(ctx_));
        }
    }
    bool close()
    {
        if (!zip_)
            return false;
        fz_try(ctx_) {
            fz_close_zip_writer(ctx_, zip_);
        }
        fz_catch(ctx_) {
            error_ = QString::fromUtf8(fz_caught_message(ctx_));
        }
        return error_.isEmpty();
    }

private:
    fz_context *ctx_ = nullptr;
    fz_zip_writer *zip_ = nullptr;
    QString error_;
};

const char *kXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

QString hexColor(const QColor &c)
{
    return QStringLiteral("%1%2%3").arg(c.red(), 2, 16, QLatin1Char('0')).arg(c.green(), 2, 16, QLatin1Char('0')).arg(c.blue(), 2, 16, QLatin1Char('0')).toUpper();
}

long long emu(double points)
{
    return std::llround(points * 12700);
}

// ---- Word (.docx) ----

QString docxRun(const Span &s, bool rtlPara)
{
    const bool rtl = arabicIn(s.text) || (rtlPara && !s.text.contains(QRegularExpression(QStringLiteral("[A-Za-z]"))));
    QString pr = QStringLiteral("<w:rFonts w:ascii=\"%1\" w:hAnsi=\"%1\" w:cs=\"%1\"/>").arg(esc(s.family));
    if (s.bold)
        pr += QLatin1String("<w:b/><w:bCs/>");
    if (s.italic)
        pr += QLatin1String("<w:i/><w:iCs/>");
    if (s.color != Qt::black)
        pr += QStringLiteral("<w:color w:val=\"%1\"/>").arg(hexColor(s.color));
    const int half = std::max(2, int(std::lround(s.size * 2)));
    pr += QStringLiteral("<w:sz w:val=\"%1\"/><w:szCs w:val=\"%1\"/>").arg(half);
    if (rtl)
        pr += QLatin1String("<w:rtl/>");
    return QStringLiteral("<w:r><w:rPr>%1</w:rPr><w:t xml:space=\"preserve\">%2</w:t></w:r>").arg(pr, esc(s.text));
}

QStringList docxWrite(const std::vector<Page> &pages, const QString &path, const Options &o, QString *error)
{
    Zip zip(path);
    if (!zip.ok()) {
        if (error)
            *error = zip.error();
        return {};
    }
    QString body, rels, comments;
    int images = 0, notes = 0, shapes = 0;
    const double pageW = pages.empty() ? 612 : pages.front().size.width(), pageH = pages.empty() ? 792 : pages.front().size.height();
    const double margin = o.flowing ? 72 : 36, room = pageW - 2 * margin;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const Page &p = pages[pi];
        // (its comments: each on the paragraph nearest it)
        std::map<size_t, std::vector<int>> noteAt;
        for (const Note &n : p.notes) {
            size_t best = 0;
            double far = 1e18;
            for (size_t k = 0; k < p.items.size(); ++k) {
                const double d = std::abs(p.items[k].y - n.box.top());
                if (!p.items[k].picture && d < far)
                    far = d, best = k;
            }
            const int id = notes++;
            noteAt[best].push_back(id);
            comments += QStringLiteral("<w:comment w:id=\"%1\" w:author=\"Master PDF\" w:initials=\"MP\"><w:p><w:r><w:t xml:space=\"preserve\">%2</w:t></w:r></w:p></w:comment>")
                            .arg(id)
                            .arg(esc(n.text));
        }
        double lastBottom = margin;
        for (size_t k = 0; k < p.items.size(); ++k) {
            const Item &it = p.items[k];
            if (it.picture) {
                if (!o.images)
                    continue;
                const int n = ++images;
                zip.add(QStringLiteral("word/media/image%1.png").arg(n), pngOf(it.pic.image), false);
                rels += QStringLiteral("<Relationship Id=\"rIdImg%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image%1.png\"/>").arg(n);
                double w = it.pic.box.width(), h = it.pic.box.height();
                if (w > room)
                    h *= room / w, w = room;
                const QString spacing = o.flowing ? QString() : QStringLiteral("<w:spacing w:before=\"%1\" w:after=\"0\"/><w:ind w:left=\"%2\"/>")
                                                                       .arg(std::max(0, int((it.pic.box.top() - lastBottom) * 20)))
                                                                       .arg(std::max(0, int((it.pic.box.left() - margin) * 20)));
                body += QStringLiteral("<w:p><w:pPr>%1</w:pPr><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%2\" cy=\"%3\"/>"
                                       "<wp:docPr id=\"%4\" name=\"Picture %4\"/><a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
                                       "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                                       "<pic:nvPicPr><pic:cNvPr id=\"%4\" name=\"image%4.png\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"rIdImg%4\"/>"
                                       "<a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%2\" cy=\"%3\"/></a:xfrm>"
                                       "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>")
                            .arg(spacing)
                            .arg(emu(w))
                            .arg(emu(h))
                            .arg(n);
                ++shapes;
                lastBottom = it.pic.box.top() + h;
                continue;
            }
            const Para &para = it.para;
            QString ppr;
            if (para.heading)
                ppr += QStringLiteral("<w:pStyle w:val=\"Heading%1\"/>").arg(para.heading);
            if (para.rtl)
                ppr += QLatin1String("<w:bidi/>");
            if (!o.flowing) {  // (where it is on the page: the gap above it, its indent)
                const int before = std::max(0, int((para.box.top() - lastBottom) * 20));
                const int ind = para.rtl ? std::max(0, int((pageW - margin - para.box.right()) * 20)) : std::max(0, int((para.box.left() - margin) * 20));
                ppr += QStringLiteral("<w:spacing w:before=\"%1\" w:after=\"0\" w:line=\"%2\" w:lineRule=\"auto\"/>").arg(before).arg(240);
                ppr += para.rtl ? QStringLiteral("<w:ind w:right=\"%1\"/>").arg(ind) : QStringLiteral("<w:ind w:left=\"%1\"/>").arg(ind);
            }
            if (para.align == QLatin1String("center"))
                ppr += QLatin1String("<w:jc w:val=\"center\"/>");
            else if (para.align == QLatin1String("justify"))
                ppr += QLatin1String("<w:jc w:val=\"both\"/>");
            else if (!para.rtl && para.align == QLatin1String("right"))
                ppr += QLatin1String("<w:jc w:val=\"right\"/>");
            QString runs;
            for (const Span &s : para.spans)
                runs += docxRun(s, para.rtl);
            const auto ns = noteAt.find(k);
            if (ns != noteAt.end()) {
                QString start, end;
                for (int id : ns->second) {
                    start += QStringLiteral("<w:commentRangeStart w:id=\"%1\"/>").arg(id);
                    end += QStringLiteral("<w:commentRangeEnd w:id=\"%1\"/><w:r><w:commentReference w:id=\"%1\"/></w:r>").arg(id);
                }
                runs = start + runs + end;
            }
            body += QStringLiteral("<w:p><w:pPr>%1</w:pPr>%2</w:p>").arg(ppr, runs);
            lastBottom = para.box.bottom();
        }
        if (!o.flowing && pi + 1 < pages.size())
            body += QLatin1String("<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>");
    }
    body += QStringLiteral("<w:sectPr><w:pgSz w:w=\"%1\" w:h=\"%2\"/><w:pgMar w:top=\"%3\" w:right=\"%3\" w:bottom=\"%3\" w:left=\"%3\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>")
                .arg(int(pageW * 20))
                .arg(int(pageH * 20))
                .arg(int(margin * 20));
    const QString doc = QString::fromLatin1(kXml) +
                        QStringLiteral("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                                       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                                       "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                                       "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                                       "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>%1</w:body></w:document>")
                            .arg(body);
    QString styles = QString::fromLatin1(kXml) +
                     QLatin1String("<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                                   "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:cs=\"Arial\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault>"
                                   "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\" w:line=\"264\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
                                   "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>");
    for (int h = 1; h <= 3; ++h)
        styles += QStringLiteral("<w:style w:type=\"paragraph\" w:styleId=\"Heading%1\"><w:name w:val=\"heading %1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
                                 "<w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"120\"/><w:outlineLvl w:val=\"%2\"/></w:pPr><w:rPr><w:b/><w:bCs/></w:rPr></w:style>")
                      .arg(h)
                      .arg(h - 1);
    styles += QLatin1String("</w:styles>");
    QString types = QString::fromLatin1(kXml) +
                    QLatin1String("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                                  "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                                  "<Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"png\" ContentType=\"image/png\"/>"
                                  "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                                  "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>");
    QString docRels = QString::fromLatin1(kXml) +
                      QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                    "<Relationship Id=\"rIdStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>");
    if (notes) {
        types += QLatin1String("<Override PartName=\"/word/comments.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml\"/>");
        docRels += QLatin1String("<Relationship Id=\"rIdComments\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\" Target=\"comments.xml\"/>");
        zip.add(QStringLiteral("word/comments.xml"),
                (QString::fromLatin1(kXml) + QStringLiteral("<w:comments xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">%1</w:comments>").arg(comments)).toUtf8());
    }
    types += QLatin1String("</Types>");
    docRels += rels + QLatin1String("</Relationships>");
    zip.add(QStringLiteral("[Content_Types].xml"), types.toUtf8());
    zip.add(QStringLiteral("_rels/.rels"),
            (QString::fromLatin1(kXml) + QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                                                       "</Relationships>"))
                .toUtf8());
    zip.add(QStringLiteral("word/document.xml"), doc.toUtf8());
    zip.add(QStringLiteral("word/styles.xml"), styles.toUtf8());
    zip.add(QStringLiteral("word/_rels/document.xml.rels"), docRels.toUtf8());
    if (!zip.close()) {
        if (error)
            *error = zip.error();
        return {};
    }
    (void)shapes;
    return {path};
}

// ---- Rich Text ----

QString rtfText(const QString &s)
{
    QString out;
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u == '\\' || u == '{' || u == '}')
            out += QLatin1Char('\\') + QString(c);
        else if (u == '\n')
            out += QLatin1String("\\line ");
        else if (u < 0x80)
            out += c;
        else
            out += QStringLiteral("\\u%1?").arg(short(u));
    }
    return out;
}

QStringList rtfWrite(const std::vector<Page> &pages, const QString &path, const Options &o, QString *error)
{
    QStringList fonts{QStringLiteral("Arial")};
    std::vector<QColor> colors{Qt::black};
    auto fontIndex = [&](const QString &f) {
        int i = int(fonts.indexOf(f));
        if (i < 0)
            fonts << f, i = int(fonts.size()) - 1;
        return i;
    };
    auto colorIndex = [&](const QColor &c) {
        for (size_t i = 0; i < colors.size(); ++i)
            if (colors[i] == c)
                return int(i) + 1;
        colors.push_back(c);
        return int(colors.size());
    };
    const double pageW = pages.empty() ? 612 : pages.front().size.width(), pageH = pages.empty() ? 792 : pages.front().size.height();
    const double margin = o.flowing ? 72 : 36;
    QString body;
    int noteId = 0;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const Page &p = pages[pi];
        double lastBottom = margin;
        std::map<size_t, std::vector<QString>> noteAt;
        for (const Note &n : p.notes) {
            size_t best = 0;
            double far = 1e18;
            for (size_t k = 0; k < p.items.size(); ++k)
                if (!p.items[k].picture && std::abs(p.items[k].y - n.box.top()) < far)
                    far = std::abs(p.items[k].y - n.box.top()), best = k;
            noteAt[best].push_back(n.text);
        }
        for (size_t k = 0; k < p.items.size(); ++k) {
            const Item &it = p.items[k];
            if (it.picture) {
                if (!o.images)
                    continue;
                double w = it.pic.box.width(), h = it.pic.box.height();
                const double room = pageW - 2 * margin;
                if (w > room)
                    h *= room / w, w = room;
                const QByteArray png = pngOf(it.pic.image);
                body += QStringLiteral("\\pard\\plain\\sb%1 {\\pict\\pngblip\\picw%2\\pich%3\\picwgoal%4\\pichgoal%5 %6}\\par\n")
                            .arg(o.flowing ? 0 : std::max(0, int((it.pic.box.top() - lastBottom) * 20)))
                            .arg(it.pic.image.width())
                            .arg(it.pic.image.height())
                            .arg(int(w * 20))
                            .arg(int(h * 20))
                            .arg(QString::fromLatin1(png.toHex()));
                lastBottom = it.pic.box.top() + h;
                continue;
            }
            const Para &para = it.para;
            QString pard = QStringLiteral("\\pard\\plain");
            pard += para.rtl ? QLatin1String("\\rtlpar") : QLatin1String("\\ltrpar");
            pard += para.align == QLatin1String("center")    ? QLatin1String("\\qc")
                    : para.align == QLatin1String("justify") ? QLatin1String("\\qj")
                    : para.align == QLatin1String("right")   ? QLatin1String("\\qr")
                                                             : QLatin1String("\\ql");
            if (para.heading)
                pard += QStringLiteral("\\outlinelevel%1\\sb240\\sa120\\keepn").arg(para.heading - 1);
            else if (o.flowing)
                pard += QLatin1String("\\sa120");
            if (!o.flowing) {
                pard += QStringLiteral("\\sb%1\\sa0").arg(std::max(0, int((para.box.top() - lastBottom) * 20)));
                pard += para.rtl ? QStringLiteral("\\ri%1").arg(std::max(0, int((pageW - margin - para.box.right()) * 20)))
                                 : QStringLiteral("\\li%1").arg(std::max(0, int((para.box.left() - margin) * 20)));
            }
            QString runs;
            for (const Span &s : para.spans) {
                const bool rtl = arabicIn(s.text);
                runs += QStringLiteral("{%1\\f%2\\fs%3%4%5\\cf%6 %7}")
                            .arg(rtl ? QStringLiteral("\\rtlch\\af%1\\afs%2").arg(fontIndex(s.family)).arg(int(std::lround(s.size * 2))) : QStringLiteral("\\ltrch"))
                            .arg(fontIndex(s.family))
                            .arg(int(std::lround(s.size * 2)))
                            .arg(s.bold ? (rtl ? QStringLiteral("\\b\\ab") : QStringLiteral("\\b")) : QString())
                            .arg(s.italic ? (rtl ? QStringLiteral("\\i\\ai") : QStringLiteral("\\i")) : QString())
                            .arg(colorIndex(s.color))
                            .arg(rtfText(s.text));
            }
            const auto ns = noteAt.find(k);
            if (ns != noteAt.end())
                for (const QString &t : ns->second)
                    runs += QStringLiteral("{\\*\\atnid MP}{\\*\\atnauthor Master PDF}\\chatn{\\*\\annotation{\\*\\atnref %1}\\pard\\plain %2}")
                                .arg(++noteId)
                                .arg(rtfText(t));
            body += pard + QLatin1Char(' ') + runs + QLatin1String("\\par\n");
            lastBottom = para.box.bottom();
        }
        if (!o.flowing && pi + 1 < pages.size())
            body += QLatin1String("\\page\n");
    }
    QString head = QStringLiteral("{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl");
    for (int i = 0; i < fonts.size(); ++i)
        head += QStringLiteral("{\\f%1\\fnil\\fcharset0 %2;}").arg(i).arg(fonts[i]);
    head += QLatin1String("}{\\colortbl;");
    for (const QColor &c : colors)
        head += QStringLiteral("\\red%1\\green%2\\blue%3;").arg(c.red()).arg(c.green()).arg(c.blue());
    head += QStringLiteral("}\\paperw%1\\paperh%2\\margl%3\\margr%3\\margt%3\\margb%3\n").arg(int(pageW * 20)).arg(int(pageH * 20)).arg(int(margin * 20));
    if (!writeFile(path, (head + body + QLatin1String("}")).toLatin1(), error))
        return {};
    return {path};
}

// ---- tables: the pages' text in rows and columns (for spreadsheets) ----

struct Cell {
    QRectF box;
    QString text;
};

struct Table {
    QString name;
    std::vector<std::vector<QString>> rows;
    bool rtl = false;
};

// a piece of a line, its letters as the page shows them (left to right): in their reading order - Arabic's turned
// round, its numbers and English kept as they read (the bidi algorithm's reordering is its own inverse here)
QString readingOrder(const std::vector<TextChar> &piece)
{
    QString shown;
    int rtl = 0, ltr = 0;
    for (const TextChar &c : piece) {
        shown += QString::fromUcs4(&c.c, 1);
        const pagetext::Bidi t = pagetext::bidiType(c.c);
        rtl += t == pagetext::Bidi::R ? 1 : 0;
        ltr += t == pagetext::Bidi::L ? 1 : 0;
    }
    if (rtl == 0)
        return shown.simplified();
    std::vector<char32_t> codes;
    for (const TextChar &c : piece)
        codes.push_back(c.c);
    std::vector<pagetext::Bidi> types;
    for (char32_t c : codes)
        types.push_back(pagetext::bidiType(c));
    const std::vector<size_t> order = pagetext::bidiOrder(pagetext::bidiLevels(types, rtl >= ltr));
    QString out;
    for (size_t i : order)
        out += QString::fromUcs4(&codes[i], 1);
    return out.simplified();
}

// each row of a page's text: its pieces apart (a gap far wider than a space between them) - each piece read as a line
// (its letters in their reading order: Arabic's too)
std::vector<std::vector<Cell>> rowsOf(Document *doc, int page, const Options &o)
{
    const auto chars = doc->text(page);
    std::vector<Cell> cells;
    int letters = 0;
    for (const TextChar &c : *chars)
        letters += QChar::isSpace(c.c) ? 0 : 1;
    if (letters < 4 && o.recognize && !ocr::dataFolder().isEmpty()) {  // (a scanned page: its words read - cells where they're far apart)
        const double dpi = 300;
        const QImage im = pagePicture(doc, page, int(dpi));
        QString error;
        for (const ocr::Line &l : ocr::read(im, &error, false, o.languages)) {
            Cell cell;
            QRect last;
            for (const ocr::Word &w : l.words) {
                if (!cell.text.isEmpty() && std::abs(w.box.center().x() - last.center().x()) - (w.box.width() + last.width()) / 2.0 > l.box.height() * 1.1) {
                    cells.push_back(cell);
                    cell = Cell();
                }
                const QRectF b(w.box.x() * 72 / dpi, w.box.y() * 72 / dpi, w.box.width() * 72 / dpi, w.box.height() * 72 / dpi);
                cell.box |= b;
                const QString word = unmarked(w.text);
                cell.text = cell.text.isEmpty() ? word : cell.text + QLatin1Char(' ') + word;
                last = w.box;
            }
            if (!cell.text.isEmpty())
                cells.push_back(cell);
        }
    }
    // the page's lines (their letters in reading order, Arabic's joined letters put right): each split where its runs are
    // far apart - a table's cells side by side
    if (cells.empty())
        for (const TextLine &l : doc->lines(page)) {
            if (std::abs(l.angle) > 0.01 || l.text.trimmed().isEmpty())
                continue;
            std::vector<size_t> byX(l.runs.size());
            for (size_t i = 0; i < byX.size(); ++i)
                byX[i] = i;
            std::sort(byX.begin(), byX.end(), [&](size_t a, size_t b) { return l.runs[a].box.left() < l.runs[b].box.left(); });
            std::vector<int> cellOf(l.runs.size(), 0);
            int n = 0;
            double right = -1e9;
            for (size_t k : byX) {
                const TextRun &r = l.runs[k];
                if (right > -1e9 && r.box.left() - right > std::max(6.0, r.size * 1.5))
                    ++n;
                cellOf[k] = n;
                right = std::max(right, r.box.right());
            }
            if (n == 0) {
                cells.push_back({l.rect, l.text.simplified()});
                continue;
            }
            std::vector<Cell> parts(size_t(n) + 1);
            for (size_t k = 0; k < l.runs.size(); ++k) {  // (each cell's runs in the line's reading order)
                Cell &c = parts[size_t(cellOf[k])];
                c.box |= l.runs[k].box;
                c.text += l.runs[k].text;
            }
            for (Cell &c : parts)
                if (!c.text.trimmed().isEmpty()) {
                    c.text = c.text.simplified();
                    cells.push_back(c);
                }
        }
    // (rows: the cells side by side - their middles at about the same height)
    std::sort(cells.begin(), cells.end(), [](const Cell &a, const Cell &b) { return a.box.center().y() < b.box.center().y(); });
    std::vector<std::vector<Cell>> rows;
    for (const Cell &c : cells) {
        if (!rows.empty()) {
            const QRectF &r = rows.back().front().box;
            if (std::abs(c.box.center().y() - r.center().y()) < std::max(2.0, std::min(c.box.height(), r.height()) * 0.5)) {
                rows.back().push_back(c);
                continue;
            }
        }
        rows.push_back({c});
    }
    return rows;
}

// the rows in columns: each column where its cells start (Arabic: end) - the cells' starts gathered
std::vector<Table> tablesOf(Document *doc, const std::vector<int> &pages, const Options &o, const Progress &pr)
{
    // a page's text in rows: a row of one piece (a paragraph's line) in the first column; rows of two or more side by side
    // - a table - each piece in its column, the table's columns where its pieces start (Arabic's: end)
    Table whole;
    whole.name = QObject::tr("Document");
    std::vector<Table> byPage, byTable;
    int arabic = 0, latin = 0;
    for (size_t k = 0; k < pages.size(); ++k) {
        const int page = pages[k];
        check(pr);
        pr.step(0.8 * double(k) / double(pages.size()), QObject::tr("Finding the tables: page %1 of %2").arg(k + 1).arg(pages.size()));
        const std::vector<std::vector<Cell>> rows = rowsOf(doc, page, o);
        int a = 0, l = 0;
        for (const auto &r : rows)
            for (const Cell &c : r)
                (arabicIn(c.text) ? a : l) += 1;
        const bool rtl = a > l;
        arabic += a, latin += l;
        Table pageTable;
        pageTable.name = QObject::tr("Page %1").arg(page + 1);
        pageTable.rtl = rtl;
        for (size_t i = 0; i < rows.size();) {
            if (rows[i].size() < 2) {  // (a line on its own)
                pageTable.rows.push_back({rows[i].front().text});
                ++i;
                continue;
            }
            // a table: the rows of two pieces or more from here (a row of one piece inside it - a cell's second line - too,
            // if more come after it)
            size_t end = i;
            while (end < rows.size() && (rows[end].size() >= 2 || (end + 1 < rows.size() && rows[end + 1].size() >= 2 && end > i)))
                ++end;
            std::vector<double> edges;
            for (size_t r = i; r < end; ++r)
                for (const Cell &c : rows[r])
                    edges.push_back(rtl ? -c.box.right() : c.box.left());
            std::sort(edges.begin(), edges.end());
            std::vector<double> cols;
            for (double e : edges)
                if (cols.empty() || e - cols.back() > 10)
                    cols.push_back(e);
            Table t;
            t.rtl = rtl;
            for (size_t r = i; r < end; ++r) {
                std::vector<QString> row(cols.size());
                for (const Cell &c : rows[r]) {
                    const double e = rtl ? -c.box.right() : c.box.left();
                    size_t best = 0;
                    for (size_t q = 0; q < cols.size(); ++q)
                        if (std::abs(cols[q] - e) < std::abs(cols[best] - e))
                            best = q;
                    row[best] = row[best].isEmpty() ? c.text : row[best] + QLatin1Char(' ') + c.text;
                }
                while (!row.empty() && row.back().isEmpty())
                    row.pop_back();
                pageTable.rows.push_back(row);
                t.rows.push_back(row);
            }
            if (t.rows.size() >= 2) {
                t.name = QObject::tr("Table %1").arg(byTable.size() + 1);
                byTable.push_back(t);
            }
            i = end;
        }
        whole.rows.insert(whole.rows.end(), pageTable.rows.begin(), pageTable.rows.end());
        whole.rows.emplace_back();  // (a row apart between pages)
        byPage.push_back(pageTable);
    }
    whole.rtl = arabic > latin;
    if (o.sheets == 2)
        return byPage;
    if (o.sheets == 1 && !byTable.empty())
        return byTable;
    return {whole};  // (one sheet - or no tables found: the whole PDF on one)
}

// a cell's number (with the separators chosen; Arabic's digits too) - none: text
bool numberOf(QString s, const Options &o, double *v)
{
    s = s.trimmed();
    if (s.isEmpty() || s.size() > 24)
        return false;
    for (QChar &c : s) {
        if (c.unicode() >= 0x0660 && c.unicode() <= 0x0669)
            c = QChar(u'0' + (c.unicode() - 0x0660));
        else if (c.unicode() >= 0x06F0 && c.unicode() <= 0x06F9)
            c = QChar(u'0' + (c.unicode() - 0x06F0));
        else if (c.unicode() == 0x066B)
            c = QLatin1Char('.');  // (Arabic's decimal sign)
        else if (c.unicode() == 0x066C)
            c = QLatin1Char(',');  // (... and its thousands one)
    }
    QChar dec = o.decimal, grp = o.thousands;
    if (o.regionalNumbers) {
        const QLocale here = QLocale::system();
        dec = here.decimalPoint().isEmpty() ? QLatin1Char('.') : here.decimalPoint().front();
        grp = here.groupSeparator().isEmpty() ? QLatin1Char(',') : here.groupSeparator().front();
        if (dec.unicode() == 0x066B)
            dec = QLatin1Char('.');
        if (grp.unicode() == 0x066C)
            grp = QLatin1Char(',');
    }
    static const QRegularExpression shape(QStringLiteral("^[-+]?[0-9.,\\s\\x{00A0}'’]+%?$"));
    if (!shape.match(s).hasMatch())
        return false;
    const bool percent = s.endsWith(QLatin1Char('%'));
    if (percent)
        s.chop(1);
    s.remove(grp);
    s.remove(QChar(0x00A0));
    s.remove(QLatin1Char(' '));
    s.replace(dec, QLatin1Char('.'));
    bool ok = false;
    const double d = s.toDouble(&ok);
    if (ok)
        *v = percent ? d / 100 : d;
    return ok;
}

QString sheetName(QString n, int i)
{
    static const QRegularExpression bad(QStringLiteral("[\\[\\]:*?/\\\\]"));
    n.remove(bad);
    n = n.left(28);
    return n.isEmpty() ? QStringLiteral("Sheet%1").arg(i) : n;
}

QString colName(int c)
{
    QString s;
    for (++c; c > 0; c = (c - 1) / 26)
        s.prepend(QChar(u'A' + (c - 1) % 26));
    return s;
}

QStringList xlsxWrite(const std::vector<Table> &tables, const QString &path, const Options &o, QString *error)
{
    Zip zip(path);
    if (!zip.ok()) {
        if (error)
            *error = zip.error();
        return {};
    }
    QString sheetsXml, wbRels, overrides;
    QStringList used;
    for (size_t t = 0; t < tables.size(); ++t) {
        const Table &tab = tables[t];
        QString name = sheetName(tab.name, int(t) + 1);
        for (int k = 2; used.contains(name, Qt::CaseInsensitive); ++k)
            name = sheetName(tab.name, int(t) + 1).left(25) + QStringLiteral(" (%1)").arg(k);
        used << name;
        QString data;
        std::vector<int> widths;
        for (size_t r = 0; r < tab.rows.size(); ++r) {
            QString cells;
            for (size_t c = 0; c < tab.rows[r].size(); ++c) {
                const QString &v = tab.rows[r][c];
                if (v.isEmpty())
                    continue;
                if (widths.size() <= c)
                    widths.resize(c + 1, 8);
                widths[c] = std::min(60, std::max(widths[c], int(v.size()) + 2));
                const QString ref = colName(int(c)) + QString::number(r + 1);
                double num = 0;
                if (numberOf(v, o, &num))
                    cells += QStringLiteral("<c r=\"%1\"><v>%2</v></c>").arg(ref).arg(num, 0, 'g', 15);
                else
                    cells += QStringLiteral("<c r=\"%1\" t=\"inlineStr\"><is><t xml:space=\"preserve\">%2</t></is></c>").arg(ref, esc(v));
            }
            if (!cells.isEmpty())
                data += QStringLiteral("<row r=\"%1\">%2</row>").arg(r + 1).arg(cells);
        }
        QString cols;
        for (size_t c = 0; c < widths.size(); ++c)
            cols += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>").arg(c + 1).arg(widths[c]);
        const QString sheet = QString::fromLatin1(kXml) +
                              QStringLiteral("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                                             "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                                             "<sheetViews><sheetView workbookViewId=\"0\"%1/></sheetViews>%2<sheetData>%3</sheetData></worksheet>")
                                  .arg(tab.rtl ? QStringLiteral(" rightToLeft=\"1\"") : QString(), cols.isEmpty() ? QString() : QStringLiteral("<cols>%1</cols>").arg(cols), data);
        zip.add(QStringLiteral("xl/worksheets/sheet%1.xml").arg(t + 1), sheet.toUtf8());
        sheetsXml += QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>").arg(esc(name)).arg(t + 1);
        wbRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%1.xml\"/>").arg(t + 1);
        overrides += QStringLiteral("<Override PartName=\"/xl/worksheets/sheet%1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>").arg(t + 1);
    }
    zip.add(QStringLiteral("[Content_Types].xml"),
            (QString::fromLatin1(kXml) +
             QStringLiteral("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                            "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>%1</Types>")
                 .arg(overrides))
                .toUtf8());
    zip.add(QStringLiteral("_rels/.rels"),
            (QString::fromLatin1(kXml) + QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                                                       "</Relationships>"))
                .toUtf8());
    zip.add(QStringLiteral("xl/workbook.xml"),
            (QString::fromLatin1(kXml) + QStringLiteral("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                                                        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>%1</sheets></workbook>")
                                             .arg(sheetsXml))
                .toUtf8());
    zip.add(QStringLiteral("xl/_rels/workbook.xml.rels"),
            (QString::fromLatin1(kXml) + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">%1"
                                                        "<Relationship Id=\"rIdStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                                                        "</Relationships>")
                                             .arg(wbRels))
                .toUtf8());
    zip.add(QStringLiteral("xl/styles.xml"),
            (QString::fromLatin1(kXml) +
             QLatin1String("<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                           "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Arial\"/></font></fonts>"
                           "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
                           "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                           "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                           "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
                           "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>"))
                .toUtf8());
    if (!zip.close()) {
        if (error)
            *error = zip.error();
        return {};
    }
    return {path};
}

QStringList xmlSpreadsheetWrite(const std::vector<Table> &tables, const QString &path, const Options &o, QString *error)
{
    QString x = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<?mso-application progid=\"Excel.Sheet\"?>\n"
                               "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
                               "xmlns:x=\"urn:schemas-microsoft-com:office:excel\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n");
    QStringList used;
    for (size_t t = 0; t < tables.size(); ++t) {
        const Table &tab = tables[t];
        QString name = sheetName(tab.name, int(t) + 1);
        for (int k = 2; used.contains(name, Qt::CaseInsensitive); ++k)
            name = sheetName(tab.name, int(t) + 1).left(25) + QStringLiteral(" (%1)").arg(k);
        used << name;
        x += QStringLiteral(" <Worksheet ss:Name=\"%1\"%2>\n  <Table>\n").arg(esc(name), tab.rtl ? QStringLiteral(" ss:RightToLeft=\"1\"") : QString());
        for (const auto &row : tab.rows) {
            x += QLatin1String("   <Row>");
            for (const QString &v : row) {
                double num = 0;
                if (v.isEmpty())
                    x += QLatin1String("<Cell/>");
                else if (numberOf(v, o, &num))
                    x += QStringLiteral("<Cell><Data ss:Type=\"Number\">%1</Data></Cell>").arg(num, 0, 'g', 15);
                else
                    x += QStringLiteral("<Cell><Data ss:Type=\"String\">%1</Data></Cell>").arg(esc(v));
            }
            x += QLatin1String("</Row>\n");
        }
        x += QLatin1String("  </Table>\n");
        if (tab.rtl)
            x += QLatin1String("  <WorksheetOptions xmlns=\"urn:schemas-microsoft-com:office:excel\"><DisplayRightToLeft/></WorksheetOptions>\n");
        x += QLatin1String(" </Worksheet>\n");
    }
    x += QLatin1String("</Workbook>\n");
    if (!writeFile(path, x.toUtf8(), error))
        return {};
    return {path};
}

// ---- PowerPoint (.pptx): a slide a page - the page drawn without its text behind, its lines typed over it ----

QStringList pptxWrite(Document *doc, const std::vector<Page> &pages, const QString &path, const Options &o, const Progress &pr, QString *error)
{
    Zip zip(path);
    if (!zip.ok()) {
        if (error)
            *error = zip.error();
        return {};
    }
    const QSizeF first = pages.empty() ? QSizeF(720, 540) : pages.front().size;
    const long long cx = std::clamp<long long>(emu(first.width()), 914400, 51206400), cy = std::clamp<long long>(emu(first.height()), 914400, 51206400);
    const double sx = double(cx) / emu(first.width()), sy = double(cy) / emu(first.height());
    const QString ns = QStringLiteral("xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                                      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                                      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"");
    const QString group = QStringLiteral("<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                                         "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");
    QString slideIds, presRels, overrides;
    for (size_t i = 0; i < pages.size(); ++i) {
        check(pr);
        pr.step(0.6 + 0.4 * double(i) / double(pages.size()), QObject::tr("Writing slide %1 of %2").arg(i + 1).arg(pages.size()));
        const Page &p = pages[i];
        // (behind: the page as drawn without its text - its pictures, its shapes, its colours)
        std::vector<QRectF> text;
        for (const TextLine &l : p.lines)
            text.push_back(l.rect.adjusted(-0.5, -0.5, 0.5, 0.5));
        const double scale = 150 / 72.0;
        const QSizeF px = p.size * scale;
        QImage back = p.read ? pagePicture(doc, p.number, 150)
                             : doc->renderHiding(p.number, scale, QRect(0, 0, int(px.width()), int(px.height())), text);
        if (p.read) {  // (a scanned page: its picture with the text read painted out - the paper's colour)
            QPainter paint(&back);
            for (const QRectF &r : text)
                paint.fillRect(QRectF(r.topLeft() * scale, r.size() * scale), QColor(pagetext::paperIn(back, QRectF(r.topLeft() * scale, r.size() * scale).toRect())));
        }
        zip.add(QStringLiteral("ppt/media/page%1.jpeg").arg(i + 1), jpegOf(back, 88), false);
        QString shapes;
        int id = 2;
        auto x = [&](double pt) { return std::llround(emu(pt) * sx); };
        auto y = [&](double pt) { return std::llround(emu(pt) * sy); };
        for (const TextLine &l : p.lines) {
            if (l.text.trimmed().isEmpty())
                continue;
            QString runs;
            for (const TextRun &r : l.runs) {
                const Span s = spanOf(r);
                if (s.text.isEmpty())
                    continue;
                runs += QStringLiteral("<a:r><a:rPr lang=\"%1\" sz=\"%2\" b=\"%3\" i=\"%4\" dirty=\"0\"><a:solidFill><a:srgbClr val=\"%5\"/></a:solidFill>"
                                       "<a:latin typeface=\"%6\"/><a:cs typeface=\"%6\"/></a:rPr><a:t>%7</a:t></a:r>")
                            .arg(arabicIn(s.text) ? QStringLiteral("ar-SA") : QStringLiteral("en-US"))
                            .arg(std::clamp(int(std::lround(s.size * 100)), 100, 400000))
                            .arg(s.bold ? 1 : 0)
                            .arg(s.italic ? 1 : 0)
                            .arg(hexColor(s.color))
                            .arg(esc(s.family))
                            .arg(esc(s.text));
            }
            const QRectF r = l.rect;
            shapes += QStringLiteral("<p:sp><p:nvSpPr><p:cNvPr id=\"%1\" name=\"Text %1\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
                                     "<p:spPr><a:xfrm><a:off x=\"%2\" y=\"%3\"/><a:ext cx=\"%4\" cy=\"%5\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr>"
                                     "<p:txBody><a:bodyPr wrap=\"none\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" anchor=\"t\"><a:noAutofit/></a:bodyPr><a:lstStyle/>"
                                     "<a:p><a:pPr algn=\"%6\"%7/>%8</a:p></p:txBody></p:sp>")
                          .arg(id++)
                          .arg(x(r.left()))
                          .arg(y(r.top()))
                          .arg(std::max<long long>(1, x(r.width() * 1.04 + 2)))
                          .arg(std::max<long long>(1, y(r.height())))
                          .arg(l.rtl ? QStringLiteral("r") : QStringLiteral("l"))
                          .arg(l.rtl ? QStringLiteral(" rtl=\"1\"") : QString())
                          .arg(runs);
        }
        if (o.comments)  // (the PDF's comments: yellow notes where they are)
            for (const Note &n : p.notes)
                shapes += QStringLiteral("<p:sp><p:nvSpPr><p:cNvPr id=\"%1\" name=\"Comment %1\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
                                         "<p:spPr><a:xfrm><a:off x=\"%2\" y=\"%3\"/><a:ext cx=\"%4\" cy=\"%5\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
                                         "<a:solidFill><a:srgbClr val=\"FFF8A0\"/></a:solidFill><a:ln w=\"6350\"><a:solidFill><a:srgbClr val=\"C0A000\"/></a:solidFill></a:ln></p:spPr>"
                                         "<p:txBody><a:bodyPr wrap=\"square\" lIns=\"45720\" tIns=\"45720\" rIns=\"45720\" bIns=\"45720\"><a:spAutoFit/></a:bodyPr><a:lstStyle/>"
                                         "<a:p><a:pPr%6/><a:r><a:rPr lang=\"en-US\" sz=\"1000\" dirty=\"0\"/><a:t>%7</a:t></a:r></a:p></p:txBody></p:sp>")
                              .arg(id++)
                              .arg(x(n.box.left()))
                              .arg(y(n.box.top()))
                              .arg(x(160))
                              .arg(y(40))
                              .arg(arabicIn(n.text) ? QStringLiteral(" algn=\"r\" rtl=\"1\"") : QString())
                              .arg(esc(n.text));
        const QString slide = QString::fromLatin1(kXml) +
                              QStringLiteral("<p:sld %1><p:cSld><p:bg><p:bgPr><a:blipFill dpi=\"0\" rotWithShape=\"1\"><a:blip r:embed=\"rIdBack\"/><a:srcRect/>"
                                             "<a:stretch><a:fillRect/></a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg><p:spTree>%2%3</p:spTree></p:cSld>"
                                             "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>")
                                  .arg(ns, group, shapes);
        zip.add(QStringLiteral("ppt/slides/slide%1.xml").arg(i + 1), slide.toUtf8());
        zip.add(QStringLiteral("ppt/slides/_rels/slide%1.xml.rels").arg(i + 1),
                (QString::fromLatin1(kXml) + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
                                                            "<Relationship Id=\"rIdBack\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/page%1.jpeg\"/>"
                                                            "</Relationships>")
                                                 .arg(i + 1))
                    .toUtf8());
        slideIds += QStringLiteral("<p:sldId id=\"%1\" r:id=\"rIdSlide%2\"/>").arg(256 + i).arg(i + 1);
        presRels += QStringLiteral("<Relationship Id=\"rIdSlide%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide%1.xml\"/>").arg(i + 1);
        overrides += QStringLiteral("<Override PartName=\"/ppt/slides/slide%1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>").arg(i + 1);
    }
    zip.add(QStringLiteral("[Content_Types].xml"),
            (QString::fromLatin1(kXml) +
             QStringLiteral("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                            "<Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
                            "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
                            "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
                            "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
                            "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>%1</Types>")
                 .arg(overrides))
                .toUtf8());
    zip.add(QStringLiteral("_rels/.rels"),
            (QString::fromLatin1(kXml) + QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
                                                       "</Relationships>"))
                .toUtf8());
    zip.add(QStringLiteral("ppt/presentation.xml"),
            (QString::fromLatin1(kXml) + QStringLiteral("<p:presentation %1 saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rIdMaster\"/></p:sldMasterIdLst>"
                                                        "<p:sldIdLst>%2</p:sldIdLst><p:sldSz cx=\"%3\" cy=\"%4\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>")
                                             .arg(ns, slideIds)
                                             .arg(cx)
                                             .arg(cy))
                .toUtf8());
    zip.add(QStringLiteral("ppt/_rels/presentation.xml.rels"),
            (QString::fromLatin1(kXml) + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                        "<Relationship Id=\"rIdMaster\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
                                                        "<Relationship Id=\"rIdTheme\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>%1</Relationships>")
                                             .arg(presRels))
                .toUtf8());
    zip.add(QStringLiteral("ppt/slideMasters/slideMaster1.xml"),
            (QString::fromLatin1(kXml) + QStringLiteral("<p:sldMaster %1><p:cSld><p:spTree>%2</p:spTree></p:cSld>"
                                                        "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
                                                        "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
                                                        "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst></p:sldMaster>")
                                             .arg(ns, group))
                .toUtf8());
    zip.add(QStringLiteral("ppt/slideMasters/_rels/slideMaster1.xml.rels"),
            (QString::fromLatin1(kXml) + QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
                                                       "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/>"
                                                       "</Relationships>"))
                .toUtf8());
    zip.add(QStringLiteral("ppt/slideLayouts/slideLayout1.xml"),
            (QString::fromLatin1(kXml) + QStringLiteral("<p:sldLayout %1 type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree>%2</p:spTree></p:cSld>"
                                                        "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>")
                                             .arg(ns, group))
                .toUtf8());
    zip.add(QStringLiteral("ppt/slideLayouts/_rels/slideLayout1.xml.rels"),
            (QString::fromLatin1(kXml) + QLatin1String("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
                                                       "</Relationships>"))
                .toUtf8());
    // (the theme every presentation has: Office's colours and fonts)
    QString clr;
    const std::pair<const char *, const char *> scheme[] = {{"dk1", "000000"}, {"lt1", "FFFFFF"}, {"dk2", "1F497D"}, {"lt2", "EEECE1"}, {"accent1", "4F81BD"}, {"accent2", "C0504D"},
                                                            {"accent3", "9BBB59"}, {"accent4", "8064A2"}, {"accent5", "4BACC6"}, {"accent6", "F79646"}, {"hlink", "0000FF"}, {"folHlink", "800080"}};
    for (const auto &[n, v] : scheme)
        clr += QStringLiteral("<a:%1><a:srgbClr val=\"%2\"/></a:%1>").arg(QLatin1String(n), QLatin1String(v));
    const QString fill = QStringLiteral("<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>");
    const QString line = QStringLiteral("<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>");
    zip.add(QStringLiteral("ppt/theme/theme1.xml"),
            (QString::fromLatin1(kXml) +
             QStringLiteral("<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office Theme\"><a:themeElements>"
                            "<a:clrScheme name=\"Office\">%1</a:clrScheme>"
                            "<a:fontScheme name=\"Office\"><a:majorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
                            "<a:minorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
                            "<a:fmtScheme name=\"Office\"><a:fillStyleLst>%2%2%2</a:fillStyleLst><a:lnStyleLst>%3%3%3</a:lnStyleLst>"
                            "<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>"
                            "<a:bgFillStyleLst>%2%2%2</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>")
                 .arg(clr, fill, line))
                .toUtf8());
    if (!zip.close()) {
        if (error)
            *error = zip.error();
        return {};
    }
    return {path};
}

// ---- pictures: a page a picture, or the PDF's own pictures ----

QString numbered(const QString &path, const QString &what, int n, const QString &ext)
{
    const QFileInfo f(path);
    return f.dir().filePath(f.completeBaseName() + QStringLiteral("_%1_%2.").arg(what).arg(n) + ext);
}

QByteArray imageBytes(const QImage &im, Format f, const Options &o)
{
    switch (f) {
    case Format::Jpeg:
        return jpegOf(im.format() == QImage::Format_Mono ? im.convertToFormat(QImage::Format_Grayscale8) : im, o.quality, o.progressive);
    case Format::Jpeg2000:
        return jp2Of(im, o.quality);
    case Format::Tiff:
        return tiffOf(im, o.tiffCompression, o.dpi);
    default:
        return pngOf(im);
    }
}

QStringList imagesWrite(Document *doc, Format f, const QString &path, const Options &o, const Progress &pr, QString *error)
{
    const QString ext = extension(f);
    QStringList written;
    const std::vector<int> which = pagesOf(doc, o);
    if (o.allImages) {  // (the PDF's own pictures, each as it is)
        int n = 0;
        for (size_t k = 0; k < which.size(); ++k) {
            check(pr);
            pr.step(double(k) / double(which.size()), QObject::tr("Page %1 of %2").arg(k + 1).arg(which.size()));
            for (const Picture &pic : doc->pictures(which[k])) {
                if (pic.vector)
                    continue;
                const QImage im = doc->pictureImage(pic);
                if (im.isNull())
                    continue;
                const QString file = numbered(path, QStringLiteral("Image"), ++n, ext);
                if (!writeFile(file, imageBytes(coloured(im, o.colour), f, o), error))
                    return {};
                written << file;
            }
        }
        if (written.isEmpty() && error)
            *error = QObject::tr("This PDF has no pictures of its own to save.");
        return written;
    }
    if (f == Format::Tiff && o.tiffOneFile) {  // (every page in one TIFF: leptonica adds them one by one)
        QTemporaryDir tmp;
        const QString file = tmp.filePath(QStringLiteral("pages.tif"));
        for (size_t k = 0; k < which.size(); ++k) {
            check(pr);
            pr.step(double(k) / double(which.size()), QObject::tr("Page %1 of %2").arg(k + 1).arg(which.size()));
            PIX *pix = pixOf(coloured(pagePicture(doc, which[k], o.dpi), o.colour));
            pixSetResolution(pix, o.dpi, o.dpi);
            const int comp = pixGetDepth(pix) == 1 ? (o.tiffCompression == 0 ? IFF_TIFF : IFF_TIFF_G4)
                                                   : o.tiffCompression == 0 ? IFF_TIFF : o.tiffCompression == 2 ? IFF_TIFF_ZIP : IFF_TIFF_LZW;
            pixWriteTiff(QDir::toNativeSeparators(file).toLocal8Bit().constData(), pix, comp, k == 0 ? "w" : "a");
            pixDestroy(&pix);
        }
        QFile::remove(path);
        if (!QFile::copy(file, path)) {
            if (error)
                *error = QObject::tr("%1 couldn't be written.").arg(QDir::toNativeSeparators(path));
            return {};
        }
        return {path};
    }
    for (size_t k = 0; k < which.size(); ++k) {
        check(pr);
        pr.step(double(k) / double(which.size()), QObject::tr("Page %1 of %2").arg(k + 1).arg(which.size()));
        const QImage im = coloured(pagePicture(doc, which[k], o.dpi), o.colour);
        QImage withDpi = im;
        withDpi.setDotsPerMeterX(int(o.dpi / 0.0254));
        withDpi.setDotsPerMeterY(int(o.dpi / 0.0254));
        const QString file = which.size() == 1 ? path : numbered(path, QStringLiteral("Page"), which[k] + 1, ext);
        if (!writeFile(file, imageBytes(withDpi, f, o), error))
            return {};
        written << file;
    }
    return written;
}

// ---- web pages ----

const char *kCss = "body{font-family:Arial,Helvetica,sans-serif;line-height:1.45;color:#222;margin:0;background:#fff}"
                   "main{max-width:820px;margin:0 auto;padding:24px 32px}"
                   "h1{font-size:1.9em;margin:.8em 0 .4em}h2{font-size:1.45em;margin:.8em 0 .4em}h3{font-size:1.15em;margin:.8em 0 .3em}"
                   "p{margin:0 0 .7em}img{max-width:100%;height:auto;display:block;margin:.6em 0}"
                   ".page{border-top:1px solid #ccc;margin-top:1.4em;padding-top:.4em;color:#888;font-size:.8em}"
                   "nav{position:fixed;top:0;bottom:0;width:240px;overflow:auto;background:#f4f4f4;border-right:1px solid #ccc;padding:12px;box-sizing:border-box;font-size:.9em}"
                   "nav a{display:block;color:#036;text-decoration:none;margin:.2em 0}nav a:hover{text-decoration:underline}"
                   "nav{left:0}nav .l2{margin-left:1em}nav .l3{margin-left:2em}body.nav main{margin-left:260px}"
                   "html[dir=rtl] nav{left:auto;right:0;border-right:none;border-left:1px solid #ccc}"
                   "html[dir=rtl] nav .l2{margin-left:0;margin-right:1em}html[dir=rtl] nav .l3{margin-left:0;margin-right:2em}"
                   "html[dir=rtl] body.nav main{margin-left:auto;margin-right:260px}"
                   ".pager{display:flex;justify-content:space-between;margin:1.5em 0;font-size:.9em}.note{background:#fff8c0;border:1px solid #d0b000;padding:4px 8px;font-size:.85em}";

QString spanHtml(const Span &s)
{
    QString style;
    if (s.bold)
        style += QLatin1String("font-weight:bold;");
    if (s.italic)
        style += QLatin1String("font-style:italic;");
    if (s.color != Qt::black)
        style += QStringLiteral("color:#%1;").arg(hexColor(s.color));
    return style.isEmpty() ? esc(s.text) : QStringLiteral("<span style=\"%1\">%2</span>").arg(style, esc(s.text));
}

struct HtmlOut {
    QString body;
    QString file;  // (which file it's in - multiple pages)
};

QStringList htmlWrite(Format f, const std::vector<Page> &pages, const std::vector<Bookmark> &marks, const QString &path, const Options &o, QString *error)
{
    const QFileInfo info(path);
    const QString base = info.completeBaseName();
    const QString folderName = base + QStringLiteral("_files");
    QDir dir = info.dir();
    QStringList written;
    int pictures = 0;
    auto picture = [&](const QImage &im) -> QString {
        dir.mkpath(folderName);
        const QString name = QStringLiteral("image%1.png").arg(++pictures);
        const QString file = dir.filePath(folderName + QLatin1Char('/') + name);
        if (writeFile(file, pngOf(im), error))
            written << file;
        return folderName + QLatin1Char('/') + name;
    };
    // the pieces: each page's items (headings with an id - linked to)
    struct Piece {
        QString html;
        int heading = 0;
        QString id, title;
        int page = 0;
    };
    std::vector<Piece> pieces;
    int headingNo = 0;
    for (const Page &p : pages) {
        pieces.push_back({QStringLiteral("<a id=\"page-%1\"></a>").arg(p.number + 1), 0, QString(), QString(), p.number});
        for (const Item &it : p.items) {
            if (it.picture) {
                if (o.images)
                    pieces.push_back({QStringLiteral("<img src=\"%1\" alt=\"\">").arg(picture(it.pic.image)), 0, QString(), QString(), p.number});
                continue;
            }
            const Para &para = it.para;
            QString inner;
            for (const Span &s : para.spans)
                inner += spanHtml(s);
            const QString dirAttr = para.rtl ? QStringLiteral(" dir=\"rtl\"") : QString();
            const QString alignAttr = para.align == QLatin1String("center")    ? QStringLiteral(" style=\"text-align:center\"")
                                      : para.align == QLatin1String("justify") ? QStringLiteral(" style=\"text-align:justify\"")
                                                                               : QString();
            Piece piece;
            piece.page = p.number;
            if (para.heading) {
                piece.heading = para.heading;
                piece.id = QStringLiteral("h-%1").arg(++headingNo);
                piece.title = para.text().simplified();
                piece.html = QStringLiteral("<h%1 id=\"%2\"%3%4>%5</h%1>").arg(para.heading).arg(piece.id, dirAttr, alignAttr, inner);
            } else {
                piece.html = QStringLiteral("<p%1%2>%3</p>").arg(dirAttr, alignAttr, inner);
            }
            pieces.push_back(piece);
        }
        if (o.comments)
            for (const Note &n : p.notes)
                pieces.push_back({QStringLiteral("<p class=\"note\"%1>%2</p>").arg(arabicIn(n.text) ? QStringLiteral(" dir=\"rtl\"") : QString(), esc(n.text)), 0, QString(), QString(), p.number});
    }
    bool rtlDoc = false;
    {
        int a = 0, l = 0;
        for (const Page &p : pages)
            for (const Item &it : p.items)
                if (!it.picture)
                    (it.para.rtl ? a : l) += 1;
        rtlDoc = a > l;
    }
    auto page = [&](const QString &title, const QString &nav, const QString &main) {
        return QStringLiteral("<!DOCTYPE html>\n<html lang=\"%1\"%2>\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                              "<title>%3</title>\n<style>%4</style>\n</head>\n<body%5>\n%6<main>\n%7</main>\n</body>\n</html>\n")
            .arg(rtlDoc ? QStringLiteral("ar") : QStringLiteral("en"), rtlDoc ? QStringLiteral(" dir=\"rtl\"") : QString(), esc(title), QLatin1String(kCss),
                 nav.isEmpty() ? QString() : QStringLiteral(" class=\"nav\""), nav, main);
    };
    if (f == Format::HtmlSingle) {
        QString nav;
        if (o.htmlNav == 1) {
            for (const Piece &pc : pieces)
                if (pc.heading)
                    nav += QStringLiteral("<a class=\"l%1\" href=\"#%2\">%3</a>\n").arg(pc.heading).arg(pc.id, esc(pc.title));
        } else if (o.htmlNav == 2) {
            for (const Bookmark &b : marks)
                if (b.page >= 0)
                    nav += QStringLiteral("<a class=\"l%1\" href=\"#page-%2\">%3</a>\n").arg(std::min(3, b.level + 1)).arg(b.page + 1).arg(esc(b.title));
        }
        if (!nav.isEmpty())
            nav = QStringLiteral("<nav>\n%1</nav>\n").arg(nav);
        QString main;
        for (const Piece &pc : pieces)
            main += pc.html + QLatin1Char('\n');
        if (!writeFile(path, page(base, nav, main).toUtf8(), error))
            return {};
        written.prepend(path);
        return written;
    }
    // several pages: split where each heading of the top level starts - or each top-level bookmark's page
    std::vector<size_t> starts{0};
    std::vector<QString> titles{base};
    if (o.htmlSplit == 1 && !marks.empty()) {
        for (const Bookmark &b : marks)
            if (b.level == 0 && b.page >= 0)
                for (size_t k = 0; k < pieces.size(); ++k)
                    if (pieces[k].page >= b.page) {
                        if (k > starts.back())
                            starts.push_back(k), titles.push_back(b.title);
                        else
                            titles.back() = b.title;
                        break;
                    }
    } else {
        int top = 9;
        for (const Piece &pc : pieces)
            if (pc.heading)
                top = std::min(top, pc.heading);
        for (size_t k = 0; k < pieces.size(); ++k)
            if (pieces[k].heading == top) {
                if (k > starts.back())
                    starts.push_back(k), titles.push_back(pieces[k].title);
                else
                    titles.back() = pieces[k].title;
            }
    }
    const int parts = int(starts.size());
    auto fileOf = [&](int i) { return QStringLiteral("%1_%2.html").arg(base).arg(i + 1); };
    QString contents;
    for (int i = 0; i < parts; ++i)
        contents += QStringLiteral("<p><a href=\"%1\">%2</a></p>\n").arg(fileOf(i), esc(titles[size_t(i)].isEmpty() ? QObject::tr("Part %1").arg(i + 1) : titles[size_t(i)]));
    if (!writeFile(path, page(base, QString(), QStringLiteral("<h1>%1</h1>\n%2").arg(esc(base), contents)).toUtf8(), error))
        return {};
    written.prepend(path);
    for (int i = 0; i < parts; ++i) {
        const size_t from = starts[size_t(i)], to = i + 1 < parts ? starts[size_t(i) + 1] : pieces.size();
        QString main;
        for (size_t k = from; k < to; ++k)
            main += pieces[k].html + QLatin1Char('\n');
        QString pager = QLatin1String("<div class=\"pager\">");
        pager += i > 0 ? QStringLiteral("<a href=\"%1\">&larr; %2</a>").arg(fileOf(i - 1), QObject::tr("Previous")) : QStringLiteral("<span></span>");
        pager += QStringLiteral("<a href=\"%1\">%2</a>").arg(info.fileName(), QObject::tr("Contents"));
        pager += i + 1 < parts ? QStringLiteral("<a href=\"%1\">%2 &rarr;</a>").arg(fileOf(i + 1), QObject::tr("Next")) : QStringLiteral("<span></span>");
        pager += QLatin1String("</div>\n");
        const QString file = dir.filePath(fileOf(i));
        if (!writeFile(file, page(titles[size_t(i)], QString(), pager + main + pager).toUtf8(), error))
            return {};
        written << file;
    }
    return written;
}

// ---- text ----

QStringList textWrite(Format f, const std::vector<Page> &pages, const QString &path, const Options &o, QString *error)
{
    QString out;
    const QString nl = QStringLiteral("\r\n");
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const Page &p = pages[pi];
        if (f == Format::TextAccessible)
            out += QObject::tr("Page %1").arg(p.number + 1) + nl + nl;
        for (const Item &it : p.items) {
            if (it.picture) {
                if (f == Format::TextAccessible)
                    out += QObject::tr("[Picture]") + nl + nl;
                continue;
            }
            const QString t = it.para.text().simplified();
            if (f == Format::TextAccessible && it.para.heading)
                out += QObject::tr("Heading %1: %2").arg(it.para.heading).arg(t) + nl + nl;
            else
                out += t + nl + (f == Format::TextAccessible ? nl : QString());
        }
        if (f == Format::TextAccessible)
            for (const Note &n : p.notes)
                out += QObject::tr("[Comment: %1]").arg(n.text) + nl + nl;
        if (f == Format::TextPlain && pi + 1 < pages.size())
            out += nl;
    }
    if (!writeFile(path, encoded(out, o.encoding), error))
        return {};
    return {path};
}

// ---- XML 1.0 (as Acrobat's: its paragraphs tagged - headings, paragraphs, figures - and its bookmarks) ----

QString xmlText(const QString &s, bool ascii)
{
    const QString e = esc(s);
    if (!ascii)
        return e;
    QString out;
    for (const QChar c : e)
        out += c.unicode() < 0x80 ? QString(c) : QStringLiteral("&#%1;").arg(c.unicode());
    return out;
}

QStringList xmlWrite(const std::vector<Page> &pages, const std::vector<Bookmark> &marks, const QString &path, const Options &o, QString *error)
{
    const bool utf = o.encoding.startsWith(QLatin1String("UTF"), Qt::CaseInsensitive);
    const QFileInfo info(path);
    QDir dir = info.dir();
    const QString folder = o.subfolder && !o.folder.trimmed().isEmpty() ? o.folder.trimmed() : QString();
    QStringList written;
    int pictures = 0;
    QString x = QStringLiteral("<?xml version=\"1.0\" encoding=\"%1\" ?>\n<TaggedPDF-doc>\n").arg(utf ? o.encoding.section(QLatin1Char(' '), 0, 0) : QStringLiteral("US-ASCII"));
    if (o.bookmarks && !marks.empty()) {
        x += QLatin1String("<bookmark-tree>\n");
        int depth = 0;
        for (size_t i = 0; i < marks.size(); ++i) {
            const Bookmark &b = marks[i];
            while (depth > b.level)
                x += QLatin1String("</bookmark>\n"), --depth;
            x += QStringLiteral("<bookmark name=\"%1\"><destination structID=\"page-%2\"/>").arg(xmlText(b.title, !utf)).arg(b.page + 1);
            const bool children = i + 1 < marks.size() && marks[i + 1].level > b.level;
            if (children)
                ++depth, x += QLatin1Char('\n');
            else
                x += QLatin1String("</bookmark>\n");
        }
        while (depth-- > 0)
            x += QLatin1String("</bookmark>\n");
        x += QLatin1String("</bookmark-tree>\n");
    }
    x += QLatin1String("<Part>\n");
    for (const Page &p : pages) {
        x += QStringLiteral("<Sect id=\"page-%1\">\n").arg(p.number + 1);
        for (const Item &it : p.items) {
            if (it.picture) {
                if (!o.xmlImages) {
                    x += QLatin1String("<Figure/>\n");
                    continue;
                }
                QImage im = it.pic.image;
                if (o.downsample > 0) {  // (no finer than the dots an inch chosen, where it's shown)
                    const double shown = std::max(1.0, it.pic.box.width() / 72.0);
                    const int most = int(shown * o.downsample);
                    if (im.width() > most)
                        im = im.scaledToWidth(most, Qt::SmoothTransformation);
                }
                const QString ext = o.imageFormat == QLatin1String("png") ? QStringLiteral("png") : o.imageFormat == QLatin1String("tif") ? QStringLiteral("tif") : QStringLiteral("jpg");
                const QString name = QStringLiteral("%1%2_img_%3.%4").arg(o.prefix, info.completeBaseName()).arg(++pictures).arg(ext);
                const QString rel = folder.isEmpty() ? name : folder + QLatin1Char('/') + name;
                if (!folder.isEmpty())
                    dir.mkpath(folder);
                const QByteArray bytes = ext == QLatin1String("png") ? pngOf(im) : ext == QLatin1String("tif") ? tiffOf(im, 1, 150) : jpegOf(im, 85);
                if (writeFile(dir.filePath(rel), bytes, error))
                    written << dir.filePath(rel);
                x += QStringLiteral("<Figure><ImageData src=\"%1\"/></Figure>\n").arg(xmlText(rel, !utf));
                continue;
            }
            const QString tag = it.para.heading ? QStringLiteral("H%1").arg(it.para.heading) : QStringLiteral("P");
            x += QStringLiteral("<%1%2>%3</%1>\n").arg(tag, it.para.rtl ? QStringLiteral(" dir=\"rtl\"") : QString(), xmlText(it.para.text().simplified(), !utf));
        }
        x += QLatin1String("</Sect>\n");
    }
    x += QLatin1String("</Part>\n</TaggedPDF-doc>\n");
    if (!writeFile(path, utf ? encoded(x, o.encoding, false) : x.toLatin1(), error))
        return {};
    written.prepend(path);
    return written;
}

// ---- PostScript, Encapsulated PostScript: each page a picture (JPEG, DCT-coded - PostScript Level 2) ----

QByteArray ascii85(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size() * 5 / 4 + in.size() / 64 + 8);
    int col = 0;
    auto put = [&](char c) {
        out += c;
        if (++col == 75)
            out += '\n', col = 0;
    };
    for (qsizetype i = 0; i < in.size(); i += 4) {
        const int n = int(std::min<qsizetype>(4, in.size() - i));
        quint32 v = 0;
        for (int k = 0; k < 4; ++k)
            v = (v << 8) | (k < n ? quint8(in[i + k]) : 0);
        if (n == 4 && v == 0) {
            put('z');
            continue;
        }
        char c[5];
        for (int k = 4; k >= 0; --k)
            c[k] = char('!' + v % 85), v /= 85;
        for (int k = 0; k < n + 1; ++k)
            put(c[k]);
    }
    out += "~>\n";
    return out;
}

QByteArray psPage(const QImage &im, double w, double h, bool binary, bool grey)
{
    const QByteArray jpeg = jpegOf(grey ? im.convertToFormat(QImage::Format_Grayscale8) : im.convertToFormat(QImage::Format_RGB32), 90);
    QByteArray s;
    s += QStringLiteral("gsave\n%1 %2 scale\n/Device%3 setcolorspace\n<< /ImageType 1 /Width %4 /Height %5 /BitsPerComponent 8 /Decode [%6] "
                        "/ImageMatrix [%4 0 0 -%5 0 %5] /DataSource currentfile%7 /DCTDecode filter >> image\n")
             .arg(w, 0, 'f', 2)
             .arg(h, 0, 'f', 2)
             .arg(grey ? QStringLiteral("Gray") : QStringLiteral("RGB"))
             .arg(im.width())
             .arg(im.height())
             .arg(grey ? QStringLiteral("0 1") : QStringLiteral("0 1 0 1 0 1"))
             .arg(binary ? QString() : QStringLiteral(" /ASCII85Decode filter"))
             .toLatin1();
    s += binary ? jpeg + "\n" : ascii85(jpeg);
    s += "grestore\n";
    return s;
}

QStringList psWrite(Document *doc, Format f, const QString &path, const Options &o, const Progress &pr, QString *error)
{
    const std::vector<int> which = pagesOf(doc, o);
    const bool grey = o.colour == 2 || o.colour == 3;
    const QString date = QDateTime::currentDateTime().toString(Qt::ISODate);
    if (f == Format::Eps) {  // (one file a page)
        QStringList written;
        for (size_t k = 0; k < which.size(); ++k) {
            check(pr);
            pr.step(double(k) / double(which.size()), QObject::tr("Page %1 of %2").arg(k + 1).arg(which.size()));
            const QSizeF size = doc->pageSize(which[k]);
            QByteArray s = QStringLiteral("%!PS-Adobe-3.0 EPSF-3.0\n%%Creator: Master PDF\n%%CreationDate: %1\n%%BoundingBox: 0 0 %2 %3\n"
                                          "%%HiResBoundingBox: 0 0 %4 %5\n%%LanguageLevel: 2\n%%Pages: 1\n%%EndComments\n%%Page: 1 1\n")
                               .arg(date)
                               .arg(int(std::ceil(size.width())))
                               .arg(int(std::ceil(size.height())))
                               .arg(size.width(), 0, 'f', 2)
                               .arg(size.height(), 0, 'f', 2)
                               .toLatin1();
            s += psPage(pagePicture(doc, which[k], o.dpi), size.width(), size.height(), o.binary, grey);
            s += "showpage\n%%Trailer\n%%EOF\n";
            const QString file = which.size() == 1 ? path : numbered(path, QStringLiteral("Page"), which[k] + 1, QStringLiteral("eps"));
            if (!writeFile(file, s, error))
                return {};
            written << file;
        }
        return written;
    }
    QByteArray s = QStringLiteral("%!PS-Adobe-3.0\n%%Creator: Master PDF\n%%CreationDate: %1\n%%LanguageLevel: 2\n%%Pages: %2\n%%EndComments\n")
                       .arg(date)
                       .arg(which.size())
                       .toLatin1();
    for (size_t k = 0; k < which.size(); ++k) {
        check(pr);
        pr.step(double(k) / double(which.size()), QObject::tr("Page %1 of %2").arg(k + 1).arg(which.size()));
        const QSizeF size = doc->pageSize(which[k]);
        s += QStringLiteral("%%Page: %1 %2\n%%PageBoundingBox: 0 0 %3 %4\n<< /PageSize [%5 %6] >> setpagedevice\n")
                 .arg(which[k] + 1)
                 .arg(k + 1)
                 .arg(int(std::ceil(size.width())))
                 .arg(int(std::ceil(size.height())))
                 .arg(size.width(), 0, 'f', 2)
                 .arg(size.height(), 0, 'f', 2)
                 .toLatin1();
        s += psPage(pagePicture(doc, which[k], o.dpi), size.width(), size.height(), o.binary, grey);
        s += "showpage\n";
    }
    s += "%%Trailer\n%%EOF\n";
    if (!writeFile(path, s, error))
        return {};
    return {path};
}

// ---- Word 97-2003: the .docx made, then saved again as .doc by Word itself (or LibreOffice) ----

bool docFromDocx(const QString &docx, const QString &doc, QString *error)
{
#ifdef Q_OS_WIN
    static const char *script = R"(
$ErrorActionPreference = 'Stop'
$word = New-Object -ComObject Word.Application
try {
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $d = $word.Documents.Open($env:MP_SRC, $false, $true, $false)
    $d.SaveAs2($env:MP_DST, 0)
    $d.Close(0)
} finally {
    $word.Quit()
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
}
)";
    QProcess word;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MP_SRC"), QDir::toNativeSeparators(docx));
    env.insert(QStringLiteral("MP_DST"), QDir::toNativeSeparators(doc));
    word.setProcessEnvironment(env);
    word.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                                                  QStringLiteral("-Command"), QString::fromLatin1(script)});
    word.waitForFinished(180000);
    if (QFileInfo::exists(doc))
        return true;
#endif
    for (const QString &soffice : {QStringLiteral("C:/Program Files/LibreOffice/program/soffice.exe"), QStringLiteral("C:/Program Files (x86)/LibreOffice/program/soffice.exe"),
                                   QStringLiteral("/Applications/LibreOffice.app/Contents/MacOS/soffice"), QStringLiteral("/usr/bin/soffice"),
                                   QStringLiteral("/usr/bin/libreoffice"), QStringLiteral("/usr/local/bin/soffice"), QStringLiteral("/snap/bin/libreoffice")}) {
        if (!QFileInfo::exists(soffice))
            continue;
        QTemporaryDir out;
        QProcess lo;
        lo.start(soffice, {QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("doc"), QStringLiteral("--outdir"), out.path(), docx});
        lo.waitForFinished(180000);
        const QString made = out.filePath(QFileInfo(docx).completeBaseName() + QStringLiteral(".doc"));
        if (QFileInfo::exists(made)) {
            QFile::remove(doc);
            if (QFile::copy(made, doc))
                return true;
        }
    }
    if (error)
        *error = QObject::tr("A Word 97-2003 document needs Microsoft Word or LibreOffice to be made - neither was found. "
                             "Word Document (.docx) can be made without them.");
    return false;
}

}  // namespace

QString extension(Format f)
{
    switch (f) {
    case Format::Docx: return QStringLiteral("docx");
    case Format::Doc: return QStringLiteral("doc");
    case Format::Xlsx: return QStringLiteral("xlsx");
    case Format::XmlSpreadsheet: return QStringLiteral("xml");
    case Format::Pptx: return QStringLiteral("pptx");
    case Format::Jpeg: return QStringLiteral("jpg");
    case Format::Jpeg2000: return QStringLiteral("jp2");
    case Format::Tiff: return QStringLiteral("tif");
    case Format::Png: return QStringLiteral("png");
    case Format::HtmlSingle:
    case Format::HtmlMulti: return QStringLiteral("html");
    case Format::Rtf: return QStringLiteral("rtf");
    case Format::Eps: return QStringLiteral("eps");
    case Format::Ps: return QStringLiteral("ps");
    case Format::TextAccessible:
    case Format::TextPlain: return QStringLiteral("txt");
    case Format::Xml: return QStringLiteral("xml");
    }
    return QStringLiteral("txt");
}

QStringList run(Document *doc, Format f, const QString &path, const Options &o, const Progress &pr, QString *error)
{
    try {
        QStringList out;
        switch (f) {
        case Format::Docx:
        case Format::Doc: {
            const std::vector<Page> pages = readAll(doc, o, pr, o.images, o.comments);
            check(pr);
            pr.step(0.7, QObject::tr("Writing the document"));
            if (f == Format::Docx)
                return docxWrite(pages, path, o, error);
            QTemporaryDir tmp;
            const QString docx = tmp.filePath(QFileInfo(path).completeBaseName() + QStringLiteral(".docx"));
            if (docxWrite(pages, docx, o, error).isEmpty())
                return {};
            pr.step(0.85, QObject::tr("Saving it as Word 97-2003"));
            if (!docFromDocx(docx, path, error))
                return {};
            return {path};
        }
        case Format::Rtf: {
            const std::vector<Page> pages = readAll(doc, o, pr, o.images, o.comments);
            pr.step(0.7, QObject::tr("Writing the document"));
            return rtfWrite(pages, path, o, error);
        }
        case Format::Xlsx:
        case Format::XmlSpreadsheet: {
            const std::vector<Table> tables = tablesOf(doc, pagesOf(doc, o), o, pr);
            check(pr);
            pr.step(0.8, QObject::tr("Writing the workbook"));
            return f == Format::Xlsx ? xlsxWrite(tables, path, o, error) : xmlSpreadsheetWrite(tables, path, o, error);
        }
        case Format::Pptx: {
            const std::vector<Page> pages = readAll(doc, o, pr, false, o.comments, 0.5);
            return pptxWrite(doc, pages, path, o, pr, error);
        }
        case Format::Jpeg:
        case Format::Jpeg2000:
        case Format::Tiff:
        case Format::Png:
            out = imagesWrite(doc, f, path, o, pr, error);
            return out;
        case Format::HtmlSingle:
        case Format::HtmlMulti: {
            const std::vector<Page> pages = readAll(doc, o, pr, o.images, o.comments);
            pr.step(0.8, QObject::tr("Writing the web pages"));
            return htmlWrite(f, pages, doc->bookmarks(), path, o, error);
        }
        case Format::TextAccessible:
        case Format::TextPlain: {
            const std::vector<Page> pages = readAll(doc, o, pr, false, f == Format::TextAccessible, 0.9);
            return textWrite(f, pages, path, o, error);
        }
        case Format::Xml: {
            const std::vector<Page> pages = readAll(doc, o, pr, o.xmlImages, false);
            pr.step(0.8, QObject::tr("Writing the XML"));
            return xmlWrite(pages, doc->bookmarks(), path, o, error);
        }
        case Format::Eps:
        case Format::Ps:
            return psWrite(doc, f, path, o, pr, error);
        }
    } catch (const Cancelled &) {
        if (error)
            error->clear();
        return {};
    }
    return {};
}

}  // namespace convert
