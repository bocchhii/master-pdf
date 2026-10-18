#include "ui/Theme.h"

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QStandardPaths>

#include <cmath>
#include <map>

namespace theme {

namespace {

using Swaps = std::map<QString, QString>;  // written colour (lower case) -> its colour

struct Scheme {
    Swaps any;                    // whatever it's for
    std::map<Role, Swaps> roles;  // only for this
};

const QString kDarkFace = "#353535", kDarkBox = "#1E1E1E", kDarkText = "#E8E8E8";

// THEME_COLORS of the Python app
const std::map<QString, Scheme> &schemes()
{
    static const std::map<QString, Scheme> s = {
        {"xp", {}},
        {"98", {{{"#ece9d8", "#C0C0C0"}, {"#f5f3e8", "#C8C8C8"}, {"#8e8c82", "#808080"},
                 {"#efefef", "#E0E0E0"}, {"#ebe8d7", "#BFBFBF"}, {"#f7f6f0", "#DFDFDF"},
                 {"#ffffe1", "#DEDEDE"}, {"#d4d0c8", "#C3C3C3"}},
                {}}},
        {"dark", {{{"#ece9d8", kDarkFace}, {"#f5f3e8", "#474747"}, {"#ffffe1", "#404040"},
                   {"#efefef", "#2b2b2b"}, {"#cccccc", "#5b5b5b"}, {"#a6a6a6", "#777777"},
                   {"#606060", "#8c8c8c"}, {"#dadada", "#454545"}, {"#5f5f5f", "#c2c2c2"},
                   {"#b0b0b0", "#626262"}, {"#a0a0a0", "#6c6c6c"}, {"#666666", "#b3b3b3"},
                   {"#888888", "#8f8f8f"}, {"#999999", "#727272"}, {"#303030", "#d4d4d4"},
                   {"#c00000", "#ff7a7a"}, {"#e4e4e4", "#3c3c3c"}, {"#d8d8d8", "#3a3a3a"},
                   {"#9a9a9a", "#1c1c1c"}, {"#f0f0f0", "#3b3b3b"}, {"#ebe8d7", "#333333"},
                   {"#0000ee", "#8AB4FF"}, {"#f7f6f0", "#262626"}, {"#7b7b7b", "#202020"},
                   {"#4b4b4b", "#0a0a0a"}},
                  {{Text, {{"#000000", kDarkText}, {"#ffffff", "#ffffff"}}},
                   {Box, {{"#ffffff", kDarkBox}, {"#000000", kDarkText}}},
                   {Edge, {{"#ffffff", "#5e5e5e"}, {"#8e8c82", "#1b1b1b"}, {"#000000", "#000000"}}}}}},
        {"pink", {{{"#ece9d8", "#F3C0C9"}, {"#f5f3e8", "#F6CFD6"}, {"#8e8c82", "#E0566E"},
                   {"#efefef", "#F9DFE3"}, {"#ebe8d7", "#E9B3BD"}, {"#f7f6f0", "#F9DFE2"},
                   {"#ffffe1", "#CBFEFE"}, {"#999999", "#DE4B65"}, {"#808080", "#DD4A64"},
                   {"#cccccc", "#EDA9B5"}, {"#a6a6a6", "#E7919F"}, {"#606060", "#B84A5D"},
                   {"#dadada", "#F7D6DC"}, {"#5f5f5f", "#84414D"}, {"#b0b0b0", "#E7A2AE"},
                   {"#a0a0a0", "#E29AA7"}, {"#e4e4e4", "#F7E3E6"}, {"#d8d8d8", "#F2D2D8"},
                   {"#888888", "#BD6878"}, {"#666666", "#7E3F4B"}, {"#7da2ce", "#6FC9BE"},
                   {"#245edc", "#00A9A6"}, {"#5a8be0", "#3CC4C0"}, {"#1f4fae", "#008C89"},
                   {"#a9c4f5", "#B5EFEB"}, {"#7b7b7b", "#C98896"}},
                  {{Box, {{"#ffffff", "#FFF0F4"}}},
                   {Edge, {{"#ffffff", "#F8DDDE"}, {"#000000", "#704049"}}}}}},
        {"jungle", {{{"#ece9d8", "#B69F67"}, {"#f5f3e8", "#C1AB75"}, {"#8e8c82", "#685832"},
                     {"#efefef", "#DFD0B8"}, {"#ebe8d7", "#AA935C"}, {"#f7f6f0", "#DFD0B7"},
                     {"#ffffe1", "#EDE2C6"}, {"#999999", "#D2BE90"}, {"#808080", "#D1BD8F"},
                     {"#cccccc", "#C9B587"}, {"#a6a6a6", "#B19B63"}, {"#606060", "#5E4F2C"},
                     {"#dadada", "#E8DCC6"}, {"#5f5f5f", "#4A3C1C"}, {"#b0b0b0", "#B8A67A"},
                     {"#a0a0a0", "#A8955F"}, {"#e4e4e4", "#EEE7D8"}, {"#d8d8d8", "#E4D9C2"},
                     {"#888888", "#6F5B30"}, {"#666666", "#4A3A18"}, {"#7da2ce", "#B0402A"},
                     {"#245edc", "#7C0000"}, {"#5a8be0", "#C04818"}, {"#1f4fae", "#600000"},
                     {"#a9c4f5", "#FFA858"}, {"#7b7b7b", "#6E5D35"}},
                    {{Box, {{"#ffffff", "#FBF5E6"}}},
                     {Edge, {{"#ffffff", "#D8C592"}, {"#000000", "#281602"}}}}}},
        {"vapor", {{{"#ece9d8", "#DCB2FE"}, {"#f5f3e8", "#E6C8FE"}, {"#8e8c82", "#B06AF8"},
                    {"#efefef", "#EAD5FE"}, {"#ebe8d7", "#CFA0FB"}, {"#f7f6f0", "#EAD5FD"},
                    {"#ffffe1", "#F3E4FF"}, {"#999999", "#A585E6"}, {"#808080", "#A484E5"},
                    {"#cccccc", "#D3A6FD"}, {"#a6a6a6", "#C394FA"}, {"#606060", "#7A4FE8"},
                    {"#dadada", "#EFDDFF"}, {"#5f5f5f", "#5A3FD0"}, {"#b0b0b0", "#C9A0FA"},
                    {"#a0a0a0", "#BE92F8"}, {"#e4e4e4", "#F1E2FF"}, {"#d8d8d8", "#E9D3FE"},
                    {"#888888", "#8B6FD8"}, {"#666666", "#5A3FC0"}, {"#c00000", "#C0004E"},
                    {"#0000ee", "#3B29F6"}, {"#7da2ce", "#9C7BF8"}, {"#245edc", "#6F3DFF"},
                    {"#5a8be0", "#A02CF4"}, {"#1f4fae", "#5A26E8"}, {"#a9c4f5", "#E6B8FF"},
                    {"#7b7b7b", "#9F78E0"}},
                   {{Text, {{"#000000", "#3A2BC4"}}},
                    {Box, {{"#ffffff", "#F7EEFF"}, {"#000000", "#3A2BC4"}}},
                    {Edge, {{"#ffffff", "#F6F0FF"}, {"#000000", "#5524E0"}}}}}},
        {"mono", {{{"#ece9d8", "#FEFEFE"}, {"#f5f3e8", "#FDFDFD"}, {"#8e8c82", "#FCFCFD"},
                   {"#efefef", "#FCFCFC"}, {"#ebe8d7", "#020202"}, {"#f7f6f0", "#FBFBFB"},
                   {"#ffffe1", "#FAFAFA"}, {"#999999", "#818181"}, {"#808080", "#7F7F7F"},
                   {"#cccccc", "#F9F9F9"}, {"#a6a6a6", "#F8F8F8"}, {"#606060", "#030303"},
                   {"#dadada", "#F7F7F7"}, {"#5f5f5f", "#040404"}, {"#b0b0b0", "#050505"},
                   {"#a0a0a0", "#060606"}, {"#e4e4e4", "#F6F6F6"}, {"#d8d8d8", "#F5F5F5"},
                   {"#888888", "#7E7E7E"}, {"#666666", "#070707"}, {"#c00000", "#080808"},
                   {"#0000ee", "#090909"}, {"#7da2ce", "#0A0A0A"}, {"#245edc", "#0B0B0B"},
                   {"#5a8be0", "#0C0C0C"}, {"#1f4fae", "#0D0D0D"}, {"#a9c4f5", "#F4F4F4"},
                   {"#9a9a9a", "#7D7D7D"}, {"#303030", "#0E0E0E"}, {"#7b7b7b", "#F3F3F3"},
                   {"#4b4b4b", "#101010"}},
                  {{Edge, {{"#ffffff", "#0F0F0F"}}}}}},
    };
    return s;
}

// THEME_LOOK: the inactive title bar (left, right), the title text (active, inactive),
// a selection (background, text)
struct Look {
    QString inactiveA, inactiveB, titleActive, titleInactive, selBg, selText;
};
const std::map<QString, Look> &looks()
{
    static const std::map<QString, Look> l = {
        {"xp", {"#808080", "#A8A8A8", "#FFFFFF", "#D4D0C8", "#316AC5", "#FFFFFF"}},
        {"98", {"#808080", "#A8A8A8", "#FFFFFF", "#C3C3C3", "#316AC5", "#FFFFFF"}},
        {"dark", {"#808080", "#A8A8A8", "#FFFFFF", "#D4D0C8", "#316AC5", "#FFFFFF"}},
        {"pink", {"#00B8A8", "#F18EA1", "#FFFFFF", "#00544C", "#A1DAD1", "#000000"}},
        {"jungle", {"#7D7040", "#7D7040", "#FFA040", "#903018", "#800000", "#FFA040"}},
        {"vapor", {"#D365FE", "#ECC0FE", "#FFFFFF", "#F8EEFF", "#6F3DFF", "#FFFFFF"}},
        {"mono", {"#FFFFFF", "#FFFFFF", "#FFFFFF", "#000000", "#000000", "#FFFFFF"}},
    };
    return l;
}

const std::map<QString, QString> &themePalette()
{
    static const std::map<QString, QString> p = {
        {"98", "Windows 98 blue"}, {"xp", "Windows 98 blue"}, {"dark", "Windows 98 blue"},
        {"pink", "Teal and pink"}, {"jungle", "Jungle black"}, {"vapor", "Vapor violet"},
        {"mono", "Black"}};
    return p;
}

struct State {
    QString choice = "98";
    QString appearance = "98";
    QColor capA = QColor("#000080"), capB = QColor("#1084D0");
    bool custom = false;  // the Custom theme: its selection is its title bar's colour
    QColor customSel, customSelText;
    QJsonObject settings;
    bool loaded = false;
    bool readOnly = false;
};

State &state()
{
    static State s;
    return s;
}

QString settingsPath(bool python)
{
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath();
    return base + (python ? "/Master PDF/settings.json" : "/Master PDF/settings-cpp.json");
}

void load()
{
    State &s = state();
    if (s.loaded)
        return;
    s.loaded = true;
    for (bool python : {false, true}) {  // (ours, or else the Python app's: the same choices)
        QFile f(settingsPath(python));
        if (f.open(QIODevice::ReadOnly)) {
            s.settings = QJsonDocument::fromJson(f.readAll()).object();
            return;
        }
    }
}

std::pair<QColor, QColor> customPalette(const QColor &c)
{
    const int r = c.red(), g = c.green(), b = c.blue();
    auto up = [](int v) { return int(std::lround(v + (255 - v) * 0.35)); };
    return {c, QColor(up(r), up(g), up(b))};
}

std::pair<QColor, QColor> paletteColors(const QString &name, const QString &customColor)
{
    if (name == "Custom..." && QColor::isValidColorName(customColor))
        return customPalette(QColor(customColor));
    for (const auto &[n, cols] : palettes())
        if (n == name)
            return cols;
    return palettes().front().second;
}

}  // namespace

const std::vector<std::pair<QString, QString>> &names()
{
    static const std::vector<std::pair<QString, QString>> n = {
        {"98", "Windows 98"}, {"xp", "Windows 98 Ivory"}, {"dark", "Windows 98 Dark"},
        {"pink", "Windows 98 Pink"}, {"jungle", "Windows 98 Jungle"},
        {"vapor", "Windows 98 Vapor"}, {"mono", "Windows 98 Black and White"}};
    return n;
}

const std::vector<std::pair<QString, std::pair<QColor, QColor>>> &palettes()
{
    static const std::vector<std::pair<QString, std::pair<QColor, QColor>>> p = {
        {"Windows 98 blue", {QColor("#000080"), QColor("#1084D0")}},
        {"Teal", {QColor("#006A6A"), QColor("#2AA8A8")}},
        {"Plum", {QColor("#4B1466"), QColor("#9A58BE")}},
        {"Maroon", {QColor("#700000"), QColor("#B83A3A")}},
        {"Forest green", {QColor("#1F5A1F"), QColor("#5E9E4A")}},
        {"Rose", {QColor("#861E4E"), QColor("#D06A92")}},
        {"Slate", {QColor("#2E3C4C"), QColor("#7E8EA0")}},
        {"Charcoal", {QColor("#101010"), QColor("#4A4A4A")}},
        {"Teal and pink", {QColor("#00BDBA"), QColor("#DC97B8")}},
        {"Jungle black", {QColor("#000000"), QColor("#000000")}},
        {"Vapor violet", {QColor("#3B29F6"), QColor("#DCC2FE")}},
        {"Black", {QColor("#000000"), QColor("#000000")}},
    };
    return p;
}

QString current() { return state().appearance; }
bool dark() { return state().appearance == "dark"; }
bool mono() { return state().appearance == "mono"; }

QColor c(const char *written, Role role)
{
    const QString key = QString::fromLatin1(written).toLower();
    const Scheme &s = schemes().at(state().appearance);
    auto r = s.roles.find(role);
    if (r != s.roles.end()) {
        auto it = r->second.find(key);
        if (it != r->second.end())
            return QColor(it->second);
    }
    auto it = s.any.find(key);
    return QColor(it != s.any.end() ? it->second : key);
}

QColor face() { return c("#ECE9D8"); }
QColor pressedFace() { return c("#F5F3E8"); }
QColor light() { return c("#FFFFFF", Edge); }
QColor shadow() { return c("#8E8C82", Edge); }
QColor darkEdge() { return c("#000000", Edge); }
QColor thinShadow() { return mono() ? darkEdge() : shadow(); }
QColor box() { return c("#FFFFFF", Box); }
QColor text() { return c("#000000", Text); }
QColor ink() { return c("#000000", Box); }
QColor workspace() { return c("#7B7B7B", Box); }
QColor pageShadow() { return c("#4B4B4B", Box); }

std::pair<QColor, QColor> engraved()
{
    if (mono())
        return {QColor("#FFFFFF"), QColor("#808080")};
    if (dark())
        return {QColor("#636363"), QColor("#1A1A1A")};
    return {light(), shadow()};
}

std::pair<QColor, QColor> selection()
{
    const State &s = state();
    if (s.custom)
        return {s.customSel, s.customSelText};
    const Look &l = looks().at(s.appearance);
    return {QColor(l.selBg), QColor(l.selText)};
}

std::pair<QColor, QColor> caption(bool active)
{
    const State &s = state();
    if (active)
        return {s.capA, s.capB};
    if (s.custom)
        return {QColor("#808080"), QColor("#A8A8A8")};
    const Look &l = looks().at(s.appearance);
    return {QColor(l.inactiveA), QColor(l.inactiveB)};
}

QColor captionText(bool active)
{
    const State &s = state();
    if (s.custom)
        return active ? s.customSelText : QColor("#C3C3C3");
    const Look &l = looks().at(s.appearance);
    return QColor(active ? l.titleActive : l.titleInactive);
}

QString choice()
{
    load();
    const QJsonObject &set = state().settings;
    QString t = set.value("theme").toString();
    if (t == "custom" && set.contains("custom_theme"))
        return t;
    bool known = false;
    for (const auto &[key, name] : names())
        known = known || key == t;
    if (!known)
        t = set.value("dark").toBool() ? "dark" : "98";
    return t;
}

void apply(const QString &choiceIn, bool save)
{
    load();
    State &s = state();
    QString appearance = choiceIn, paletteName, customColor;
    if (choiceIn == "custom") {
        const QJsonObject ct = s.settings.value("custom_theme").toObject();
        appearance = ct.value("appearance").toString();
        if (!schemes().count(appearance))
            appearance = "98";
        paletteName = ct.value("palette").toString();
        customColor = ct.value("custom_color").toString();
        bool known = paletteName == "Custom...";
        for (const auto &[n, cols] : palettes())
            known = known || n == paletteName;
        if (!known)
            paletteName = themePalette().at(appearance);
    } else {
        if (!schemes().count(appearance))
            appearance = "98";
        paletteName = themePalette().at(appearance);
    }
    s.choice = choiceIn == "custom" ? QStringLiteral("custom") : appearance;
    s.appearance = appearance;
    std::tie(s.capA, s.capB) = paletteColors(paletteName, customColor);
    s.custom = s.choice == "custom";
    if (s.custom) {  // selection_for: the title bar's colour, white or black text on it
        s.customSel = s.capA;
        const int y = 299 * s.capA.red() + 587 * s.capA.green() + 114 * s.capA.blue();
        s.customSelText = y > 150000 ? QColor("#010101") : QColor("#FFFFFF");
    }
    if (save)
        saveSetting("theme", s.choice);
    emit Notifier::instance()->changed();
}

QPalette palette()
{
    QPalette p;
    const auto [sel, selText] = selection();
    const QColor grey = dark() ? QColor("#7a7a7a") : c("#999999", Text);
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Window, face());
        p.setColor(group, QPalette::Button, face());
        p.setColor(group, QPalette::Base, box());
        p.setColor(group, QPalette::AlternateBase, box());
        p.setColor(group, QPalette::Text, group == QPalette::Disabled ? grey : text());
        p.setColor(group, QPalette::WindowText, group == QPalette::Disabled ? grey : text());
        p.setColor(group, QPalette::ButtonText, group == QPalette::Disabled ? grey : text());
        p.setColor(group, QPalette::PlaceholderText, grey);
        p.setColor(group, QPalette::Light, light());
        p.setColor(group, QPalette::Midlight, face());
        p.setColor(group, QPalette::Mid, shadow());
        p.setColor(group, QPalette::Dark, shadow());
        p.setColor(group, QPalette::Shadow, darkEdge());
        p.setColor(group, QPalette::Highlight, sel);
        p.setColor(group, QPalette::HighlightedText, selText);
        p.setColor(group, QPalette::ToolTipBase, c("#FFFFE1", Box));
        p.setColor(group, QPalette::ToolTipText, text());
        p.setColor(group, QPalette::Link, c("#0000EE"));
    }
    return p;
}

