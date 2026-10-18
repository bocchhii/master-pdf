#include "core/Fonts.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

#include <future>
#include <map>
#include <mutex>

namespace fonts {

namespace {

struct Member {
    QString path;
    int index;
    QString family;
    bool bold, italic, plain;
};

struct Index {
    QHash<QString, std::pair<QString, QString>> byKey;  // key -> (file, family)
    QHash<QString, std::pair<QString, QString>> cut;    // first 31 letters -> (file, family)
    std::map<QString, std::vector<Member>> families;    // family key -> its fonts
    QHash<QString, std::pair<bool, bool>> styles;       // file -> (bold, italic)
};

Index g_index;
// (lookups and additions: the PDFs' own fonts are added while pages are read, on other threads)
std::recursive_mutex g_indexMutex;
// a PDF's own font, as added (put back after a new scan)
struct Embedded {
    QString path, family;
    QStringList names;
    bool bold, italic;
};
std::vector<Embedded> g_embedded;

void addToIndex(Index &ix, const Embedded &e)
{
    // (after the installed fonts: one installed of the same name is still the one found)
    for (const QString &name : e.names) {
        if (name.trimmed().isEmpty())
            continue;
        if (!ix.byKey.contains(key(name)))
            ix.byKey.insert(key(name), {e.path, e.family});
    }
    ix.styles.insert(e.path, {e.bold, e.italic});
    ix.families[key(e.family)].push_back({e.path, 0, e.family, e.bold, e.italic, !e.bold && !e.italic});
}
std::shared_future<void> g_scan;
std::once_flag g_started;
std::mutex g_faceMutex;
FT_Library g_lib = nullptr;
QHash<QString, FT_Face> g_faces;  // (for asking whether a font has letters)

std::pair<bool, bool> styleOf(const QString &sub)
{
    const QString s = sub.toLower();
    return {s.contains("bold") || s.contains("black") || s.contains("heavy"),
            s.contains("italic") || s.contains("oblique")};
}

QStringList folders()
{
    QStringList out;
    QString win = qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"));
    out << win + QStringLiteral("/Fonts");
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) {
        out << local + QStringLiteral("/Microsoft/Windows/Fonts");
        // (Office's cloud fonts - Aptos, its italic and bold, the fonts Word now writes with: downloaded
        // there, not installed in Windows)
        QDirIterator cache(local + QStringLiteral("/Microsoft/FontCache"), {QStringLiteral("CloudFonts")}, QDir::Dirs,
                           QDirIterator::Subdirectories);
        while (cache.hasNext())
            out << cache.next();
    }
    out << myFolder() << bundledFolder();
    return out;
}

// the names in a font's name table: full name (4), family (1), style (2), PostScript name (6)
void readNames(FT_Face face, QString *full, QString *family, QString *style, QString *ps)
{
    std::map<int, QString> found;
    const FT_UInt n = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt k = 0; k < n; ++k) {
        FT_SfntName name;
        if (FT_Get_Sfnt_Name(face, k, &name))
            continue;
        if (name.name_id != 1 && name.name_id != 2 && name.name_id != 4 && name.name_id != 6)
            continue;
        QString text;
        if (name.platform_id == TT_PLATFORM_MICROSOFT) {
            for (FT_UInt i = 0; i + 1 < name.string_len; i += 2)
                text += QChar(char16_t((name.string[i] << 8) | name.string[i + 1]));
        } else if (name.platform_id == TT_PLATFORM_MACINTOSH) {
            text = QString::fromLatin1(reinterpret_cast<const char *>(name.string), int(name.string_len));
        } else {
            continue;
        }
        text = text.trimmed();
        const bool english = name.platform_id == TT_PLATFORM_MACINTOSH || name.language_id == 0x409;
        if (!text.isEmpty() && (english || !found.count(name.name_id)))
            found[name.name_id] = text;
    }
    *family = found.count(1) ? found[1] : QString::fromUtf8(face->family_name ? face->family_name : "");
    *style = found.count(2) ? found[2] : QString::fromUtf8(face->style_name ? face->style_name : "");
    *full = found.count(4) ? found[4] : QString();
    *ps = found.count(6) ? found[6] : QString();
}

