#pragma once

#include <QColor>
#include <QFont>
#include <QJsonObject>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

#include <utility>

// The app's look, as the Python app's: every colour is written in the "Ivory" (xp) scheme
// and each theme swaps some of them (THEME_COLORS) - some differently depending on what
// they're for (text, a box's background, a 3D edge). The title bar has its own colours
// (a palette), and a selection colour goes with the theme.
namespace theme {

enum Role { Face, Text, Box, Edge };

// the themes, in the Theme menu's order: (key, name)
const std::vector<std::pair<QString, QString>> &names();

QString current();  // the appearance in use: "98", "xp", "dark", "pink", "jungle", "vapor", "mono"
bool dark();
bool mono();

// a written (Ivory) colour in the current theme
QColor c(const char *written, Role role = Face);

// the colours used everywhere
QColor face();         // window / button face (BG)
QColor pressedFace();  // a pressed button's face
QColor light();        // 3D edges: the light side
QColor shadow();       // ... the shadow
QColor darkEdge();     // ... the darkest (black)
QColor thinShadow();   // the top / left of a 1 px sunken edge
QColor box();          // a white box's background (text boxes, lists)
QColor text();         // text
QColor ink();          // black shapes drawn on the face (arrows, symbols)
QColor workspace();    // behind the pages
QColor pageShadow();
std::pair<QColor, QColor> engraved();  // greyed-out text: (light copy, dark)
std::pair<QColor, QColor> selection(); // (background, text)
std::pair<QColor, QColor> caption(bool active);  // the title bar (left, right)
QColor captionText(bool active);

// The title bar palettes (Settings > Color palette), by name
const std::vector<std::pair<QString, std::pair<QColor, QColor>>> &palettes();

// The Theme menu's choice: a theme key, or "custom" (an appearance + a palette of its own)
QString choice();
void apply(const QString &choice, bool save = true);
QPalette palette();  // for Qt's own widgets
QFont font(int points = 8, bool bold = false);  // the app's font (MS Sans Serif's look)

// Settings, kept like the Python app's (its file is read the first time, so the C++ app
// starts with the same choices)
QJsonObject settings();
void saveSetting(const QString &key, const QJsonValue &value);
void keepSettings(bool readOnly);  // (the self-tests: nothing is saved)

// tells the windows to draw themselves again in the new colours
class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier *instance();
signals:
    void changed();
};

}  // namespace theme
