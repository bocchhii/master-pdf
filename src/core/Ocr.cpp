#include "core/Ocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>

#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace ocr {

QString dataFolder()
{
    QStringList places = {QCoreApplication::applicationDirPath() + QStringLiteral("/tessdata"),
                          QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/tessdata"),  // (macOS' bundle)
                          QCoreApplication::applicationDirPath() + QStringLiteral("/../share/tessdata")};
    const QString env = qEnvironmentVariable("TESSDATA_PREFIX");
    if (!env.isEmpty())
        places << env << env + QStringLiteral("/tessdata");
    places << QStringLiteral("C:/msys64/ucrt64/share/tessdata");  // (built here: MSYS2's)
    for (const QString &p : places)
        if (!QDir(p).entryList({QStringLiteral("*.traineddata")}, QDir::Files).isEmpty())
            return QDir(p).absolutePath();
    return {};
}

const QSet<QString> &dictionary(const QString &lang)
{
    static std::mutex lock;
    static std::map<QString, QSet<QString>> loaded;
    std::lock_guard guard(lock);
    auto it = loaded.find(lang);
    if (it != loaded.end())
        return it->second;
    QSet<QString> words;
    QFile f(dataFolder() + QLatin1Char('/') + lang + QStringLiteral("_words.txt"));
    if (f.open(QIODevice::ReadOnly)) {
        const QStringList all = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        words.reserve(all.size());
        for (const QString &w : all)
            words.insert(w.trimmed());
    }
    return loaded.emplace(lang, std::move(words)).first->second;
}

QStringList languages()
{
    QStringList out;
    const QString dir = dataFolder();
    if (dir.isEmpty())
        return out;
    for (const QFileInfo &f : QDir(dir).entryInfoList({QStringLiteral("*.traineddata")}, QDir::Files)) {
        const QString name = f.completeBaseName();
        if (name != QLatin1String("osd") && name != QLatin1String("equ"))  // (not languages: the page's turn, maths)
            out << name;
    }
    out.sort();
    if (out.removeOne(QStringLiteral("eng")))  // (English first: the one most text is in)
        out.prepend(QStringLiteral("eng"));
    return out;
}

struct Engine::Impl {
    std::unique_ptr<tesseract::TessBaseAPI> api;
};

Engine::Engine() : d(std::make_unique<Impl>()) {}

Engine::~Engine()
{
    if (d->api)
        d->api->End();
}

bool Engine::start(const QString &chosen, QString *error)
{
    const QString dir = dataFolder();
    const QStringList langs = languages();
    if (dir.isEmpty() || langs.isEmpty()) {
        if (error)
            *error = QStringLiteral("Text recognition's languages weren't found (a \"tessdata\" folder next to the program).");
        return false;
    }
    auto api = std::make_unique<tesseract::TessBaseAPI>();
    const QString use = chosen.isEmpty() ? langs.join(QLatin1Char('+')) : chosen;
    if (api->Init(dir.toUtf8().constData(), use.toUtf8().constData(), tesseract::OEM_LSTM_ONLY) != 0) {
        if (error)
            *error = QStringLiteral("Text recognition couldn't be started.");
        return false;
    }
    if (d->api)
        d->api->End();
    d->api = std::move(api);
    return true;
}

bool Engine::started() const
{
    return d->api != nullptr;
}

