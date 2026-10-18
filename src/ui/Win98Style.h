#pragma once

#include <QProxyStyle>

// Qt's own widgets drawn like the Python app's Windows 98 ones, in the theme's colours:
// menus and the menu bar, scrollbars (17 px, checkered track), text boxes and dropdowns
// (sunken), buttons (raised), tooltips (Windows' little yellow note).
class Win98Style : public QProxyStyle {
    Q_OBJECT
public:
    Win98Style();

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p,
                       const QWidget *w = nullptr) const override;
    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p,
                     const QWidget *w = nullptr) const override;
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w = nullptr) const override;
    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc,
                         const QWidget *w = nullptr) const override;
    int pixelMetric(PixelMetric m, const QStyleOption *opt = nullptr,
                    const QWidget *w = nullptr) const override;
    QSize sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size,
                           const QWidget *w = nullptr) const override;
    int styleHint(StyleHint sh, const QStyleOption *opt = nullptr, const QWidget *w = nullptr,
                  QStyleHintReturn *ret = nullptr) const override;
    void drawItemText(QPainter *p, const QRect &rect, int flags, const QPalette &pal, bool enabled, const QString &text,
                      QPalette::ColorRole textRole = QPalette::NoRole) const override;
    void polish(QPalette &pal) override;
    void polish(QWidget *w) override;

    // a menu's text starts this far in (the menu bar's lists: 8, right-click menus: 22)
    static constexpr const char *kIndent = "classicIndent";

private:
    void drawTriangle(QPainter *p, QPoint centre, Qt::ArrowType dir, const QColor &c) const;
};
