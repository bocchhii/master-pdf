#pragma once

// Reading a page's content streams (its drawing instructions): each operator with its
// operands and where it is - for changing a drawing right where the page draws it.

#include <QByteArray>

#include <vector>

struct fz_context;
struct pdf_document;
struct pdf_obj;

namespace content {

struct Op {
    QByteArray op;                 // the operator (cm, Do, q, Q, re, f...)
    std::vector<double> numbers;   // its number operands
    QByteArray name;               // its last name operand (Do's)
    qint64 start = 0, end = 0;     // its operands' start .. the operator's end, in the stream
};

std::vector<Op> readOps(const QByteArray &data);
std::vector<pdf_obj *> contentStreams(fz_context *ctx, pdf_obj *pageobj);  // (borrowed)
QByteArray streamData(fz_context *ctx, pdf_obj *stm);
void setStreamData(fz_context *ctx, pdf_document *doc, pdf_obj *stm, const QByteArray &data);

}  // namespace content
