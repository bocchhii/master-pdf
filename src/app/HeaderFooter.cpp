// Pages > Header & Footer: the text at the top and the bottom of the pages - found where the PDF has it already (the
// same text on page after page, its page number changing), changed here, put on pages that have none, or taken off

#include "app/MainWindow.h"

#include "core/Document.h"
#include "core/Fonts.h"
#include "ui/Sounds.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDate>
#include <QFontDatabase>
#include <QFileInfo>
#include <QFontComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace {

constexpr double kBand = 0.12;  // (the top and the bottom of a page where they're looked for: this part of its height)

// a header's or footer's text found: where (top / bottom; left, middle, right), as typed (its page numbers {page},
// the page count {pages}) - and its lines on each page
struct Found {
    bool top = true;
    int slot = 1;  // (0 left, 1 middle, 2 right)
    double edge = 36;  // (how far from the page's top / bottom edge - points)
    QString text;
    QString key;   // (its words, numbers as #: the same on each page)
    double y = 0;  // (where it is: its top - from the bottom, for a footer)
    std::map<int, std::vector<TextLine>> lines;
    QString family;
    double size = 10;
    bool arabicDigits = false;
};

int slotOf(const QRectF &r, double width)
{
    const double middle = r.center().x() - width / 2;
    if (std::abs(middle) < width * 0.12)
        return 1;
    return middle < 0 ? 0 : 2;
}

// its numbers: each run of digits (Arabic's too) - its value and where
struct Number {
    int at, length, value;
};
std::vector<Number> numbersIn(const QString &text)
{
    std::vector<Number> out;
    for (int i = 0; i < text.size();) {
        if (text[i].digitValue() < 0) {
            ++i;
            continue;
        }
        int v = 0, j = i;
        for (; j < text.size() && text[j].digitValue() >= 0; ++j)
            v = v * 10 + text[j].digitValue();
        out.push_back({i, j - i, v});
        i = j;
    }
    return out;
}

QString keyOf(const QString &text)
{
    QString k = text.simplified();
    static const QRegularExpression digits(QStringLiteral("\\d+"));
    k.replace(digits, QStringLiteral("#"));
    return k;
}

// a base font's name, as Qt knows it ("ABCDEF+Arial-BoldMT" -> "Arial")
QString familyOf(const QString &pdfFont)
{
    QString f = pdfFont.section(QLatin1Char('+'), -1);
    f = f.section(QLatin1Char('-'), 0, 0).section(QLatin1Char(','), 0, 0);
    f.remove(QRegularExpression(QStringLiteral("(MT|PS|PSMT)$")));
    static const QRegularExpression camel(QStringLiteral("([a-z])([A-Z])"));
    f.replace(camel, QStringLiteral("\\1 \\2"));
    return f;
}

