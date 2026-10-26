#include "ui/Keyboard.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLocale>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace keyboard {

bool arabic()
{
#ifdef Q_OS_WIN
    // (the window's thread's keyboard - as switched with Alt + Shift / Win + Space)
    const auto layout = reinterpret_cast<quintptr>(GetKeyboardLayout(0));
    return PRIMARYLANGID(LOWORD(layout)) == LANG_ARABIC;
#else
    return QGuiApplication::inputMethod()->locale().language() == QLocale::Arabic;
#endif
}

std::unique_ptr<QKeyEvent> arabicDigit(const QKeyEvent *e)
{
    const QString text = e->text();
    if (text.size() != 1 || text[0] < QLatin1Char('0') || text[0] > QLatin1Char('9') ||
        (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) || !arabic())
        return nullptr;
    const QString digit(QChar(char16_t(0x0660 + (text[0].unicode() - '0'))));
    return std::make_unique<QKeyEvent>(e->type(), e->key(), e->modifiers(), e->nativeScanCode(), e->nativeVirtualKey(),
                                       e->nativeModifiers(), digit, e->isAutoRepeat(), e->count());
}

}  // namespace keyboard
