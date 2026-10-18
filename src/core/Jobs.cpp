#include "core/Jobs.h"

#include "core/Content.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QRegularExpression>

#include <cmath>
#include <map>

// (No C++ objects are made inside fz_try blocks.)

namespace jobs {

namespace {

void quiet(void *, const char *) {}

// a context of the job's own, and the copy of the PDF opened in it
struct Copy {
    fz_context *ctx = nullptr;
    pdf_document *doc = nullptr;
    QString error;
    explicit Copy(const QByteArray &pdf)
    {
        ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (!ctx) {
            error = QStringLiteral("The PDF engine couldn't start.");
            return;
        }
        fz_set_warning_callback(ctx, quiet, nullptr);
        fz_set_error_callback(ctx, quiet, nullptr);
        fz_buffer *buf = nullptr;
        fz_stream *stm = nullptr;
        const char *failed = nullptr;
        fz_var(buf);
        fz_var(stm);
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            buf = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char *>(pdf.constData()), size_t(pdf.size()));
            stm = fz_open_buffer(ctx, buf);
            doc = pdf_open_document_with_stream(ctx, stm);
        }
        fz_always(ctx) {
            fz_drop_stream(ctx, stm);
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            failed = fz_caught_message(ctx);
        }
        if (failed)
            error = QString::fromUtf8(failed);
    }
    ~Copy()
    {
        if (ctx) {
            pdf_drop_document(ctx, doc);
            fz_drop_context(ctx);
        }
    }
};

pdf_write_options fullSave()
{
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 3;
    o.do_compress = 1;
    o.do_compress_images = 1;
    o.do_compress_fonts = 1;
    return o;
}

QString plural(int n, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
}

// How big each picture is shown (points), the biggest place it's drawn: the page's content
// (and forms it draws, a few deep) read with the content-stream reader
struct Shown {
    double w = 0, h = 0;
};

void scanShown(fz_context *ctx, pdf_obj *res, const QByteArray &data, fz_matrix start, std::map<int, Shown> &shown, int depth)
{
    pdf_obj *xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    std::vector<fz_matrix> stack;
    fz_matrix ctm = start;
    for (const content::Op &op : content::readOps(data)) {
        const auto &n = op.numbers;
        if (op.op == "q") {
            stack.push_back(ctm);
        } else if (op.op == "Q") {
            if (!stack.empty()) {
                ctm = stack.back();
                stack.pop_back();
            }
        } else if (op.op == "cm" && n.size() >= 6) {
            const size_t b = n.size() - 6;
            ctm = fz_concat({float(n[b]), float(n[b + 1]), float(n[b + 2]), float(n[b + 3]), float(n[b + 4]), float(n[b + 5])}, ctm);
        } else if (op.op == "Do" && !op.name.isEmpty()) {
            pdf_obj *x = pdf_dict_gets(ctx, xobjs, op.name.constData());
            pdf_obj *type = pdf_dict_get(ctx, x, PDF_NAME(Subtype));
            if (pdf_name_eq(ctx, type, PDF_NAME(Image)) && pdf_is_indirect(ctx, x)) {
                Shown &s = shown[pdf_to_num(ctx, x)];
                s.w = std::max(s.w, double(std::hypot(ctm.a, ctm.b)));
                s.h = std::max(s.h, double(std::hypot(ctm.c, ctm.d)));
            } else if (pdf_name_eq(ctx, type, PDF_NAME(Form)) && depth < 4) {
                pdf_obj *own = pdf_dict_get(ctx, x, PDF_NAME(Resources));
                scanShown(ctx, own ? own : res, content::streamData(ctx, x),
                          fz_concat(pdf_dict_get_matrix(ctx, x, PDF_NAME(Matrix)), ctm), shown, depth + 1);
            }
        }
    }
}

