#pragma once

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QString>

// Drawing the Windows 98 look sharp on any screen: the 3D edges are drawn in whole screen
// pixels (a scaled screen would otherwise smear a 1 px line over two), the text as usual.
namespace classic {

// While it lives, the painter draws in screen pixels; dev() turns the widget's own
// coordinates into them. k: how many screen pixels one line of an edge is.
class Px {
public:
    explicit Px(QPainter &p);
    ~Px();
    QRect dev(const QRectF &r) const;
    int k = 1;
    double dpr = 1;
    QPainter &p;
    void fill(const QRect &d, const QColor &c) { p.fillRect(d, c); }
    // one layer of edge, n screen pixels thick: the top / left in tl, then the bottom / right
    // in br (over the corners, like Windows draws them)
    void bevel(const QRect &d, const QColor &tl, const QColor &br, int n) const;

private:
    QTransform full_;  // (the painter's own coordinates to the screen's pixels, where they really land)
    QPoint base_;      // (where its origin lands: a whole screen pixel)
};

// The edges (r: the whole box, in the widget's coordinates)
void raisedEdge(QPainter &p, const QRectF &r, bool pressed = false, bool fill = true);  // buttons, menus
void sunkenEdge(QPainter &p, const QRectF &r, const QColor &inside);  // text boxes, panels (2 px)
void thinRaised(QPainter &p, const QRectF &r);  // a toolbar button under the mouse (1 px)
void thinSunken(QPainter &p, const QRectF &r);  // ... held down; a status bar panel
void smallButton(QPainter &p, const QRectF &r, bool pressed);  // title bar / + buttons
void scrollBox(QPainter &p, const QRectF &r);  // a scrollbar's thumb and arrow buttons
void etchedH(QPainter &p, int x0, int x1, int y);  // shadow over light
void etchedV(QPainter &p, int x, int y0, int y1);  // shadow beside light
void checkered(QPainter &p, const QRectF &r, const QColor &a, const QColor &b, QPoint phase = {});

// Pixel art drawn bigger without blurring: each of its pixels becomes a block of screen pixels
void pixelArt(QPainter &p, const QRectF &r, const QImage &im);
// Black blocks [x0, x1) x [y0, y1) of a pixel drawing whose pixel (0, 0) is at origin
void block(QPainter &p, QPointF origin, int x0, int y0, int x1, int y1, const QColor &c);
// A solid triangle pointing down, as wide and deep as box (widget pixels), drawn in screen pixels:
// each row a pixel narrower at both ends at any scale (rows of blocks scaled one by one came out
// 1 or 2 pixels tall, lopsided)
void downArrow(QPainter &p, const QRectF &box, const QColor &c);

// Text laid out in the screen's own pixels - each letter's width a whole screen pixel, so the spacing is
// even at 125% / 150%, as Windows 98 drew it (Qt lays text out at 100% and stretches it: some gaps a pixel
// wider than others). The painter's font and pen; bounds: where it went; elide: cut short with "..."
void crispText(QPainter &p, const QRectF &r, int flags, const QString &text, QRectF *bounds = nullptr,
               Qt::TextElideMode elide = Qt::ElideNone);
QRectF crispBounds(QPainter &p, const QRectF &r, int flags, const QString &text);  // (where it would go)

// Windows 98's hourglass: the pointer while the app is busy (drawn pixel by pixel, sharp at any scale)
QCursor waitCursor();

// Greyed-out text, engraved into the face like Windows 98's
void engravedText(QPainter &p, const QRectF &r, int flags, const QString &text);
// A picture greyed out the same way; and one with its black parts white (the dark theme)
QImage engravedImage(const QImage &im);
QImage whitened(const QImage &im, int rows = 16);
// just its dark blues made grey - the icons' standard, in every theme (dark: a mid grey, as whitened
// makes them; light: a darker one, to show on the light bars) - its black left as it is
QImage greyBlues(const QImage &im, bool dark);

}  // namespace classic
