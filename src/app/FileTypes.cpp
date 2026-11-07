#include "app/FileTypes.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#ifdef Q_OS_WIN

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>

namespace {

const std::wstring kProgId = L"MasterPDF.Document";
const std::wstring kAppName = L"Master PDF";
const std::wstring kClasses = L"Software\\Classes\\";
const std::wstring kCapabilities = L"Software\\Master PDF\\Capabilities";
const std::wstring kRegistered = L"Software\\RegisteredApplications";
const std::wstring kOpenWith = kClasses + L".pdf\\OpenWithProgids";
const std::wstring kApplication = kClasses + L"Applications\\MasterPDF.exe";
const std::wstring kVerb = kClasses + L"SystemFileAssociations\\.pdf\\shell\\MasterPDF.Edit";

std::wstring exePath()
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    return path;
}

bool put(const std::wstring &key, const wchar_t *name, const std::wstring &value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

namespace filetypes {

bool supported()
{
    return true;
}

bool available()
{
    return QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/Qt6Core.dll"));
}

bool registerApp()
{
    const std::wstring exe = exePath();
    const std::wstring open = L"\"" + exe + L"\" \"%1\"", icon = L"\"" + exe + L"\",0";
    const std::wstring fileIcon = L"\"" + exe + L"\",1";  // (a PDF's own icon, not the app's: the program's second, app.rc)
    const std::wstring prog = kClasses + kProgId;
    bool ok = true;
    // the kind of file it opens
    ok &= put(prog, nullptr, L"PDF Document");
    ok &= put(prog + L"\\DefaultIcon", nullptr, fileIcon);
    ok &= put(prog + L"\\shell\\open", L"FriendlyAppName", kAppName);
    ok &= put(prog + L"\\shell\\open\\command", nullptr, open);
    // Open with
    ok &= RegSetKeyValueW(HKEY_CURRENT_USER, kOpenWith.c_str(), kProgId.c_str(), REG_NONE, nullptr, 0) == ERROR_SUCCESS;
    ok &= put(kApplication, L"FriendlyAppName", kAppName);
    ok &= put(kApplication + L"\\DefaultIcon", nullptr, fileIcon);
    ok &= put(kApplication + L"\\SupportedTypes", L".pdf", L"");
    ok &= put(kApplication + L"\\shell\\open\\command", nullptr, open);
    // Windows' default apps
    ok &= put(kCapabilities, L"ApplicationName", kAppName);
    ok &= put(kCapabilities, L"ApplicationDescription", L"A fast PDF editor");
    ok &= put(kCapabilities, L"ApplicationIcon", icon);
    ok &= put(kCapabilities + L"\\FileAssociations", L".pdf", kProgId);
    ok &= put(kRegistered, kAppName.c_str(), kCapabilities);
    // the right-click menu (Windows 11: under "Show more options" - its own part of the menu is for signed apps only)
    ok &= put(kVerb, nullptr, L"Edit with Master PDF");
    ok &= put(kVerb, L"Icon", icon);
    ok &= put(kVerb + L"\\command", nullptr, open);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

void unregisterApp()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kVerb.c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegistered.c_str(), kAppName.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kCapabilities.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Master PDF");  // (only if nothing else is in it)
    RegDeleteTreeW(HKEY_CURRENT_USER, kApplication.c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kOpenWith.c_str(), kProgId.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + kProgId).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool isDefault()
{
    std::wstring found(32768, L'\0');
    DWORD size = DWORD(found.size());
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".pdf", L"open", found.data(), &size)))
        return false;
    return CompareStringOrdinal(found.c_str(), -1, exePath().c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool openDefaultsPage()
{
    return INT_PTR(ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Master%20PDF", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

}  // namespace filetypes

#else

namespace filetypes {

bool supported() { return false; }
bool available() { return false; }
bool registerApp() { return false; }
void unregisterApp() {}
bool isDefault() { return false; }
bool openDefaultsPage() { return false; }

}  // namespace filetypes

#endif
