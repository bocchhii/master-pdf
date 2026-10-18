#include "app/Updates.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifndef MASTERPDF_VERSION
#define MASTERPDF_VERSION "dev"
#endif

namespace updates {

namespace {
const char *kReleases = "https://api.github.com/repos/bocchhii/master-pdf/releases/latest";
}

QString appVersion()
{
    return QStringLiteral(MASTERPDF_VERSION);
}

QList<int> parseVersion(const QString &text)
{
    QList<int> out;
    static const QRegularExpression number(QStringLiteral("[0-9]+"));
    for (auto m = number.globalMatch(text); m.hasNext() && out.size() < 3;)
        out << m.next().captured().toInt();
    return out;
}

bool canUpdate()
{
    return !parseVersion(appVersion()).isEmpty();
}

bool newer(const QString &version)
{
    const QList<int> theirs = parseVersion(version), mine = parseVersion(appVersion());
    return !theirs.isEmpty() && !mine.isEmpty() && theirs > mine;
}

bool fileIsMine(const QString &nameIn)
{
    const QString name = nameIn.toLower();
#if defined(Q_OS_WIN)
    return name.endsWith(QLatin1String("setup.exe"));
#elif defined(Q_OS_MACOS)
    const bool intel = QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("x86"));
    return name.endsWith(QLatin1String(".dmg")) && name.contains(QLatin1String("intel")) == intel;
#else
    return name.endsWith(QLatin1String(".appimage"));
#endif
}

void latest(QObject *context, std::function<void(std::optional<Release>)> then)
{
    auto *net = new QNetworkAccessManager(context);
    QNetworkRequest req(QUrl(QString::fromLatin1(kReleases)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MasterPDF"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(10000);
    QNetworkReply *reply = net->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, net, then] {
        reply->deleteLater();
        net->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            then(std::nullopt);
            return;
        }
        const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object();
        Release r;
        r.version = data.value(QStringLiteral("tag_name")).toString();
        r.version.remove(QRegularExpression(QStringLiteral("^[vV]")));
        r.notes = data.value(QStringLiteral("body")).toString();
        for (const QJsonValue &a : data.value(QStringLiteral("assets")).toArray())
            if (fileIsMine(a.toObject().value(QStringLiteral("name")).toString())) {
                r.url = a.toObject().value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        if (r.url.isEmpty() || parseVersion(r.version).isEmpty())
            then(std::nullopt);
        else
            then(r);
    });
}

QString plainNotes(const QString &markdown)
{
    QStringList lines;
    for (QString line : QString(markdown).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(QLatin1Char('\n'))) {
        line.remove(QRegularExpression(QStringLiteral("^\\s*#+\\s*")));                                   // "## Title" -> "Title"
        line.replace(QRegularExpression(QStringLiteral("^(\\s*)[-*]\\s+")), QStringLiteral("\\1\u2022  "));  // "- item" -> a bullet
        line.replace(QRegularExpression(QStringLiteral("\\*\\*(.+?)\\*\\*|__(.+?)__")), QStringLiteral("\\1\\2"));  // bold
        line.replace(QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]\\([^)]+\\)")), QStringLiteral("\\1"));    // [text](link)
        line.remove(QLatin1Char('`'));
        lines << line;
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

bool runInstallerAfterExit(const QString &setup)
{
#ifdef Q_OS_WIN
    // (why wait: the installer replaces MasterPDF.exe and the files beside it - while the app runs they can't be. A hidden
    // helper waits for it to close, then runs the installer silently; the installer starts the new version)
    const QString log = QDir::toNativeSeparators(QDir::tempPath() + QStringLiteral("/MasterPDF-update.log"));
    auto q = [](const QString &s) { return QStringLiteral("'") + QString(s).replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\''); };
    const QString script = QStringList{
        QStringLiteral("$ErrorActionPreference = 'SilentlyContinue'"),
        QStringLiteral("Wait-Process -Id %1 -Timeout 60").arg(QCoreApplication::applicationPid()),  // (the app: closing right now)
        QStringLiteral("Start-Sleep -Milliseconds 300"),
        QStringLiteral("Start-Process -FilePath %1 -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CLOSEAPPLICATIONS',%2")
            .arg(q(QDir::toNativeSeparators(setup)), q(QStringLiteral("\"/LOG=%1\"").arg(log))),
    }.join(QStringLiteral("\r\n"));
    const QString helper = QDir::tempPath() + QStringLiteral("/MasterPDF-update.ps1");
    QFile f(helper);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("\xEF\xBB\xBF");  // (UTF-8, with its mark: PowerShell reads the path's letters right)
        f.write(script.toUtf8());
        f.close();
        QProcess p;
        p.setProgram(QStringLiteral("powershell.exe"));
        p.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"),
                        QStringLiteral("Hidden"), QStringLiteral("-File"), QDir::toNativeSeparators(helper)});
        p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP; });
        if (p.startDetached())
            return true;
    }
    // (no PowerShell: the installer started straight away - it waits for the app to close itself)
    return QProcess::startDetached(setup, {QStringLiteral("/VERYSILENT"), QStringLiteral("/SUPPRESSMSGBOXES"), QStringLiteral("/NORESTART"),
                                           QStringLiteral("/CLOSEAPPLICATIONS"), QStringLiteral("/LOG=") + log});
#else
    Q_UNUSED(setup);
    return false;
#endif
}

}  // namespace updates