// one picture made dpi sharp where it's shown biggest, as a JPEG at quality - kept only if
// smaller. Pictures that can't be (masks, colour-key masks, not 8 bits...) are left as they are.
// (MuPDF 1.24.3's own pdf_rewrite_images crashed on some pictures - this is ours.)
void shrinkPicture(fz_context *ctx, pdf_document *doc, int num, const Shown &s, int dpi, int quality)
{
    pdf_obj *ref = nullptr;
    fz_image *img = nullptr;
    fz_pixmap *pix = nullptr, *conv = nullptr, *scaled = nullptr;
    fz_buffer *buf = nullptr;
    fz_var(ref);
    fz_var(img);
    fz_var(pix);
    fz_var(conv);
    fz_var(scaled);
    fz_var(buf);
    fz_try(ctx) {
        ref = pdf_new_indirect(ctx, doc, num, 0);
        const int w = pdf_dict_get_int(ctx, ref, PDF_NAME(Width)), h = pdf_dict_get_int(ctx, ref, PDF_NAME(Height));
        const double k = std::max(dpi * s.w / 72.0 / std::max(1, w), dpi * s.h / 72.0 / std::max(1, h));
        if (pdf_dict_get_bool(ctx, ref, PDF_NAME(ImageMask)) || pdf_is_array(ctx, pdf_dict_get(ctx, ref, PDF_NAME(Mask))) ||
            s.w <= 0 || s.h <= 0 || w < 16 || h < 16 || k * 1.3 >= 1)
            break;  // (sharp enough already, or not a kind to touch)
        const int tw = std::max(1, int(std::lround(w * k))), th = std::max(1, int(std::lround(h * k)));
        img = pdf_load_image(ctx, doc, ref);
        pix = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr, nullptr, nullptr);
        if (!pix->colorspace)
            break;
        const bool gray = fz_colorspace_n(ctx, pix->colorspace) == 1;
        conv = fz_convert_pixmap(ctx, pix, gray ? fz_device_gray(ctx) : fz_device_rgb(ctx), nullptr, nullptr,
                                 fz_default_color_params, 0);
        scaled = fz_scale_pixmap(ctx, conv, 0, 0, float(tw), float(th), nullptr);
        if (!scaled)
            break;
        buf = fz_new_buffer_from_pixmap_as_jpeg(ctx, scaled, fz_default_color_params, quality, 0);
        if (int64_t(fz_buffer_storage(ctx, buf, nullptr)) >= pdf_dict_get_int64(ctx, ref, PDF_NAME(Length)))
            break;  // (no smaller)
        pdf_update_stream(ctx, doc, ref, buf, 1);
        pdf_dict_put(ctx, ref, PDF_NAME(Filter), PDF_NAME(DCTDecode));
        pdf_dict_del(ctx, ref, PDF_NAME(DecodeParms));
        pdf_dict_del(ctx, ref, PDF_NAME(Decode));
        pdf_dict_put_int(ctx, ref, PDF_NAME(Width), scaled->w);
        pdf_dict_put_int(ctx, ref, PDF_NAME(Height), scaled->h);
        pdf_dict_put_int(ctx, ref, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(ctx, ref, PDF_NAME(ColorSpace), gray ? PDF_NAME(DeviceGray) : PDF_NAME(DeviceRGB));
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_pixmap(ctx, scaled);
        fz_drop_pixmap(ctx, conv);
        fz_drop_pixmap(ctx, pix);
        fz_drop_image(ctx, img);
        pdf_drop_obj(ctx, ref);
    }
    fz_catch(ctx) {
        // (left as it was)
    }
}

}  // namespace

std::vector<std::pair<int, std::optional<int>>> parseRanges(const QString &text, QString *error)
{
    std::vector<std::pair<int, std::optional<int>>> out;
    static const QRegularExpression range(QStringLiteral("^(\\d*)-(\\d*)$"));
    for (QString part : text.split(QRegularExpression(QStringLiteral("[,;]")))) {
        part = part.trimmed().remove(QLatin1Char(' '));
        if (part.isEmpty())
            continue;
        const auto m = range.match(part);
        int a = 0;
        std::optional<int> b;
        bool ok = true;
        if (m.hasMatch() && (!m.captured(1).isEmpty() || !m.captured(2).isEmpty())) {
            a = m.captured(1).isEmpty() ? 1 : m.captured(1).toInt();
            if (!m.captured(2).isEmpty())
                b = m.captured(2).toInt();
        } else {
            a = part.toInt(&ok);
            b = a;
        }
        if (!ok) {
            if (error)
                *error = QStringLiteral("\"%1\" isn't a page or a range of pages (like 1-3).").arg(part);
            return {};
        }
        if (a < 1 || (b && *b < a)) {
            if (error)
                *error = QStringLiteral("\"%1\" isn't a range of pages that can be.").arg(part);
            return {};
        }
        out.emplace_back(a, b);
    }
    if (out.empty() && error)
        *error = QStringLiteral("Type the pages, like 1-3, 5, 8-");
    return out;
}

QString uniquePath(const QString &folder, const QString &base, const QString &ext)
{
    QString path = folder + QLatin1Char('/') + base + ext;
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = folder + QLatin1Char('/') + QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext);
    return path;
}

