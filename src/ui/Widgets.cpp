#include "ui/Widgets.h"

#include "app/Updates.h"

#include "ui/Classic.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"

#include <QApplication>
#include <QColorDialog>
#include <QGridLayout>
#include <QPlainTextEdit>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>

// ---- StatusPanel ----

StatusPanel::StatusPanel(QWidget *parent) : QLabel(parent)
{
    setFont(theme::font());
    setContentsMargins(4, 1, 4, 1);
    setFixedHeight(19);
}

void StatusPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
    classic::thinSunken(p, rect());
    p.setPen(theme::text());
    p.setFont(font());
    classic::crispText(p, rect().adjusted(4, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter, text(), nullptr, Qt::ElideRight);
}

// ---- EngravedLabel ----

EngravedLabel::EngravedLabel(QWidget *parent) : QWidget(parent)
{
    setFont(theme::font());
}

void EngravedLabel::setText(const QString &text)
{
    text_ = text;
    updateGeometry();
    update();
}

QSize EngravedLabel::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(text_) + 6, fontMetrics().height() + 2};
}

void EngravedLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setFont(font());
    classic::engravedText(p, rect().adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text_);
}

// ---- ChooserButton ----

ChooserButton::ChooserButton(QWidget *parent) : QWidget(parent)
{
    setFont(theme::font());
}

void ChooserButton::setText(const QString &text)
{
    text_ = text;
    setFixedSize(sizeHint());
    update();
}

void ChooserButton::setPressed(bool on)
{
    pressed_ = on;
    update();
}

QSize ChooserButton::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(text_) + 10 + 2 * 4 + 2 * 2 + 2, fontMetrics().height() + 6};
}

void ChooserButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), theme::face());
    if (pressed_)
        classic::sunkenEdge(p, rect(), QColor());
    const int o = pressed_ ? 1 : 0;
    p.setPen(theme::text());
    p.setFont(font());
    const int tw = fontMetrics().horizontalAdvance(text_);
    classic::crispText(p, QRect(6 + o, o, tw + 6, height()), Qt::AlignLeft | Qt::AlignVCenter, text_);
    // the little arrow: a triangle 10 wide and 5 deep
    classic::downArrow(p, QRectF(6 + tw + 3 + o, height() / 2 - 2 + o, 10, 5), theme::text());
}

void ChooserButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit pressed();
}

// ---- StayOpenMenu ----

void StayOpenMenu::mouseReleaseEvent(QMouseEvent *e)
{
    QAction *a = actionAt(e->position().toPoint());
    if (!a || !a->isEnabled() || a->isSeparator() || a->menu()) {
        QMenu::mouseReleaseEvent(e);
        return;
    }
    a->trigger();  // (tried: the list stays open, made again to show the new choice)
    if (refill)
        refill();
    update();
}

// ---- ClassicButton ----

ClassicButton::ClassicButton(const QString &text, QWidget *parent) : QPushButton(text, parent)
{
    setFont(theme::font());
    setAutoDefault(false);
}

QSize ClassicButton::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(text()) + 2 * 8 + 2 * 4 + 4, fontMetrics().height() + 14};
}

// ---- StartPanel ----

StartPanel::StartPanel(QWidget *parent) : QWidget(parent)
{
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(3 + 20, 3 + 14, 3 + 20, 3 + 14);
    box->setSpacing(0);
    auto *logo = new QLabel(this);
    QImage app = icons::get(QStringLiteral("app"));
    const double dpr = qApp->devicePixelRatio();
    QImage scaled = app.scaled(QSize(64, 64) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    logo->setPixmap(QPixmap::fromImage(scaled));
    logo->setAlignment(Qt::AlignCenter);
    box->addWidget(logo);
    box->addSpacing(8);
    auto *title = new QLabel(tr("No PDF is open"), this);
    title->setFont(theme::font(10, true));
    title->setAlignment(Qt::AlignCenter);
    box->addWidget(title);
    box->addSpacing(4);
    auto *note = new QLabel(tr("Open a PDF, or drag one onto this window."), this);
    note->setFont(theme::font());
    note->setAlignment(Qt::AlignCenter);
    box->addWidget(note);
    box->addSpacing(10);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    openButton = new ClassicButton(tr("Open..."), this);
    createButton = new ClassicButton(tr("Create PDF from files..."), this);
    row->addStretch();
    row->addWidget(openButton);
    row->addWidget(createButton);
    row->addStretch();
    box->addLayout(row);
}

void StartPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    classic::raisedEdge(p, rect());
}

