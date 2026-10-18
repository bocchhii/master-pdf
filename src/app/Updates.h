#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

class QObject;

// Updates, as the Python app's: new versions come from the project's GitHub Releases - only the public release list
// is read. Windows: the new installer downloaded and run silently over this version once the app has closed (it starts
// the new version, which then shows what's new). macOS, Linux: the new version's file opened in the browser, installed
// the way the first one was.
namespace updates {

// this copy's version - put in by the release build from its tag (v2.0.0 -> "2.0.0"); "dev" built from the source code
QString appVersion();
// "v2.10.1" -> {2, 10, 1}, so versions compare as numbers; empty if there's none
QList<int> parseVersion(const QString &text);
bool canUpdate();  // (a released copy: a version to compare - not one built from the source)
bool newer(const QString &version);  // (newer than this copy)

struct Release {
    QString version, notes, url;  // (url: the file for this system - the Windows installer, the Mac's disk image, the AppImage)
};
// the newest release, asked of GitHub in the background: then(it) on the window's thread - nothing if GitHub couldn't be
// reached (no internet...), or it has no file for this system
void latest(QObject *context, std::function<void(std::optional<Release>)> then);
// whether a release's file is the one for this system: the Windows installer, the macOS disk image (Apple chips or
// Intel), the Linux AppImage
bool fileIsMine(const QString &name);
// GitHub's release notes are written in Markdown: as tidy plain text
QString plainNotes(const QString &markdown);
// Windows: the installer downloaded to setup started - silently, over this version - once this app has closed (a small
// hidden helper waits for it); its log in %TEMP%\MasterPDF-update.log
bool runInstallerAfterExit(const QString &setup);

}  // namespace updates