QStringList pictureFormats()
{
    QStringList out;
    const QList<QByteArray> can = QImageWriter::supportedImageFormats();
    for (const char *f : {"PNG", "JPEG", "WEBP", "TIFF", "BMP"}) {
        const QByteArray lower = QByteArray(f).toLower();
        if (can.contains(lower) || (lower == "jpeg" && can.contains("jpg")))
            out << QString::fromLatin1(f);
    }
    return out;
}

int pageCount(const QByteArray &pdf)
{
    Copy c(pdf);
    if (!c.doc)
        return 0;
    int n = 0;
    fz_try(c.ctx) {
        n = pdf_count_pages(c.ctx, c.doc);
    }
    fz_catch(c.ctx) {
    }
    return n;
}

Result exportPictures(const QByteArray &pdf, const std::vector<int> &pages, const QString &format, int dpi, int quality,
                      const QString &folder, const QString &base, const Step &step)
{
    Result r;
    Copy c(pdf);
    if (!c.doc) {
        r.error = c.error;
        return r;
    }
    const int n = pdf_count_pages(c.ctx, c.doc);
    const QString ext = format == "JPEG" ? ".jpg" : format == "TIFF" ? ".tiff" : QLatin1Char('.') + format.toLower();
    for (size_t k = 0; k < pages.size(); ++k) {
        if (!step(double(k) / double(pages.size()))) {
            r.cancelled = true;
            return r;
        }
        const int p = pages[k];
        fz_pixmap *pix = nullptr;
        const char *failed = nullptr;
        fz_var(pix);
        fz_try(c.ctx) {
            fz_page *page = fz_load_page(c.ctx, reinterpret_cast<fz_document *>(c.doc), p);
            fz_try(c.ctx) {
                const float s = float(dpi) / 72;
                pix = fz_new_pixmap_from_page(c.ctx, page, fz_scale(s, s), fz_device_rgb(c.ctx), 0);
            }
            fz_always(c.ctx) {
                fz_drop_page(c.ctx, page);
            }
            fz_catch(c.ctx) {
                fz_rethrow(c.ctx);
            }
        }
        fz_catch(c.ctx) {
            failed = fz_caught_message(c.ctx);
        }
        if (failed) {
            r.error = QString::fromUtf8(failed);
            return r;
        }
        QImage im(fz_pixmap_samples(c.ctx, pix), fz_pixmap_width(c.ctx, pix), fz_pixmap_height(c.ctx, pix),
                  int(fz_pixmap_stride(c.ctx, pix)), QImage::Format_RGB888);
        im = im.copy();
        fz_drop_pixmap(c.ctx, pix);
        im.setDotsPerMeterX(int(std::lround(dpi / 0.0254)));
        im.setDotsPerMeterY(int(std::lround(dpi / 0.0254)));
        const QString name = n > 1 ? QStringLiteral("%1 - page %2").arg(base).arg(p + 1) : base;
        const QString dst = uniquePath(folder, name, ext);
        const bool lossy = format == "JPEG" || format == "WEBP";
        if (!im.save(dst, format.toLatin1().constData(), lossy ? quality : -1)) {
            r.error = QStringLiteral("Couldn't write %1.").arg(QFileInfo(dst).fileName());
            return r;
        }
        r.written << dst;
    }
    r.message = QStringLiteral("Saved %1.").arg(plural(int(r.written.size()), "picture", "pictures"));
    return r;
}

Result split(const QByteArray &pdf, const std::vector<std::pair<int, int>> &parts, const QString &folder,
             const QString &base, const Step &step)
{
    Result r;
    Copy c(pdf);
    if (!c.doc) {
        r.error = c.error;
        return r;
    }
    for (size_t k = 0; k < parts.size(); ++k) {
        if (!step(double(k) / double(parts.size()))) {
            r.cancelled = true;
            return r;
        }
        const auto [a, b] = parts[k];
        const QString label = a == b ? QStringLiteral("page %1").arg(a) : QStringLiteral("pages %1-%2").arg(a).arg(b);
        const QString dst = uniquePath(folder, QStringLiteral("%1 - %2").arg(base, label), QStringLiteral(".pdf"));
        const QByteArray name = dst.toUtf8();
        pdf_document *part = nullptr;
        pdf_graft_map *map = nullptr;
        const char *failed = nullptr;
        fz_var(part);
        fz_var(map);
        fz_try(c.ctx) {
            part = pdf_create_document(c.ctx);
            map = pdf_new_graft_map(c.ctx, part);
            for (int p = a - 1; p <= b - 1; ++p)
                pdf_graft_mapped_page(c.ctx, map, -1, c.doc, p);
            const pdf_write_options o = fullSave();
            pdf_save_document(c.ctx, part, name.constData(), &o);
        }
        fz_always(c.ctx) {
            pdf_drop_graft_map(c.ctx, map);
            pdf_drop_document(c.ctx, part);
        }
        fz_catch(c.ctx) {
            failed = fz_caught_message(c.ctx);
        }
        if (failed) {
            r.error = QString::fromUtf8(failed);
            return r;
        }
        r.written << dst;
    }
    r.message = QStringLiteral("Split into %1.").arg(plural(int(r.written.size()), "PDF file", "PDF files"));
    return r;
}