QFont font(int points, bool bold)
{
    // Windows 98's MS Sans Serif: its outline twin, drawn as the bitmap font was - each pixel on or off,
    // no grey smoothing (the bitmap font itself can't be: the screen's scale stretches it, blurred)
    QFont f(QStringLiteral("Microsoft Sans Serif"));
    f.setPointSizeF(points);
    f.setBold(bold);
    f.setStyleStrategy(QFont::NoAntialias);
    f.setHintingPreference(QFont::PreferFullHinting);
    // (the space between letters: 0 as Windows 98's - text is laid out in the screen's own pixels now
    // (classic::crispText), so its spacing is even; 0.8 = 1 screen pixel more at 125%)
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0);

    if (f.styleStrategy() & QFont::NoAntialias)
        return f;
    // (sharp: fitted to the screen's pixels - at 100% / 200% wholly; at 125% / 150% only up and
    // down: fitted across too, each letter's width is rounded to a whole pixel before the scale,
    // and the letters land unevenly - some jammed together, some apart)
    const QScreen *screen = QGuiApplication::primaryScreen();
    const double dpr = screen ? screen->devicePixelRatio() : 1.0;
    const bool whole = std::abs(dpr - std::round(dpr)) < 0.01;
    f.setHintingPreference(whole ? QFont::PreferFullHinting : QFont::PreferVerticalHinting);
    return f;
}

QJsonObject settings()
{
    load();
    return state().settings;
}

void saveSetting(const QString &key, const QJsonValue &value)
{
    load();
    State &s = state();
    if (s.settings.value(key) == value)
        return;
    s.settings.insert(key, value);
    if (s.readOnly)
        return;
    const QString path = settingsPath(false);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(s.settings).toJson(QJsonDocument::Compact));
}

void keepSettings(bool readOnly)
{
    state().readOnly = readOnly;
}

Notifier *Notifier::instance()
{
    static Notifier n;
    return &n;
}

}  // namespace theme
