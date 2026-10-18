// MainWindow's updates (as the Python app's): what's new shown once after an update, a newer version offered at start,
// Help > Check for updates, the title bar's update button, and the update downloaded and installed.

#include "app/MainWindow.h"
#include "app/Updates.h"

#include "ui/Sounds.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// a message box with a heading and a block of text read only, scrolled (the release notes)
void notesDialog(QWidget *owner, const QString &heading, const QString &notes)
{
    ClassicDialog dlg(owner, QObject::tr("Master PDF"));
    auto *box = new QVBoxLayout(dlg.body());
    box->setContentsMargins(12, 12, 12, 12);
    box->setSpacing(8);
    auto *head = new QLabel(heading, dlg.body());
    head->setFont(theme::font(8, true));
    box->addWidget(head);
    auto *text = new QPlainTextEdit(dlg.body());
    text->setFont(theme::font());
    text->setReadOnly(true);
    text->setPlainText(notes.isEmpty() ? QObject::tr("(no release notes)") : notes);
    const QFontMetrics fm(theme::font());
    text->setFixedSize(fm.horizontalAdvance(QLatin1Char('x')) * 58 + 24, fm.lineSpacing() * 14 + 12);  // (58 x 14, as the Python app's)
    box->addWidget(text);
    auto *row = new QHBoxLayout;
    row->addStretch();
    auto *ok = new ClassicButton(QObject::tr("OK"), dlg.body());
    ok->setMinimumWidth(75);
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QWidget::close);
    row->addWidget(ok);
    row->addStretch();
    box->addLayout(row);
    ok->setFocus();
    sounds::play(sounds::Done);
    dlg.exec();
}

// the update's box: what it's downloading, how far - its close button does nothing (it goes when it's done)
class UpdatingBox : public ClassicDialog {
public:
    explicit UpdatingBox(QWidget *owner) : ClassicDialog(owner, QObject::tr("Updating"))
    {
        auto *box = new QVBoxLayout(body());
        box->setContentsMargins(12, 12, 12, 14);
        box->setSpacing(6);
        label = new QLabel(body());
        label->setFont(theme::font());
        box->addWidget(label);
        bar = new ClassicProgress(body());
        box->addWidget(bar);
        body()->setFixedWidth(344);
    }
    QLabel *label = nullptr;
    ClassicProgress *bar = nullptr;
    bool done = false;  // (closed by the download's end, not by its X)

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (!done) {
            e->ignore();
            return;
        }
        ClassicDialog::closeEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape && !done)
            return;
        ClassicDialog::keyPressEvent(e);
    }
};

QString megabytes(qint64 bytes)
{
    return QString::number(std::lround(bytes / 1048576.0));
}

}  // namespace

void MainWindow::startupUpdateTasks()
{
    if (qEnvironmentVariableIsSet("MASTERPDF_SELFTEST") || QApplication::arguments().contains(QStringLiteral("--self-test")))
        return;  // (the build's check: offline, no questions)
    showUpdateNotes();
    // (a newer version, asked of GitHub in the background - not from a copy built from the source, nor if turned off in the
    // settings file)
    if (!updates::canUpdate() || !theme::settings().value("check_updates").toBool(true))
        return;
    updates::latest(this, [this](std::optional<updates::Release> r) {
        if (r)
            offerUpdate(*r);
    });
}

void MainWindow::showUpdateNotes()
{
    const QJsonObject info = theme::settings().value("just_updated").toObject();
    if (info.isEmpty())
        return;
    theme::saveSetting("just_updated", QJsonValue());  // (shown once)
    if (info.value(QStringLiteral("version")).toString() != updates::appVersion())
        return;  // (the update didn't go through: nothing to tell)
    notesDialog(this, tr("Master PDF was updated to version %1.").arg(updates::appVersion()),
                updates::plainNotes(info.value(QStringLiteral("notes")).toString()));
}

void MainWindow::offerUpdate(const updates::Release &r)
{
    if (!updates::newer(r.version))
        return;  // (up to date)
    if (theme::settings().value("skipped_version").toString() == r.version)
        return;  // (skipped: not asked again - Help > Check for updates can still install it)
    sounds::play(sounds::Done);
    const QString now = tr("Update now"), later = tr("Not now"), skip = tr("Skip this version");
    const QString choice = classicMessage(this, tr("Update available"),
                                          tr("Master PDF %1 is available.\nYou have version %2.").arg(r.version, updates::appVersion()),
                                          {now, later, skip});
    if (choice == now)
        installUpdate(r);
    else if (choice == skip)
        theme::saveSetting("skipped_version", r.version);  // (no button, and not asked again)
    else  // (Not now, or the box closed: the title bar's update button, for later - and asked again next time)
        showUpdateButton(r);
}