Result compress(const QByteArray &pdf, int dpi, int quality, bool strip, const QString &folder, const QString &base,
                qint64 before, const Step &step)
{
    Result r;
    Copy c(pdf);
    if (!c.doc) {
        r.error = c.error;
        return r;
    }
    if (!step(0.05)) {
        r.cancelled = true;
        return r;
    }
    const QString dst = uniquePath(folder, base + QStringLiteral(" (compressed)"), QStringLiteral(".pdf"));
    const QByteArray name = dst.toUtf8();
    const char *failed = nullptr;
    if (dpi > 0) {  // pictures sharper than about 1.3 x the target brought down to it
        std::map<int, Shown> shown;
        const int pages = pdf_count_pages(c.ctx, c.doc);
        for (int i = 0; i < pages; ++i) {
            pdf_obj *page = nullptr;
            fz_try(c.ctx) {
                page = pdf_lookup_page_obj(c.ctx, c.doc, i);
            }
            fz_catch(c.ctx) {
                continue;
            }
            QByteArray data;
            for (pdf_obj *stm : content::contentStreams(c.ctx, page))
                data += content::streamData(c.ctx, stm) + QByteArray(1, ' ');  // (streams join with a space)
            scanShown(c.ctx, pdf_dict_get_inheritable(c.ctx, page, PDF_NAME(Resources)), data, fz_identity, shown, 0);
            if (!step(0.05 + 0.15 * (i + 1) / pages)) {
                r.cancelled = true;
                return r;
            }
        }
        int done = 0;
        for (const auto &[num, s] : shown) {
            shrinkPicture(c.ctx, c.doc, num, s, dpi, std::clamp(quality, 1, 100));
            if (!step(0.2 + 0.5 * ++done / double(shown.size()))) {
                r.cancelled = true;
                return r;
            }
        }
    }
    fz_try(c.ctx) {
        if (strip) {  // the document's details: out
            pdf_dict_del(c.ctx, pdf_trailer(c.ctx, c.doc), PDF_NAME(Info));
            pdf_dict_del(c.ctx, pdf_dict_get(c.ctx, pdf_trailer(c.ctx, c.doc), PDF_NAME(Root)), PDF_NAME(Metadata));
        }
    }
    fz_catch(c.ctx) {
        failed = fz_caught_message(c.ctx);
    }
    if (failed) {
        r.error = QString::fromUtf8(failed);
        return r;
    }
    if (!step(0.7)) {
        r.cancelled = true;
        return r;
    }
    fz_try(c.ctx) {
        pdf_write_options o = fullSave();
        o.do_garbage = 4;
        o.do_clean = 1;
        o.do_use_objstms = 1;
        pdf_save_document(c.ctx, c.doc, name.constData(), &o);
    }
    fz_catch(c.ctx) {
        failed = fz_caught_message(c.ctx);
    }
    if (failed) {
        r.error = QString::fromUtf8(failed);
        return r;
    }
    const qint64 after = QFileInfo(dst).size();
    const qint64 was = before > 0 ? before : qint64(pdf.size());
    auto size = [](qint64 n) {
        return n < 1024 * 1024 ? QStringLiteral("%1 KB").arg(std::max<qint64>(1, n / 1024))
                               : QStringLiteral("%1 MB").arg(n / 1024.0 / 1024.0, 0, 'f', 1);
    };
    if (after >= was) {  // no smaller: no pointless copy left
        QFile::remove(dst);
        r.message = QStringLiteral("This PDF is already as small as it can be made with these settings. "
                                   "Try a smaller setting for the pictures.");
        return r;
    }
    r.written << dst;
    r.message = QStringLiteral("Saved %1: %2 > %3 (%4% smaller).")
                    .arg(QFileInfo(dst).fileName(), size(was), size(after))
                    .arg(std::lround(100 - after * 100.0 / was));
    return r;
}

}  // namespace jobs
