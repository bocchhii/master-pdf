#pragma once

#include <QImage>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

// Text read in pictures (OCR), with Tesseract - every language it has (eng, ara...), all at once.
// Its languages: the "tessdata" folder next to the program (MSYS2's while it's built here).
namespace ocr {

struct Word {
    QString text;
    QRect box;  // (in the picture's pixels)
    float confidence = 0;  // (0..100)
};

struct Line {
    QString text;
    QRect box;
    QPoint baseStart, baseEnd;  // its baseline, left to right (pixels)
    std::vector<Word> words;    // (in the order they're read)
    float confidence = 0;
    int block = 0, para = 0;    // (the page's layout: the block, the paragraph it's in - counted down the page)
    bool inText = true;         // (in a block of text - not one of a picture, a ruled line, noise)
};

QString dataFolder();   // where its languages are (empty: none found)
QStringList languages();  // the ones it can read ("eng", "ara"...)
// a language's words - its model's own dictionary, as a list ("ara_words.txt" beside its languages; empty: none). Any thread
const QSet<QString> &dictionary(const QString &lang);
// A recogniser kept started - its languages loaded once - to read many pictures: a page, then lines of it
// one by one. psm: Tesseract's layout - 3 a page, 6 a block, 7 a line, 13 a line as it is (no layout of
// its own: a line Tesseract's own page layout misses)
class Engine {
public:
    Engine();
    ~Engine();
    bool start(const QString &langs, QString *error);  // (empty: every language there is, English first)
    bool started() const;
    // (progress: told how far it is, 0..100, as it reads the words; cancelled: asked now and then - true stops it)
    std::vector<Line> read(const QImage &image, int psm, QString *error, const std::function<void(int)> &progress = {},
                           const std::function<bool()> &cancelled = {});

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// the text in a picture (its pixels - see-through ones read as on white): line by line; error set
// if it couldn't be read at all
// (block: the picture is just lines of text, level - a label stood straight: read as one block)
// (langs: the languages read in, the main one first - "ara+eng"; empty: every one there is, English first)
std::vector<Line> read(const QImage &image, QString *error, bool block = false, const QString &langs = QString(),
                       const std::function<void(int)> &progress = {}, const std::function<bool()> &cancelled = {});

}  // namespace ocr
