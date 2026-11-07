#pragma once

// Windows: Master PDF offered for PDF files - in Open with, among Windows' default apps, on the right-click menu. Written
// for the user who runs it (no administrator needed); the installer asks for it, the Settings button does too.
// Which app is the default, only the user can say: Windows lets no app choose itself. It's asked on its own page.
namespace filetypes {

bool supported();  // (Windows: elsewhere the system's own ways)
bool available();  // (a copy Windows can start by itself: its libraries beside it - not one run from the source code)
bool registerApp();
void unregisterApp();
bool isDefault();         // (it's what opens a PDF now)
bool openDefaultsPage();  // (Windows' default apps, at Master PDF)

}  // namespace filetypes
