#include "app/MainWindow.h"
#include "app/HomePage.h"
#include "app/OrganizeView.h"
#include "ui/DocTabs.h"
#include "ui/WhatsThis.h"

#include "core/Document.h"
#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/ScrollBox.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"
#include "ui/Widgets.h"
#include "ui/Win98Style.h"
#include "view/PageView.h"
#include "view/SearchResults.h"
#include "view/ThumbnailBar.h"

#include <QCheckBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QThreadPool>
#include <QTimer>
#include <QAbstractButton>
#include <QVBoxLayout>
#include <QWhatsThis>

#include <cmath>

namespace {

// the Custom theme's brush in the Theme list: just the brush, no button round it - pressed, it goes in as a button
// does, a pixel down and right
class BrushButton : public QAbstractButton {
public:
    using QAbstractButton::QAbstractButton;

protected:
    void paintEvent(QPaintEvent *) override
    {
        static const QPixmap brush(QStringLiteral(":/icons/brush.png"));
        QPainter p(this);
        const int in = isDown() ? 1 : 0;
        p.drawPixmap((width() - brush.width()) / 2 + in, (height() - brush.height()) / 2 + in, brush);
    }
};

constexpr int kMaxHits = 2000;  // (enough to go through)
constexpr int kSideWidth = 150;
constexpr int kSideMin = 110;

// Every place text is in the PDF, page by page; stops early (empty) if serial moves on
std::vector<FindHit> findAll(const std::shared_ptr<Document> &doc, const QString &text,
                             const std::shared_ptr<std::atomic<int>> &serial, int mine)
{
    std::vector<FindHit> hits;
    for (int i = 0; doc && i < doc->pageCount(); ++i) {
        if (serial->load() != mine)
            return {};
        std::vector<FindHit> found = pagetext::find(*doc->text(i), i, text);
        hits.insert(hits.end(), found.begin(), found.end());
        if (int(hits.size()) > kMaxHits)
            break;
    }
    return hits;
}

// (key, what it's called, What's This? note) - the Python app's TOOLS
struct Tool {
    const char *key, *icon, *name, *note;
    bool split;
    int whiten;
};
const Tool kTools[] = {
    {"move", "move", "Move", "Move: drag to move around the pages; click a text, note or drawing to pick it up.", false, 0},
    {"edit", "edit", "Edit text & pictures", "Edit text & pictures: click a line of the PDF's own text to pick it up and retype it; drag a picture to move it.", false, 0},
    {nullptr, nullptr, nullptr, nullptr, false, 0},
    {"text", "text_tool", "Add text", "Add text: click on the page where the text goes, then type.", false, 16},
    {"highlight", "highlight_tool", "Marker", "Marker: drag to draw anywhere on the page like a highlighter pen.", false, 0},
    {"pen", "pen_tool", "Draw", "Draw: drag on the page to draw freehand.", false, 0},
    {nullptr, nullptr, nullptr, nullptr, false, 0},
    {"eraser", "eraser_tool", "Eraser", "Eraser: White-out covers what's there with a box; Eraser rubs out drawings.", true, 0},
    {"image", "image_tool", "Image", "Image: click where a picture goes, or drag a box for it.", true, 0},
    {"shape", "shape", "Shape", "Shape: drag on the page to draw a rectangle, an ellipse, a line or an arrow.", true, 0},
    {nullptr, nullptr, nullptr, nullptr, false, 0},
    {"sign", "sign", "Sign", "Sign: click where your signature goes.", true, 16},
    {"link", "link", "Link", "Link: drag a box over what should become a link.", false, 16},
    {"annots", "annots", "Annotations", "Shows the list of annotations (text, highlights, drawings, notes) at the left - click one to go to it.", false, 0},
};

const char *kSelectHint = "Drag across text to select it (then copy or mark it). Choose a tool above to "
                          "add or change things.";

// A page's size as people know it: "A4", "Letter (sideways)", or "210 x 99 mm"
QString pageSizeText(double w, double h)
{
    static const std::pair<const char *, QSizeF> sizes[] = {
        {"A4", {595.28, 841.89}}, {"Letter", {612.0, 792.0}}, {"A5", {419.53, 595.28}}, {"Legal", {612.0, 1008.0}}};
    for (const auto &[name, s] : sizes) {
        if (std::abs(w - s.width()) < 2 && std::abs(h - s.height()) < 2)
            return QString::fromLatin1(name);
        if (std::abs(w - s.height()) < 2 && std::abs(h - s.width()) < 2)
            return QStringLiteral("%1 (sideways)").arg(QLatin1String(name));
    }
    return QStringLiteral("%1 x %2 mm").arg(std::lround(w * 25.4 / 72)).arg(std::lround(h * 25.4 / 72));
}

// the width of a Tk widget that's n characters wide
int chars(int n)
{
    return QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0')) * n;
}

// Read mode's picture: an open book, pixel by pixel - black lines on white pages; a dark theme's: inverted
QImage bookIcon(bool dark)
{
    static const char *rows[] = {"................", "................", ".kkkkkk..kkkkkk.", "kWWWWWWkkWWWWWWk", "kWkkkkWkkWkkkkWk",
                                 "kWWWWWWkkWWWWWWk", "kWkkkkWkkWkkkkWk", "kWWWWWWkkWWWWWWk", "kWkkkkWkkWkkkkWk", "kWWWWWWkkWWWWWWk",
                                 "kWkkkWWkkWkkkWWk", "kWWWWWWkkWWWWWWk", ".kkkkkkkkkkkkkk.", "................"};
    const QRgb ink = dark ? qRgb(255, 255, 255) : qRgb(0, 0, 0), paper = dark ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
    QImage book(16, 16, QImage::Format_ARGB32);
    book.fill(Qt::transparent);
    for (int y = 0; y < int(std::size(rows)); ++y)
        for (int x = 0; x < 16; ++x)
            if (rows[y][x] == 'k' || rows[y][x] == 'W')
                book.setPixel(x, y, rows[y][x] == 'k' ? ink : paper);
    return book;
}

QLabel *label(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setFont(theme::font());
    return l;
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : ClassicWindow(true, parent)
{
    setTitle(QStringLiteral("Master PDF"));
    setAcceptDrops(true);
    setMinimumSize(780, 520);
    resize(1040, 760);
    const QJsonObject set = theme::settings();
    applyIcon();

    auto *outer = new QVBoxLayout(body());
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    buildMenus();
    // the tabs - Home, then each PDF open - and under them Home, or the PDF's tools and pages
    tabs_ = new DocTabs(body());
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 2, 0, 0);
    tabRow->addWidget(tabs_);
    outer->addLayout(tabRow);
    pages_ = new QStackedWidget(body());
    home_ = new HomePage(pages_);
    pages_->addWidget(home_);
    docPage_ = new QWidget(pages_);
    pages_->addWidget(docPage_);
    outer->addWidget(pages_, 1);
    auto *box = new QVBoxLayout(docPage_);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    toolsBox_ = new QWidget(docPage_);
    {
        auto *tb = new QVBoxLayout(toolsBox_);
        tb->setContentsMargins(0, 0, 0, 0);
        tb->setSpacing(0);
        buildToolbars(tb);
    }
    box->addWidget(toolsBox_);

    // the middle: the side panel, the splitter, the pages
    view_ = new PageView(this);
    side_ = new QWidget(this);
    auto *sideBox = new QVBoxLayout(side_);
    sideBox->setContentsMargins(0, 0, 0, 0);
    sideBox->setSpacing(0);
    chooser_ = new ChooserButton(side_);
    chooser_->setWhatsThis(tr("Choose what the side panel shows: the pages, or Find's results."));
    connect(chooser_, &ChooserButton::pressed, this, &MainWindow::openChooser);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(1, 0, 0, 1);
    head->addWidget(chooser_);
    head->addStretch();
    sideBox->addLayout(head);
    sideStack_ = new QStackedWidget(side_);
    thumbs_ = new ThumbnailBar(sideStack_);
    thumbs_->setWhatsThis(tr("The pages: click one to go to it. The + between them puts pages in there."));
    sideStack_->addWidget(new ScrollBox(thumbs_, false, sideStack_));
    results_ = new SearchResults(sideStack_);
    sideStack_->addWidget(results_);
    sideBox->addWidget(sideStack_, 1);
    buildAnnotationList();
    buildBookmarkList();
    showSideView(0);

    viewBox_ = new ScrollBox(view_, true, this);
    view_->setWhatsThis(tr("The pages. Drag across text to select it; Ctrl + the mouse wheel zooms."));
    split_ = new QSplitter(Qt::Horizontal, this);
    split_->setOpaqueResize(false);  // (a dotted bar while it's dragged, like Windows 98's)
    split_->setChildrenCollapsible(false);
    split_->addWidget(side_);
    split_->addWidget(viewBox_);
    split_->setStretchFactor(1, 1);
    side_->setMinimumWidth(kSideMin);
    const int width = set.value("side_width").toInt(kSideWidth);
    split_->setSizes({std::clamp(width, kSideMin, 600), 1000});
    connect(split_, &QSplitter::splitterMoved, this,
            [this] { theme::saveSetting("side_width", side_->width()); });
    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(2, 2, 2, 0);
    middleStack_ = new QStackedWidget(this);
    middleStack_->addWidget(split_);
    organize_ = new OrganizeView(middleStack_);
    middleStack_->addWidget(organize_);
    middle->addWidget(middleStack_);
    box->addLayout(middle, 1);
    showThumbs_ = set.value("thumbnails").toBool(true);
    side_->setVisible(showThumbs_);

    start_ = new StartPanel(view_->viewport());
    start_->adjustSize();
    connect(start_->openButton, &QPushButton::clicked, this, &MainWindow::openDialog);
    connect(start_->createButton, &QPushButton::clicked, this, [this] { createFromFiles({}); });
    view_->viewport()->installEventFilter(this);

    buildStatusBar(outer);
    tabs_->addTab(tr("Home"), tr("Your recent and starred PDFs"), false);
    connect(tabs_, &DocTabs::currentChanged, this, &MainWindow::tabChosen);
    connect(tabs_, &DocTabs::closeRequested, this, [this](int i) { closeTab(i - 1); });
    connect(home_, &HomePage::openRequested, this, [this](const QString &path) { openFile(path); });
    connect(home_, &HomePage::openDialogRequested, this, &MainWindow::openDialog);
    connect(home_, &HomePage::createRequested, this, [this] { createFromFiles({}); });
    connect(home_, &HomePage::scanRequested, this, &MainWindow::scanFile);
    connect(home_, &HomePage::convertRequested, this, [this] { convertDialog(); });
    connect(home_, &HomePage::message, this, [this](const QString &text) {
        if (current_ < 0) {
            say(text);
            sizeLabel_->setText(home_->summary());
        }
    });
    tabs_->setCurrent(0);

    findTimer_ = new QTimer(this);
    findTimer_->setSingleShot(true);
    findTimer_->setInterval(250);  // (searched a moment after the typing stops)
    connect(findTimer_, &QTimer::timeout, this, [this] {
        // (in the background: a long PDF doesn't stop the typing)
        const QString text = findBox_->text().trimmed();
        const int mine = ++*findSerial_;
        if (!view_->document() || text.isEmpty()) {
            searchDone(text, {}, true);
            return;
        }
        auto doc = view_->document();
        auto serial = findSerial_;
        QPointer<MainWindow> self(this);
        QThreadPool::globalInstance()->start([=] {
            std::vector<FindHit> hits = findAll(doc, text, serial, mine);
            QMetaObject::invokeMethod(
                qApp, [=]() mutable {
                    if (self && serial->load() == mine && self->view_->document() == doc)
                        self->searchDone(text, std::move(hits), true);
                },
                Qt::QueuedConnection);
        });
    });

    connect(thumbs_, &ThumbnailBar::pageClicked, view_, &PageView::goToPage);
    connect(thumbs_, &ThumbnailBar::plusClicked, this, [this](int n, QPoint at) {
        QMenu menu(this);
        const bool can = view_->document() && view_->document()->editable();
        menu.addAction(tr("Insert pages from file..."), this, [this, n] { insertFromFile(n); })->setEnabled(can);
        menu.addAction(tr("Insert blank page"), this, [this, n] { insertBlank(n); })->setEnabled(can);
        menu.exec(at);
        thumbs_->plusClosed();
    });
    connect(thumbs_, &ThumbnailBar::menuRequested, this, [this](QPoint at) {
        QMenu menu(this);
        fillPagesMenu(&menu);
        menu.exec(at);
    });
    connect(thumbs_, &ThumbnailBar::movePages, this, &MainWindow::movePages);
    connect(thumbs_, &ThumbnailBar::deleteRequested, this, [this] { deletePages(thumbs_->selection()); });
    connect(results_, &SearchResults::resultClicked, this, [this](int k) { goResult(k); });
    connect(results_, &SearchResults::step, this, &MainWindow::findNext);
    connect(view_, &PageView::message, this, &MainWindow::say);
    connectTools();
    tintTools();
    // Organize Pages: what its buttons do - the Pages menu's own (the pages picked there, not the thumbnails')
    organize_->rotate = [this](const std::vector<int> &pages, int degrees) { rotatePages(pages, degrees); };
    organize_->remove = [this](const std::vector<int> &pages) { deletePages(pages); };
    organize_->extract = [this](const std::vector<int> &pages) { extractPages(pages); };
    organize_->replace = [this](const std::vector<int> &pages) { replacePages(pages); };
    organize_->insertBlank = [this](int at) { insertBlank(at); };
    organize_->insertFile = [this](int at) { insertFromFile(at); };
    organize_->split = [this] { splitDialog(); };
    organize_->close = [this] {
        if (organize_->creating())
            cancelCreating();
        else
            showOrganize(false);
    };
    organize_->create = [this](const QString &name) { finishCreating(name); };
    organize_->move = [this](const std::vector<int> &pages, int to) {
        const int k = to - int(std::count_if(pages.begin(), pages.end(), [to](int q) { return q < to; }));
        if (k == pages.front() && std::adjacent_find(pages.begin(), pages.end(), [](int a, int b) { return b != a + 1; }) == pages.end())
            return;  // (dropped where they are)
        movePages(pages, to);
        std::vector<int> moved;
        for (int n = 0; n < int(pages.size()); ++n)
            moved.push_back(k + n);
        organize_->select(moved);
    };
    // Read mode's bar: over the pages, at their foot - the pages turned, zoomed, the mode left
    {
        class ReadBar : public QWidget {
        public:
            using QWidget::QWidget;

        protected:
            void paintEvent(QPaintEvent *) override
            {
                QPainter p(this);
                classic::raisedEdge(p, QRectF(rect()));
            }
        };
        readBar_ = new ReadBar(viewBox_);
        auto *l = new QHBoxLayout(readBar_);
        l->setContentsMargins(6, 4, 6, 4);
        l->setSpacing(1);
        auto add = [this, l](const QString &icon, const QString &tip, const QString &label, std::function<void()> fn) {
            auto *b = new ToolButton(icon, tip, {}, false, label, readBar_);
            b->setWhiten(label.isEmpty() ? 16 : 0);
            connect(b, &ToolButton::clicked, this, fn);
            l->addWidget(b);
        };
        add(QStringLiteral("up"), tr("Previous page (Page Up)"), {}, [this] { stepPage(-1); });
        add(QStringLiteral("down"), tr("Next page (Page Down)"), {}, [this] { stepPage(1); });
        readPage_ = new QLabel(readBar_);
        readPage_->setFont(theme::font());
        readPage_->setMinimumWidth(chars(9));
        readPage_->setAlignment(Qt::AlignCenter);
        l->addWidget(readPage_);
        l->addWidget(new ToolSeparator(readBar_));
        add(QStringLiteral("zoom_out"), tr("Zoom out (Ctrl+-)"), {}, [this] { zoomStep(-1); });
        add(QStringLiteral("zoom_in"), tr("Zoom in (Ctrl++)"), {}, [this] { zoomStep(1); });
        add({}, tr("Fit the page in the window (Ctrl+0)"), tr("Fit page"), [this] { zoomTyped(QStringLiteral("Fit page")); });
        l->addWidget(new ToolSeparator(readBar_));
        add({}, tr("Leave Read mode (Esc)"), tr("Exit Read mode"), [this] { setReadMode(false); });
        readBar_->hide();
        viewBox_->installEventFilter(this);
    }
    view_->setShowBoxes(boxesCheck_->isChecked());
    connect(view_, &PageView::zoomChanged, this, &MainWindow::updateUi);
    connect(view_, &PageView::currentPageChanged, this, &MainWindow::updateUi);
    connect(theme::Notifier::instance(), &theme::Notifier::changed, this, &MainWindow::themeChanged);

    // the keys that work everywhere in the window
    auto key = [this](const QKeySequence &seq, auto fn) {
        auto *a = new QAction(this);
        a->setShortcut(seq);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        return a;
    };
    key(QKeySequence::Open, [this] { openDialog(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_S), [this] { save(); });
    key(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { saveAs(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Z), [this] { undo(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { redo(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_W), [this] { closeFile(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this] { tabs_->setCurrent((tabs_->current() + 1) % tabs_->count()); });
    key(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), [this] { tabs_->setCurrent((tabs_->current() + tabs_->count() - 1) % tabs_->count()); });
    key(QKeySequence::Find, [this] { focusFind(); });
    key(QKeySequence::Copy, [this] { view_->copySelection(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Plus), [this] { zoomStep(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { zoomStep(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Minus), [this] { zoomStep(-1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { zoomTyped(QStringLiteral("Fit page")); });
    key(QKeySequence(Qt::Key_F4), [this] { toggleThumbs(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { setReadMode(!readMode_); });
    key(QKeySequence(Qt::Key_Delete), [this] {  // (what's picked up; on the thumbnails, the pages)
        if (organizing()) {
            deletePages(organize_->selection());
            return;
        }
        if (!view_->group().empty() && view_->groupPage() >= 0) {  // (things picked together: all of them)
            std::vector<int> lines, pictures;
            for (const PageView::GroupItem &g : view_->group())
                (g.picture ? pictures : lines).push_back(g.index);
            const int page = view_->groupPage();
            view_->clearGroup();
            groupDeleted(page, lines, pictures);
        } else if (view_->pickedId())
            deletePicked();
        else if (view_->pickedPicture() >= 0)
            deletePicture();
        else if (view_->pickedLine() >= 0)
            emit view_->lineDelete(view_->pickedLinePage(), view_->pickedLine());
        else if (thumbs_->hasFocus())
            deletePages(thumbs_->selection());
    })->setShortcutContext(Qt::WindowShortcut);

    say(tr("Open a PDF to start."));
    updateUi();
    view_->setFocus();
    // Ctrl + a letter by the key, not the letter it types (an Arabic keyboard...)
    qApp->installEventFilter(this);
}

// ---- building the window ----

void MainWindow::buildMenus()
{
    menuBar_ = new QMenuBar(body());
    menuBar_->setNativeMenuBar(false);
    struct Def {
        QString title;
        void (MainWindow::*fill)(QMenu *);
        const char *note;
    };
    const Def defs[] = {
        {tr("&File"), &MainWindow::fillFileMenu, "Open, save and close PDFs; make a PDF from files; export the pages as pictures, split the PDF, or make a smaller copy of it."},
        {tr("&Edit"), &MainWindow::fillEditMenu, "Undo and redo, delete what's picked up, find words, and the settings."},
        {tr("&View"), &MainWindow::fillViewMenu, "Zoom, fit the pages to the window, and show or hide the page thumbnails."},
        {tr("&Pages"), &MainWindow::fillPagesMenu, "Turn, add, take out and delete pages."},
        {tr("&Theme"), &MainWindow::fillThemeMenu, "Changes the app's look: Windows 98, Ivory, Dark, or your own Custom theme."},
        {tr("&Help"), &MainWindow::fillHelpMenu, "What's This?, updates, and About Master PDF."},
    };
    for (const Def &d : defs) {
        QMenu *m = d.fill == &MainWindow::fillThemeMenu ? new StayOpenMenu(d.title, menuBar_) : new QMenu(d.title, menuBar_);
        m->setProperty(Win98Style::kIndent, 8);
        auto fill = d.fill;
        connect(m, &QMenu::aboutToShow, this, [this, m, fill] {
            m->clear();
            (this->*fill)(m);
        });
        if (auto *stay = dynamic_cast<StayOpenMenu *>(m))
            stay->refill = [this, m, fill] {
                m->clear();
                (this->*fill)(m);
            };
        menuBar_->addMenu(m);
        m->menuAction()->setWhatsThis(tr(d.note));
    }
    menuBar_->setWhatsThis(tr("The menus: File, Edit, View, Pages, Theme and Help."));
    auto *row = new QHBoxLayout;
    row->setContentsMargins(2, 1, 2, 0);
    row->addWidget(menuBar_);
    static_cast<QVBoxLayout *>(body()->layout())->addLayout(row);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(2, 1, 2, 0);
    line->addWidget(new EtchedLine(body()));
    static_cast<QVBoxLayout *>(body()->layout())->addLayout(line);
}

void MainWindow::buildToolbars(QVBoxLayout *box)
{
    auto etched = [this, box] {
        auto *l = new QHBoxLayout;
        l->setContentsMargins(2, 0, 2, 0);
        l->addWidget(new EtchedLine(body()));
        box->addLayout(l);
    };
    auto button = [this](QHBoxLayout *row, const QString &key, const QString &icon, const QString &tip,
                         const QString &note, bool split = false, int whiten = 0) {
        auto *b = new ToolButton(icon, tip, note, split, {}, body());
        b->setWhiten(whiten);
        row->addWidget(b);
        buttons_.insert(key, b);
        return b;
    };

    // the first toolbar: open and save, undo, the pages, zoom - and Find at the right end
    auto *row = new QHBoxLayout;
    row->setContentsMargins(2, 2, 4, 2);
    row->setSpacing(1);  // (a pixel between the buttons: at 125% one's edge isn't covered by the next)
    row->addWidget(new ToolGrip(body()));
    connect(button(row, "open", "open", tr("Open (Ctrl+O)"), tr("Opens a PDF.")), &ToolButton::clicked, this,
            &MainWindow::openDialog);
    connect(button(row, "save", "save", tr("Save (Ctrl+S)"),
                   tr("Saves the changes into the PDF. (File > Save As saves them as a new file.)")),
            &ToolButton::clicked, this, &MainWindow::save);
    row->addWidget(new ToolSeparator(body()));
    connect(button(row, "undo", "undo", tr("Undo (Ctrl+Z)"), tr("Takes back the last change.")),
            &ToolButton::clicked, this, &MainWindow::undo);
    connect(button(row, "redo", "redo", tr("Redo (Ctrl+Y)"), tr("Puts back the change you took back.")),
            &ToolButton::clicked, this, &MainWindow::redo);
    row->addWidget(new ToolSeparator(body()));
    connect(button(row, "prev", "up", tr("Previous page (Page Up)"), tr("Goes to the page before."), false, 16),
            &ToolButton::clicked, this, [this] { stepPage(-1); });
    connect(button(row, "next", "down", tr("Next page (Page Down)"), tr("Goes to the next page."), false, 16),
            &ToolButton::clicked, this, [this] { stepPage(1); });
    row->addSpacing(4);
    pageBox_ = new QLineEdit(body());
    pageBox_->setFont(theme::font());
    pageBox_->setAlignment(Qt::AlignRight);
    pageBox_->setFixedSize(chars(4) + 10, 20);
    pageBox_->setValidator(new QIntValidator(1, 999999, pageBox_));
    pageBox_->setWhatsThis(tr("The page you're on. Type a page number and press Enter to go there."));
    connect(pageBox_, &QLineEdit::returnPressed, this, &MainWindow::goToTyped);
    connect(pageBox_, &QLineEdit::editingFinished, this, &MainWindow::updateUi);
    row->addWidget(pageBox_);
    row->addSpacing(3);
    pageCount_ = label(QStringLiteral("/ 0"), body());
    row->addWidget(pageCount_);
    row->addSpacing(2);
    row->addWidget(new ToolSeparator(body()));
    connect(button(row, "zoom_out", "zoom_out", tr("Zoom out (Ctrl+-)"), tr("Makes the pages smaller.")),
            &ToolButton::clicked, this, [this] { zoomStep(-1); });
    row->addSpacing(2);
    zoomBox_ = new QComboBox(body());
    zoomBox_->setEditable(true);
    zoomBox_->setFont(theme::font());
    zoomBox_->setInsertPolicy(QComboBox::NoInsert);
    QStringList zooms = {QStringLiteral("Fit page"), QStringLiteral("Fit width")};
    for (int z = 25; z <= 400; z += 25)
        zooms << QStringLiteral("%1%").arg(z);
    zoomBox_->addItems(zooms);
    zoomBox_->setMaxVisibleItems(zooms.size());  // (the whole list, no scrolling)
    zoomBox_->setFixedSize(chars(9) + 26, 20);
    zoomBox_->setWhatsThis(tr("How big the pages are shown: a size, or fitted to the window."));
    connect(zoomBox_, &QComboBox::textActivated, this, &MainWindow::zoomTyped);
    row->addWidget(zoomBox_);
    row->addSpacing(2);
    connect(button(row, "zoom_in", "zoom_in", tr("Zoom in (Ctrl++)"), tr("Makes the pages bigger.")),
            &ToolButton::clicked, this, [this] { zoomStep(1); });
    row->addStretch(1);
    {
        auto *hf = new ToolButton(QStringLiteral("headerfooter"), tr("Header & Footer"),
                                  tr("Header & Footer: the text at the top and the bottom of the pages - the PDF's own found and "
                                     "changed, or new put on (page numbers, the date, the file's name), or taken off."),
                                  false, tr("Header & Footer"), body());
        buttons_.insert(QStringLiteral("headerfooter"), hf);
        connect(hf, &ToolButton::clicked, this, [this] { headerFooterDialog(); });
        row->addWidget(hf);
    }
    {
        auto *b = new ToolButton(QStringLiteral("arrange"), tr("Organize pages"),
                                 tr("Organize Pages: every page in a grid - pick them, drag them to a new place, turn, delete, "
                                    "extract, insert or replace them."),
                                 false, tr("Organize pages"), body());
        buttons_.insert(QStringLiteral("organize"), b);
        connect(b, &ToolButton::clicked, this, [this] {
            if (!organize_->creating())  // (a new PDF: left only by Create or Cancel)
                showOrganize(!organizing());
            else
                buttons_["organize"]->setLatched(true);
        });
        row->addWidget(b);
        auto *scan = new ToolButton(QStringLiteral("scan"), tr("Scan"),
                                    tr("Scan: the text in scanned pages read (OCR) - the pages as they are, their text found, picked, "
                                       "copied, and edited in the font most like it."),
                                    false, tr("Scan"), body());
        buttons_.insert(QStringLiteral("scan"), scan);
        connect(scan, &ToolButton::clicked, this, [this] { scanDialog(); });
        row->addWidget(scan);
        auto *conv = new ToolButton(QStringLiteral("convert"), tr("Convert"),
                                    tr("Convert: this PDF saved as another kind of file - Word, Excel, PowerPoint, pictures, a web page, "
                                       "Rich Text, PostScript, text or XML."),
                                    false, tr("Convert"), body());
        buttons_.insert(QStringLiteral("convert"), conv);
        connect(conv, &ToolButton::clicked, this, [this] { convertDialog(); });
        row->addWidget(conv);
    }
    row->addSpacing(8);
    row->addWidget(label(tr("Find:"), body()));
    row->addSpacing(2);
    findBox_ = new QLineEdit(body());
    findBox_->setFont(theme::font());
    findBox_->setFixedSize(chars(18) + 10, 20);
    findBox_->setWhatsThis(tr("Type words to find in the PDF and press Enter. Enter again goes to the next "
                              "place they are; Shift + Enter, back; Esc, done."));
    findBox_->installEventFilter(this);
    connect(findBox_, &QLineEdit::textEdited, this, &MainWindow::findSoon);
    row->addWidget(findBox_);
    row->addSpacing(1);
    connect(button(row, "find", "find", tr("Find next (Enter)"),
                   tr("Finds the next place the words typed beside it are.")),
            &ToolButton::clicked, this, [this] { findNext(1); });
    box->addLayout(row);
    etched();

    // the second toolbar: the tools, in groups like Windows 98's
    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(2, 2, 4, 2);
    tools->setSpacing(1);
    tools->addWidget(new ToolGrip(body()));
    for (const Tool &t : kTools) {
        if (!t.key) {
            tools->addWidget(new ToolSeparator(body()));
            continue;
        }
        ToolButton *b = button(tools, t.key, t.icon, tr(t.name), tr(t.note), t.split, t.whiten);
        if (QLatin1String(t.key) == QLatin1String("edit"))
            b->setGreyBlues(true);  // (its blue I: the dark themes' grey, as the other icons' blues)
        const QString key = QString::fromLatin1(t.key);
        connect(b, &ToolButton::clicked, this, [this, key] { toolClicked(key); });
        connect(b, &ToolButton::arrowClicked, this, [this, key] { toolMenu(key); });
    }
    tools->addStretch(1);
    box->addLayout(tools);
    etched();

    // the third: what the tool does
    auto *props = new QWidget(body());
    props->setMinimumHeight(ToolButton::H + 4);  // (taller when its groups go on more rows - a narrow window)
    auto *pr = new QHBoxLayout(props);
    pr->setContentsMargins(2, 1, 4, 2);
    pr->setSpacing(0);
    pr->addWidget(new ToolGrip(props));
    pr->addSpacing(2);
    propsTitle_ = label(QString(), props);
    pr->addWidget(propsTitle_);
    pr->addSpacing(6);
    buildProps(pr, props);
    buildTextProps(pr, props);
    buildPictureProps(pr, props);
    propsHint_ = new EngravedLabel(props);
    pr->addWidget(propsHint_);
    pr->addStretch(1);
    // (the Edit tool's: every block's and picture's outline shown at once)
    boxesGroup_ = new QWidget(props);
    {
        auto *bl = new QHBoxLayout(boxesGroup_);
        bl->setContentsMargins(6, 0, 4, 0);
        boxesCheck_ = new QCheckBox(tr("Show bounding boxes"), boxesGroup_);
        boxesCheck_->setFont(theme::font());
        boxesCheck_->setToolTip(tr("Show the outline of every block of text and picture at once"));
        connect(boxesCheck_, &QCheckBox::toggled, this, [this](bool on) {
            view_->setShowBoxes(on);
            theme::saveSetting(QStringLiteral("show_boxes"), on);
        });
        // (on, as Acrobat's, unless it was turned off)
        boxesCheck_->blockSignals(true);  // (the view told once it's made)
        boxesCheck_->setChecked(theme::settings().value("show_boxes").toBool(true));
        boxesCheck_->blockSignals(false);
        bl->addWidget(boxesCheck_);
    }
    pr->addWidget(boxesGroup_);
    box->addWidget(props);
    etched();
}

void MainWindow::buildStatusBar(QVBoxLayout *box)
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(2, 2, 2, 2);
    row->setSpacing(2);
    msg_ = new StatusPanel(body());
    msg_->setMinimumWidth(20);
    row->addWidget(msg_, 1);
    readButton_ = new ToolButton(QString(), tr("Read mode (Ctrl+H)"),
                                 tr("Read mode: just the pages - the toolbars and the side panel put away. Esc (or this again) "
                                    "brings them back."),
                                 false, {}, body());
    readButton_->setImage(bookIcon(theme::dark()));
    connect(readButton_, &ToolButton::clicked, this, [this] { setReadMode(!readMode_); });
    row->addWidget(readButton_);
    pageLabel_ = new StatusPanel(body());
    pageLabel_->setFixedWidth(chars(14) + 8);
    row->addWidget(pageLabel_);
    sizeLabel_ = new StatusPanel(body());
    sizeLabel_->setFixedWidth(chars(24) + 8);
    row->addWidget(sizeLabel_);
    zoomLabel_ = new StatusPanel(body());
    zoomLabel_->setFixedWidth(chars(6) + 8);
    row->addWidget(zoomLabel_);
    box->addLayout(row);
}

// ---- the menus (made each time they open, like the Python app's) ----

void MainWindow::fillFileMenu(QMenu *m)
{
    const bool has = view_->document() != nullptr;
    m->addAction(tr("Open..."), this, &MainWindow::openDialog);
    m->addAction(tr("Create PDF from files..."), this, [this] { createFromFiles({}); });
    m->addSeparator();
    const bool can = has && view_->document()->editable();
    m->addAction(tr("Save"), this, &MainWindow::save)->setEnabled(can);
    m->addAction(tr("Save As..."), this, &MainWindow::saveAs)->setEnabled(can);
    m->addSeparator();
    m->addAction(tr("Export as pictures..."), this, &MainWindow::exportDialog)->setEnabled(has);
    m->addAction(tr("Split..."), this, &MainWindow::splitDialog)->setEnabled(has && view_->document()->editable());
    m->addAction(tr("Compress..."), this, &MainWindow::compressDialog)->setEnabled(has && view_->document()->editable());
    m->addSeparator();
    m->addAction(tr("Close"), this, &MainWindow::closeFile)->setEnabled(has);
    const QStringList recent = HomePage::recentPaths().mid(0, 5);
    if (!recent.isEmpty()) {  // the last few PDFs, numbered like Windows 98 programs did
        m->addSeparator();
        for (int n = 0; n < recent.size(); ++n) {
            const QString path = recent[n];
            m->addAction(QStringLiteral("%1  %2").arg(n + 1).arg(QFileInfo(path).fileName()), this,
                         [this, path] { openFile(path); });
        }
    }
    m->addSeparator();
    m->addAction(tr("Exit"), this, &QWidget::close);
}

void MainWindow::fillEditMenu(QMenu *m)
{
    const bool has = view_->document() != nullptr;
    m->addAction(tr("Undo"), this, &MainWindow::undo)->setEnabled(has && view_->document()->canUndo());
    m->addAction(tr("Redo"), this, &MainWindow::redo)->setEnabled(has && view_->document()->canRedo());
    m->addSeparator();
    m->addAction(tr("Delete"), this, [this] {
        if (!view_->group().empty() && view_->groupPage() >= 0) {  // (things picked together: all of them)
            std::vector<int> lines, pictures;
            for (const PageView::GroupItem &g : view_->group())
                (g.picture ? pictures : lines).push_back(g.index);
            const int page = view_->groupPage();
            view_->clearGroup();
            groupDeleted(page, lines, pictures);
        } else if (view_->pickedId())
            deletePicked();
        else if (view_->pickedPicture() >= 0)
            deletePicture();
        else if (view_->pickedLine() >= 0)
            emit view_->lineDelete(view_->pickedLinePage(), view_->pickedLine());
    })->setEnabled(has && (view_->pickedId() || view_->pickedPicture() >= 0 || view_->pickedLine() >= 0));
    m->addSeparator();
    const bool can = has && view_->document()->editable();
    m->addAction(tr("Edit text & pictures"), this, [this] { setTool(QStringLiteral("edit"), false); })->setEnabled(can);
    m->addAction(tr("Add picture..."), this, [this] {
        // (in the middle of the page you're on)
        const int page = std::max(0, view_->currentPage());
        const QSizeF s = view_->document()->pageSize(page);
        addPicture(page, QPointF(s.width() / 4, s.height() / 4), QRectF());
    })->setEnabled(can);
    m->addSeparator();
    m->addAction(tr("Find..."), this, &MainWindow::focusFind)->setEnabled(has);
    m->addSeparator();
    m->addAction(tr("Settings..."), this, &MainWindow::openSettings);
}

void MainWindow::fillViewMenu(QMenu *m)
{
    const bool has = view_->document() != nullptr;
    m->addAction(tr("Zoom in"), this, [this] { zoomStep(1); })->setEnabled(has);
    m->addAction(tr("Zoom out"), this, [this] { zoomStep(-1); })->setEnabled(has);
    m->addSeparator();
    m->addAction(tr("Actual size"), this, [this] { zoomTyped(QStringLiteral("100%")); })->setEnabled(has);
    m->addAction(tr("Fit page"), this, [this] { zoomTyped(QStringLiteral("Fit page")); })->setEnabled(has);
    m->addAction(tr("Fit width"), this, [this] { zoomTyped(QStringLiteral("Fit width")); })->setEnabled(has);
    m->addSeparator();
    QAction *read = m->addAction(tr("&Read mode"), this, [this] { setReadMode(!readMode_); });
    read->setCheckable(true);
    read->setChecked(readMode_);
    read->setEnabled(has || readMode_);
    m->addSeparator();
    QAction *thumbs = m->addAction(tr("Page thumbnails"), this, &MainWindow::toggleThumbs);
    thumbs->setCheckable(true);
    thumbs->setChecked(showThumbs_);
    m->addSeparator();
    m->addAction(tr("First page"), this, [this] { view_->goToPage(0); })->setEnabled(has);
    m->addAction(tr("Last page"), this, [this] {
        view_->goToPage(view_->document()->pageCount() - 1);
    })->setEnabled(has);
}

void MainWindow::fillPagesMenu(QMenu *m)
{
    const bool can = view_->document() && view_->document()->editable();
    m->addAction(tr("Rotate left"), this, [this] { rotatePages(thumbs_->selection(), -90); })->setEnabled(can);
    m->addAction(tr("Rotate right"), this, [this] { rotatePages(thumbs_->selection(), 90); })->setEnabled(can);
    m->addSeparator();
    m->addAction(tr("Insert pages from file..."), this, [this] { insertFromFile(-1); })->setEnabled(can);
    m->addAction(tr("Insert blank page"), this, [this] { insertBlank(-1); })->setEnabled(can);
    m->addAction(tr("Extract pages..."), this, [this] { extractPages(thumbs_->selection()); })->setEnabled(can);
    m->addSeparator();
    m->addAction(tr("Header && Footer..."), this, [this] { headerFooterDialog(); })->setEnabled(can);
    m->addSeparator();
    m->addAction(tr("Delete pages"), this, [this] { deletePages(thumbs_->selection()); })->setEnabled(can);
}

void MainWindow::fillThemeMenu(QMenu *m)
{
    const QString now = theme::choice();
    for (const auto &[key, name] : theme::names()) {
        QAction *a = m->addAction(name);
        a->setCheckable(true);
        a->setChecked(key == now);
        const QString k = key;
        connect(a, &QAction::triggered, this, [this, k] { pickTheme(k); });
    }
    m->addSeparator();
    QAction *custom = m->addAction(tr("Custom"));
    custom->setCheckable(true);
    custom->setChecked(now == "custom");
    custom->setEnabled(theme::settings().contains("custom_theme"));
    connect(custom, &QAction::triggered, this, [this] { pickTheme(QStringLiteral("custom")); });
    // (the Custom theme in use: a brush at its item's right end - Settings, where it's made - as Master Converter has)
    for (QAbstractButton *old : m->findChildren<QAbstractButton *>(QStringLiteral("customBrush"), Qt::FindDirectChildrenOnly))
        old->deleteLater();
    if (now != QLatin1String("custom"))
        return;
    auto *brush = new BrushButton(m);
    brush->setObjectName(QStringLiteral("customBrush"));
    brush->setFocusPolicy(Qt::NoFocus);
    brush->setCursor(Qt::PointingHandCursor);
    brush->setToolTip(tr("Change the Custom theme (Settings)"));
    brush->setWhatsThis(tr("Opens Settings, where the Custom theme's look and title bar colour are chosen."));
    connect(brush, &QAbstractButton::clicked, this, [this, m] {
        m->hide();
        openSettings();
    });
    QTimer::singleShot(0, brush, [m, custom, brush] {  // (where the item is, once the list is laid out)
        const QRect r = m->actionGeometry(custom);
        const int side = std::max(18, r.height() - 2);
        brush->setGeometry(r.right() - side - 2, r.top() + (r.height() - side) / 2, side, side);
        brush->show();
        brush->raise();
    });
}

void MainWindow::fillHelpMenu(QMenu *m)
{
    m->addAction(tr("What's This?"), this, [this] { whatsThis::start(this); });
    m->addSeparator();
    m->addAction(tr("Check for updates..."), this, &MainWindow::checkForUpdates);
    m->addAction(tr("About Master PDF"), this, [this] { aboutBox(this); });
}

// ---- themes ----

void MainWindow::pickTheme(const QString &choice, bool save)
{
    theme::apply(choice, save);
}

void MainWindow::themeChanged()
{
    qApp->setPalette(theme::palette());
    if (readButton_)
        readButton_->setImage(bookIcon(theme::dark()));  // (dark: its colours inverted)
    const auto all = findChildren<QWidget *>();
    for (QWidget *w : all)
        w->update();
    update();
    view_->viewport()->update();
}

// ---- opening and closing ----

bool MainWindow::openFile(const QString &path)
{
    for (size_t k = 0; k < docs_.size(); ++k)  // (open already: its tab)
        if (QFileInfo(docs_[k].doc->path()).absoluteFilePath().compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
            showTab(int(k));
            addRecent(QFileInfo(path).absoluteFilePath());
            return true;
        }
    QString error;
    auto doc = Document::open(path, &error);
    if (!doc) {
        classicMessage(this, tr("Master PDF"),
                       tr("Couldn't open %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    showDocument(doc, QFileInfo(path).fileName());
    addRecent(QFileInfo(path).absoluteFilePath());
    theme::saveSetting("last_folder", QFileInfo(path).absolutePath());
    say(tr("Opened %1.").arg(name_));
    return true;
}

void MainWindow::showDocument(std::shared_ptr<Document> doc, const QString &name)
{
    docs_.push_back({std::move(doc), name});
    const QString path = docs_.back().doc->path();
    tabs_->addTab(name, path.isEmpty() ? name : QDir::toNativeSeparators(path), true);
    showTab(int(docs_.size()) - 1);
}

void MainWindow::showTab(int doc)
{
    if (doc >= -1 && doc < int(docs_.size()))
        tabs_->setCurrent(doc + 1);
}

// a tab chosen: where the PDF shown was kept; Home, or the PDF chosen put back where it was
void MainWindow::tabChosen(int index)
{
    if (view_->lineEditorOpen())
        view_->commitEditor();
    keepTabPlace();
    current_ = index - 1;
    if (current_ < 0) {
        loadIntoView(nullptr, QString());
        pages_->setCurrentWidget(home_);
        home_->refresh();  // (the status bar: how many PDFs it lists)
        updateUi();
        return;
    }
    pages_->setCurrentWidget(docPage_);
    const DocTab t = docs_[size_t(current_)];
    loadIntoView(t.doc, t.name);
    if (t.placed) {
        if (t.fit == PageView::Fit::Width)
            view_->fitWidth();
        else if (t.fit == PageView::Fit::Page)
            view_->fitPage();
        else
            view_->setZoom(t.zoom);
        view_->horizontalScrollBar()->setValue(t.scrollX);
        view_->verticalScrollBar()->setValue(t.scrollY);
    }
    updateUi();
    view_->setFocus();
    if (isCreating(t.doc))  // (a new PDF not created yet: its pages to put in order, its name)
        showOrganize(true);
}

void MainWindow::keepTabPlace()
{
    if (current_ < 0 || current_ >= int(docs_.size()) || view_->document() != docs_[size_t(current_)].doc)
        return;
    DocTab &t = docs_[size_t(current_)];
    t.placed = true;
    t.name = name_;
    t.page = std::max(0, view_->currentPage());
    t.zoom = view_->zoom();
    t.fit = view_->fit();
    t.scrollX = view_->horizontalScrollBar()->value();
    t.scrollY = view_->verticalScrollBar()->value();
}

void MainWindow::closeTab(int doc)
{
    if (doc < 0 || doc >= int(docs_.size()))
        return;
    const bool unmade = isCreating(docs_[size_t(doc)].doc);  // (a new PDF not created: thrown away, not asked about)
    if (unmade)
        creating_.reset();
    if (!unmade && doc != current_ && docs_[size_t(doc)].doc->dirty())
        showTab(doc);  // (its changes asked about where they can be seen)
    if (doc == current_) {
        if (!unmade && !maybeSave())
            return;
        stopScan(docs_[size_t(doc)].doc);
        current_ = -1;  // (gone: not kept)
        docs_.erase(docs_.begin() + doc);
        tabs_->removeTab(doc + 1);  // (the tab after it chosen - or before it, the last; Home, none)
        tabChosen(tabs_->current());
        return;
    }
    stopScan(docs_[size_t(doc)].doc);
    docs_.erase(docs_.begin() + doc);
    if (doc < current_)
        --current_;
    tabs_->removeTab(doc + 1);
}

void MainWindow::loadIntoView(std::shared_ptr<Document> doc, const QString &name)
{
    if (organize_ && organizing())
        showOrganize(false);
    ++*findSerial_;
    hits_.clear();
    findText_.clear();
    findPos_ = -1;
    name_ = name;
    view_->setFloatBar(nullptr, -1, {});
    thumbs_->setDocument(doc);
    view_->setDocument(std::move(doc));
    setTool(QStringLiteral("select"), false);
    if (annotationsShowing())
        fillAnnotationList();
    fillBookmarkList(true);
    updateUi();
    if (!findBox_->text().trimmed().isEmpty())
        search(findBox_->text().trimmed(), false);  // (the words still in Find: in this PDF)
    else
        results_->fill({}, QString());
}

void MainWindow::addRecent(const QString &path)
{
    HomePage::noteOpened(path);
}

void MainWindow::openDialog()
{
    const QString folder = theme::settings().value("last_folder").toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open"), folder,
                                                      tr("PDF files (*.pdf);;All files (*)"));
    if (path.isEmpty())
        return;
    openFile(path);  // (in a tab of its own)
}

void MainWindow::closeFile()
{
    if (current_ >= 0)
        closeTab(current_);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    for (size_t k = 0; k < docs_.size(); ++k)  // (each PDF with changes: asked about, in its tab)
        if (docs_[k].doc->dirty() && !isCreating(docs_[k].doc)) {
            showTab(int(k));
            if (!maybeSave()) {
                e->ignore();
                return;
            }
        }
    stopScan(nullptr);  // (a scan still reading: called off, its thread waited for)
    theme::saveSetting("start_maximized", isMaximizedClassic());
    ClassicWindow::closeEvent(e);
}

// ---- saving, Undo, and the pages ----

// a change to the pages themselves (not just what's on them)
bool MainWindow::pageStep(const QString &what)
{
    static const QStringList steps = {"rotate", "delete pages", "move pages", "insert a page", "insert pages"};
    return steps.contains(what);
}

bool MainWindow::isPdf(const QString &path)
{
    return path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
}

bool MainWindow::isPicture(const QString &path)
{
    static const char *exts[] = {".png", ".jpg", ".jpeg", ".jpe", ".jfif", ".bmp", ".gif", ".tif",
                                 ".tiff", ".webp", ".ico", ".tga", ".ppm", ".pcx"};
    for (const char *e : exts)
        if (path.endsWith(QLatin1String(e), Qt::CaseInsensitive))
            return true;
    return false;
}

bool MainWindow::maybeSave()
{
    auto doc = view_->document();
    if (!doc || !doc->dirty())
        return true;
    const QString choice = classicMessage(this, tr("Master PDF"),
                                          tr("Do you want to save the changes to %1?").arg(name_),
                                          {tr("Yes"), tr("No"), tr("Cancel")});
    if (choice == tr("Yes"))
        return save();
    return choice == tr("No");
}

bool MainWindow::save()
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return false;
    if (doc->path().isEmpty())  // (a new PDF: where it goes, first)
        return saveAs();
    return write(doc->path());
}

bool MainWindow::saveAs()
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return false;
    const QString folder = doc->path().isEmpty() ? theme::settings().value("last_folder").toString()
                                                 : QFileInfo(doc->path()).absolutePath();
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), folder + QLatin1Char('/') + QFileInfo(name_).completeBaseName() + QStringLiteral(".pdf"),
                                                tr("PDF files (*.pdf);;All files (*)"));
    if (path.isEmpty())
        return false;  // (Windows' box itself asks before replacing a file)
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".pdf");
    return saveTo(path);
}

bool MainWindow::saveTo(const QString &path)
{
    if (!write(path))
        return false;
    name_ = QFileInfo(path).fileName();
    addRecent(QFileInfo(path).absoluteFilePath());
    updateUi();
    return true;
}

bool MainWindow::write(const QString &path)
{
    QString error;
    QApplication::setOverrideCursor(classic::waitCursor());
    const bool ok = view_->document()->save(path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        classicMessage(this, tr("Master PDF"),
                       tr("Couldn't save %1:\n%2\n\n(Is it open in another program?)").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    const qint64 size = QFileInfo(path).size();
    say(tr("Saved %1 (%2).").arg(QFileInfo(path).fileName(),
                                  size < 1024 * 1024 ? tr("%1 KB").arg(std::max<qint64>(1, size / 1024))
                                                     : tr("%1 MB").arg(size / 1024.0 / 1024.0, 0, 'f', 1)));
    updateUi();
    return true;
}

void MainWindow::undo()
{
    auto doc = view_->document();
    if (!doc || !doc->canUndo())
        return;
    const QString what = doc->undoName();
    doc->undo();
    afterChange(-1, !pageStep(what));
    say(tr("Undid: %1.").arg(what));
}

void MainWindow::redo()
{
    auto doc = view_->document();
    if (!doc || !doc->canRedo())
        return;
    const QString what = doc->redoName();
    doc->redo();
    afterChange(-1, !pageStep(what));
    say(tr("Redid: %1.").arg(what));
}

// After a change: everything drawn and laid out again; Find's results brought up to date
// (the pages may have moved); go: the page to go to (-1: stay)
void MainWindow::afterChange(int go, bool keep)
{
    view_->setFloatBar(nullptr, -1, {});
    view_->documentChanged(keep);
    thumbs_->documentChanged(keep);
    if (go >= 0) {
        view_->goToPage(std::min(go, view_->document()->pageCount() - 1));
        thumbs_->setCurrent(std::min(go, view_->document()->pageCount() - 1));
    }
    hits_.clear();
    findText_.clear();
    findPos_ = -1;
    if (!findBox_->text().trimmed().isEmpty())
        search(findBox_->text().trimmed(), false);
    if (annotationsShowing())
        fillAnnotationList();
    if (showThumbs_ && sideStack_->currentIndex() == 3)
        fillBookmarkList(false);
    if (organizing())
        organize_->refresh();
    updateUi();
}

void MainWindow::failed(const QString &what, const QString &error)
{
    afterChange(-1);
    classicMessage(this, tr("Master PDF"), tr("Couldn't %1:\n%2").arg(what, error));
}

void MainWindow::rotatePages(const std::vector<int> &pages, int degrees)
{
    auto doc = view_->document();
    if (!doc || pages.empty())
        return;
    QString error;
    if (!doc->rotatePages(pages, degrees, &error))
        return failed(tr("rotate"), error);
    afterChange(-1);
    say(tr("Turned %1 page(s) %2.").arg(pages.size()).arg(degrees < 0 ? tr("left") : tr("right")));
}

void MainWindow::deletePages(const std::vector<int> &pages)
{
    auto doc = view_->document();
    if (!doc || pages.empty() || !doc->editable())
        return;
    if (int(pages.size()) >= doc->pageCount()) {
        classicMessage(this, tr("Master PDF"), tr("A PDF needs at least one page, so not all of them can be deleted."));
        return;
    }
    const QString what = pages.size() == 1 ? tr("page %1").arg(pages[0] + 1) : tr("these %1 pages").arg(pages.size());
    if (classicMessage(this, tr("Master PDF"), tr("Delete %1?").arg(what), {tr("Yes"), tr("No")}) != tr("Yes"))
        return;
    thumbs_->select({});
    QString error;
    if (!doc->deletePages(pages, &error))
        return failed(tr("delete pages"), error);
    afterChange(-1);
    say(tr("Deleted %1 page(s).").arg(pages.size()));
}

void MainWindow::movePages(const std::vector<int> &pages, int to)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    const int k = to - int(std::count_if(pages.begin(), pages.end(), [to](int p) { return p < to; }));
    QString error;
    if (!doc->movePages(pages, to, &error))
        return failed(tr("move pages"), error);
    afterChange(k);
    std::vector<int> moved;
    for (int n = 0; n < int(pages.size()); ++n)
        moved.push_back(k + n);
    thumbs_->select(moved);
    say(tr("Moved %1 page(s).").arg(pages.size()));
}

void MainWindow::insertBlank(int at)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    if (at < 0)  // (after this page)
        at = std::max(0, view_->currentPage()) + 1;
    const QSizeF size = doc->pageSize(std::clamp(at - 1, 0, doc->pageCount() - 1));  // (the page next to it)
    QString error;
    if (!doc->insertBlank(at, size, &error))
        return failed(tr("insert a page"), error);
    afterChange(at);
    say(tr("Added a blank page (page %1).").arg(at + 1));
}

void MainWindow::insertFromFile(int at)
{
    auto doc = view_->document();
    if (!doc || !doc->editable())
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Insert pages from file"), theme::settings().value("last_folder").toString(),
        tr("PDFs, Word documents and pictures (*.pdf *.docx *.doc *.docm *.rtf *.odt *.png *.jpg *.jpeg *.jpe *.jfif *.bmp *.gif *.tif *.tiff *.webp *.ico *.tga *.ppm *.pcx);;All files (*)"));
    if (paths.isEmpty())
        return;
    insertPaths(paths, at < 0 ? std::max(0, view_->currentPage()) + 1 : at);
}

void MainWindow::insertPaths(const QStringList &paths, int at)
{
    auto doc = view_->document();
    QStringList problems;
    const QStringList files = fromWord(paths, &problems);  // (Word documents: made PDFs first)
    QApplication::setOverrideCursor(classic::waitCursor());
    const int added = files.isEmpty() ? 0 : doc->insertFiles(files, at, &problems);
    QApplication::restoreOverrideCursor();
    afterChange(added ? at : -1);
    if (!problems.isEmpty())
        classicMessage(this, tr("Some files were left out"), problems.mid(0, 10).join(QLatin1Char('\n')));
    if (added)
        say(tr("Added %1 page(s).").arg(added));
}

void MainWindow::extractPages(const std::vector<int> &pages)
{
    auto doc = view_->document();
    if (!doc || pages.empty() || !doc->editable())
        return;
    const QString folder = QFileInfo(doc->path()).absolutePath();
    QString path = QFileDialog::getSaveFileName(
        this, tr("Extract pages"), folder + QLatin1Char('/') + QFileInfo(name_).completeBaseName() + tr(" (pages).pdf"),
        tr("PDF files (*.pdf);;All files (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".pdf");
    QString error;
    if (!doc->extractPages(pages, path, &error)) {
        classicMessage(this, tr("Master PDF"), tr("Couldn't save the pages:\n%1").arg(error));
        return;
    }
    if (classicMessage(this, tr("Master PDF"),
                       tr("Saved %1 page(s) as %2.").arg(pages.size()).arg(QFileInfo(path).fileName()),
                       {tr("Show file"), tr("OK")}) == tr("Show file"))
        showInFolder(path);
}

// ---- what's shown ----

void MainWindow::updateUi()
{
    auto doc = view_->document();
    const bool has = doc != nullptr;
    const int n = has ? doc->pageCount() : 0;
    const int page = has ? std::max(0, view_->currentPage()) : 0;
    const QString title = has ? tr("%1%2 - Master PDF").arg(name_, doc->dirty() ? QStringLiteral(" *") : QString())
                              : QStringLiteral("Master PDF");
    if (title != this->title())
        setTitle(title);
    buttons_["save"]->setEnabled(has && doc->editable());
    buttons_["undo"]->setEnabled(has && doc->canUndo());
    buttons_["redo"]->setEnabled(has && doc->canRedo());
    buttons_["prev"]->setEnabled(has && page > 0);
    buttons_["next"]->setEnabled(has && page < n - 1);
    for (const char *k : {"zoom_in", "zoom_out", "find"})
        buttons_[k]->setEnabled(has);
    for (const Tool &t : kTools)
        if (t.key)
            buttons_[t.key]->setEnabled(has);
    buttons_["annots"]->setLatched(annotationsShowing());
    if (!pageBox_->hasFocus())
        pageBox_->setText(has ? QString::number(page + 1) : QString());
    pageCount_->setText(QStringLiteral("/ %1").arg(n));
    if (!zoomBox_->hasFocus() && !zoomBox_->lineEdit()->hasFocus()) {
        const PageView::Fit fit = view_->fit();
        zoomBox_->setEditText(fit == PageView::Fit::Width  ? QStringLiteral("Fit width")
                              : fit == PageView::Fit::Page ? QStringLiteral("Fit page")
                                                           : QStringLiteral("%1%").arg(std::lround(view_->zoom() * 100)));
    }
    refreshProps();
    pageLabel_->setText(has ? tr("Page %1 of %2").arg(page + 1).arg(n) : QString());
    if (has) {
        const QSizeF s = doc->pageSize(page);
        const QString name = pageSizeText(s.width(), s.height());
        sizeLabel_->setText(name.endsWith(QLatin1String("mm"))
                                ? name
                                : QStringLiteral("%1  (%2 x %3 mm)")
                                      .arg(name)
                                      .arg(std::lround(s.width() * 25.4 / 72))
                                      .arg(std::lround(s.height() * 25.4 / 72)));
    } else {
        sizeLabel_->setText(pages_ && pages_->currentWidget() == home_ ? home_->summary() : QString());
    }
    zoomLabel_->setText(has ? QStringLiteral("%1%").arg(std::lround(view_->zoom() * 100)) : QString());
    if (has)
        thumbs_->setCurrent(page);
    if (current_ >= 0 && current_ < int(docs_.size()) && has) {
        docs_[size_t(current_)].name = name_;
        tabs_->setTabText(current_ + 1, name_, doc->path().isEmpty() ? name_ : QDir::toNativeSeparators(doc->path()));
    }
    start_->setVisible(!has && pages_->currentWidget() == docPage_);
    if (readButton_) {
        readButton_->setEnabled(has || readMode_);
        buttons_["organize"]->setEnabled(has && doc->editable());
        buttons_["headerfooter"]->setEnabled(has && doc->editable());
        buttons_["scan"]->setEnabled(has && doc->editable());
        buttons_["convert"]->setEnabled(has);
        if (readPage_)
            readPage_->setText(has ? tr("%1 / %2").arg(page + 1).arg(n) : QString());
        if (readMode_ && !has)
            setReadMode(false);
    }
    placeStartPanel();
}

void MainWindow::placeStartPanel()
{
    if (start_->isHidden())
        return;
    start_->adjustSize();
    const QSize vs = view_->viewport()->size();
    start_->move((vs.width() - start_->width()) / 2, (vs.height() - start_->height()) / 2);
}

void MainWindow::say(const QString &text)
{
    msg_->setText(text);
}

void MainWindow::notYet(const QString &what)
{
    say(tr("%1: not in the C++ version yet.").arg(what));
}

void MainWindow::stepPage(int by)
{
    auto doc = view_->document();
    if (!doc)
        return;
    view_->goToPage(std::clamp(view_->currentPage() + by, 0, doc->pageCount() - 1));
}

void MainWindow::goToTyped()
{
    auto doc = view_->document();
    bool ok = false;
    const int page = pageBox_->text().toInt(&ok) - 1;
    if (doc && ok)
        view_->goToPage(std::clamp(page, 0, doc->pageCount() - 1));
    view_->setFocus();
    updateUi();
}

void MainWindow::zoomStep(int d)
{
    if (!view_->document())
        return;
    // one step of 25% in or out, from the zoom it's at now
    const long now = std::lround(view_->zoom() * 100);
    int to = -1;
    if (d > 0) {
        for (int z = 25; z <= 400 && to < 0; z += 25)
            if (z > now + 1)
                to = z;
    } else {
        for (int z = 400; z >= 25 && to < 0; z -= 25)
            if (z < now - 1)
                to = z;
    }
    if (to > 0)
        view_->setZoom(to / 100.0);
}

void MainWindow::zoomTyped(const QString &text)
{
    if (view_->document()) {
        if (text == QLatin1String("Fit width"))
            view_->fitWidth();
        else if (text == QLatin1String("Fit page"))
            view_->fitPage();
        else {
            bool ok = false;
            const double z = QString(text).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
            if (ok)
                view_->setZoom(std::clamp(z, 25.0, 400.0) / 100.0);
        }
    }
    view_->setFocus();
    updateUi();
}

void MainWindow::toggleThumbs()
{
    showThumbs_ = !showThumbs_;
    side_->setVisible(showThumbs_ && !readMode_);
    theme::saveSetting("thumbnails", showThumbs_);
}

void MainWindow::showSideView(int which)
{
    sideStack_->setCurrentIndex(which);
    chooser_->setText(which == 0 ? tr("Pages") : which == 2 ? tr("Annotations") : which == 3 ? tr("Bookmarks")
                                                                                              : tr("Search results"));
    if (which == 2)
        fillAnnotationList();
    if (which == 3)
        fillBookmarkList(false);
    if (!buttons_.isEmpty())
        buttons_["annots"]->setLatched(annotationsShowing());
}

void MainWindow::openChooser()
{
    chooser_->setPressed(true);  // (pressed in while its list is open)
    QMenu menu(this);
    const int now = sideStack_->currentIndex();
    struct Item {
        QString name;
        int view;
    };
    for (const Item &it : {Item{tr("Pages"), 0}, Item{tr("Bookmarks"), 3}, Item{tr("Annotations"), 2},
                           Item{tr("Search results"), 1}}) {
        QAction *a = menu.addAction(it.name);
        a->setCheckable(true);
        a->setChecked(it.view == now);
        a->setEnabled(it.view >= 0);
        const int v = it.view;
        connect(a, &QAction::triggered, this, [this, v] { showSideView(v); });
    }
    menu.setMinimumWidth(side_->width() - 10);
    menu.exec(chooser_->mapToGlobal(QPoint(0, chooser_->height())));
    chooser_->setPressed(false);
}

// ---- Find ----

void MainWindow::focusFind()
{
    findBox_->setFocus(Qt::ShortcutFocusReason);
    findBox_->selectAll();
}

void MainWindow::findSoon()
{
    findTimer_->start();
}

void MainWindow::search(const QString &text, bool jump)
{
    findTimer_->stop();
    const int mine = ++*findSerial_;
    if (!view_->document() || text.isEmpty()) {
        searchDone(text, {}, jump);
        return;
    }
    QApplication::setOverrideCursor(classic::waitCursor());
    std::vector<FindHit> hits = findAll(view_->document(), text, findSerial_, mine);
    QApplication::restoreOverrideCursor();
    searchDone(text, std::move(hits), jump);
}

void MainWindow::searchDone(const QString &text, std::vector<FindHit> hits, bool jump)
{
    if (!view_->document() || text.isEmpty()) {
        findText_.clear();
        hits_.clear();
        findPos_ = -1;
        results_->fill({}, QString());
        if (viewBeforeFind_ >= 0 && sideStack_->currentIndex() == 1)
            showSideView(viewBeforeFind_);
        viewBeforeFind_ = -1;
        view_->setFindHits({}, -1);
        return;
    }
    // where you were: to stay at (or near) it when the list is only brought up to date
    const bool had = findPos_ >= 0 && findPos_ < int(hits_.size());
    const FindHit was = had ? hits_[size_t(findPos_)] : FindHit();
    findText_ = text;
    hits_ = std::move(hits);
    if (sideStack_->currentIndex() != 1) {
        viewBeforeFind_ = sideStack_->currentIndex();
        showSideView(1);
    }
    results_->fill(hits_, text);
    if (hits_.empty()) {
        findPos_ = -1;
        view_->setFindHits({}, -1);
        say(tr("Can't find \"%1\".").arg(text));
        return;
    }
    int k = 0;
    if (jump) {  // the first one from this page on
        const int here = view_->currentPage();
        for (int n = 0; n < int(hits_.size()); ++n)
            if (hits_[size_t(n)].page >= here) {
                k = n;
                break;
            }
    } else if (had) {  // the nearest to where you were
        double best = 1e18;
        for (int n = 0; n < int(hits_.size()); ++n) {
            const FindHit &h = hits_[size_t(n)];
            const double d = std::abs(h.page - was.page) * 1e6 + std::abs(h.rect.top() - was.rect.top()) +
                             std::abs(h.rect.left() - was.rect.left());
            if (d < best) {
                best = d;
                k = n;
            }
        }
    }
    goResult(k, jump);
}

void MainWindow::findNext(int step)
{
    const QString text = findBox_->text().trimmed();
    if (!view_->document() || text.isEmpty())
        return;
    if (text != findText_ || findTimer_->isActive()) {
        search(text);
        return;
    }
    if (hits_.empty()) {
        say(tr("Can't find \"%1\".").arg(text));
        QApplication::beep();
        return;
    }
    const int n = int(hits_.size());
    goResult(((findPos_ + step) % n + n) % n);
}

void MainWindow::goResult(int k, bool scroll)
{
    if (k < 0 || k >= int(hits_.size()))
        return;
    findPos_ = k;
    const FindHit &h = hits_[size_t(k)];
    if (scroll)
        view_->showRect(h.page, h.rect);
    view_->setFindHits(hits_, k);
    results_->setResult(k, int(hits_.size()));
    say(tr("\"%1\": result %2 of %3 (Enter: next, Shift + Enter: back, Esc: done).")
            .arg(findText_)
            .arg(k + 1)
            .arg(hits_.size()));
}

// ---- keys, and the start box's place ----

// ---- Read mode, Organize Pages ----

void MainWindow::setReadMode(bool on)
{
    if (on && (!view_->document() || current_ < 0))
        return;
    if (on == readMode_)
        return;
    if (on) {  // (just reading: what's being typed put in, nothing picked up, the mouse selects text to copy)
        if (view_->lineEditorOpen())
            view_->commitEditor();
        toolBeforeRead_ = tool_;
        setTool(QStringLiteral("select"), false);
        view_->setFloatBar(nullptr, -1, {});
    }
    readMode_ = on;
    view_->setReadOnly(on);
    if (!on && !toolBeforeRead_.isEmpty())
        setTool(toolBeforeRead_, false);
    if (on && organizing())
        showOrganize(false);
    toolsBox_->setVisible(!on);
    side_->setVisible(!on && showThumbs_);
    tabs_->setVisible(!on);
    readBar_->setVisible(on);
    readButton_->setLatched(on);
    placeReadBar();
    say(on ? tr("Read mode: just the pages. Esc - or Read mode in the status bar - brings the tools back.") : tr("The tools are back."));
    updateUi();
    view_->setFocus();
}

void MainWindow::placeReadBar()
{
    if (!readBar_ || !readBar_->isVisible())
        return;
    readBar_->adjustSize();
    const QRect r = viewBox_->rect();
    readBar_->move(r.left() + (r.width() - readBar_->width()) / 2, r.bottom() - readBar_->height() - 24);
    readBar_->raise();
}

bool MainWindow::organizing() const
{
    return middleStack_ && middleStack_->currentWidget() == organize_;
}

void MainWindow::showOrganize(bool on)
{
    if (on && (!view_->document() || current_ < 0))
        return;
    if (on && readMode_)
        setReadMode(false);
    organize_->setCreating(on && isCreating(view_->document()));
    if (on) {
        organize_->setDocument(view_->document());
        organize_->select({std::max(0, view_->currentPage())});
        middleStack_->setCurrentWidget(organize_);
        organize_->setFocus();
        say(tr("Organize Pages: click a page to pick it (Ctrl, Shift: more), drag them to move them. Close (or Esc) goes back."));
    } else {
        middleStack_->setCurrentWidget(split_);
        if (organize_->selection().size() >= 1 && view_->document())
            view_->goToPage(std::min(organize_->selection().front(), view_->document()->pageCount() - 1));
        view_->setFocus();
    }
    buttons_["organize"]->setLatched(on);
    updateUi();
}

void MainWindow::replacePages(const std::vector<int> &pagesIn)
{
    // the picked pages taken out, another file's pages put in their place
    auto doc = view_->document();
    if (!doc || pagesIn.empty() || !doc->editable())
        return;
    std::vector<int> pages = pagesIn;
    std::sort(pages.begin(), pages.end());
    const QString path = QFileDialog::getOpenFileName(this, tr("Replace pages with"), theme::settings().value("last_folder").toString(),
                                                      tr("PDF files (*.pdf);;All files (*)"));
    if (path.isEmpty())
        return;
    const QString what = pages.size() == 1 ? tr("page %1").arg(pages[0] + 1) : tr("these %1 pages").arg(pages.size());
    if (classicMessage(this, tr("Replace pages"), tr("Replace %1 with the pages of %2?").arg(what, QFileInfo(path).fileName()),
                       {tr("Replace"), tr("Cancel")}) != tr("Replace"))
        return;
    QStringList problems;
    QApplication::setOverrideCursor(classic::waitCursor());
    const int at = pages.front();
    const int added = doc->insertFiles({path}, at, &problems);
    QString error;
    bool ok = added > 0;
    if (ok) {
        std::vector<int> old;
        for (int q : pages)
            old.push_back(q + (q >= at ? added : 0));
        ok = doc->deletePages(old, &error);
    }
    QApplication::restoreOverrideCursor();
    if (!ok) {
        if (!problems.isEmpty())
            error = problems.join(QLatin1Char('\n'));
        return failed(tr("replace pages"), error);
    }
    afterChange(at);
    std::vector<int> fresh;
    for (int n = 0; n < added; ++n)
        fresh.push_back(at + n);
    organize_->select(fresh);
    say(tr("Replaced %1 page(s) with %2 page(s) of %3.").arg(pages.size()).arg(added).arg(QFileInfo(path).fileName()));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *e)
{
    if (watched == viewBox_ && e->type() == QEvent::Resize)
        placeReadBar();
    if (view_ && watched == view_->viewport() && e->type() == QEvent::Resize)
        placeStartPanel();
    if (e->type() != QEvent::KeyPress)
        return ClassicWindow::eventFilter(watched, e);
    auto *key = static_cast<QKeyEvent *>(e);
    if (readMode_ && key->key() == Qt::Key_Escape && !QApplication::activePopupWidget() && isActiveWindow()) {
        setReadMode(false);
        return true;
    }
    if (watched == findBox_) {
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            findNext(key->modifiers() & Qt::ShiftModifier ? -1 : 1);
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            findBox_->clear();
            searchDone(QString(), {}, false);
            view_->setFocus();
            return true;
        }
    }
    auto *w = qobject_cast<QWidget *>(watched);
    if (!w || w->window() != this)
        return ClassicWindow::eventFilter(watched, e);
    // the page keys work anywhere but in a text box - the text typed on the page too (Page Up / Down, Home, End)
    const bool typing = qobject_cast<QLineEdit *>(w) || qobject_cast<QComboBox *>(w) || qobject_cast<QTextEdit *>(w) ||
                        qobject_cast<QPlainTextEdit *>(w);
    if (!typing && key->modifiers() == Qt::NoModifier && view_->document() && w != view_ &&
        w != view_->viewport()) {
        switch (key->key()) {
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            QApplication::sendEvent(view_, key);
            return true;
        default:
            break;
        }
    }
    // Another keyboard layout (Arabic...): Ctrl + the key that's C on an English keyboard
    // types another letter, and Qt matches shortcuts by the letter - so the key itself is
    // looked at (in text boxes too: copy, paste, cut, select all, undo, redo)
    const Qt::KeyboardModifiers mods =
        key->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    const quint32 vk = key->nativeVirtualKey();
    if (!(mods & Qt::ControlModifier) || vk < 'A' || vk > 'Z' ||
        (key->key() >= Qt::Key_A && key->key() <= Qt::Key_Z))
        return ClassicWindow::eventFilter(watched, e);  // (a Latin letter: the usual way works)
    if (auto *line = qobject_cast<QLineEdit *>(w); line && mods == Qt::ControlModifier) {
        switch (vk) {
        case 'C': line->copy(); return true;
        case 'V': line->paste(); return true;
        case 'X': line->cut(); return true;
        case 'A': line->selectAll(); return true;
        case 'Z': line->undo(); return true;
        case 'Y': line->redo(); return true;
        default: break;
        }
    }
    // (... and in text typed on the page - its own Undo, not the PDF's)
    auto inBox = [&](auto *box) {
        switch (vk) {
        case 'C': box->copy(); return true;
        case 'V': box->paste(); return true;
        case 'X': box->cut(); return true;
        case 'A': box->selectAll(); return true;
        case 'Z': box->undo(); return true;
        case 'Y': box->redo(); return true;
        default: return false;
        }
    };
    if (auto *box = qobject_cast<QTextEdit *>(w); box && mods == Qt::ControlModifier && (vk == 'Z' || vk == 'Y')) {
        undoInPlace(box, vk == 'Y');
        return true;
    }
    if (auto *box = qobject_cast<QTextEdit *>(w); box && mods == Qt::ControlModifier && inBox(box))
        return true;
    if (auto *box = qobject_cast<QPlainTextEdit *>(w); box && mods == Qt::ControlModifier && inBox(box))
        return true;
    const QKeySequence wanted(QKeyCombination(mods, Qt::Key(Qt::Key_A + int(vk - 'A'))));
    for (QAction *a : actions()) {
        if (!a->isEnabled() || !a->shortcuts().contains(wanted))
            continue;
        a->trigger();
        return true;
    }
    return ClassicWindow::eventFilter(watched, e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const QUrl &url : e->mimeData()->urls())
        if (url.isLocalFile() && (isPdf(url.toLocalFile()) || isPicture(url.toLocalFile()) || isWord(url.toLocalFile())))
            paths << url.toLocalFile();
    if (paths.isEmpty()) {
        say(tr("Only PDFs, Word documents and pictures can be dropped here."));
        return;
    }
    e->acceptProposedAction();
    // (after the drop has finished: Windows waits for it)
    QTimer::singleShot(10, this, [this, paths] { dropped(paths); });
}

void MainWindow::dropped(const QStringList &paths)
{
    auto doc = view_->document();
    if (!doc) {
        if (paths.size() == 1 && isPdf(paths[0]))
            openFile(paths[0]);
        else
            createFromFiles(paths);
        return;
    }
    const QString what = paths.size() == 1 ? QFileInfo(paths[0]).fileName() : tr("these %1 files").arg(paths.size());
    const QString choice = classicMessage(
        this, tr("Master PDF"),
        tr("Add the pages of %1 to the end of %2?\n(Or open it on its own instead.)").arg(what, name_),
        {tr("Add pages"), tr("Open"), tr("Cancel")});
    if (choice == tr("Add pages") && doc->editable())
        insertPaths(paths, doc->pageCount());
    else if (choice == tr("Open")) {
        if (paths.size() == 1 && isPdf(paths[0])) {
            openFile(paths[0]);
        } else {
            createFromFiles(paths);
        }
    }
}