// ---- ClassicDialog ----

ClassicDialog::ClassicDialog(QWidget *owner, const QString &title) : ClassicWindow(false, nullptr), owner_(owner)
{
    setWindowFlag(Qt::Dialog, true);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setTitle(title);
}

void ClassicDialog::exec(bool placed)
{
    adjustSize();
    QWidget *top = owner_ ? owner_->window() : nullptr;
    const QRect over = top && top->isVisible() ? top->frameGeometry()
                                               : QGuiApplication::primaryScreen()->availableGeometry();
    if (!placed)
        move(over.left() + (over.width() - width()) / 2, over.top() + (over.height() - height()) / 3);
    show();
    activateWindow();
    QEventLoop loop;
    loop_ = &loop;
    loop.exec();
    loop_ = nullptr;
}

void ClassicDialog::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    ClassicWindow::keyPressEvent(e);
}

void ClassicDialog::closeEvent(QCloseEvent *e)
{
    ClassicWindow::closeEvent(e);
    if (loop_)
        loop_->quit();
}

static std::function<QString(const QString &, const QStringList &)> g_autoAnswer;

void setAutoAnswer(std::function<QString(const QString &, const QStringList &)> answer)
{
    g_autoAnswer = std::move(answer);
}

QString classicMessage(QWidget *owner, const QString &title, const QString &message,
                       const QStringList &buttons)
{
    if (g_autoAnswer)
        return g_autoAnswer(message, buttons);
    ClassicDialog dlg(owner, title);
    auto *box = new QVBoxLayout(dlg.body());
    box->setContentsMargins(12, 12, 12, 12);
    auto *label = new QLabel(message, dlg.body());
    label->setFont(theme::font());
    label->setWordWrap(true);
    label->setMinimumWidth(280);
    label->setMaximumWidth(420);
    box->addWidget(label);
    box->addSpacing(8);
    auto *row = new QHBoxLayout;
    row->addStretch();
    QString chosen;
    for (const QString &text : buttons) {
        auto *b = new ClassicButton(text, dlg.body());
        QObject::connect(b, &QPushButton::clicked, &dlg, [&dlg, &chosen, text] {
            chosen = text;
            dlg.close();
        });
        row->addWidget(b);
    }
    row->addStretch();
    box->addLayout(row);
    dlg.exec();
    return chosen;
}

