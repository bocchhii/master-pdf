#include "ui/Win98Style.h"

#include "ui/Classic.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QMenu>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOption>

using namespace classic;

namespace {

constexpr int kItemH = 21;     // a menu's item
constexpr int kSepH = 8;       // ... its line between groups
constexpr int kBarItemH = 20;  // a menu bar title
constexpr int kScroll = 17;    // a scrollbar's width, and its arrow buttons'
constexpr int kComboArrow = 16;

bool checkeredTrack()
{
    const QString t = theme::current();
    return t == "98" || t == "xp" || t == "mono";
}

}  // namespace

Win98Style::Win98Style() : QProxyStyle(QStyleFactory::create(QStringLiteral("Windows"))) {}

void Win98Style::polish(QPalette &pal)
{
    pal = theme::palette();
}

void Win98Style::polish(QWidget *w)
{
    QProxyStyle::polish(w);
    if (qobject_cast<QMenu *>(w))  // (Windows 98's menus had no shadow)
        w->setWindowFlag(Qt::NoDropShadowWindowHint);
    if (w->inherits("QComboBoxPrivateContainer"))  // (a drop-down's list: see eventFilter)
        w->installEventFilter(this);
}

bool Win98Style::eventFilter(QObject *o, QEvent *e)
{
    // a drop-down's list opened: what's chosen lit - not the line the mouse last passed over (the list
    // closed without choosing kept that one lit, the chosen one not)
    if (e->type() == QEvent::Show)
        if (auto *combo = qobject_cast<QComboBox *>(static_cast<QWidget *>(o)->parentWidget()); combo && combo->currentIndex() >= 0) {
            const QModelIndex at = combo->model()->index(combo->currentIndex(), combo->modelColumn(), combo->rootModelIndex());
            combo->view()->selectionModel()->setCurrentIndex(at, QItemSelectionModel::ClearAndSelect);
        }
    return QProxyStyle::eventFilter(o, e);
}

void Win98Style::drawTriangle(QPainter *p, QPoint c, Qt::ArrowType dir, const QColor &color) const
{
    // a solid triangle 7 px wide and 4 deep (as the Python app's), drawn in screen pixels: as many
    // rows as 4 make on this screen, each a pixel narrower at both ends - scaled row by row (125%)
    // its rows came out 1 or 2 pixels tall and shifted, a lopsided bowl
    Px x(*p);
    const QRect at = x.dev(QRectF(c.x(), c.y(), 0, 0));
    const int cx = at.x(), cy = at.y();
    const int n = std::max(4, int(std::lround(4 * x.dpr)));
    for (int r = 0; r < n; ++r) {
        const int wide = n - 1 - r;  // (half its width, the widest row first)
        switch (dir) {
        case Qt::UpArrow: x.fill(QRect(cx - r, cy - n / 2 + r, 2 * r + 1, 1), color); break;
        case Qt::DownArrow: x.fill(QRect(cx - wide, cy - n / 2 + r, 2 * wide + 1, 1), color); break;
        case Qt::LeftArrow: x.fill(QRect(cx - n / 2 + r, cy - r, 1, 2 * r + 1), color); break;
        default: x.fill(QRect(cx - n / 2 + r, cy - wide, 1, 2 * wide + 1), color); break;
        }
    }
}

