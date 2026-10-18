#pragma once

#include <QString>

class QWidget;

// Windows 95 / 98's "What's This?" help, in every window (the Python app's WhatsThis): the ? button by the
// title bar's X; clicking it turns the pointer into an arrow with a question mark, and the next click on any
// part of a window shows a short note about it. The parts' own notes (setWhatsThis) are used; a part without
// one gets one made for it - its tooltip, or what a button, a tick box, a box to type in or a list does.
namespace whatsThis {

// ? clicked in a window: help mode on (or, on already, off)
void start(QWidget *window);
// the ? button shown in every window, or none (Settings > Help)
void showButtons(bool on);
// (the setting: on unless turned off)
bool buttonsOn();

}  // namespace whatsThis
