// MainWindow's File tools (step 7): Export as pictures, Split, Compress - each in a Windows 98
// dialog box like the Python app's JobDialog (its options, a progress bar, a status line,
// Start and Close - Cancel while it works); Create PDF from files; Word documents made PDFs
// by Microsoft Word itself (or LibreOffice).

#include "app/MainWindow.h"

#include "core/Document.h"
#include "core/Jobs.h"
#include "ui/Classic.h"
#include "ui/DocTabs.h"
#include "ui/Sounds.h"
#include "ui/Theme.h"
#include "ui/TrackBar.h"
#include "ui/Widgets.h"
#include "view/PageView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <atomic>
#include <thread>

namespace {

// the job's dialog: its options, then progress, a status line, Start / Close
class JobDialog : public ClassicDialog {
public:
    JobDialog(QWidget *owner, const QString &title, const QString &start, const QString &folder)
        : ClassicDialog(owner, title), folder_(folder)
    {
        auto *box = new QVBoxLayout(body());
        box->setContentsMargins(10, 10, 10, 10);
        options = new QGroupBox(QStringLiteral(" Options "), body());
        options->setFont(theme::font());
        grid = new QGridLayout(options);
        box->addWidget(options);
        box->addSpacing(4);
        progress = new QProgressBar(body());
        progress->setRange(0, 100);
        progress->setValue(0);
        progress->setTextVisible(false);
        progress->setFixedHeight(18);
        box->addWidget(progress);
        status = new QLabel(QObject::tr("Ready."), body());
        box->addWidget(status);
        auto *row = new QHBoxLayout;
        row->addStretch();
        startButton = new ClassicButton(start, body());
        closeButton = new ClassicButton(QObject::tr("Close"), body());
        startButton->setMinimumWidth(75);
        closeButton->setMinimumWidth(75);
        row->addWidget(startButton);
        row->addWidget(closeButton);
        box->addLayout(row);
        body()->setMinimumWidth(380);
        QObject::connect(closeButton, &QPushButton::clicked, this, [this] {
            if (busy)
                cancel = true;  // (the job stops at its next step)
            else
                close();
        });
    }
    // "Save to:" a folder, with Browse...
    void folderRow(int row)
    {
        grid->addWidget(new QLabel(QObject::tr("Save to:"), options), row, 0);
        auto *where = new QLineEdit(QDir::toNativeSeparators(folder_), options);
        where->setReadOnly(true);
        where->setMinimumWidth(QFontMetrics(theme::font()).horizontalAdvance(QLatin1Char('0')) * 30);
        grid->addWidget(where, row, 1);
        auto *browse = new ClassicButton(QObject::tr("Browse..."), options);
        grid->addWidget(browse, row, 2);
        QObject::connect(browse, &QPushButton::clicked, this, [this, where] {
            const QString d = QFileDialog::getExistingDirectory(this, QObject::tr("Choose a folder"), folder_);
            if (!d.isEmpty()) {
                folder_ = d;
                where->setText(QDir::toNativeSeparators(d));
            }
        });
    }
    QString folder() const { return folder_; }
    // the job on a worker thread; the dialog shows how far it is, and what came of it
    void run(std::function<jobs::Result(const jobs::Step &)> job)
    {
        busy = true;
        cancel = false;
        startButton->setEnabled(false);
        closeButton->setText(QObject::tr("Cancel"));
        progress->setValue(0);
        status->setText(QObject::tr("Working..."));
        QPointer<JobDialog> self(this);
        std::atomic<bool> *stop = &cancel;
        std::thread([self, job, stop] {
            jobs::Result r = job([self, stop](double f) {
                QMetaObject::invokeMethod(qApp, [self, f] {
                    if (self)
                        self->progress->setValue(int(f * 100));
                });
                return !stop->load();
            });
            QMetaObject::invokeMethod(qApp, [self, r] {
                if (self)
                    self->finish(r);
            });
        }).detach();
    }

