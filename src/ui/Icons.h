#pragma once

#include <QImage>
#include <QPainter>
#include <QString>

// The toolbar pictures, taken from the Python app (resources/icons): 16 x 16 pixel drawings,
// and for the ones drawn smooth (the magnifying glass, the signature...) a 64 x 64 copy, so
// they're sharp on a scaled screen too.
namespace icons {

QImage get(const QString &name);  // the 16 x 16 picture
bool has(const QString &name);

// Draws a picture in r (16 x 16 in the widget's coordinates): pixel drawings in whole
// screen pixels, smooth ones from their big copy. Greyed out: engraved. whiten: its black
// parts white, down to that many of its 16 rows (the dark theme), 0: as it is.
void draw(QPainter &p, const QRectF &r, const QString &name, bool enabled = true, int whiten = 0);
void draw(QPainter &p, const QRectF &r, const QImage &image, bool enabled = true, int whiten = 0);

// The colour button's picture: the colour in a black-edged square (16 x 16)
QImage swatch(const QColor &color);
// A picture with the parts in one colour (and its darker shade) in another: the tools'
// buttons show the colour they draw in
QImage tinted(const QImage &image, const QColor &from, const QColor &to);
// A picture by name, at its biggest (the smooth ones: their 64 x 64 copy)
QImage best(const QString &name);
// a picture layout's picture, as Word's (the Python app's layout_icon): the picture - a grey dome -
// and the text - thin blue lines - round it, over it or under it; 32 x 26 (small: 16 x 16)
QImage layout(const QString &mode, bool small);

}  // namespace icons
