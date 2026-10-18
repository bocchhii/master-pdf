#include "core/Content.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>

namespace content {

namespace {

// ---- reading a content stream: its operators, with where each is ----

bool isWhite(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

bool isDelim(char c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
}

}  // namespace

std::vector<Op> readOps(const QByteArray &d)
{
    std::vector<Op> ops;
    Op cur;
    bool started = false;
    qint64 i = 0;
    const qint64 n = d.size();
    auto mark = [&](qint64 at) {
        if (!started) {
            cur.start = at;
            started = true;
        }
    };
    while (i < n) {
        const char c = d[i];
        if (isWhite(c)) {
            ++i;
            continue;
        }
        if (c == '%') {  // a comment, to the line's end
            while (i < n && d[i] != '\n' && d[i] != '\r')
                ++i;
            continue;
        }
        mark(i);
        if (c == '(') {  // a string: nested brackets, escapes
            int depth = 0;
            for (; i < n; ++i) {
                if (d[i] == '\\') {
                    ++i;
                    continue;
                }
                if (d[i] == '(')
                    ++depth;
                else if (d[i] == ')' && --depth == 0) {
                    ++i;
                    break;
                }
            }
            continue;
        }
        if (c == '<' && i + 1 < n && d[i + 1] == '<') {
            i += 2;
            continue;
        }
        if (c == '>' && i + 1 < n && d[i + 1] == '>') {
            i += 2;
            continue;
        }
        if (c == '<') {  // a hex string
            while (i < n && d[i] != '>')
                ++i;
            ++i;
            continue;
        }
        if (c == '[' || c == ']' || c == '{' || c == '}') {
            ++i;
            continue;
        }
        if (c == '/') {  // a name
            qint64 j = i + 1;
            while (j < n && !isWhite(d[j]) && !isDelim(d[j]))
                ++j;
            cur.name = d.mid(i + 1, j - i - 1);
            i = j;
            continue;
        }
        qint64 j = i;
        while (j < n && !isWhite(d[j]) && !isDelim(d[j]))
            ++j;
        if (j == i) {
            ++i;
            continue;
        }
        const QByteArray word = d.mid(i, j - i);
        bool isNumber = false;
        const double v = word.toDouble(&isNumber);
        if (isNumber || word == "+" || word == "-") {
            cur.numbers.push_back(isNumber ? v : 0);
            i = j;
            continue;
        }
        if (word == "true" || word == "false" || word == "null") {
            i = j;
            continue;
        }
        // an operator
        cur.op = word;
        cur.end = j;
        if (word == "BI") {  // an inline picture: skipped to its EI
            qint64 k = d.indexOf("ID", j);
            if (k < 0)
                k = j;
            k += 3;
            while (k + 2 <= n) {
                if (d[k] == 'E' && d[k + 1] == 'I' && (k == 0 || isWhite(d[k - 1])) && (k + 2 == n || isWhite(d[k + 2])))
                    break;
                ++k;
            }
            cur.end = std::min(n, k + 2);
            cur.op = "BI";
            j = cur.end;
        }
        ops.push_back(cur);
        cur = Op();
        started = false;
        i = j;
    }
    return ops;
}

// the page's content streams, in order
std::vector<pdf_obj *> contentStreams(fz_context *ctx, pdf_obj *pageobj)
{
    std::vector<pdf_obj *> out;
    pdf_obj *c = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
    if (pdf_is_array(ctx, c)) {
        for (int i = 0; i < pdf_array_len(ctx, c); ++i)
            out.push_back(pdf_array_get(ctx, c, i));
    } else if (c) {
        out.push_back(c);
    }
    return out;
}

QByteArray streamData(fz_context *ctx, pdf_obj *stm)
{
    QByteArray out;
    fz_buffer *buf = nullptr;
    fz_var(buf);
    fz_try(ctx) {
        buf = pdf_load_stream(ctx, stm);
        unsigned char *data = nullptr;
        const size_t len = fz_buffer_storage(ctx, buf, &data);
        out = QByteArray(reinterpret_cast<const char *>(data), qsizetype(len));
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        out.clear();
    }
    return out;
}

void setStreamData(fz_context *ctx, pdf_document *doc, pdf_obj *stm, const QByteArray &data)
{
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char *>(data.constData()),
                                                    size_t(data.size()));
    fz_try(ctx) {
        pdf_update_stream(ctx, doc, stm, buf, 0);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}


}  // namespace content