void Win98Style::drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p,
                               const QWidget *w) const
{
    switch (pe) {
    case PE_PanelLineEdit:
        if (const auto *f = qstyleoption_cast<const QStyleOptionFrame *>(opt); f && f->lineWidth > 0)
            sunkenEdge(*p, opt->rect, opt->palette.color(QPalette::Base));
        else
            p->fillRect(opt->rect, opt->palette.color(QPalette::Base));
        return;
    case PE_FrameLineEdit:
        sunkenEdge(*p, opt->rect, QColor());
        return;
    case PE_Frame:
    case PE_FrameStatusBarItem:
        if (opt->state & State_Sunken)
            sunkenEdge(*p, opt->rect, QColor());
        else if (opt->state & State_Raised)
            raisedEdge(*p, opt->rect, false, false);
        return;
    case PE_PanelButtonCommand:
    case PE_PanelButtonBevel:
        raisedEdge(*p, opt->rect, opt->state & (State_Sunken | State_On));
        return;
    case PE_FrameDefaultButton:
    case PE_FrameFocusRect:
        return;  // (no focus rectangles, like the Python app)
    case PE_IndicatorCheckBox: {
        // Windows 98's: a sunken box, its tick in columns three pixels tall - down to its low point,
        // then up - drawn in screen pixels at this screen's size (Qt's, scaled, was blurred and ragged)
        const bool enabled = opt->state & State_Enabled;
        sunkenEdge(*p, opt->rect, enabled ? theme::box() : theme::face());
        if (!(opt->state & (State_On | State_NoChange)))
            return;
        Px x(*p);
        const QRect d = x.dev(opt->rect);
        const int n = std::max(7, int(std::lround(7 * x.dpr)));  // (its columns)
        const int t = 3;  // (each one's height: three screen pixels, as Windows 98's)
        const int low = int(std::lround((n - 1) / 3.0));        // (the column at its low point)
        const int left = d.left() + (d.width() - n) / 2, top = d.top() + (d.height() - n) / 2;
        const QColor ink = enabled ? theme::text() : theme::engraved().second;
        for (int c = 0; c < n; ++c) {
            const int bottom = n - 1 - std::abs(c - low);
            x.fill(QRect(left + c, top + bottom - t + 1, 1, t), ink);
        }
        return;
    }
    case PE_PanelTipLabel: {  // Windows' little yellow note: a 1 px solid edge
        p->fillRect(opt->rect, theme::c("#FFFFE1", theme::Box));
        Px x(*p);
        const QRect d = x.dev(opt->rect);
        x.bevel(d, theme::text(), theme::text(), x.k);
        return;
    }
    case PE_PanelMenu:
        p->fillRect(opt->rect, theme::face());
        return;
    case PE_FrameMenu:
        raisedEdge(*p, opt->rect, false, false);
        return;
    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const Qt::ArrowType dir = pe == PE_IndicatorArrowUp ? Qt::UpArrow
                                  : pe == PE_IndicatorArrowDown ? Qt::DownArrow
                                  : pe == PE_IndicatorArrowLeft ? Qt::LeftArrow : Qt::RightArrow;
        const QColor c = opt->state & State_Enabled ? theme::ink() : theme::engraved().second;
        drawTriangle(p, opt->rect.center(), dir, c);
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}

void Win98Style::drawItemText(QPainter *p, const QRect &rect, int flags, const QPalette &pal, bool enabled,
                              const QString &text, QPalette::ColorRole textRole) const
{
    // (a label's, a button's, a check box's words: in the screen's own pixels)
    if (text.isEmpty())
        return;
    if (!enabled && textRole == QPalette::NoRole) {
        engravedText(*p, rect, flags, text);
        return;
    }
    const QPen was = p->pen();
    if (textRole != QPalette::NoRole)
        p->setPen(QPen(pal.brush(textRole), was.widthF()));
    crispText(*p, rect, flags, text);
    p->setPen(was);
}

void Win98Style::drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p,
                             const QWidget *w) const
{
    switch (ce) {
    case CE_MenuItem: {
        const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (!mi)
            break;
        const QRect r = opt->rect;
        p->fillRect(r, theme::face());
        if (mi->menuItemType == QStyleOptionMenuItem::Separator) {  // etched: grey over white
            Px x(*p);
            const QRect d = x.dev(QRectF(r.left() + 1, r.top() + 3, r.width() - 2, 2));
            x.fill(QRect(d.left(), d.top(), d.width(), x.k), theme::c("#A0A0A0", theme::Edge));
            x.fill(QRect(d.left(), d.top() + x.k, d.width(), x.k), theme::light());
            return;
        }
        const int indent = w && w->property(kIndent).isValid() ? w->property(kIndent).toInt() : 22;
        const bool enabled = mi->state & State_Enabled;
        const bool latched = mi->checkType != QStyleOptionMenuItem::NotCheckable && mi->checked;
        QColor fg = theme::text();
        if (latched) {  // pressed in, like a pressed menu bar title
            sunkenEdge(*p, r, theme::face());
        } else if (enabled && (mi->state & State_Selected)) {
            const auto [bg, selText] = theme::selection();
            p->fillRect(r, bg);
            fg = selText;
        }
        if (!enabled)
            fg = theme::c("#808080", theme::Text);
        p->setFont(mi->font);
        p->setPen(fg);
        const QString text = mi->text.section(QLatin1Char('\t'), 0, 0);
        crispText(*p, r.adjusted(2 + indent, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic,
                    text);
        if (mi->menuItemType == QStyleOptionMenuItem::SubMenu)
            drawTriangle(p, QPoint(r.right() - 8, r.center().y()), Qt::RightArrow, fg);
        return;
    }
    case CE_MenuBarItem: {
        const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (!mi)
            break;
        p->fillRect(opt->rect, theme::face());
        const bool open = opt->state & State_Sunken;
        if (open)
            sunkenEdge(*p, opt->rect, QColor());
        p->setFont(mi->font);
        p->setPen(theme::text());
        const int o = open ? 1 : 0;
        crispText(*p, opt->rect.translated(o, o), Qt::AlignCenter | Qt::TextShowMnemonic, mi->text);
        return;
    }
    case CE_MenuBarEmptyArea:
    case CE_MenuEmptyArea:
        p->fillRect(opt->rect, theme::face());
        return;
    case CE_PushButtonLabel:
        if (const auto *b = qstyleoption_cast<const QStyleOptionButton *>(opt);
            b && !(b->state & State_Enabled) && b->icon.isNull()) {
            engravedText(*p, b->rect, Qt::AlignCenter | Qt::TextShowMnemonic, b->text);
            return;
        }
        break;
    case CE_RubberBand:  // the splitter being dragged: a dotted bar, like Windows 98's
        checkered(*p, opt->rect, Qt::black, Qt::white);
        return;
    case CE_Splitter:
        p->fillRect(opt->rect, theme::face());
        return;
    case CE_HeaderSection:  // a list's column heading: a raised button; held down, flat in a grey line
        if (opt->state & State_Sunken) {
            Px x(*p);
            const QRect d = x.dev(opt->rect);
            x.fill(d, theme::face());
            x.bevel(d, theme::shadow(), theme::shadow(), x.k);
        } else {
            raisedEdge(*p, opt->rect);
        }
        return;
    case CE_ItemViewItem:  // (a list's item: drawn as usual, its words in the screen's own pixels)
        if (const auto *v = qstyleoption_cast<const QStyleOptionViewItem *>(opt); v && !v->text.isEmpty() &&
                                                                                  v->decorationPosition == QStyleOptionViewItem::Top) {
            // (a picture over its name - Thumbnails: as Windows 98's, only the name highlighted, on one line cut
            // short)
            QStyleOptionViewItem o(*v);
            o.text.clear();
            const bool selected = v->state & State_Selected;
            o.state &= ~State_Selected;
            o.state &= ~State_HasFocus;
            QProxyStyle::drawControl(ce, &o, p, w);
            const QPalette::ColorGroup cg = !(v->state & State_Enabled) ? QPalette::Disabled
                                            : (v->state & State_Active) ? QPalette::Normal
                                                                        : QPalette::Inactive;
            const QRect pic = proxy()->subElementRect(SE_ItemViewItemDecoration, v, w);
            QRect tr = v->rect;
            tr.setTop(pic.bottom() + 3);
            tr.adjust(2, 0, -2, 0);
            p->save();
            p->setFont(v->font);
            const QRectF used = crispBounds(*p, tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                                            QFontMetrics(v->font).elidedText(v->text, Qt::ElideRight, tr.width()));
            if (selected) {
                const QColor hl = v->palette.color(cg, QPalette::Highlight);
                Px x(*p);
                x.fill(x.dev(used.adjusted(-2, 0, 2, 1).intersected(QRectF(tr))), hl);
            }
            p->setPen(v->palette.color(cg, selected ? QPalette::HighlightedText : QPalette::Text));
            crispText(*p, tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine, v->text, nullptr, Qt::ElideRight);
            p->restore();
            return;
        }
        if (const auto *v = qstyleoption_cast<const QStyleOptionViewItem *>(opt); v && !v->text.isEmpty()) {
            QStyleOptionViewItem o(*v);
            o.text.clear();
            QProxyStyle::drawControl(ce, &o, p, w);
            QRect tr = proxy()->subElementRect(SE_ItemViewItemText, v, w);
            const int margin = proxy()->pixelMetric(PM_FocusFrameHMargin, nullptr, w) + 1;
            tr.adjust(margin, 0, -margin, 0);
            const QPalette::ColorGroup cg = !(v->state & State_Enabled) ? QPalette::Disabled
                                            : (v->state & State_Active) ? QPalette::Normal
                                                                        : QPalette::Inactive;
            p->save();
            p->setPen(v->palette.color(cg, v->state & State_Selected ? QPalette::HighlightedText : QPalette::Text));
            p->setFont(v->font);
            const bool wrap = v->features & QStyleOptionViewItem::WrapText;
            QString text = v->text;
            text.replace(QLatin1Char('\n'), QChar::LineSeparator);
            crispText(*p, tr, int(v->displayAlignment) | (wrap ? Qt::TextWordWrap : Qt::TextSingleLine), text, nullptr,
                      wrap ? Qt::ElideNone : v->textElideMode);
            p->restore();
            return;
        }
        break;
    case CE_HeaderEmptyArea:  // (past the last column: a blank heading, as Windows 98's)
        raisedEdge(*p, opt->rect);
        return;
    case CE_HeaderLabel:
        if (const auto *h = qstyleoption_cast<const QStyleOptionHeader *>(opt)) {
            const int shift = h->state & State_Sunken ? 1 : 0;
            const QRect r = h->rect.adjusted(6 + shift, shift, -4 + shift, shift);
            p->setPen(theme::text());
            crispText(*p, r, int(h->textAlignment & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter | Qt::TextSingleLine, h->text,
                      nullptr, Qt::ElideRight);
            return;
        }
        break;
    case CE_ScrollBarAddPage:
    case CE_ScrollBarSubPage:
    case CE_ScrollBarSlider:
    case CE_ScrollBarAddLine:
    case CE_ScrollBarSubLine:
        return;  // (drawn together in drawComplexControl)
    default:
        break;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

void Win98Style::drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                                    const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt);
        if (!sb)
            return;
        const bool vertical = sb->orientation == Qt::Vertical;
        const QRect sub = subControlRect(cc, opt, SC_ScrollBarSubLine, w);
        const QRect add = subControlRect(cc, opt, SC_ScrollBarAddLine, w);
        const QRect slider = subControlRect(cc, opt, SC_ScrollBarSlider, w);
        const QRect groove = subControlRect(cc, opt, SC_ScrollBarGroove, w);
        const bool held = opt->state & State_Sunken;
        // the track: checkered face and light (the grey themes), else plain
        QColor a = theme::face(), b = theme::light();
        if (theme::mono())
            a = Qt::white, b = Qt::black;
        if (checkeredTrack())
            checkered(*p, groove, a, b, groove.topLeft());
        else
            p->fillRect(groove, theme::c("#EFEFEF", theme::Box));
        // the part of the track held down: dark
        for (SubControl page : {SC_ScrollBarSubPage, SC_ScrollBarAddPage}) {
            if (!(held && (sb->activeSubControls & page)))
                continue;
            const QRect r = subControlRect(cc, opt, page, w);
            if (checkeredTrack())
                checkered(*p, r, theme::darkEdge(), theme::shadow(), groove.topLeft());
            else
                p->fillRect(r, theme::shadow());
        }
        if (slider.isValid() && sb->maximum > sb->minimum)
            scrollBox(*p, slider);
        for (SubControl part : {SC_ScrollBarSubLine, SC_ScrollBarAddLine}) {
            const QRect r = part == SC_ScrollBarSubLine ? sub : add;
            const bool down = held && (sb->activeSubControls & part);
            if (down) {  // pressed: flat, a 1 px shadow round it
                Px x(*p);
                const QRect d = x.dev(r);
                x.fill(d, theme::face());
                x.bevel(d, theme::shadow(), theme::shadow(), x.k);
            } else {
                scrollBox(*p, r);
            }
            const Qt::ArrowType dir = vertical ? (part == SC_ScrollBarSubLine ? Qt::UpArrow : Qt::DownArrow)
                                               : (part == SC_ScrollBarSubLine ? Qt::LeftArrow : Qt::RightArrow);
            QPoint c(r.left() + r.width() / 2, r.top() + r.height() / 2);
            if (down)
                c += QPoint(1, 1);
            drawTriangle(p, c, dir, theme::ink());
        }
        return;
    }
    if (cc == CC_ComboBox) {
        const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(opt);
        if (!cb)
            return;
        const bool enabled = opt->state & State_Enabled;
        sunkenEdge(*p, opt->rect, enabled ? theme::box() : theme::face());
        const QRect arrow = subControlRect(cc, opt, SC_ComboBoxArrow, w);
        const bool down = (opt->state & State_Sunken) && (cb->activeSubControls & SC_ComboBoxArrow);
        if (down) {
            Px x(*p);
            const QRect d = x.dev(arrow);
            x.fill(d, theme::face());
            x.bevel(d, theme::shadow(), theme::shadow(), x.k);
        } else {
            scrollBox(*p, arrow);
        }
        QPoint c(arrow.left() + arrow.width() / 2, arrow.top() + arrow.height() / 2);
        if (down)
            c += QPoint(1, 1);
        drawTriangle(p, c, Qt::DownArrow, enabled ? theme::ink() : theme::engraved().second);
        // (focused, not typed in: its choice in the selection's blue, as Windows 98 shows it - the label is drawn in the
        // selection's text colour then: on the box's white it was gone)
        if (enabled && (opt->state & State_HasFocus) && !cb->editable) {
            const QRect field = subControlRect(cc, opt, SC_ComboBoxEditField, w).adjusted(0, 1, 0, -1);
            p->fillRect(field, opt->palette.brush(QPalette::Highlight));
        }
        return;
    }
    QProxyStyle::drawComplexControl(cc, opt, p, w);
}

QRect Win98Style::subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc,
                                 const QWidget *w) const
{
    if (cc == CC_ComboBox) {
        const QRect r = opt->rect;
        const QRect arrow(r.right() - 1 - kComboArrow, r.top() + 2, kComboArrow, r.height() - 4);
        switch (sc) {
        case SC_ComboBoxArrow: return arrow;
        case SC_ComboBoxEditField: return QRect(r.left() + 3, r.top() + 2, arrow.left() - r.left() - 4, r.height() - 4);
        case SC_ComboBoxFrame:
        case SC_ComboBoxListBoxPopup: return r;
        default: break;
        }
    }
    return QProxyStyle::subControlRect(cc, opt, sc, w);
}