// the headers and footers of the pages: text in their top and bottom bands, the same on most of them (one page: what's
// near its top and bottom edges)
std::vector<Found> findHeaders(Document *doc)
{
    const int count = doc->pageCount();
    const int sample = std::min(count, 60);
    std::vector<Found> groups;
    for (int p = 0; p < sample; ++p) {
        const QSizeF size = doc->pageSize(p);
        const double band = count == 1 ? 0.08 : kBand;
        for (const TextLine &l : doc->lines(p)) {
            if (l.text.trimmed().isEmpty() || std::abs(l.angle) > 0.01)
                continue;
            const bool top = l.rect.bottom() <= size.height() * band;
            const bool bottom = l.rect.top() >= size.height() * (1 - band);
            if (!top && !bottom)
                continue;
            const int slot = slotOf(l.rect, size.width());
            const QString key = keyOf(l.text);
            const double y = top ? l.rect.top() : size.height() - l.rect.bottom();
            Found *g = nullptr;
            for (Found &f : groups)
                if (f.top == top && f.slot == slot && f.key == key && std::abs(f.y - y) < 6) {
                    g = &f;
                    break;
                }
            if (!g) {
                Found f;
                f.top = top;
                f.slot = slot;
                f.key = key;
                f.y = y;
                f.edge = y;
                f.text = l.text;
                f.size = l.mainRun().size;
                f.family = familyOf(l.mainRun().font);
                groups.push_back(f);
                g = &groups.back();
            }
            g->lines[p].push_back(l);
        }
    }
    // (on most of the pages - or the one page there is)
    const size_t enough = count == 1 ? 1 : size_t(std::max(2, int(std::ceil(sample * 0.5))));
    // (one page: nothing to tell a footer from the page's own text by - just what reads as its number: "1", "Page 1 of
    // 1", "- 1 -", "صفحة ١")
    static const QRegularExpression pageNumber(QStringLiteral("^\\W*(\\w+\\W*){0,1}#(\\W+(\\w+\\W+)?#)?\\W*$"),
                                               QRegularExpression::UseUnicodePropertiesOption);
    std::vector<bool> kept(groups.size());
    for (size_t i = 0; i < groups.size(); ++i)
        kept[i] = groups[i].lines.size() >= enough && (count > 1 || pageNumber.match(groups[i].key).hasMatch());
    // (or the page's number, on a few of them: short, its number going up with the page - "Page | 17", "P a g e | 1")
    for (size_t i = 0; i < groups.size(); ++i) {
        const Found &g = groups[i];
        if (kept[i] || count == 1 || g.lines.size() < 2 || g.key.size() > 20)
            continue;
        std::optional<int> offset;
        bool going = true;
        for (const auto &[p, ls] : g.lines) {
            const std::vector<Number> n = numbersIn(ls.front().text);
            going = going && n.size() == 1 && (!offset || n[0].value - p == *offset);
            if (!n.empty())
                offset = n[0].value - p;
        }
        kept[i] = going;
    }
    // (and what's in the same row as one of those, on its pages - a part's own title beside the footer, its page
    // number: a PDF put together from several, each with its own - as small as they are, and short: not the page's own
    // text come up to its top)
    for (bool more = count > 1; more;) {
        more = false;
        for (size_t i = 0; i < groups.size(); ++i) {
            if (kept[i])
                continue;
            double wide = 0;
            for (const auto &pl : groups[i].lines)
                wide = std::max(wide, pl.second.front().rect.width() / doc->pageSize(pl.first).width());
            for (size_t j = 0; j < groups.size() && !kept[i] && wide < 0.45; ++j) {
                if (!kept[j] || groups[j].top != groups[i].top || std::abs(groups[j].y - groups[i].y) >= 6 ||
                    std::abs(groups[j].size - groups[i].size) > 0.6)
                    continue;
                size_t shared = 0;
                for (const auto &pl : groups[i].lines)
                    shared += groups[j].lines.count(pl.first);
                kept[i] = more = shared == groups[i].lines.size();
            }
        }
    }
    std::vector<Found> out;
    for (size_t i = 0; i < groups.size(); ++i) {
        Found &f = groups[i];
        if (!kept[i])
            continue;
        // its numbers: the page's own ({page}), the page count ({pages}) - or as they are
        const std::vector<Number> first = numbersIn(f.lines.begin()->second.front().text);
        std::vector<int> pageOffset(first.size(), 1 << 30);
        std::vector<bool> isPage(first.size(), true), isCount(first.size(), true);
        for (const auto &[p, ls] : f.lines) {
            const std::vector<Number> n = numbersIn(ls.front().text);
            for (size_t k = 0; k < first.size(); ++k) {
                if (k >= n.size()) {
                    isPage[k] = isCount[k] = false;
                    continue;
                }
                if (pageOffset[k] == 1 << 30)
                    pageOffset[k] = n[k].value - p;
                isPage[k] = isPage[k] && n[k].value - p == pageOffset[k];
                isCount[k] = isCount[k] && n[k].value == count;
            }
        }
        QString text = f.lines.begin()->second.front().text;
        for (size_t k = first.size(); k-- > 0;) {
            const bool page = isPage[k] && f.lines.size() > 1, all = isCount[k] && !page;
            if (page || all) {
                f.arabicDigits = f.arabicDigits || text[first[k].at].unicode() >= 0x0660;
                text.replace(first[k].at, first[k].length, page ? QStringLiteral("{page}") : QStringLiteral("{pages}"));
            }
        }
        f.text = text.simplified();
        out.push_back(f);
    }
    return out;
}