void scan()
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib))
        return;
    Index ix;
    for (const QString &folder : folders()) {
        if (!QFileInfo(folder).isDir())
            continue;
        QDirIterator it(folder, {QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.ttc"),
                                 QStringLiteral("*.TTF"), QStringLiteral("*.OTF"), QStringLiteral("*.TTC")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            FT_Face face;
            if (FT_New_Face(lib, path.toUtf8().constData(), 0, &face))
                continue;
            QString full, family, style, ps;
            readNames(face, &full, &family, &style, &ps);
            FT_Done_Face(face);
            if (family.isEmpty())
                continue;
            const QString familyStyle = family + QLatin1Char(' ') + style;
            for (const QString &name : {full, familyStyle, ps}) {
                if (name.trimmed().isEmpty())
                    continue;
                if (!ix.byKey.contains(key(name)))
                    ix.byKey.insert(key(name), {path, family});
                // (PDF readers cut long names at 31 letters: found by that too)
                const QString c = QString(name).remove(QLatin1Char(' ')).left(31).toLower();
                if (!ix.cut.contains(c))
                    ix.cut.insert(c, {path, family});
            }
            const auto [b, i] = styleOf(style);
            ix.styles.insert(path, {b, i});
            const QString sk = key(style);
            ix.families[key(family)].push_back({path, 0, family, b, i, sk.isEmpty() || sk == "book"});
        }
    }
    FT_Done_FreeType(lib);
    std::lock_guard guard(g_indexMutex);
    for (const Embedded &e : g_embedded)
        addToIndex(ix, e);
    g_index = std::move(ix);
}

Found familyFont(const QString &k, bool bold, bool italic)
{
    auto it = g_index.families.find(k);
    if (it == g_index.families.end() || it->second.empty())
        return {};
    const Member *best = nullptr;
    int score = 1 << 20;
    for (const Member &m : it->second) {
        const int s = 4 * (m.bold != bold) + 2 * (m.italic != italic) + (m.plain ? 0 : 1);
        if (s < score) {
            score = s;
            best = &m;
        }
    }
    Found f;
    f.path = best->path;
    f.index = best->index;
    f.family = best->family;
    f.realBold = best->bold;
    f.realItalic = best->italic;
    f.bold = bold;
    f.italic = italic;
    return f;
}

Found fromIndex(const std::pair<QString, QString> &hit, bool bold, bool italic)
{
    Found f;
    f.path = hit.first;
    f.family = hit.second;
    const auto st = g_index.styles.value(hit.first);
    f.realBold = st.first;
    f.realItalic = st.second;
    f.bold = bold || st.first;
    f.italic = italic || st.second;
    return f;
}

QStringList lookAlikes(const QString &k, bool mono, bool serif)
{
    static const std::vector<std::pair<QStringList, QStringList>> table = {
        {{"calibri"}, {"calibri", "carlito"}},
        {{"cambria"}, {"cambria", "caladea"}},
        {{"segoe"}, {"segoeui", "selawik"}},
        {{"comicsans"}, {"comicsansms", "comicneue"}},
        {{"aptos"}, {"aptos", "arial", "liberationsans"}},
        // (the URW clones TeX papers use: their Windows twins)
        {{"palladio", "urwpalladio", "palatino", "p052", "bookantiqua"}, {"palatinolinotype", "bookantiqua", "p052"}},
        {{"nimbusroman", "nimbusrom"}, {"timesnewroman", "liberationserif"}},
        {{"nimbussans", "nimbussan"}, {"arial", "liberationsans"}},
        {{"nimbusmono", "nimbusmon"}, {"couriernew", "liberationmono"}},
        {{"arial", "helvetica", "arimo", "liberationsans"}, {"arial", "liberationsans"}},
        {{"times", "tinos", "liberationserif"}, {"timesnewroman", "liberationserif"}},
        {{"courier", "cousine", "liberationmono"}, {"couriernew", "liberationmono", "courierprime"}},
        {{"georgia"}, {"georgia", "ptserif", "liberationserif"}},
        {{"verdana", "tahoma", "trebuchet"}, {"verdana", "tahoma", "dejavusans", "liberationsans"}},
    };
    for (const auto &[names, alikes] : table)
        for (const QString &n : names)
            if (k.startsWith(n))
                return alikes;
    if (mono)
        return {"couriernew", "liberationmono", "dejavusansmono"};
    if (serif)
        return {"timesnewroman", "liberationserif", "dejavuserif"};
    return {"arial", "liberationsans", "dejavusans"};
}

FT_Face faceFor(const Found &f)
{
    if (!g_lib && FT_Init_FreeType(&g_lib))
        return nullptr;
    const QString k = f.path + QLatin1Char('#') + QString::number(f.index);
    auto it = g_faces.find(k);
    if (it != g_faces.end())
        return *it;
    FT_Face face = nullptr;
    if (FT_New_Face(g_lib, f.path.toUtf8().constData(), f.index, &face))
        face = nullptr;
    g_faces.insert(k, face);
    return face;
}

}  // namespace