    QGroupBox *options;
    QGridLayout *grid;
    QProgressBar *progress;
    QLabel *status;
    ClassicButton *startButton, *closeButton;
    std::atomic<bool> cancel{false};
    bool busy = false;

private:
    void finish(const jobs::Result &r)
    {
        busy = false;
        startButton->setEnabled(true);
        closeButton->setText(QObject::tr("Close"));
        if (r.cancelled) {
            progress->setValue(0);
            status->setText(QObject::tr("Cancelled. %1 file(s) saved before stopping.").arg(r.written.size()));
            return;
        }
        if (!r.error.isEmpty()) {
            status->setText(QObject::tr("It didn't work - see the message."));
            sounds::play(sounds::Error);
            classicMessage(this, QObject::tr("Master PDF"), QObject::tr("Couldn't finish:\n%1").arg(r.error));
            return;
        }
        progress->setValue(100);
        status->setText(r.message);
        if (r.written.isEmpty()) {
            sounds::play(sounds::Error);
            classicMessage(this, QObject::tr("Master PDF"), r.message);
            return;
        }
        sounds::play(sounds::Done);
        if (classicMessage(this, QObject::tr("Master PDF"), r.message, {QObject::tr("Show files"), QObject::tr("OK")}) ==
            QObject::tr("Show files"))
            showInFolder(r.written.first());
    }

    QString folder_;
};

QString documentsFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

}  // namespace

QString MainWindow::jobFolder() const
{
    auto doc = view_->document();
    return doc && !doc->path().isEmpty() ? QFileInfo(doc->path()).absolutePath() : documentsFolder();
}

void MainWindow::exportDialog()
{
    auto doc = view_->document();
    if (!doc)
        return;
    JobDialog dlg(this, tr("Export as pictures"), tr("Export"), jobFolder());
    auto *g = dlg.grid;
    g->addWidget(new QLabel(tr("Format:"), dlg.options), 0, 0);
    auto *format = new QComboBox(dlg.options);
    format->addItems(jobs::pictureFormats());
    g->addWidget(format, 0, 1, Qt::AlignLeft);
    g->addWidget(new QLabel(tr("Resolution:"), dlg.options), 1, 0);
    auto *dpi = new QComboBox(dlg.options);
    dpi->addItems({"72 dpi", "96 dpi", "150 dpi", "200 dpi", "300 dpi", "600 dpi"});
    dpi->setCurrentText(QStringLiteral("150 dpi"));
    g->addWidget(dpi, 1, 1, Qt::AlignLeft);
    g->addWidget(new QLabel(tr("Quality:"), dlg.options), 2, 0);
    auto *quality = new TrackBar(dlg.options, 200);
    quality->setRange(0, 100, 10);
    quality->setValue(90);
    auto *qnote = new EngravedLabel(dlg.options);
    auto *qrow = new QHBoxLayout;
    qrow->addWidget(quality);
    qrow->addWidget(qnote);
    qrow->addStretch();
    g->addLayout(qrow, 2, 1, 1, 2);
    g->addWidget(new QLabel(tr("Pages:"), dlg.options), 3, 0, Qt::AlignTop);
    const int n = doc->pageCount(), here = std::max(0, view_->currentPage());
    auto *all = new QRadioButton(tr("All %1 pages").arg(n), dlg.options);
    auto *current = new QRadioButton(tr("Just this page (%1)").arg(here + 1), dlg.options);
    auto *some = new QRadioButton(tr("Pages:"), dlg.options);
    auto *range = new QLineEdit(QStringLiteral("1-%1").arg(n), dlg.options);
    all->setChecked(true);
    auto *col = new QVBoxLayout;
    col->addWidget(all);
    col->addWidget(current);
    auto *rr = new QHBoxLayout;
    rr->addWidget(some);
    rr->addWidget(range);
    rr->addStretch();
    col->addLayout(rr);
    g->addLayout(col, 3, 1, 1, 2);
    dlg.folderRow(4);
    auto changed = [format, quality, qnote, some, range] {
        const bool lossy = format->currentText() == "JPEG" || format->currentText() == "WEBP";
        quality->setEnabled(lossy);
        qnote->setText(lossy ? QString() : tr("(not used by %1)").arg(format->currentText()));
        range->setEnabled(some->isChecked());
    };
    connect(format, &QComboBox::currentTextChanged, &dlg, changed);
    for (QRadioButton *b : {all, current, some})
        connect(b, &QRadioButton::toggled, &dlg, changed);
    changed();
    const QString base = QFileInfo(name_).completeBaseName();
    connect(dlg.startButton, &QPushButton::clicked, &dlg, [&, n, here, base] {
        std::vector<int> pages;
        if (all->isChecked()) {
            for (int p = 0; p < n; ++p)
                pages.push_back(p);
        } else if (current->isChecked()) {
            pages.push_back(here);
        } else {
            QString error;
            const auto ranges = jobs::parseRanges(range->text(), &error);
            if (ranges.empty()) {
                classicMessage(&dlg, tr("Master PDF"), error);
                return;
            }
            for (const auto &[a, b] : ranges)
                for (int p = a; p <= std::min(n, b.value_or(n)); ++p)
                    if (std::find(pages.begin(), pages.end(), p - 1) == pages.end())
                        pages.push_back(p - 1);
            if (pages.empty()) {
                classicMessage(&dlg, tr("Master PDF"), tr("The PDF has only %1 pages.").arg(n));
                return;
            }
        }
        const QByteArray pdf = doc->snapshot();
        const QString fmt = format->currentText(), folder = dlg.folder();
        const int d = dpi->currentText().section(QLatin1Char(' '), 0, 0).toInt(), q = std::max(1, quality->value());
        dlg.run([=](const jobs::Step &step) { return jobs::exportPictures(pdf, pages, fmt, d, q, folder, base, step); });
    });
    dlg.exec();
}