void showInFolder(const QString &path)
{
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void aboutBox(QWidget *owner)
{
    ClassicDialog dlg(owner, QObject::tr("About Master PDF"));
    auto *box = new QVBoxLayout(dlg.body());
    box->setContentsMargins(16, 14, 12, 10);
    box->setSpacing(0);
    auto *top = new QHBoxLayout;
    auto *logo = new QLabel(dlg.body());
    const double dpr = qApp->devicePixelRatio();
    QImage app = icons::get(QStringLiteral("app")).scaled(QSize(64, 64) * dpr, Qt::KeepAspectRatio,
                                                          Qt::SmoothTransformation);
    app.setDevicePixelRatio(dpr);
    logo->setPixmap(QPixmap::fromImage(app));
    logo->setAlignment(Qt::AlignTop);
    top->addWidget(logo);
    top->addSpacing(14);
    auto *col = new QVBoxLayout;
    col->setSpacing(0);
    auto *name = new QLabel(QStringLiteral("Master PDF"), dlg.body());
    name->setFont(theme::font(14, true));
    col->addWidget(name);
    auto *version = new QLabel(updates::canUpdate() ? QObject::tr("Version %1").arg(updates::appVersion())
                                                       : QObject::tr("Version: dev (running from the source code)"), dlg.body());
    version->setFont(theme::font());
    col->addSpacing(2);
    col->addWidget(version);
    col->addSpacing(8);
    auto *what = new QLabel(QObject::tr("Read, mark up and change your PDFs - without uploading them\n"
                                        "anywhere. Everything happens on your own computer."),
                            dlg.body());
    what->setFont(theme::font());
    col->addWidget(what);
    top->addLayout(col);
    box->addLayout(top);
    box->addSpacing(12);
    box->addWidget(new EtchedLine(dlg.body()));
    box->addSpacing(12);
    auto *dev = new QLabel(QObject::tr("Developer: bocchi the old"), dlg.body());
    dev->setFont(theme::font());
    box->addWidget(dev);
    box->addSpacing(4);
    auto *more = new QLabel(QObject::tr("For more tools, check out my profile:"), dlg.body());
    more->setFont(theme::font());
    box->addWidget(more);
    const QString url = QStringLiteral("https://github.com/bocchhii");
    auto *link = new QLabel(QStringLiteral("<a href=\"%1\" style=\"color:%2\">%1</a>")
                                .arg(url, theme::c("#0000EE").name()),
                            dlg.body());
    link->setFont(theme::font());
    link->setOpenExternalLinks(true);
    box->addWidget(link);
    box->addSpacing(10);
    auto *row = new QHBoxLayout;
    row->addStretch();
    auto *ok = new ClassicButton(QObject::tr("OK"), dlg.body());
    ok->setMinimumWidth(75);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QWidget::close);
    row->addWidget(ok);
    box->addLayout(row);
    dlg.body()->setMinimumWidth(380);
    dlg.exec();
}


QString textDialog(QWidget *owner, const QString &title, const QString &prompt, const QString &text)
{
    ClassicDialog dlg(owner, title);
    auto *box = new QVBoxLayout(dlg.body());
    box->setContentsMargins(12, 12, 12, 12);
    box->addWidget(new QLabel(prompt, dlg.body()));
    auto *edit = new QPlainTextEdit(text, dlg.body());
    edit->setMinimumSize(300, 100);
    box->addWidget(edit);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    bool ok = false;
    auto *okButton = new ClassicButton(QObject::tr("OK"), dlg.body());
    auto *cancel = new ClassicButton(QObject::tr("Cancel"), dlg.body());
    QObject::connect(okButton, &QPushButton::clicked, &dlg, [&] {
        ok = true;
        dlg.close();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QWidget::close);
    buttons->addWidget(okButton);
    buttons->addWidget(cancel);
    box->addLayout(buttons);
    edit->setFocus();
    dlg.exec();
    return ok ? edit->toPlainText() : QString();
}

ClassicProgress::ClassicProgress(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(20);
}

void ClassicProgress::setValue(double f)
{
    f = std::clamp(f, 0.0, 1.0);
    const bool more = blocks(f) != blocks(value_);
    value_ = f;
    if (more)
        update();
}

int ClassicProgress::room() const
{
    return std::max(1, (width() - 4 + kGap) / (kBlock + kGap));  // (the blocks it holds)
}

int ClassicProgress::blocks(double f) const
{
    return int(f * room() + 1e-9);  // (whole blocks only)
}

void ClassicProgress::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // (inside: the text boxes' colour, darkened - a well in the face, as the update's box has it)
    const QColor face = theme::face(), well = theme::dark() ? face.darker(150) : theme::box();
    classic::sunkenEdge(p, QRectF(rect()), well);
    // (the blocks: the title bar's colour - its strong end; on a dark well, its middle: as bright as the bar looks)
    const auto [left, right] = theme::caption(true);
    const QColor color = !theme::dark() ? left
                                        : QColor((left.red() + right.red()) / 2, (left.green() + right.green()) / 2, (left.blue() + right.blue()) / 2);
    const int H = height();
    for (int i = 0, n = blocks(value_); i < n; ++i)
        classic::block(p, QPointF(3 + i * (kBlock + kGap), 3), 0, 0, kBlock, H - 6, color);
}
