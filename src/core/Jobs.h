#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

// The File menu's jobs (the Python app's Export as pictures, Split, Compress): each works on a
// copy of the PDF (its bytes, as it is now - changes not saved yet too), on a worker thread,
// with its own MuPDF context. step(0..1) says how far it is; false back: cancelled.
namespace jobs {

using Step = std::function<bool(double)>;

struct Result {
    QStringList written;  // the files made
    QString message;      // what's said at the end
    QString error;        // it didn't work: why
    bool cancelled = false;
};

// "1-3, 5, 8-" -> (1, 3), (5, 5), (8, none: to the last page); empty and error if it can't be read
std::vector<std::pair<int, std::optional<int>>> parseRanges(const QString &text, QString *error);
QString uniquePath(const QString &folder, const QString &base, const QString &ext);  // never over a file
QStringList pictureFormats();  // the ones that can be written here (PNG, JPEG...)

Result exportPictures(const QByteArray &pdf, const std::vector<int> &pages, const QString &format, int dpi,
                      int quality, const QString &folder, const QString &base, const Step &step);
Result split(const QByteArray &pdf, const std::vector<std::pair<int, int>> &parts, const QString &folder,
             const QString &base, const Step &step);
// pictures sharper than dpi (x 1.3) brought down to it, as JPEG at quality (dpi 0: kept);
// strip: the document's details taken out
Result compress(const QByteArray &pdf, int dpi, int quality, bool strip, const QString &folder, const QString &base,
                qint64 before, const Step &step);
int pageCount(const QByteArray &pdf);

}  // namespace jobs