void MainWindow::splitDialog()
{
    auto doc = view_->document();
    if (!doc)
        return;
    JobDialog dlg(this, tr("Split"), tr("Split"), jobFolder());
    auto *g = dlg.grid;
    g->addWidget(new QLabel(tr("Split into:"), dlg.options), 0, 0);
    auto *mode = new QComboBox(dlg.options);
    mode->addItems({tr("Single pages"), tr("Every few pages"), tr("Page ranges")});
    g->addWidget(mode, 0, 1, Qt::AlignLeft);
    g->addWidget(new QLabel(tr("Pages:"), dlg.options), 1, 0);
    auto *pages = new QLineEdit(dlg.options);
    auto *pnote = new EngravedLabel(dlg.options);
    auto *row = new QHBoxLayout;
    row->addWidget(pages);
    row->addWidget(pnote);
    row->addStretch();
    g->addLayout(row, 1, 1, 1, 2);
    dlg.folderRow(2);
    QStringList typed = {QString(), QStringLiteral("2"), QStringLiteral("1-3, 5, 8-")};  // (kept for each)
    int was = 0;
    auto changed = [&] {
        if (was > 0)
            typed[was] = pages->text();
        was = mode->currentIndex();
        pages->setEnabled(was > 0);
        pages->setText(typed[was]);
        pnote->setText(was == 0 ? tr("(every page its own file)") : was == 1 ? tr("(how many pages in each file)")
                                                                             : tr("(for example 1-3, 5, 8-)"));
    };
    connect(mode, &QComboBox::currentIndexChanged, &dlg, changed);
    changed();
    const int n = doc->pageCount();
    const QString base = QFileInfo(name_).completeBaseName();
    connect(dlg.startButton, &QPushButton::clicked, &dlg, [&, n, base] {
        std::vector<std::pair<int, int>> parts;
        if (mode->currentIndex() == 0) {
            for (int p = 1; p <= n; ++p)
                parts.emplace_back(p, p);
        } else if (mode->currentIndex() == 1) {
            bool ok = false;
            const int k = pages->text().trimmed().toInt(&ok);
            if (!ok || k < 1) {
                classicMessage(&dlg, tr("Master PDF"), tr("Type how many pages each file gets (a number, like 2)."));
                return;
            }
            for (int a = 1; a <= n; a += k)
                parts.emplace_back(a, std::min(a + k - 1, n));
        } else {
            QString error;
            const auto ranges = jobs::parseRanges(pages->text(), &error);
            if (ranges.empty()) {
                classicMessage(&dlg, tr("Master PDF"), error);
                return;
            }
            for (const auto &[a, b] : ranges) {
                if (a > n) {
                    classicMessage(&dlg, tr("Master PDF"), tr("The PDF has only %1 pages, so it has no page %2.").arg(n).arg(a));
                    return;
                }
                parts.emplace_back(a, std::min(n, b.value_or(n)));
            }
        }
        const QByteArray pdf = doc->snapshot();
        const QString folder = dlg.folder();
        dlg.run([=](const jobs::Step &step) { return jobs::split(pdf, parts, folder, base, step); });
    });
    dlg.exec();
}

