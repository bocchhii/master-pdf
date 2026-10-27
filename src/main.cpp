#include "app/MainWindow.h"
#include "core/Document.h"
#include "core/Fonts.h"
#include "core/Ocr.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "ui/Win98Style.h"

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QTimer>

#include <cstdio>

namespace {

// (the self-test: how far it got, written as it goes - a build's check that never ends shows where it stopped)
void selfTestStep(const char *step)
{
    if (!QApplication::arguments().contains(QStringLiteral("--self-test")))
        return;
    QFile out(qEnvironmentVariable("MASTERPDF_SELFTEST_OUT"));
    if (!out.fileName().isEmpty() && out.open(QIODevice::Append))
        out.write(QByteArray("step: ") + step + '\n');
}

// the build's check (GitHub's, on each system): MasterPDF --self-test file.pdf [shot.png] - the PDF opened, its first page
// read by the text reader (OCR), the fonts and the reader's languages found next to the program, a picture of the window
// saved. Exit 0 if it all went; what was found printed (and in the file MASTERPDF_SELFTEST_OUT names). No questions asked.
int selfTest(QApplication &app, MainWindow &window, const QStringList &args)
{
    setAutoAnswer([](const QString &message, const QStringList &buttons) {
        std::fprintf(stderr, "message: %s\n", qPrintable(message));
        return buttons.value(0);
    });
    QStringList found, problems;
    QString error;
    selfTestStep("opening the PDF");
    auto doc = Document::open(args.value(2), &error);
    if (!doc) {
        problems << QStringLiteral("couldn't open %1: %2").arg(args.value(2), error);
    } else {
        window.showDocument(doc, QFileInfo(args.value(2)).fileName());
        found << QStringLiteral("opened: %1 page(s)").arg(doc->pageCount());
        selfTestStep("drawing its page");
        const QSizeF size = doc->pageSize(0) * 2.0;
        const QImage page = doc->render(0, 2.0, QRect(0, 0, int(size.width()), int(size.height())));
        selfTestStep("reading it (OCR)");
        QString text;
        for (const ocr::Line &line : ocr::read(page, &error))
            text += line.text + QLatin1Char(' ');
        if (text.trimmed().isEmpty())
            problems << QStringLiteral("the text reader read nothing: %1").arg(error);
        else
            found << QStringLiteral("read: %1").arg(text.trimmed().left(80));
    }
    const int fontFiles = QDir(fonts::bundledFolder()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    if (fontFiles == 0)
        problems << QStringLiteral("no fonts in %1").arg(fonts::bundledFolder());
    else
        found << QStringLiteral("fonts: %1 families").arg(fontFiles);
    if (ocr::dataFolder().isEmpty())
        problems << QStringLiteral("no languages for the text reader");
    else
        found << QStringLiteral("text reader: %1").arg(ocr::languages().join(QLatin1Char('+')));
    int code = 0;
    selfTestStep("waiting for the window to be drawn");
    QTimer::singleShot(3000, &window, [&] {  // (its pages drawn meanwhile)
        selfTestStep("saving the picture of the window");
        if (args.size() > 3)
            window.grab().save(args.at(3));
        const QString report = (problems.isEmpty() ? QStringLiteral("ok\n") : QStringLiteral("FAILED\n")) +
                               (problems + found).join(QLatin1Char('\n')) + QLatin1Char('\n');
        std::fputs(report.toUtf8().constData(), stdout);
        std::fflush(stdout);
        QFile out(qEnvironmentVariable("MASTERPDF_SELFTEST_OUT"));
        if (!out.fileName().isEmpty() && out.open(QIODevice::WriteOnly))
            out.write(report.toUtf8());
        code = problems.isEmpty() ? 0 : 1;
        app.quit();
    });
    app.exec();
    return code;
}

}  // namespace

int main(int argc, char *argv[])
{
    // no JIT for regular expressions: the code it makes faults on purpose (Windows handles it),
    // which stops a debugger (F5) with SIGSEGV in pcre2 when the font index starts - and the
    // app's patterns are tiny, nothing's lost
    qputenv("QT_ENABLE_REGEXP_JIT", "0");
    QApplication app(argc, argv);
    selfTestStep("started");
    QApplication::setApplicationName(QStringLiteral("Master PDF"));
    QApplication::setOrganizationName(QStringLiteral("Master PDF"));
    // the Python app's look: Windows 98, in the theme chosen last time
    QApplication::setStyle(new Win98Style);
    const QStringList args = QApplication::arguments();
    theme::apply(theme::choice(), false);
    QApplication::setPalette(theme::palette());
    QApplication::setFont(theme::font());
    QIcon icon(QStringLiteral(":/icons/app.png"));
    icon.addFile(QStringLiteral(":/icons/app16.png"));  // (where it's small: the picture drawn for 16 pixels)
    QApplication::setWindowIcon(icon);

    selfTestStep("looking up the fonts");
    fonts::startScan();  // (the installed fonts looked up in the background)
    for (const QString &folder : {fonts::bundledFolder(), fonts::myFolder()}) {
        QDirIterator it(folder, {QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            QFontDatabase::addApplicationFont(it.next());
    }
    selfTestStep("making the window");
    MainWindow window;
    selfTestStep("showing the window");
    window.show();
    if (theme::settings().value("start_maximized").toBool())
        window.toggleMaximize();

    if (args.size() > 2 && args.at(1) == QLatin1String("--self-test"))
        return selfTest(app, window, args);
    if (args.size() > 1)
        window.openFile(args.at(1));
    // (just updated: what's new; then a newer version looked for in the background - as the Python app, a moment after it opens)
    QTimer::singleShot(800, &window, &MainWindow::startupUpdateTasks);
    return app.exec();
}
