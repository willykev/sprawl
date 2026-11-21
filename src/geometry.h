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
// segments that stay within `tolerance` of the curve. At a point given twice
// it comes to a point, as it does at its ends: that is a corner.
QVector<QPointF> smooth(const QVector<QPointF> &points, qreal tolerance);
// What the end of a line drawn by hand comes to as the hand goes on: the
// last `back` of its points give way to `points`, which give a corner twice.
struct Corner {
    int back = 0;
    QVector<QPointF> points;
};
// Where the line through `points`, going on to `next`, has made a corner
// that it does not come to a point at yet, judged as it is drawn; `pixel`
// is how long a screen pixel is. A corner is where the line turned by 75
// degrees or more, at a point or split between two that it went by either
// side of the corner, or by 40 or more where the line goes on straight
// either side and the corner would show rounded off: an L, a V, the points
// of a zigzag or a star, the corners of a box, an arrow's head, a tick.
// Rounded off, a corner bulges out past where it was, the more the longer
// the stretches either side of it. The corner is no more than two points
// back (see Pen::redraw()). Where the pen went is only known to the pixel,
// so one the hand went by is placed as near as the pixels tell, which for
// the sharpest is a pixel or two.
Corner cornerBefore(const QVector<QPointF> &points, QPointF next, qreal pixel);
// Drops the points the curve through them does not need: the curve through
// those kept (see smooth()) is nowhere further than `tolerance` from the
// curve through them all. A corner is kept twice or not at all, and every
// point the path needs is kept (see simplify()). A line that only waves by
// less than `tolerance` is kept as the line, unless the curve through its
// points waves further than that.
QVector<QPointF> simplifyCurve(const QVector<QPointF> &points, qreal tolerance);
// How closely a pen stroke on the board follows its curve, in units.
inline constexpr qreal strokeTolerance = 0.1;

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

// Appends the triangles of a line through `points` (moved by `offset`). Its
// round ends and corners are cut finely enough for any zoom; given a
// `scale`, only as finely as they show when a unit is that many screen
// pixels, which for a line a pixel or two wide on screen is hardly at all.
void strokePolyline(StrokeMesh &mesh, const QVector<QPointF> &points, QPointF offset, qreal halfWidth, quint32 color,
                    Cap cap = Cap::Round, Join join = Join::Round, qreal scale = 0);

// The same line cut a point at a time. Where one segment meets the next
// depends on both, so a segment is cut when the point after it comes, and
// the last one, with the end of the line, by end(). All it keeps is how the
// line ends so far, which is small: a copy of one is the line as it was,
// and can be carried on from there in another direction.
class StrokeLine {
public:
    StrokeLine(qreal halfWidth, quint32 color, Cap cap = Cap::Round, Join join = Join::Round, qreal scale = 0);
    void add(StrokeMesh &mesh, QPointF point);
    void end(StrokeMesh &mesh) const;

private:
    struct Pair {
        quint32 left = 0, right = 0;
    };
    Pair pairAt(StrokeMesh &mesh, QPointF center, QPointF normal) const;
    void corner(StrokeMesh &mesh, QPointF direction, QPointF normal);

    float m_halfWidth;
    quint32 m_color;
    Cap m_cap;
    Join m_join;
    // How much of an arc one triangle takes.
    qreal m_step;
    int m_count = 0;
    QPointF m_first, m_last;
    // The segment that ends at the last point, and the pair of vertices it
    // starts from.
    QPointF m_direction, m_normal;
    Pair m_start;
};

// --- The pen ---------------------------------------------------------------

// The line the pen is drawing, smoothed as smooth() smooths a stroke and
// cut into triangles as it is drawn. A point bends the curve no further back
// than the point before last, and the last two points can be taken back
// (see redraw()), so the line up to two points before that is cut once and
// kept, and each point only has the end of it cut again: a long line costs
// no more to carry on than a short one. It is cut as finely as it shows when
// a unit is `scale` screen pixels and no finer, never more finely than it
// will be once it is on the board, so a line drawn far out is no more
// triangles than the same line drawn close up.
class Pen {
public:
    explicit Pen(qreal halfWidth = 1, quint32 color = 0, qreal scale = 1);
    void add(QPointF point);
    // Takes back the last `back` points, two at most, and goes on through
    // `points` instead: the end of the line as the pen thinks again about
    // it (see cornerBefore()).
    void redraw(int back, const QVector<QPointF> &points);
    const QVector<QPointF> &points() const { return m_points; }
    const StrokeMesh &mesh() const { return m_mesh; }
    qsizetype triangles() const { return m_mesh.indices.size() / 3; }
    // How closely it follows the curve through its points, in units.
    qreal tolerance() const { return m_tolerance; }
    // Whether the two are cut alike: as wide, in one color, for one scale.
    bool cutAlike(const Pen &other) const {
        return m_halfWidth == other.m_halfWidth && m_color == other.m_color && m_scale == other.m_scale;
    }

private:
    void cut();

    qreal m_halfWidth;
    quint32 m_color;
    qreal m_scale, m_tolerance;
    QVector<QPointF> m_points;
    StrokeMesh m_mesh;
    // The line as far as it is settled: the curve between the points that
    // no point to come can bend or take back, of which `m_settled`
    // stretches are cut, and so much of the mesh.
    StrokeLine m_line;
    int m_settled = 0;
    qsizetype m_vertices = 0, m_indices = 0;
};

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
bool polylineIntersectsRect(const QVector<QPointF> &points, const QRectF &rect, qreal radius = 0);
bool shapeContains(Item::Shape shape, const QRectF &rect, QPointF p);

// --- Connectors -------------------------------------------------------------

// Where a line from the middle of the shape toward `target` leaves it.
QPointF shapeEdgeToward(Item::Shape shape, const QRectF &rect, QPointF target);

} // namespace Geo
