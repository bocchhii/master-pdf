#pragma once

// Picture shapes (the Python app's _shape_own, _shape_under, shape_in, own_fits, picture_clip):
// the shape a picture shows when it's on a plain background saved into it (PowerPoint's shapes,
// a logo on a slide's band, clip art on white) or has see-through parts of its own. Pure image
// work - Document gives the pictures. Cut to its shape ONLY when that changes nothing visible:
// when unsure, the whole picture. Signatures and sketches (lines, little ink) stay whole.

#include <QByteArray>
#include <QImage>
#include <QPointF>

#include <optional>
#include <vector>

namespace shapes {

// a mask: Format_Grayscale8, 255 where the shape is, 0 its background

// the shape from the picture alone (rgb no bigger than the size looked at; alpha: its own
// see-through parts, null if none) - or none
std::optional<QImage> own(const QImage &rgb, const QImage &alpha);
// the shape judged against what the page draws under it (under: that, the same size) - or none
// (judging: whether it has a shape - its background plain, not just lines (a signature); false:
// its shape looked at finely for the clip, once judged so - borderline numbers at the finer size
// mustn't undo that)
std::optional<QImage> onPage(const QImage &rgb, const QImage &under, bool judging = true);
// may the shape found in the picture alone be cut out? (its background round it is the colour
// the page shows under it, so cut it looks the same)
bool ownFits(const QImage &rgb, const QImage &under);

struct Found {
    std::vector<QPointF> hull;  // its outline round it, as parts of the width and height from the top left
    QImage pieces;              // just its own pieces (a mask)
};
// the shape in a mask (bits at its edges much smaller than it - pieces of the shapes beside it -
// left out; what's inside its edges kept) - or none (nothing much there, it fills its box,
// strokes not a filled shape)
std::optional<Found> shapeIn(QImage fg);

// a clip path in the picture's own unit square (PDF's way up): fine (a mask looked at finely)
// cut to pieces (shapeIn's), as rows of boxes that overlap a little (no hairline seams)
QByteArray clipPath(const QImage &fine, const QImage &pieces);

}  // namespace shapes
