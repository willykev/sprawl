#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVector>
#include <utility>

#include "item.h"

// The math of the canvas, free of Qt Quick so it can be tested on its own:
// pen paths, the triangles strokes are drawn with, hit tests, and where a
// connector meets a shape.
namespace Geo {

// --- Pen paths -----------------------------------------------------------

// Drops the points a path does not need (Ramer-Douglas-Peucker): none of the
// removed points was further than `tolerance` from the simplified path.
QVector<QPointF> simplify(const QVector<QPointF> &points, qreal tolerance);

// A smooth curve through the points (centripetal Catmull-Rom, which never
// loops or overshoots between unevenly spaced points), flattened to line
// segments that stay within `tolerance` of the curve.
QVector<QPointF> smooth(const QVector<QPointF> &points, qreal tolerance);

QRectF boundsOf(const QVector<QPointF> &points);

// --- Stroke meshes ---------------------------------------------------------

// One corner of a triangle of a stroke. The shader moves it along `normal`
// so the stroke is antialiased over exactly one screen pixel at any zoom,
// and never thinner than a pixel.
struct StrokeVertex {
    float x, y;         // on the stroke's outline
    float nx, ny;       // which way is out, scaled for mitered corners
    float halfWidth;    // of the stroke here
    float side;         // -1 and 1 on the two edges, 0 along the middle
    quint8 r, g, b, a;  // premultiplied
};
static_assert(sizeof(StrokeVertex) == 28, "StrokeVertex is uploaded to the GPU as is");

struct StrokeMesh {
    QVector<StrokeVertex> vertices;
    QVector<quint32> indices;
    void clear() {
        vertices.clear();
        indices.clear();
    }
};

enum class Cap { Round, Butt };
// Bevel is the cheap one: a corner is cut straight across, with a single
// triangle, which is all a stroke needs when it is small on screen.
enum class Join { Round, Miter, Bevel };

// Premultiplied color as the bytes a StrokeVertex carries.
quint32 packColor(const QColor &color, qreal opacity = 1);

// Appends the triangles of a line through `points` (moved by `offset`).
void strokePolyline(StrokeMesh &mesh, const QVector<QPointF> &points, QPointF offset, qreal halfWidth, quint32 color,
                    Cap cap = Cap::Round, Join join = Join::Round);

// --- Shape meshes ----------------------------------------------------------

// One corner of the quad a shape is drawn in. The shader works out, for each
// pixel, how far it is from the shape's edge.
struct ShapeVertex {
    float x, y;           // a corner of the shape's box
    float localX, localY; // the same corner, from the middle of the shape
    float kind, radius, borderWidth, unused;
    float axisX, axisY;   // which way the shape's own x axis points
    quint8 fill[4];       // premultiplied
    quint8 border[4];     // premultiplied
};
static_assert(sizeof(ShapeVertex) == 48, "ShapeVertex is uploaded to the GPU as is");

// The outlines the shape shader knows.
enum class Outline { Rect = 0, Ellipse = 1, Diamond = 2, Arrowhead = 3 };

struct ShapeMesh {
    // Four vertices per shape.
    QVector<ShapeVertex> vertices;
    void clear() { vertices.clear(); }
    int count() const { return int(vertices.size() / 4); }

    void add(const QRectF &rect, Outline outline, qreal radius, qreal borderWidth, quint32 fill, quint32 border);
    // A shape turned so its x axis points along `axis` (a unit vector).
    void add(QPointF center, QSizeF halfSize, QPointF axis, Outline outline, qreal radius, qreal borderWidth,
             quint32 fill, quint32 border);
};

// --- Shapes ----------------------------------------------------------------

// A shape's outline in a box of a given size with its corner at the origin:
// a closed path of straight edges (curves are cut into edges that stay
// within a tolerance of them), and, for the few shapes that have one, a line
// drawn inside it. One description of each shape, from which it is drawn,
// exported, clicked, and met by connectors.
struct Figure {
    QVector<QPointF> edge;
    QVector<QPointF> line;
};
Figure figure(Item::Shape shape, QSizeF size, qreal tolerance = 0.05);
// The same with fewer points: no further than `tolerance` from the original.
Figure simplified(const Figure &figure, qreal tolerance);

// The shapes the shape shader draws by itself, from one quad, exact at any
// zoom: boxes with square or round corners, ellipses and diamonds. The rest
// are drawn as their figures.
bool isQuad(Item::Shape shape);
Outline outlineOf(Item::Shape shape);
// How round the corners of a box are: as round as the desktop's windows for
// a rectangle, rounder for a rounded one, and all the way for a pill.
qreal cornerRadius(Item::Shape shape, QSizeF size, qreal desktopRadius);

// Where what is written on a shape goes: the part of its box that is
// inside its outline, more or less.
QRectF textBox(Item::Shape shape, QSizeF size);

// Cuts a polygon without holes into triangles: three indices into `polygon`
// for each. It need not be convex.
QVector<quint32> triangulate(const QVector<QPointF> &polygon);
bool polygonContains(const QVector<QPointF> &polygon, QPointF p);

// Appends a figure as it is drawn: filled, with a border centered on its
// outline (so give it a figure half the border's width inside the shape's
// box), and its inner line in the border's color.
void strokeFigure(StrokeMesh &mesh, const Figure &figure, QPointF offset, quint32 fill, quint32 border,
                  qreal borderWidth = 1);

// --- Shared outlines -------------------------------------------------------

// A frame's edge: a rectangle one unit wide with a gap in the top for the
// title (no gap when gapTo is not past gapFrom). It starts and ends mid-edge
// so its square ends meet.
QVector<QPointF> frameEdge(QSizeF size, qreal gapFrom, qreal gapTo);

// The line a connector follows from one end to the other: two points when it
// is straight, four with square corners when it is an elbow, and a curve cut
// into edges that stay within `tolerance` of it.
QVector<QPointF> connectorPath(const Item &connector, qreal tolerance = 0.1);

// A place along a path, so far from its start, and which way the path is
// going there.
struct PathPoint {
    QPointF at, heading;
};
qreal pathLength(const QVector<QPointF> &path);
PathPoint alongPath(const QVector<QPointF> &path, qreal distance);

// A connector as it is drawn. Its line is one run or, dashed, many; an open
// arrow and a bar are runs of line too. The other ends are shapes: a
// triangle, a circle, or a diamond, filled or not, turned to face along
// the line. One description, for the canvas and for exported files.
struct ConnectorArt {
    QVector<QVector<QPointF>> lines;
    struct Head {
        QPointF center;
        QSizeF half;
        // Which way it points: away from the line.
        QPointF axis;
        Outline outline = Outline::Arrowhead;
        bool hollow = false;
    };
    QVector<Head> heads;
    // The middle of the line, for what is written beside it.
    PathPoint middle;
};
ConnectorArt connectorArt(const Item &connector);

// --- Hit tests -------------------------------------------------------------

qreal distanceToSegment(QPointF p, QPointF a, QPointF b);
qreal distanceToPolyline(const QVector<QPointF> &points, QPointF p);
bool segmentIntersectsRect(QPointF a, QPointF b, const QRectF &rect);
bool polylineIntersectsRect(const QVector<QPointF> &points, const QRectF &rect);
bool shapeContains(Item::Shape shape, const QRectF &rect, QPointF p);

// --- Connectors -------------------------------------------------------------

// Where a line from the middle of the shape toward `target` leaves it.
QPointF shapeEdgeToward(Item::Shape shape, const QRectF &rect, QPointF target);

} // namespace Geo