int Win98Style::pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const
{
    switch (m) {
    case PM_ScrollBarExtent: return kScroll;
    case PM_ScrollBarSliderMin: return 20;
    case PM_MenuPanelWidth: return 4;  // (its raised edge, and 1 px of face)
    case PM_MenuHMargin:
    case PM_MenuVMargin:
    case PM_MenuBarPanelWidth:
    case PM_MenuBarItemSpacing:
    case PM_MenuBarHMargin:
    case PM_MenuBarVMargin:
        return 0;
    case PM_DefaultFrameWidth: return 2;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical: return 1;
    case PM_ToolTipLabelFrameWidth: return 2;
    case PM_SplitterWidth: return 5;
    case PM_SubMenuOverlap: return 0;
    default: break;
    }
    return QProxyStyle::pixelMetric(m, opt, w);
}

QSize Win98Style::sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size,
                                   const QWidget *w) const
{
    if (ct == CT_MenuItem) {
        const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (mi && mi->menuItemType == QStyleOptionMenuItem::Separator)
            return {size.width(), kSepH};
        if (mi) {  // the longest item with 22 px on both sides (and each item's 2 px edge)
            const QString text = mi->text.section(QLatin1Char('\t'), 0, 0);
            return {QFontMetrics(mi->font).horizontalAdvance(QString(text).remove(QLatin1Char('&'))) + 48, kItemH};
        }
    }
    if (ct == CT_MenuBarItem) {
        const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (mi) {  // as wide as its bold version, so a bold title wouldn't push the others
            QFont bold = mi->font;
            bold.setBold(true);
            return {QFontMetrics(bold).horizontalAdvance(QString(mi->text).remove(QLatin1Char('&'))) + 16, kBarItemH};
        }
    }
    if (ct == CT_ComboBox)
        return {size.width() + kComboArrow + 8, std::max(size.height() + 4, 21)};
    if (ct == CT_LineEdit)
        return {size.width(), std::max(size.height(), 20)};
    return QProxyStyle::sizeFromContents(ct, opt, size, w);
}

int Win98Style::styleHint(StyleHint sh, const QStyleOption *opt, const QWidget *w,
                          QStyleHintReturn *ret) const
{
    switch (sh) {
    case SH_UnderlineShortcut: return 1;  // (the menu titles' letters always underlined)
    case SH_ToolTip_WakeUpDelay: return 600;
    case SH_ComboBox_Popup: return 0;
    case SH_ComboBox_ListMouseTracking:
    case SH_MenuBar_MouseTracking:
    case SH_Menu_MouseTracking: return 1;
    case SH_Menu_FlashTriggeredItem: return 0;
    case SH_ComboBox_PopupFrameStyle: return QFrame::Box | QFrame::Plain;
    case SH_Menu_SubMenuPopupDelay: return 250;
    default: break;
    }
    return QProxyStyle::styleHint(sh, opt, w, ret);
}