QString bundledFolder()
{
#ifdef Q_OS_MACOS
    return QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/fonts");  // (in the app's bundle)
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/fonts");
#endif
}

QString myFolder()
{
    return qEnvironmentVariable("APPDATA") + QStringLiteral("/Master PDF/fonts");
}

QString embeddedFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/embedded fonts");
}

bool isEmbedded(const Found &f)
{
    return !f.path.isEmpty() && f.path.startsWith(embeddedFolder());
}

void addEmbedded(const QString &pdfFont, const QByteArray &program, int flags)
{
    // (TrueType / OpenType only: what's shaped and written with - a bare CFF or Type 1 program isn't)
    if (program.size() < 12)
        return;
    const QByteArray tag = program.left(4);
    if (tag != QByteArray("\x00\x01\x00\x00", 4) && tag != "OTTO" && tag != "true")
        return;
    waitScan();
    const QString dir = embeddedFolder();
    const QString path = dir + QLatin1Char('/') +
                         QString::fromLatin1(QCryptographicHash::hash(program, QCryptographicHash::Sha1).toHex().left(20)) +
                         (tag == "OTTO" ? QStringLiteral(".otf") : QStringLiteral(".ttf"));
    {
        std::lock_guard guard(g_indexMutex);
        for (const Embedded &e : g_embedded)
            if (e.path == path)
                return;
    }
    if (!QFileInfo::exists(path)) {
        QDir().mkpath(dir);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(program) != program.size())
            return;
    }
    // its names - and letters it can be asked for by (a font with no Unicode map: not used)
    FT_Library lib;
    if (FT_Init_FreeType(&lib))
        return;
    Embedded e;
    e.path = path;
    bool ok = false;
    FT_Face face;
    if (!FT_New_Face(lib, path.toUtf8().constData(), 0, &face)) {
        ok = FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0;
        QString full, style, ps;
        readNames(face, &full, &e.family, &style, &ps);
        e.names << full << ps;
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    if (!ok)
        return;
    // (as the PDF names it too: "ABCDEF+Aptos,Bold" - a subset's own names may be cut short)
    const QString base = pdfFont.section(QLatin1Char('+'), -1);
    if (e.family.isEmpty())
        e.family = base.section(QLatin1Char(','), 0, 0).section(QLatin1Char('-'), 0, 0);
    const QString k = key(base);
    e.bold = (flags & 1) || k.contains("bold") || k.contains("black") || k.contains("heavy");
    e.italic = (flags & 2) || k.contains("italic");
    e.names << base;
    std::lock_guard guard(g_indexMutex);
    g_embedded.push_back(e);
    addToIndex(g_index, e);
}

void startScan()
{
    std::call_once(g_started, [] { g_scan = std::async(std::launch::async, scan).share(); });
}

void waitScan()
{
    startScan();
    g_scan.wait();
}

void rescan()
{
    waitScan();
    scan();
}

QString key(const QString &name)
{
    static const QRegularExpression notAlnum(QStringLiteral("[^a-z0-9]"));
    QString n = name.section(QLatin1Char('+'), -1).toLower();
    n.remove(notAlnum);
    n.replace(QLatin1String("oblique"), QLatin1String("italic"));
    n.remove(QLatin1String("regular"));
    n.remove(QLatin1String("normal"));
    if (n.endsWith(QLatin1String("mt")))
        n.chop(2);
    if (n.endsWith(QLatin1String("ps")))
        n.chop(2);
    static const QRegularExpression ps(QStringLiteral("ps(?=bold|italic|$)"));
    n.remove(ps);
    return n;
}