// a header's or footer's part on one page, as found (its lines top down; none: empty)
QString slotText(const std::vector<Found> &found, bool top, int slot, int page)
{
    std::vector<const Found *> here;
    for (const Found &f : found)
        if (f.top == top && f.slot == slot && f.lines.count(page))
            here.push_back(&f);
    std::sort(here.begin(), here.end(), [](const Found *x, const Found *y) { return x->top ? x->y < y->y : x->y > y->y; });
    QStringList text;
    for (const Found *f : here)
        text << f->text;
    return text.join(QLatin1Char('\n'));
}

QString digitsAs(const QString &number, bool arabic)
{
    if (!arabic)
        return number;
    QString out = number;
    for (QChar &c : out)
        if (c.isDigit())
            c = QChar(char16_t(0x0660 + c.digitValue()));
    return out;
}

}  // namespace

void MainWindow::headerFooterDialog()
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    const int count = doc->pageCount();
    setCursor(Qt::WaitCursor);
    const std::vector<Found> found = findHeaders(doc.get());
    unsetCursor();

    ClassicDialog dlg(this, tr("Header & Footer"));
    QWidget *body = dlg.body();
    body->setFont(theme::font());
    auto *page = new QVBoxLayout(body);
    page->setContentsMargins(12, 10, 12, 10);
    page->setSpacing(8);
    {
        int heads = 0, foots = 0;
        for (const Found &f : found)
            (f.top ? heads : foots) += 1;
        auto *said = new QLabel(found.empty() ? tr("This PDF has no header or footer yet: type one to put it on its pages.")
                                              : tr("Found in this PDF: %1. Change it here - it's put back on its pages as you type it, "
                                                   "or Remove takes it off.")
                                                    .arg(heads && foots ? tr("a header and a footer") : heads ? tr("a header") : tr("a footer")),
                                body);
        said->setWordWrap(true);
        page->addWidget(said);
    }
    // the header's and the footer's text: left, middle, right (more than one line: Enter)
    // (not the same on every page - each part of the PDF its own: the one on this page shown, else the most common)
    QPlainTextEdit *fields[2][3];
    const int shownPage = view_->currentPage();
    for (int band = 0; band < 2; ++band) {
        auto *g = new QGroupBox(band == 0 ? tr(" Header ") : tr(" Footer "), body);
        auto *grid = new QGridLayout(g);
        grid->setHorizontalSpacing(8);
        const QString names[3] = {tr("Left"), tr("Middle"), tr("Right")};
        for (int s = 0; s < 3; ++s) {
            grid->addWidget(new QLabel(names[s], g), 0, s);
            auto *e = new QPlainTextEdit(g);
            e->setFont(theme::font());
            e->setTabChangesFocus(true);
            e->setFixedHeight(QFontMetrics(theme::font()).lineSpacing() * 2 + 12);
            e->setMinimumWidth(150);
            std::map<QString, int> seen;  // (each text: on how many pages)
            for (int p = 0; p < count && !found.empty(); ++p)
                if (const QString t = slotText(found, band == 0, s, p); !t.isEmpty())
                    ++seen[t];
            QString text = slotText(found, band == 0, s, shownPage);
            if (text.isEmpty())
                for (const auto &[t, pages] : seen)
                    if (text.isEmpty() || pages > seen[text])
                        text = t;
            e->setPlainText(text);
            if (s > 0)  // (typed where it goes: in the middle, at the right)
                e->document()->setDefaultTextOption(QTextOption(s == 1 ? Qt::AlignHCenter : Qt::AlignRight));
            grid->addWidget(e, 1, s);
            fields[band][s] = e;
        }
        page->addWidget(g);
    }
    // what can be put in: the page's number, the page count, today's date, the file's name - where the caret is
    {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Put in:"), body));
        QPlainTextEdit *last = fields[1][1];
        for (int b = 0; b < 2; ++b)
            for (int s = 0; s < 3; ++s)
                connect(fields[b][s], &QPlainTextEdit::cursorPositionChanged, &dlg, [&last, e = fields[b][s]] { last = e; });
        const std::pair<QString, QString> tokens[] = {{tr("Page number"), QStringLiteral("{page}")},
                                                      {tr("Page count"), QStringLiteral("{pages}")},
                                                      {tr("Date"), QStringLiteral("{date}")},
                                                      {tr("File name"), QStringLiteral("{file}")}};
        for (const auto &[label, token] : tokens) {
            auto *b = new ClassicButton(label, body);
            connect(b, &QPushButton::clicked, &dlg, [&last, t = token] {
                last->insertPlainText(t);
                last->setFocus();
            });
            row->addWidget(b);
        }
        row->addStretch();
        page->addLayout(row);
    }
    // its look, its place, its pages
    auto *look = new QGroupBox(tr(" Look and place "), body);
    auto *lg = new QGridLayout(look);
    lg->setHorizontalSpacing(8);
    auto *family = new QFontComboBox(look);
    auto *size = new QSpinBox(look);
    size->setRange(5, 72);
    auto *digits = new QComboBox(look);
    digits->addItem(tr("1 2 3"));
    digits->addItem(QStringLiteral("١ ٢ ٣"));
    auto *topEdge = new QSpinBox(look), *bottomEdge = new QSpinBox(look), *sides = new QSpinBox(look);
    for (QSpinBox *b : {topEdge, bottomEdge, sides}) {
        b->setRange(0, 300);
        b->setSuffix(tr(" pt"));
    }
    auto *startAt = new QSpinBox(look);
    startAt->setRange(-9999, 99999);
    // (as found - else the usual: Arial 10, half an inch from the edges, an inch from the sides)
    QString fam = QStringLiteral("Arial");
    double sz = 10, topE = 36, bottomE = 36, side = 72;
    int start = 1;
    bool arabic = false;
    for (const Found &f : found) {
        if (!f.family.isEmpty() && QFontDatabase::families().contains(f.family))
            fam = f.family;
        sz = f.size;
        (f.top ? topE : bottomE) = f.edge;
        arabic = arabic || f.arabicDigits;
        if (f.slot == 0)
            side = std::max(18.0, f.lines.begin()->second.front().rect.left());
    }
    for (const Found &f : found)  // (the page numbers as found: from what)
        if (f.text.contains(QStringLiteral("{page}"))) {
            const auto &[p, ls] = *f.lines.begin();
            const std::vector<Number> n = numbersIn(ls.front().text);
            if (!n.empty())
                start = n.front().value - p;  // (the first number on its first page: the page's own)
            break;
        }
    family->setCurrentFont(QFont(fam));
    size->setValue(int(std::lround(sz)));
    digits->setCurrentIndex(arabic ? 1 : 0);
    topEdge->setValue(int(std::lround(topE)));
    bottomEdge->setValue(int(std::lround(bottomE)));
    sides->setValue(int(std::lround(side)));
    startAt->setValue(start);
    int r = 0;
    lg->addWidget(new QLabel(tr("Font:"), look), r, 0);
    lg->addWidget(family, r, 1);
    lg->addWidget(new QLabel(tr("Size:"), look), r, 2);
    lg->addWidget(size, r, 3);
    ++r;
    lg->addWidget(new QLabel(tr("Header from the top:"), look), r, 0);
    lg->addWidget(topEdge, r, 1);
    lg->addWidget(new QLabel(tr("Numbers:"), look), r, 2);
    lg->addWidget(digits, r, 3);
    ++r;
    lg->addWidget(new QLabel(tr("Footer from the bottom:"), look), r, 0);
    lg->addWidget(bottomEdge, r, 1);
    lg->addWidget(new QLabel(tr("First page's number:"), look), r, 2);
    lg->addWidget(startAt, r, 3);
    ++r;
    lg->addWidget(new QLabel(tr("From the sides:"), look), r, 0);
    lg->addWidget(sides, r, 1);
    page->addWidget(look);
    auto *pagesBox = new QGroupBox(tr(" Pages "), body);
    auto *pl = new QHBoxLayout(pagesBox);
    auto *all = new QRadioButton(tr("All"), pagesBox);
    auto *range = new QRadioButton(tr("From"), pagesBox);
    all->setChecked(true);
    auto *from = new QSpinBox(pagesBox), *to = new QSpinBox(pagesBox);
    from->setRange(1, count);
    to->setRange(1, count);
    from->setValue(1);
    to->setValue(count);
    pl->addWidget(all);
    pl->addSpacing(12);
    pl->addWidget(range);
    pl->addWidget(from);
    pl->addWidget(new QLabel(tr("to"), pagesBox));
    pl->addWidget(to);
    pl->addStretch();
    page->addWidget(pagesBox);

    auto *buttons = new QHBoxLayout;
    auto *remove = new ClassicButton(tr("Remove"), body);
    remove->setEnabled(!found.empty());
    buttons->addWidget(remove);
    buttons->addStretch();
    auto *ok = new ClassicButton(tr("OK"), body);
    auto *cancel = new ClassicButton(tr("Cancel"), body);
    for (ClassicButton *b : {remove, ok, cancel})
        b->setMinimumWidth(75);
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    page->addLayout(buttons);
    // (as the box opened: what's left so is left on the pages as it is - its own place, its own look)
    QString was[2][3];
    for (int b = 0; b < 2; ++b)
        for (int s = 0; s < 3; ++s)
            was[b][s] = fields[b][s]->toPlainText().trimmed();
    auto lookNow = [&] {
        return QStringList{family->currentFont().family(), QString::number(size->value()), QString::number(digits->currentIndex()),
                           QString::number(topEdge->value()), QString::number(bottomEdge->value()), QString::number(sides->value()),
                           QString::number(startAt->value())}.join(QLatin1Char('|'));
    };
    const QString lookWas = lookNow();
    int choice = 0;  // (1: OK, 2: Remove)
    connect(ok, &QPushButton::clicked, &dlg, [&] {
        choice = 1;
        dlg.close();
    });
    connect(remove, &QPushButton::clicked, &dlg, [&] {
        choice = 2;
        dlg.close();
    });
    connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    fields[0][1]->setFocus();
    dlg.exec();
    if (choice == 0)
        return;

    // put on (or taken off) the pages chosen: what was found taken off first, then the new written - one Undo step
    const int first = all->isChecked() ? 0 : std::min(from->value(), to->value()) - 1;
    const int last = all->isChecked() ? count - 1 : std::max(from->value(), to->value()) - 1;
    QString texts[2][3];
    bool redo[2][3];  // (taken off and written again: changed - or all of them, its look or place changed; Remove: all)
    const bool lookChanged = lookNow() != lookWas;
    for (int b = 0; b < 2; ++b)
        for (int s = 0; s < 3; ++s) {
            texts[b][s] = choice == 1 ? fields[b][s]->toPlainText().trimmed() : QString();
            redo[b][s] = choice != 1 || lookChanged || texts[b][s] != was[b][s];
        }
    const QString fontFamily = family->currentFont().family();
    const double fontSize = size->value();
    const bool arabicNumbers = digits->currentIndex() == 1;
    const QString date = QLocale().toString(QDate::currentDate(), QLocale::ShortFormat);
    const QString file = QFileInfo(doc->path()).completeBaseName();
    QString error;
    bool okAll = true;
    setCursor(Qt::WaitCursor);
    doc->beginChanges(choice == 1 ? "header and footer" : "remove header and footer");
    for (int p = first; p <= last && okAll; ++p) {
        // (what was found on it: taken off - found again on this page, as its text is now)
        std::vector<TextLine> old;
        std::vector<const Found *> own[2][3];  // (and whose it was)
        if (!found.empty()) {
            for (const TextLine &l : doc->lines(p)) {
                const QSizeF ps = doc->pageSize(p);
                const bool top = l.rect.bottom() <= ps.height() * (count == 1 ? 0.08 : kBand);
                const bool bottom = l.rect.top() >= ps.height() * (1 - (count == 1 ? 0.08 : kBand));
                if (!top && !bottom)
                    continue;
                const QString key = keyOf(l.text);
                const double y = top ? l.rect.top() : ps.height() - l.rect.bottom();
                for (const Found &f : found)
                    if (f.top == top && f.key == key && std::abs(f.y - y) < 6) {
                        if (redo[top ? 0 : 1][f.slot])
                            old.push_back(l);
                        auto &mine = own[top ? 0 : 1][f.slot];
                        if (std::find(mine.begin(), mine.end(), &f) == mine.end())
                            mine.push_back(&f);
                        break;
                    }
            }
        }
        if (!old.empty())
            okAll = doc->deleteLines(p, old, &error);
        if (choice != 1)
            continue;
        const QSizeF ps = doc->pageSize(p);
        for (int b = 0; b < 2 && okAll; ++b)
            for (int s = 0; s < 3 && okAll; ++s) {
                QString text = texts[b][s];
                if (text == was[b][s]) {  // (left as it was: each page its own - on the pages it was on)
                    auto &mine = own[b][s];
                    std::sort(mine.begin(), mine.end(), [](const Found *x, const Found *y) { return x->top ? x->y < y->y : x->y > y->y; });
                    QStringList rows;
                    for (const Found *f : mine)
                        rows << f->text;
                    text = rows.join(QLatin1Char('\n'));
                }
                if (text.isEmpty() || !redo[b][s])
                    continue;
                text.replace(QStringLiteral("{page}"), digitsAs(QString::number(p + startAt->value()), arabicNumbers));
                text.replace(QStringLiteral("{pages}"), digitsAs(QString::number(count), arabicNumbers));
                text.replace(QStringLiteral("{date}"), date);
                text.replace(QStringLiteral("{file}"), file);
                // (its widest line: where it starts - at the left, in the middle, at the right)
                const QStringList rows = text.split(QLatin1Char('\n'));
                double wide = 0;
                for (const QString &row : rows)
                    wide = std::max(wide, doc->textWidth(row, fonts::forText(fonts::findFamily(fontFamily, false, false), row), fontSize));
                const double x = s == 0 ? sides->value() : s == 1 ? (ps.width() - wide) / 2 : ps.width() - sides->value() - wide;
                const double tall = fontSize * 1.2 * rows.size();
                const double y = b == 0 ? topEdge->value() : ps.height() - bottomEdge->value() - tall;
                const std::vector<QRectF> boxes =
                    doc->addText(p, QPointF(x, y), text, fontFamily, fontSize, Qt::black, false, false,
                                 s == 0 ? QStringLiteral("left") : s == 1 ? QStringLiteral("center") : QStringLiteral("right"), QString(), &error);
                okAll = !boxes.empty();
            }
    }
    doc->endChanges();
    unsetCursor();
    annotChanged(okAll, choice == 1 ? tr("put the header and footer on") : tr("take the header and footer off"), error);
    if (okAll)
        say(choice == 1 ? tr("Header and footer put on pages %1 to %2.").arg(first + 1).arg(last + 1)
                        : tr("Header and footer taken off pages %1 to %2.").arg(first + 1).arg(last + 1));
}
