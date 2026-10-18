#pragma once

// The text reader (step 7): letters drawn as shapes read with Windows' own text reader
// (Windows.Media.Ocr) - Arabic first, then English.

#include <QRectF>
#include <QString>

#include <utility>
#include <vector>

class Document;

namespace reader {

// the lines read in that part of the page (points from its top left): text, and where it is;
// empty, with error set, if nothing could be read
std::vector<std::pair<QString, QRectF>> read(Document &doc, int page, const QRectF &rect, QString *error);

}  // namespace reader