Found findFont(const QString &pdfFont, int flags)
{
    waitScan();
    std::lock_guard guard(g_indexMutex);
    const QString k = key(pdfFont);
    const QString raw = pdfFont.section(QLatin1Char('+'), -1).remove(QLatin1Char(' ')).toLower();
    // the very font: as bold / italic as it really is - or as the page shows it (a regular font made bold
    // by its outline, as a PDF without the bold font does: bold, made so)
    auto shownAs = [flags](Found f) {
        f.bold = f.bold || (flags & 16);  // (16: its letters outlined on the page - not just a font marked bold)
        return f;
    };
    if (g_index.byKey.contains(k))
        return shownAs(fromIndex(g_index.byKey.value(k), false, false));
    if (raw.size() >= 31 && g_index.cut.contains(raw.left(31)))
        return shownAs(fromIndex(g_index.cut.value(raw.left(31)), false, false));
    // a family, and a style after it ("Century-Italic")
    QString bestFamily;
    bool fb = false, fi = false;
    for (const auto &[fk, members] : g_index.families) {
        if (fk == k) {  // (just the family's name - "Calibri": as bold / italic as the font says it is)
            Found f = familyFont(fk, flags & 1, flags & 2);
            f.bold = f.bold || (flags & 1);
            f.italic = f.italic || (flags & 2);
            return f;
        }
        if (!k.startsWith(fk) || fk.size() <= bestFamily.size())
            continue;
        const QString rest = k.mid(fk.size());
        if (rest == "bi" || QStringLiteral("bold").startsWith(rest) || QStringLiteral("italic").startsWith(rest) ||
            QStringLiteral("bolditalic").startsWith(rest)) {
            bestFamily = fk;
            fb = rest.startsWith(QLatin1Char('b'));
            fi = rest.startsWith(QLatin1Char('i')) || rest == "bi" || rest.size() > 4;
        }
    }
    if (!bestFamily.isEmpty()) {
        Found f = familyFont(bestFamily, fb, fi);
        f.bold = fb || f.realBold;
        f.italic = fi || f.realItalic;
        return f;
    }
    const bool bold = (flags & 1) || k.contains("bold") || k.contains("black") || k.contains("heavy");
    const bool italic = (flags & 2) || k.contains("italic");
    const QString style = QString(bold ? "bold" : "") + (italic ? "italic" : "");
    static const QRegularExpression tail(QStringLiteral("(semibold|bold|black|heavy|italic|light|medium)+$"));
    QString plain = k;
    plain.remove(tail);
    for (const QString &kk : {plain + style, plain})
        if (g_index.byKey.contains(kk))
            return fromIndex(g_index.byKey.value(kk), bold, italic);
    static const QStringList sansWords = {"sans", "aptos", "calibri", "arial", "helvetica", "segoe", "verdana", "tahoma",
                                          "roboto", "inter", "lato", "gothic", "grotesk", "frutiger", "univers", "myriad"};
    bool sans = false;
    for (const QString &w : sansWords)
        sans = sans || k.contains(w);
    const bool serif = !sans && ((flags & 4) || k.contains("times") || k.contains("serif") || k.contains("georgia") ||
                                 k.contains("garamond") || k.contains("cambria") || k.contains("roman") || k.contains("book"));
    static const QRegularExpression monoRe(QStringLiteral("courier|mono(?!type)|consol"));
    const bool mono = (flags & 8) || monoRe.match(k).hasMatch();
    for (const QString &family : lookAlikes(k, mono, serif)) {
        for (const QString &kk : {family + style, family})
            if (g_index.byKey.contains(kk))
                return fromIndex(g_index.byKey.value(kk), bold, italic);
        Found f = familyFont(family, bold, italic);
        if (!f.path.isEmpty())
            return f;
    }
    Found none;
    none.family = mono ? "Courier New" : serif ? "Times New Roman" : "Arial";
    none.bold = bold;
    none.italic = italic;
    return none;
}

Found findFamily(const QString &family, bool bold, bool italic)
{
    waitScan();
    std::lock_guard guard(g_indexMutex);
    Found f = familyFont(key(family), bold, italic);
    if (!f.path.isEmpty())
        return f;
    f = findFont(family + (bold ? "Bold" : "") + (italic ? "Italic" : ""), (bold ? 1 : 0) | (italic ? 2 : 0));
    f.bold = bold;
    f.italic = italic;
    return f;
}

QStringList families()
{
    waitScan();
    std::lock_guard guard(g_indexMutex);
    QStringList out;
    for (const auto &[k, members] : g_index.families)
        if (!members.empty())
            out << members.front().family;
    out.removeDuplicates();
    out.sort(Qt::CaseInsensitive);
    return out;
}

bool hasLetters(const Found &f, const QString &text)
{
    if (f.path.isEmpty())
        return false;
    std::lock_guard guard(g_faceMutex);
    FT_Face face = faceFor(f);
    if (!face)
        return false;
    for (uint c : text.toUcs4())
        if (!QChar::isSpace(c) && !FT_Get_Char_Index(face, c))
            return false;
    return true;
}

Found forText(Found f, const QString &text)
{
    if (hasLetters(f, text))
        return f;
    // (Arial has most of the world's letters - or the app's own fonts)
    for (const char *name : {"Arial", "Liberation Sans", "DejaVu Sans", "Amiri", "Tajawal"}) {
        Found other = findFamily(QString::fromLatin1(name), f.bold, f.italic);
        if (!other.path.isEmpty() && key(other.family).startsWith(key(QString::fromLatin1(name))) && hasLetters(other, text))
            return other;
    }
    return f;
}

}  // namespace fonts
