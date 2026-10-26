// Edit > Settings..., like the Python app's: where the app is, sounds, the app's icon, the
// theme (an appearance and a title bar palette - saved as the Custom theme), My fonts, how
// the window opens, the ? button, the version.

#include "app/MainWindow.h"
#include "ui/WhatsThis.h"
#include "ui/Sounds.h"
#include <map>
#include <QAbstractButton>
#include <QRegularExpression>
#include <QProcess>

#include "core/Fonts.h"
#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QRadioButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// the title bar's colours, in a sunken strip (like Windows' Display settings)
class PaletteStrip : public QWidget {
public:
    explicit PaletteStrip(QWidget *parent) : QWidget(parent) { setFixedHeight(18); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        classic::sunkenEdge(p, rect(), QColor());
        const auto [a, b] = theme::caption(true);
        QLinearGradient g(2, 0, width() - 2, 0);
        g.setColorAt(0, a);
        g.setColorAt(1, b);
        p.fillRect(rect().adjusted(2, 2, -2, -2), g);
    }
};

const std::vector<std::pair<QString, QString>> kAppearances = {
    {"98", "Light grey"}, {"xp", "Ivory"}, {"dark", "Dark grey"}, {"pink", "Pink"},
    {"jungle", "Jungle"}, {"vapor", "Vapor"}, {"mono", "Black and white"}};

QString customIconPath()
{
    return qEnvironmentVariable("APPDATA") + QStringLiteral("/Master PDF/custom_icon.png");
}

QGroupBox *group(const QString &title, QWidget *parent)
{
    auto *g = new QGroupBox(QStringLiteral(" %1 ").arg(title), parent);
    g->setFont(theme::font());
    return g;
}

EngravedLabel *note(const QString &text, QWidget *parent)
{
    auto *n = new EngravedLabel(parent);
    n->setText(text);
    return n;
}

}  // namespace

QImage MainWindow::appIcon() const
{
    const QImage custom(customIconPath());
    return custom.isNull() ? icons::get(QStringLiteral("app")) : custom;
}

void MainWindow::applyIcon()
{
    // (the app's own: its 16-pixel picture where it's small - the title bar, Windows' small icons - drawn for that size)
    const bool own = QImage(customIconPath()).isNull();
    const QImage icon = appIcon(), small = own ? icons::get(QStringLiteral("app16")) : icon;
    QIcon both(QPixmap::fromImage(icon));
    if (own)
        both.addPixmap(QPixmap::fromImage(small));
    QApplication::setWindowIcon(both);
    setTitleIcon(theme::settings().value("no_icon").toBool() ? QImage() : small);
}