void MainWindow::compressDialog()
{
    auto doc = view_->document();
    if (!doc)
        return;
    JobDialog dlg(this, tr("Compress"), tr("Compress"), jobFolder());
    auto *g = dlg.grid;
    g->addWidget(new QLabel(tr("Pictures:"), dlg.options), 0, 0);
    auto *level = new QComboBox(dlg.options);
    const std::vector<std::pair<QString, int>> levels = {{tr("Small file (96 dpi)"), 96}, {tr("Balanced (150 dpi)"), 150},
                                                         {tr("High quality (220 dpi)"), 220},
                                                         {tr("Keep the pictures as they are"), 0}};
    for (const auto &[name, d] : levels)
        level->addItem(name, d);
    level->setCurrentIndex(1);
    g->addWidget(level, 0, 1, Qt::AlignLeft);
    g->addWidget(new QLabel(tr("Quality:"), dlg.options), 1, 0);
    auto *quality = new TrackBar(dlg.options, 200);
    quality->setRange(0, 100, 10);
    quality->setValue(70);
    auto *qnote = new EngravedLabel(dlg.options);
    auto *qrow = new QHBoxLayout;
    qrow->addWidget(quality);
    qrow->addWidget(qnote);
    qrow->addStretch();
    g->addLayout(qrow, 1, 1, 1, 2);
    auto *strip = new QCheckBox(tr("Remove the document's details (title, author, program...)"), dlg.options);
    g->addWidget(strip, 2, 0, 1, 3);
    dlg.folderRow(3);
    auto changed = [level, quality, qnote] {
        const bool used = level->currentData().toInt() > 0;
        quality->setEnabled(used);
        qnote->setText(used ? QString() : tr("(not used: the pictures are kept)"));
    };
    connect(level, &QComboBox::currentIndexChanged, &dlg, changed);
    changed();
    const QString base = QFileInfo(name_).completeBaseName();
    connect(dlg.startButton, &QPushButton::clicked, &dlg, [&, base] {
        const qint64 before = !doc->path().isEmpty() && !doc->dirty() ? QFileInfo(doc->path()).size() : 0;
        const QByteArray pdf = doc->snapshot();
        const QString folder = dlg.folder();
        const int d = level->currentData().toInt(), q = std::max(1, quality->value());
        const bool s = strip->isChecked();
        dlg.run([=](const jobs::Step &step) { return jobs::compress(pdf, d, q, s, folder, base, before, step); });
    });
    dlg.exec();
}

// ---- Word documents made PDFs ----

bool MainWindow::isWord(const QString &path)
{
    for (const char *e : {".docx", ".doc", ".docm", ".dotx", ".dotm", ".dot", ".rtf", ".odt"})
        if (path.endsWith(QLatin1String(e), Qt::CaseInsensitive))
            return true;
    return false;
}

// a Word document made a PDF by Microsoft Word itself (read-only, no questions) - or by
// LibreOffice when there's no Word. An empty string: it couldn't be (error says why).
QString MainWindow::wordToPdf(const QString &src, const QString &folder, QString *error)
{
    const QString dst = folder + QLatin1Char('/') + QFileInfo(src).completeBaseName() + QStringLiteral(".pdf");
    static const char *script = R"(
$ErrorActionPreference = 'Stop'
$word = New-Object -ComObject Word.Application
try {
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $doc = $word.Documents.Open($env:MP_SRC, $false, $true, $false, '~no password~')
    $doc.ExportAsFixedFormat($env:MP_DST, 17)
    $doc.Close(0)
} finally {
    $word.Quit()
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
}
)";
    QProcess word;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MP_SRC"), QDir::toNativeSeparators(src));
    env.insert(QStringLiteral("MP_DST"), QDir::toNativeSeparators(dst));
    word.setProcessEnvironment(env);
#ifdef Q_OS_WIN
    word.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                                                  QStringLiteral("Bypass"), QStringLiteral("-Command"), QString::fromLatin1(script)});
    word.waitForFinished(180000);
    if (QFileInfo::exists(dst))
        return dst;