void MainWindow::showUpdateButton(const updates::Release &r)
{
    setUpdateButton([this, r] {
        const QString now = tr("Update now");
        if (classicMessage(this, tr("Update available"),
                           tr("Master PDF %1 is available.\nYou have version %2.").arg(r.version, updates::appVersion()),
                           {now, tr("Not now")}) == now)
            installUpdate(r);
    });
}

void MainWindow::checkForUpdates()
{
    say(tr("Checking for updates..."));
    updates::latest(this, [this](std::optional<updates::Release> r) {
        updateUi();  // (the status bar back to what it says)
        if (!r) {
            sounds::play(sounds::Error);
            classicMessage(this, tr("Master PDF"), tr("Couldn't reach GitHub - check the internet connection and try again."));
        } else if (!updates::canUpdate()) {
            classicMessage(this, tr("Master PDF"),
                           tr("The newest version is %1.\nThis copy runs from the source code, so it can't update itself.").arg(r->version));
        } else if (updates::newer(r->version)) {
            theme::saveSetting("skipped_version", QJsonValue());
            offerUpdate(*r);
        } else {
            sounds::play(sounds::Done);
            classicMessage(this, tr("Master PDF"), tr("You have the newest version (%1).").arg(updates::appVersion()));
        }
    });
}

void MainWindow::installUpdate(const updates::Release &r)
{
#ifndef Q_OS_WIN
    // (macOS, Linux: the new version's file downloaded in the browser - installed the way the first one was)
    QDesktopServices::openUrl(QUrl(r.url));
#ifdef Q_OS_MACOS
    const QString how = tr(" (drag it into Applications).");
#else
    const QString how = tr(" (replace the old AppImage file).");
#endif
    classicMessage(this, tr("Master PDF"),
                   tr("Master PDF %1 is downloading in your browser.\nInstall it the way you installed this one").arg(r.version) + how +
                       tr("\nThen close this version."));
#else
    // each PDF with changes: asked about first, in its tab (the app closes for the installer)
    for (size_t k = 0; k < docs_.size(); ++k)
        if (docs_[k].doc->dirty() && !isCreating(docs_[k].doc)) {
            showTab(int(k));
            if (!maybeSave())
                return;
        }
    UpdatingBox box(this);
    box.label->setText(tr("Downloading Master PDF %1...").arg(r.version));
    const QString dst = QDir::tempPath() + QStringLiteral("/MasterPDF-Setup-%1.exe").arg(r.version);
    auto *file = new QFile(dst + QStringLiteral(".part"), &box);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("The update couldn't be downloaded:\n%1").arg(file->errorString()));
        return;
    }
    auto *net = new QNetworkAccessManager(&box);
    QNetworkRequest req{QUrl(r.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MasterPDF"));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = net->get(req);
    QString error;
    bool ready = false;
    QPointer<UpdatingBox> alive(&box);
    connect(reply, &QNetworkReply::readyRead, &box, [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, &box, [&box, r](qint64 got, qint64 total) {
        if (total <= 0)
            return;
        box.bar->setValue(double(got) / double(total));
        box.label->setText(tr("Downloading Master PDF %1...   %2 of %3 MB").arg(r.version, megabytes(got), megabytes(total)));
    });
    connect(reply, &QNetworkReply::finished, &box, [&, reply, file] {
        file->write(reply->readAll());
        file->close();
        if (reply->error() != QNetworkReply::NoError) {
            error = reply->errorString();
            QFile::remove(file->fileName());
        } else {
            QFile::remove(dst);
            if (QFile::rename(file->fileName(), dst))
                ready = true;
            else
                error = tr("it couldn't be saved in the temporary folder");
        }
        reply->deleteLater();
        box.done = true;
        box.close();
    });
    box.exec();
    if (!ready) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("The update couldn't be downloaded:\n%1").arg(error));
        return;
    }
    // remembered for the new version to show once it's running; the installer started once this app has closed - and the
    // app closed (its PDFs were saved or let go above)
    theme::saveSetting("just_updated", QJsonObject{{QStringLiteral("version"), r.version}, {QStringLiteral("notes"), r.notes}});
    if (!updates::runInstallerAfterExit(dst)) {
        sounds::play(sounds::Error);
        classicMessage(this, tr("Master PDF"), tr("The update couldn't be started. Its installer is here:\n%1").arg(QDir::toNativeSeparators(dst)));
        return;
    }
    stopScan(nullptr);
    QTimer::singleShot(0, qApp, [] { QApplication::exit(0); });
#endif
}