namespace {

// What's This? in Settings (the Python app's SETTINGS_NOTES): by a part's words, or its box's title
void settingsNotes(QWidget *dlg)
{
    static const std::map<QString, const char *> notes = {
        {"App location", "The folder Master PDF is installed in. To move it, build it again into another folder."},
        {"Open folder", "Opens the folder Master PDF is installed in."},
        {"Sound effects", "Plays a short chime when a job is done and a low tone when something fails. Untick it for silence."},
        {"Play a sound when a job is done or something fails",
         "Plays a short chime when a job is done and a low tone when something fails. Untick it for silence."},
        {"App icon", "The picture shown at the left of the title bar and on the taskbar."},
        {"Choose file...", "Use your own picture (.ico, .png, .jpg ...) as the app's icon."},
        {"Reset to default", "Goes back to Master PDF's own icon."},
        {"No icon in the title bar", "Hides the icon at the left of the title bar. The taskbar still shows it."},
        {"Theme", "The app's colours. A change here is saved as the Custom theme, which you can pick again any time from the Theme menu."},
        {"Appearance", "The colours of the whole app: Light grey, Ivory, Dark grey... A change is saved as the Custom theme."},
        {"Color palette", "The colours of the title bar. Choose Custom... to pick any colour. A change is saved as the Custom theme."},
        {"Window", "Whether Master PDF opens at its normal size or maximized (filling the screen)."},
        {"Normal size", "Whether Master PDF opens at its normal size or maximized (filling the screen)."},
        {"Maximized", "Whether Master PDF opens at its normal size or maximized (filling the screen)."},
        {"Help", "Shows or hides the ? button in the title bar - the one you just used. It explains whatever you click next."},
        {"Show the ? button in the title bar",
         "Shows or hides the ? button in the title bar - the one you just used. Untick it and it disappears from every window (turn it back on here)."},
        {"My fonts", "The fonts you've added to Master PDF (from a .ttf, .otf or .ttc file - a font downloaded from the internet, say). They show in the font list and are used for text, by this app only: nothing is installed."},
        {"Add font...", "Adds font files to Master PDF. They're copied into its own folder, so they stay even if you delete the file you downloaded."},
        {"Remove", "Takes the font picked in the list out of Master PDF. Text already written with it keeps it."},
        {"Updates", "Which version of Master PDF you have."},
        {"OK", "Closes Settings. (Changes are saved as soon as they're made.)"},
    };
    auto words = [](QString t) {
        t.remove(QLatin1Char('&'));
        t = t.trimmed();
        while (t.endsWith(QLatin1Char(':')))
            t.chop(1);
        return t;
    };
    auto note = [&](const QString &key) { return notes.count(key) ? QObject::tr(notes.at(key)) : QString(); };
    for (QWidget *w : dlg->findChildren<QWidget *>()) {
        QString text;
        if (auto *b = qobject_cast<QAbstractButton *>(w))
            text = words(b->text());
        else if (auto *g = qobject_cast<QGroupBox *>(w))
            text = words(g->title());
        else if (auto *l = qobject_cast<QLabel *>(w))
            text = words(l->text());
        QString n = note(text);
        if (n.isEmpty()) {  // (anything else in a box: its box's note - the list, the strip, the icon...)
            for (QWidget *p = w->parentWidget(); p && n.isEmpty() && p != dlg; p = p->parentWidget())
                if (auto *g = qobject_cast<QGroupBox *>(p))
                    n = note(words(g->title()));
            if (auto *c = qobject_cast<QComboBox *>(w)) {  // (its two lists: by what's in them)
                n = note(c->findText(QStringLiteral("Custom...")) >= 0 ? QStringLiteral("Color palette") : QStringLiteral("Appearance"));
            }
        }
        if (!n.isEmpty())
            w->setWhatsThis(n);
    }
}

}  // namespace