#endif
    // no Word (or it failed): LibreOffice
    for (const QString &soffice : {QStringLiteral("C:/Program Files/LibreOffice/program/soffice.exe"),
                                   QStringLiteral("C:/Program Files (x86)/LibreOffice/program/soffice.exe"),
                                   QStringLiteral("/Applications/LibreOffice.app/Contents/MacOS/soffice"),
                                   QStringLiteral("/usr/bin/soffice"), QStringLiteral("/usr/bin/libreoffice"),
                                   QStringLiteral("/usr/local/bin/soffice"), QStringLiteral("/snap/bin/libreoffice")}) {
        if (!QFileInfo::exists(soffice))
            continue;
        QProcess lo;
        lo.start(soffice, {QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("pdf"),
                           QStringLiteral("--outdir"), folder, src});
        lo.waitForFinished(180000);
        if (QFileInfo::exists(dst))
            return dst;
    }
    if (error)
        *error = tr("%1 couldn't be made a PDF (it needs Microsoft Word or LibreOffice).").arg(QFileInfo(src).fileName());
    return {};
}

// the Word documents among paths made PDFs (in a temporary folder, kept until the app closes);
// the others as they are; problems: the ones that couldn't be
QStringList MainWindow::fromWord(const QStringList &paths, QStringList *problems)
{
    QStringList out;
    for (const QString &p : paths) {
        if (!isWord(p)) {
            out << p;
            continue;
        }
        if (!convertDir_) {
            convertDir_ = std::make_unique<QTemporaryDir>();
        }
        say(tr("Making %1 a PDF (with Word)...").arg(QFileInfo(p).fileName()));
        QApplication::setOverrideCursor(classic::waitCursor());
        QApplication::processEvents();
        QString error;
        const QString pdf = wordToPdf(p, convertDir_->path(), &error);
        QApplication::restoreOverrideCursor();
        if (pdf.isEmpty())
            *problems << error;
        else
            out << pdf;
    }
    return out;
}

// Create PDF from files (Home's Create, the File menu's, files dropped): PDFs, pictures and Word documents put
// together into a new PDF - shown first in Organize Pages to be put in order, named and created there (not saved
// until Save)
void MainWindow::createFromFiles(QStringList paths)
{
    if (paths.isEmpty())
        paths = QFileDialog::getOpenFileNames(
            this, tr("Create PDF from files"), theme::settings().value("last_folder").toString(),
            tr("PDFs, Word documents and pictures (*.pdf *.docx *.doc *.docm *.rtf *.odt *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp);;All files (*)"));
    if (paths.isEmpty())
        return;
    QStringList problems;
    const QStringList files = fromWord(paths, &problems);
    QString error;
    auto doc = Document::create(&error);
    int added = 0;
    if (doc && !files.isEmpty()) {
        QApplication::setOverrideCursor(classic::waitCursor());
        added = doc->insertFiles(files, 0, &problems);
        QApplication::restoreOverrideCursor();
    }
    if (!problems.isEmpty()) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Some files were left out"), problems.mid(0, 10).join(QLatin1Char('\n')));
    }
    if (!added)
        return;
    creating_ = doc;
    showDocument(doc, tr("New PDF"));  // (in Organize Pages, as it's being made: tabChosen)
    say(tr("%1 page(s) from %2 file(s): put them in order, name the PDF, then press Create.").arg(added).arg(paths.size()));
}

bool MainWindow::isCreating(const std::shared_ptr<Document> &doc) const
{
    return doc && creating_.lock() == doc;
}

// the new PDF's pages put in order and named: made - a PDF like any other, its name the one Save suggests
void MainWindow::finishCreating(const QString &typed)
{
    auto doc = view_->document();
    if (!isCreating(doc) || current_ < 0)
        return;
    const QString name = typed.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) ? typed : typed + QStringLiteral(".pdf");
    creating_.reset();
    name_ = name;
    docs_[size_t(current_)].name = name;
    tabs_->setTabText(current_ + 1, name, name);
    showOrganize(false);
    updateUi();
    say(tr("Created %1 (%2 page(s)). Save it with File > Save.").arg(name).arg(doc->pageCount()));
}

// ... or not made: thrown away, its tab closed without asking
void MainWindow::cancelCreating()
{
    if (current_ < 0 || !isCreating(view_->document()))
        return;
    const int doc = current_;
    creating_.reset();
    current_ = -1;
    docs_.erase(docs_.begin() + doc);
    tabs_->removeTab(doc + 1);
    tabChosen(tabs_->current());
    say(tr("The new PDF wasn't created."));
}
