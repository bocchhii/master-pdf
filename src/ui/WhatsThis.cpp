#include "ui/WhatsThis.h"

#include "ui/ClassicWindow.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSlider>
#include <QTextEdit>
#include <QWhatsThis>

namespace whatsThis {

namespace {

const char *kAuto = "mpdfAutoNote";  // (a note made here - made again each time: the part may have changed)

QString plain(QString text)  // (a part's words: no & of its shortcut, no rich text, no "...")
{
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
    text = text.trimmed();
    while (text.endsWith(QLatin1Char('.')) || text.endsWith(QLatin1Char(':')) || text.endsWith(QChar(0x2026)))
        text.chop(1);
    return text.trimmed();
}

QString sentence(QString text)  // (a note's words as a sentence: a capital, a full stop)
{
    text = text.trimmed();
    if (text.isEmpty())
        return text;
    text[0] = text[0].toUpper();
    if (!text.endsWith(QLatin1Char('.')) && !text.endsWith(QLatin1Char('!')) && !text.endsWith(QLatin1Char('?')) &&
        !text.endsWith(QLatin1Char(')')))
        text += QLatin1Char('.');
    return text;
}

// the label beside a box (its buddy, or the words just left of it / just over it, in the same parent)
QString labelFor(QWidget *w)
{
    QWidget *parent = w->parentWidget();
    if (!parent)
        return {};
    const QRect r = w->geometry();
    QLabel *best = nullptr;
    int far = 1 << 20;
    for (QLabel *l : parent->findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) {
        if (l->buddy() == w)
            return plain(l->text());
        if (!l->isVisible() || plain(l->text()).isEmpty())
            continue;
        const QRect g = l->geometry();
        int d = 1 << 20;
        if (g.center().y() >= r.top() - 2 && g.center().y() <= r.bottom() + 2 && g.right() <= r.left() + 4)
            d = r.left() - g.right();  // (left of it, on its row)
        else if (g.bottom() <= r.top() + 2 && r.top() - g.bottom() < 24 && g.left() < r.right() && g.right() > r.left())
            d = 40 + r.top() - g.bottom();  // (just over it)
        if (d < far) {
            far = d;
            best = l;
        }
    }
    return best ? plain(best->text()) : QString();
}

// the note of the box it's in (a group box's own - set, or made from its title)
QString boxNote(QWidget *w)
{
    for (QWidget *p = w->parentWidget(); p && !p->isWindow(); p = p->parentWidget())
        if (auto *g = qobject_cast<QGroupBox *>(p)) {
            if (!g->whatsThis().isEmpty() && !g->property(kAuto).toBool())
                return g->whatsThis();
            if (!plain(g->title()).isEmpty())
                return QObject::tr("The %1 settings: the choices in this box.").arg(plain(g->title()));
        }
    return {};
}

QString buttonNote(const QString &t)
{
    static const QMap<QString, const char *> known = {
        {"ok", "Uses what you've chosen and closes this box."},
        {"cancel", "Closes this box without changing anything."},
        {"close", "Closes this box."},
        {"yes", "Answers yes: goes ahead."},
        {"no", "Answers no: doesn't."},
        {"save", "Saves it."},
        {"don't save", "Goes on without saving: the changes are lost."},
        {"apply", "Uses what you've chosen now, and keeps this box open."},
        {"browse", "Opens a box to choose a file or folder."},
        {"help", "Shows help about this."},
        {"retry", "Tries again."},
        {"delete", "Deletes it."},
        {"remove", "Takes the item picked in the list out."},
    };
    const QString key = t.toLower();
    if (known.contains(key))
        return QObject::tr(known.value(key));
    QString what = t;
    if (what.size() > 1 && what[1].isLower())
        what[0] = what[0].toLower();
    return QObject::tr("Click it to %1.").arg(what);
}

// a note for a part that has none of its own
QString made(QWidget *w)
{
    if (qobject_cast<QScrollBar *>(w))
        return QObject::tr("Drag it, or click its arrows, to scroll.");
    if (auto *b = qobject_cast<QAbstractButton *>(w)) {
        const QString t = plain(b->text());
        if (qobject_cast<QCheckBox *>(w))
            return t.isEmpty() ? QObject::tr("Tick it to turn this on, untick it to turn it off.")
                               : QObject::tr("Tick it to turn this on, untick it to turn it off: %1.").arg(t);
        if (qobject_cast<QRadioButton *>(w))
            return QObject::tr("Click it to choose %1 (instead of the others beside it).").arg(t.isEmpty() ? QObject::tr("this") : QStringLiteral("\"") + t + QStringLiteral("\""));
        if (!b->toolTip().isEmpty())
            return sentence(plain(b->toolTip()));
        if (!t.isEmpty())
            return buttonNote(t);
        return {};
    }
    if (!w->toolTip().isEmpty())  // (the toolbars' buttons, the parts with a tip: what it says)
        return sentence(plain(w->toolTip()));
    if (qobject_cast<QLineEdit *>(w) || qobject_cast<QPlainTextEdit *>(w) || qobject_cast<QTextEdit *>(w)) {
        if (auto *e = qobject_cast<QLineEdit *>(w); e && e->isReadOnly())
            return labelFor(w).isEmpty() ? QObject::tr("Shows it - it can be copied, not changed.")
                                         : QObject::tr("Shows the %1 - it can be copied, not changed.").arg(labelFor(w).toLower());
        const QString l = labelFor(w);
        return l.isEmpty() ? QObject::tr("A box to type in: click it, then type.") : QObject::tr("Type the %1 here.").arg(l.toLower());
    }
    if (qobject_cast<QComboBox *>(w)) {
        const QString l = labelFor(w);
        return l.isEmpty() ? QObject::tr("Click it to choose from its list.") : QObject::tr("Click it to choose the %1 from its list.").arg(l.toLower());
    }
    if (qobject_cast<QAbstractSpinBox *>(w))
        return QObject::tr("Type a number, or click the arrows to make it bigger or smaller.");
    if (qobject_cast<QSlider *>(w))
        return QObject::tr("Drag it to change the value.");
    if (qobject_cast<QAbstractItemView *>(w))
        return QObject::tr("A list: click an item to choose it.");
    if (auto *l = qobject_cast<QLabel *>(w)) {
        if (l->buddy() && !l->buddy()->whatsThis().isEmpty())
            return l->buddy()->whatsThis();
        if (const QString b = boxNote(w); !b.isEmpty())
            return b;
        return {};  // (words of a box: the window's note)
    }
    if (auto *g = qobject_cast<QGroupBox *>(w))
        return plain(g->title()).isEmpty() ? QString() : QObject::tr("The %1 settings: the choices in this box.").arg(plain(g->title()));
    return {};
}

// every part of a window without a note of its own: one made for it
void fill(QWidget *window)
{
    QList<QWidget *> parts = window->findChildren<QWidget *>();
    parts.prepend(window);
    for (QWidget *w : parts) {
        if (!w->whatsThis().isEmpty() && !w->property(kAuto).toBool())
            continue;  // (its own)
        QString note = w == window ? QString() : made(w);
        if (w == window || (note.isEmpty() && w->parentWidget() == window)) {  // (the window: what it is)
            auto *cw = qobject_cast<ClassicWindow *>(window);
            const QString title = cw ? cw->title() : window->windowTitle();
            note = title.isEmpty() ? QObject::tr("This window. Click ? and then any part of it to see what it does.")
                                   : QObject::tr("The \"%1\" window. Click ? and then any part of it to see what it does.").arg(title);
        }
        w->setWhatsThis(note);
        w->setProperty(kAuto, !note.isEmpty());
    }
}

}  // namespace

bool buttonsOn()
{
    return theme::settings().value(QStringLiteral("whats_this")).toBool(true);
}

void start(QWidget *)
{
    if (QWhatsThis::inWhatsThisMode()) {  // (? again: out of help mode)
        QWhatsThis::leaveWhatsThisMode();
        return;
    }
    for (QWidget *top : QApplication::topLevelWidgets())  // (a click in any window gets its note)
        if (top->isVisible())
            fill(top);
    QWhatsThis::enterWhatsThisMode();
}

void showButtons(bool on)
{
    if (!on && QWhatsThis::inWhatsThisMode())
        QWhatsThis::leaveWhatsThisMode();
    for (QWidget *top : QApplication::topLevelWidgets())
        if (auto *cw = qobject_cast<ClassicWindow *>(top))
            cw->setHelpButton(on);
}

}  // namespace whatsThis