std::vector<Line> Engine::read(const QImage &image, int psm, QString *error, const std::function<void(int)> &progress,
                               const std::function<bool()> &cancelled)
{
    std::vector<Line> lines;
    if (!d->api || image.isNull())
        return lines;
    tesseract::TessBaseAPI *api = d->api.get();
    // (see-through parts as white paper; 8 bits a colour)
    QImage rgb(image.size(), QImage::Format_RGB888);
    rgb.fill(Qt::white);
    {
        QPainter p(&rgb);
        p.drawImage(0, 0, image);
    }
    api->SetPageSegMode(tesseract::PageSegMode(psm));
    api->SetImage(rgb.constBits(), rgb.width(), rgb.height(), 3, int(rgb.bytesPerLine()));
    api->SetSourceResolution(300);
    // (how far it is, as it reads - and stopped if it's called off)
    struct Watch {
        const std::function<void(int)> *progress;
        const std::function<bool()> *cancelled;
    } watch{&progress, &cancelled};
    tesseract::ETEXT_DESC monitor;
    monitor.cancel_this = &watch;
    monitor.cancel = [](void *w, int) {
        const Watch *at = static_cast<const Watch *>(w);
        return *at->cancelled && (*at->cancelled)();
    };
    monitor.progress_callback2 = [](tesseract::ETEXT_DESC *m, int, int, int, int) {
        const Watch *at = static_cast<const Watch *>(m->cancel_this);
        if (*at->progress)
            (*at->progress)(m->progress);
        return true;
    };
    if (api->Recognize(&monitor) != 0) {
        if (error)
            *error = QStringLiteral("The picture's text couldn't be read.");
        api->Clear();
        return lines;
    }
    const bool oneLine = psm == tesseract::PSM_SINGLE_LINE || psm == tesseract::PSM_RAW_LINE;
    // (an Arabic word given left to right - its letters in the order they lie across, not read: a heavy face's word read
    // alone came back "نانسألا" - put in its reading order, by where its letters are)
    std::map<std::tuple<int, int, int, int>, QString> turned;  // (the word's box -> its text the right way round)
    {
        std::unique_ptr<tesseract::ResultIterator> s(api->GetIterator());
        std::vector<std::pair<int, QString>> symbols;
        std::tuple<int, int, int, int> word{};
        auto settle = [&] {
            if (symbols.size() < 3)
                return;
            int arabic = 0, latin = 0, digits = 0, rising = 0, falling = 0;
            for (const auto &[x, t] : symbols)
                for (const QChar c : t) {
                    if (c.unicode() >= 0x0621 && c.unicode() <= 0x064A)
                        ++arabic;
                    else if (c.isDigit())
                        ++digits;
                    else if (c.isLetter())
                        ++latin;
                }
            for (size_t i = 1; i < symbols.size(); ++i) {
                if (symbols[i].first > symbols[i - 1].first)
                    ++rising;
                else if (symbols[i].first < symbols[i - 1].first)
                    ++falling;
            }
            if (arabic < 3 || latin > 0 || digits > 1 || rising < 3 * std::max(1, falling))
                return;  // (not an Arabic word - a code's digits run left to right - or in its reading order already)
            QString text;
            for (auto k = symbols.rbegin(); k != symbols.rend(); ++k)
                text += k->second;
            turned[word] = text;
        };
        if (s) {
            do {
                if (s->Empty(tesseract::RIL_SYMBOL))
                    continue;
                if (s->IsAtBeginningOf(tesseract::RIL_WORD)) {
                    settle();
                    symbols.clear();
                    int x1, y1, x2, y2;
                    s->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                    word = {x1, y1, x2, y2};
                }
                int x1, y1, x2, y2;
                s->BoundingBox(tesseract::RIL_SYMBOL, &x1, &y1, &x2, &y2);
                std::unique_ptr<char[]> t(s->GetUTF8Text(tesseract::RIL_SYMBOL));
                symbols.emplace_back((x1 + x2) / 2, t ? QString::fromUtf8(t.get()) : QString());
            } while (s->Next(tesseract::RIL_SYMBOL));
            settle();
        }
    }
    std::unique_ptr<tesseract::ResultIterator> it(api->GetIterator());
    int blockNo = -1, paraNo = -1;
    if (it) {
        do {
            if (it->IsAtBeginningOf(tesseract::RIL_BLOCK))
                ++blockNo;
            if (it->IsAtBeginningOf(tesseract::RIL_PARA))
                ++paraNo;
            if (it->Empty(tesseract::RIL_WORD))
                continue;
            if (lines.empty() || it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
                Line l;
                int x1, y1, x2, y2;
                if (it->BoundingBox(tesseract::RIL_TEXTLINE, &x1, &y1, &x2, &y2))
                    l.box = QRect(QPoint(x1, y1), QPoint(x2 - 1, y2 - 1));
                if (it->Baseline(tesseract::RIL_TEXTLINE, &x1, &y1, &x2, &y2)) {
                    l.baseStart = QPoint(x1, y1);
                    l.baseEnd = QPoint(x2, y2);
                }
                std::unique_ptr<char[]> t(it->GetUTF8Text(tesseract::RIL_TEXTLINE));
                l.text = t ? QString::fromUtf8(t.get()).trimmed() : QString();
                l.confidence = it->Confidence(tesseract::RIL_TEXTLINE);
                l.block = std::max(0, blockNo);
                l.para = std::max(0, paraNo);
                l.inText = oneLine || PTIsTextType(it->BlockType());
                lines.push_back(std::move(l));
            }
            Word w;
            int x1, y1, x2, y2;
            if (it->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2))
                w.box = QRect(QPoint(x1, y1), QPoint(x2 - 1, y2 - 1));
            std::unique_ptr<char[]> t(it->GetUTF8Text(tesseract::RIL_WORD));
            w.text = t ? QString::fromUtf8(t.get()).trimmed() : QString();
            if (const auto k = turned.find({x1, y1, x2, y2}); k != turned.end())
                w.text = k->second;
            w.confidence = it->Confidence(tesseract::RIL_WORD);
            if (!w.text.isEmpty())
                lines.back().words.push_back(std::move(w));
        } while (it->Next(tesseract::RIL_WORD));
    }
    api->Clear();
    return lines;
}

std::vector<Line> read(const QImage &image, QString *error, bool block, const QString &chosen, const std::function<void(int)> &progress,
                       const std::function<bool()> &cancelled)
{
    Engine e;
    if (!e.start(chosen, error))
        return {};
    return e.read(image, block ? tesseract::PSM_SINGLE_BLOCK : tesseract::PSM_AUTO, error, progress, cancelled);
}

}  // namespace ocr