void MainWindow::openSettings()
{
    ClassicDialog dlg(this, tr("Settings"));
    auto *page = new QVBoxLayout(dlg.body());
    page->setContentsMargins(12, 10, 12, 10);
    page->setSpacing(6);
    QWidget *body = dlg.body();
    body->setFont(theme::font());

    // where the app is
    {
        auto *g = group(tr("App location"), body);
        auto *l = new QVBoxLayout(g);
        l->addWidget(new QLabel(tr("Master PDF is installed in:"), g));
        auto *row = new QHBoxLayout;
        auto *where = new QLineEdit(QDir::toNativeSeparators(QApplication::applicationDirPath()), g);
        where->setReadOnly(true);
        where->setEnabled(false);
        row->addWidget(where, 1);
        auto *open = new ClassicButton(tr("Open folder"), g);
        connect(open, &QPushButton::clicked, &dlg,
                [] { QDesktopServices::openUrl(QUrl::fromLocalFile(QApplication::applicationDirPath())); });
        row->addWidget(open);
        l->addLayout(row);
        l->addWidget(note(tr("To move it, build it again into another folder."), g));
        page->addWidget(g);
    }
    // sound effects
    {
        auto *g = group(tr("Sound effects"), body);
        auto *l = new QVBoxLayout(g);
        auto *box = new QCheckBox(tr("Play a sound when a job is done or something fails"), g);
        box->setChecked(theme::settings().value("sounds").toBool(true));
        connect(box, &QCheckBox::toggled, &dlg, [](bool on) { theme::saveSetting("sounds", on); });
        l->addWidget(box);
        page->addWidget(g);
    }
    // the app's icon
    {
        auto *g = group(tr("App icon"), body);
        auto *l = new QVBoxLayout(g);
        auto *row = new QHBoxLayout;
        auto *preview = new QLabel(g);
        preview->setFixedSize(40, 40);
        preview->setAlignment(Qt::AlignCenter);
        preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
        auto showIcon = [this, preview] {
            const double dpr = preview->devicePixelRatioF();
            QImage im = appIcon().scaled(QSize(32, 32) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            im.setDevicePixelRatio(dpr);
            preview->setPixmap(QPixmap::fromImage(im));
        };
        showIcon();
        row->addWidget(preview);
        auto *col = new QVBoxLayout;
        col->addWidget(new QLabel(tr("Shown in the title bar and on the taskbar."), g));
        auto *buttons = new QHBoxLayout;
        auto *choose = new ClassicButton(tr("Choose file..."), g);
        auto *reset = new ClassicButton(tr("Reset to default"), g);
        connect(choose, &QPushButton::clicked, &dlg, [this, showIcon, &dlg] {
            const QString path = QFileDialog::getOpenFileName(&dlg, tr("App icon"), QString(),
                                                              tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.ico)"));
            const QImage im(path);
            if (path.isEmpty() || im.isNull())
                return;
            QDir().mkpath(QFileInfo(customIconPath()).absolutePath());
            im.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(customIconPath());
            applyIcon();
            showIcon();
        });
        connect(reset, &QPushButton::clicked, &dlg, [this, showIcon] {
            QFile::remove(customIconPath());
            applyIcon();
            showIcon();
        });
        buttons->addWidget(choose);
        buttons->addWidget(reset);
        buttons->addStretch();
        col->addLayout(buttons);
        row->addLayout(col, 1);
        l->addLayout(row);
        auto *noIcon = new QCheckBox(tr("No icon in the title bar"), g);
        noIcon->setChecked(theme::settings().value("no_icon").toBool());
        connect(noIcon, &QCheckBox::toggled, &dlg, [this](bool on) {
            theme::saveSetting("no_icon", on);
            applyIcon();
        });
        l->addWidget(noIcon);
        page->addWidget(g);
    }
    // the theme: saved as the Custom theme
    {
        auto *g = group(tr("Theme"), body);
        auto *l = new QVBoxLayout(g);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Appearance:"), g));
        auto *appearance = new QComboBox(g);
        for (const auto &[key, name] : kAppearances)
            appearance->addItem(tr(qPrintable(name)), key);
        appearance->setCurrentIndex(std::max(0, appearance->findData(theme::current())));
        row->addWidget(appearance);
        row->addSpacing(16);
        row->addWidget(new QLabel(tr("Color palette:"), g));
        auto *palette = new QComboBox(g);
        for (const auto &[name, cols] : theme::palettes())
            palette->addItem(name);
        palette->addItem(QStringLiteral("Custom..."));
        const QJsonObject ct = theme::settings().value("custom_theme").toObject();
        QString nowPalette = ct.value("palette").toString();
        if (theme::choice() != "custom" || palette->findText(nowPalette) < 0) {
            static const std::map<QString, QString> defaults = {
                {"98", "Windows 98 blue"}, {"xp", "Windows 98 blue"}, {"dark", "Windows 98 blue"}, {"pink", "Teal and pink"},
                {"jungle", "Jungle black"}, {"vapor", "Vapor violet"}, {"mono", "Black"}};
            nowPalette = defaults.at(theme::current());
        }
        palette->setCurrentText(nowPalette);
        row->addWidget(palette);
        row->addStretch();
        l->addLayout(row);
        auto *strip = new PaletteStrip(g);
        l->addWidget(strip);
        l->addWidget(note(tr("Changes here are saved as the Custom theme."), g));
        auto save = [this, appearance, palette, strip, &dlg](QString customColor) {
            QJsonObject ct = theme::settings().value("custom_theme").toObject();
            ct["appearance"] = appearance->currentData().toString();
            ct["palette"] = palette->currentText();
            if (!customColor.isEmpty())
                ct["custom_color"] = customColor;
            theme::saveSetting("custom_theme", ct);
            pickTheme(QStringLiteral("custom"));
            strip->update();
            dlg.update();
        };
        connect(appearance, &QComboBox::activated, &dlg, [save] { save(QString()); });
        connect(palette, &QComboBox::activated, &dlg, [this, palette, save, &dlg] {
            if (palette->currentText() == "Custom...") {
                const QColor c = colorDialog(&dlg, tr("Edit Colors"),
                                             QColor(theme::settings().value("custom_theme").toObject().value("custom_color").toString("#AA0000")), true);
                if (!c.isValid())
                    return;
                save(c.name().toUpper());
                return;
            }
            save(QString());
        });
        page->addWidget(g);
    }
    // the fonts added to the app
    {
        auto *g = group(tr("My fonts"), body);
        auto *l = new QVBoxLayout(g);
        auto *row = new QHBoxLayout;
        auto *list = new QListWidget(g);
        list->setFixedHeight(QFontMetrics(theme::font()).height() * 4 + 8);
        auto fill = [list] {
            list->clear();
            QDirIterator it(fonts::myFolder(), {QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.ttc")}, QDir::Files);
            while (it.hasNext()) {
                const QString path = it.next();
                auto *item = new QListWidgetItem(QFileInfo(path).completeBaseName(), list);
                item->setData(Qt::UserRole, path);
            }
            if (list->count() == 0) {
                auto *item = new QListWidgetItem(QObject::tr("(none yet - add one with Add font...)"), list);
                item->setFlags(Qt::NoItemFlags);
            }
        };
        fill();
        row->addWidget(list, 1);
        auto *buttons = new QVBoxLayout;
        auto *add = new ClassicButton(tr("Add font..."), g);
        auto *remove = new ClassicButton(tr("Remove"), g);
        remove->setEnabled(false);
        connect(list, &QListWidget::currentItemChanged, &dlg, [remove](QListWidgetItem *it) {
            remove->setEnabled(it && !it->data(Qt::UserRole).toString().isEmpty());
        });
        connect(add, &QPushButton::clicked, &dlg, [this, fill, &dlg] {
            const QStringList paths =
                QFileDialog::getOpenFileNames(&dlg, tr("Add font"), QString(), tr("Fonts (*.ttf *.otf *.ttc)"));
            if (paths.isEmpty())
                return;
            QDir().mkpath(fonts::myFolder());
            for (const QString &p : paths) {
                const QString to = fonts::myFolder() + QLatin1Char('/') + QFileInfo(p).fileName();
                QFile::remove(to);
                if (QFile::copy(p, to))
                    QFontDatabase::addApplicationFont(to);
            }
            fonts::rescan();
            fill();
            fontsChanged();
        });
        connect(remove, &QPushButton::clicked, &dlg, [this, list, fill, &dlg] {
            QListWidgetItem *it = list->currentItem();
            if (!it)
                return;
            const QString path = it->data(Qt::UserRole).toString();
            if (classicMessage(&dlg, tr("Remove font"),
                               tr("Remove %1 from Master PDF?\n\nText already written with it stays as it is.").arg(it->text()),
                               {tr("Remove"), tr("Cancel")}) != tr("Remove"))
                return;
            QFile::remove(path);
            fonts::rescan();
            fill();
            fontsChanged();
        });
        buttons->addWidget(add);
        buttons->addWidget(remove);
        buttons->addStretch();
        row->addLayout(buttons);
        l->addLayout(row);
        l->addWidget(note(tr("Fonts you add are kept by Master PDF and used by it only - nothing is installed."), g));
        page->addWidget(g);
    }
    // Window and Help side by side
    {
        auto *pair = new QHBoxLayout;
        auto *w = group(tr("Window"), body);
        auto *wl = new QVBoxLayout(w);
        wl->addWidget(new QLabel(tr("When the app starts:"), w));
        auto *choices = new QHBoxLayout;
        auto *normal = new QRadioButton(tr("Normal size"), w);
        auto *max = new QRadioButton(tr("Maximized"), w);
        (theme::settings().value("start_maximized").toBool() ? max : normal)->setChecked(true);
        connect(max, &QRadioButton::toggled, &dlg, [](bool on) { theme::saveSetting("start_maximized", on); });
        choices->addWidget(normal);
        choices->addWidget(max);
        choices->addStretch();
        wl->addLayout(choices);
        pair->addWidget(w, 1);
        auto *h = group(tr("Help"), body);
        auto *hl = new QVBoxLayout(h);
        auto *help = new QCheckBox(tr("Show the ? button in the title bar"), h);
        help->setChecked(theme::settings().value("whats_this").toBool(true));
        connect(help, &QCheckBox::toggled, &dlg, [this](bool on) {
            theme::saveSetting("whats_this", on);
            whatsThis::showButtons(on);  // (every window's ?)
        });
        hl->addWidget(help);
        auto *under = new QHBoxLayout;  // (under the box's words, as the Python app's)
        under->addSpacing(20);
        under->addWidget(note(tr("(What's This? - it explains what you click)"), h));
        under->addStretch();
        hl->addLayout(under);
        pair->addWidget(h, 1);
        page->addLayout(pair);
    }
    // the version, and checking for a newer one (as the Python app's: what was found said under it - a newer one offered
    // straight away)
    {
        auto *g = group(tr("Updates"), body);
        auto *l = new QVBoxLayout(g);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(updates::canUpdate() ? tr("Version: %1").arg(updates::appVersion())
                                                       : tr("Version: dev (running from the source code)"), g));
        row->addStretch();
        auto *check = new ClassicButton(tr("Check for updates"), g);
        row->addWidget(check);
        l->addLayout(row);
        auto *status = new QLabel(g);
        status->setFont(theme::font());
        status->setWordWrap(true);
        l->addWidget(status);
        connect(check, &QPushButton::clicked, &dlg, [this, check, status, &dlg] {
            check->setEnabled(false);
            status->setStyleSheet(QStringLiteral("color: #666666"));
            status->setText(tr("Checking for updates..."));
            QPointer<QLabel> alive(status);
            updates::latest(status, [this, check, status, alive, &dlg](std::optional<updates::Release> r) {
                if (!alive)
                    return;  // (Settings closed meanwhile)
                check->setEnabled(true);
                if (!r) {
                    status->setStyleSheet(QStringLiteral("color: #C00000"));
                    status->setText(tr("Couldn't reach GitHub - check the internet connection and try again."));
                } else if (!updates::canUpdate()) {
                    status->setText(tr("Newest version: %1. This copy runs from the source code, so it can't update itself.").arg(r->version));
                } else if (updates::newer(r->version)) {
                    status->setText(tr("Version %1 is available.").arg(r->version));
                    sounds::play(sounds::Done);
                    const QString now = tr("Update now");
                    if (classicMessage(&dlg, tr("Update available"),
                                       tr("Master PDF %1 is available.\nYou have version %2.").arg(r->version, updates::appVersion()),
                                       {now, tr("Not now")}) == now) {
                        dlg.close();
                        installUpdate(*r);
                    } else {
                        showUpdateButton(*r);  // (Not now: the title bar's update button, for later)
                    }
                } else {
                    status->setText(tr("You have the newest version (%1).").arg(updates::appVersion()));
                }
            });
        });
        page->addWidget(g);
    }
    auto *row = new QHBoxLayout;
    row->addStretch();
    auto *ok = new ClassicButton(tr("OK"), body);
    ok->setMinimumWidth(75);
    connect(ok, &QPushButton::clicked, &dlg, &QWidget::close);
    row->addWidget(ok);
    page->addLayout(row);
    settingsNotes(&dlg);
    dlg.exec();
}

