// Document's links (the Link tool, and links followed with Select / Move): read from the page,
// added, changed, deleted - each one change, undone in one go. A link to a page of this PDF is
// written as "#page=N", which MuPDF makes a GoTo action.
// (No C++ objects are made inside fz_try blocks.)

#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>

namespace {

QByteArray linkUri(const QString &uri, int toPage)
{
    return uri.isEmpty() ? QByteArray("#page=") + QByteArray::number(toPage + 1) : uri.toUtf8();
}

}  // namespace

std::vector<PdfLink> Document::links(int page)
{
    std::lock_guard guard(docMutex_);
    if (!pdf_ || page < 0 || page >= int(lists_.size()))
        return {};
    const QPointF o = geometry()->origins[size_t(page)];
    pdf_page *pg = nullptr;
    fz_link *head = nullptr;
    fz_var(pg);
    fz_var(head);
    fz_try(ctx_) {
        pg = pdf_load_page(ctx_, pdf_, page);
        head = fz_load_links(ctx_, reinterpret_cast<fz_page *>(pg));
    }
    fz_catch(ctx_) {
        head = nullptr;
    }
    std::vector<PdfLink> out;
    int n = 0;
    for (fz_link *l = head; l; l = l->next, ++n) {
        if (!l->uri)
            continue;
        PdfLink k;
        k.index = n;
        k.rect = QRectF(QPointF(l->rect.x0 - o.x(), l->rect.y0 - o.y()), QPointF(l->rect.x1 - o.x(), l->rect.y1 - o.y()));
        if (fz_is_external_link(ctx_, l->uri)) {
            k.uri = QString::fromUtf8(l->uri);
        } else {
            int to = -1;
            fz_try(ctx_) {
                float x = 0, y = 0;
                to = fz_page_number_from_location(ctx_, doc_, fz_resolve_link(ctx_, doc_, l->uri, &x, &y));
            }
            fz_catch(ctx_) {
                to = -1;
            }
            if (to < 0)
                continue;  // (a link to nowhere this app can go)
            k.page = to;
        }
        out.push_back(k);
    }
    fz_drop_link(ctx_, head);
    fz_drop_page(ctx_, reinterpret_cast<fz_page *>(pg));
    return out;
}

bool Document::addLink(int page, const QRectF &rect, const QString &uri, int toPage, QString *error)
{
    if (rect.width() < 1 || rect.height() < 1)
        return false;
    const QPointF o = geometry()->origins.at(size_t(page));
    const fz_rect fr = fz_make_rect(float(rect.left() + o.x()), float(rect.top() + o.y()),
                                    float(rect.right() + o.x()), float(rect.bottom() + o.y()));
    const QByteArray target = linkUri(uri, toPage);
    std::lock_guard guard(docMutex_);
    return onPage(page, "add a link", [&](pdf_page *pg) {
        pdf_create_link(ctx_, pg, fr, target.constData());  // (the page keeps it)
    }, error);
}

bool Document::changeLink(int page, const PdfLink &link, const QString &uri, int toPage, QString *error)
{
    const QPointF o = geometry()->origins.at(size_t(page));
    const fz_rect fr = fz_make_rect(float(link.rect.left() + o.x()), float(link.rect.top() + o.y()),
                                    float(link.rect.right() + o.x()), float(link.rect.bottom() + o.y()));
    const QByteArray target = linkUri(uri, toPage);
    const bool remake = !uri.isNull() || toPage >= 0;
    std::lock_guard guard(docMutex_);
    return onPage(page, remake ? "change a link" : "delete a link", [&](pdf_page *pg) {
        // the page's own list (not kept: deleting takes it out of the list and drops it)
        fz_link *l = pg->links;
        for (int n = 0; l && n < link.index; ++n)
            l = l->next;
        if (!l)
            fz_throw(ctx_, FZ_ERROR_GENERIC, "That link isn't there any more.");
        pdf_delete_link(ctx_, pg, l);
        if (remake)
            pdf_create_link(ctx_, pg, fr, target.constData());
    }, error);
}

bool Document::deleteLink(int page, const PdfLink &link, QString *error)
{
    return changeLink(page, link, QString(), -1, error);
}

// ---- bookmarks (the PDF's own table of contents) ----

namespace {

void walkOutline(fz_context *ctx, fz_document *doc, fz_outline *o, int level, const std::vector<QPointF> &origins,
                 std::vector<Bookmark> &out)
{
    for (; o; o = o->next) {
        Bookmark b;
        b.level = level;
        b.title = QString::fromUtf8(o->title ? o->title : "").simplified();
        if (b.title.isEmpty())
            b.title = QStringLiteral("(untitled)");
        int page = -1;
        fz_try(ctx) {
            page = fz_page_number_from_location(ctx, doc, o->page);
        }
        fz_catch(ctx) {
            page = -1;
        }
        b.page = page;
        if (page >= 0 && page < int(origins.size()) && !std::isnan(o->y) && o->y > 0)
            b.at = QPointF(std::max(0.0, o->x - origins[size_t(page)].x()), o->y - origins[size_t(page)].y());
        out.push_back(b);
        walkOutline(ctx, doc, o->down, level + 1, origins, out);
    }
}

}  // namespace

std::vector<Bookmark> Document::bookmarks()
{
    std::lock_guard guard(docMutex_);
    if (!doc_)
        return {};
    fz_outline *outline = nullptr;
    fz_var(outline);
    fz_try(ctx_) {
        outline = fz_load_outline(ctx_, doc_);
    }
    fz_catch(ctx_) {
        outline = nullptr;
    }
    std::vector<Bookmark> out;
    walkOutline(ctx_, doc_, outline, 0, geometry()->origins, out);
    fz_drop_outline(ctx_, outline);
    return out;
}
