#include "geometry.h"

#include <QColor>
#include <QLineF>
#include <algorithm>
#include <cmath>

namespace Geo {

namespace {

inline qreal length(QPointF v) { return std::hypot(v.x(), v.y()); }
inline qreal dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
inline qreal cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }

// An arc is drawn as one triangle per this many radians: fine enough that a
// thick stroke's cap stays round when zoomed all the way in.
const qreal arcStep = M_PI / 10;

struct Builder {
    StrokeMesh &mesh;
    float halfWidth;
    quint32 color;

    quint32 vertex(QPointF center, QPointF normal, float side) {
        StrokeVertex v;
        v.x = float(center.x() + normal.x() * side * halfWidth);
        v.y = float(center.y() + normal.y() * side * halfWidth);
        v.nx = float(normal.x());
        v.ny = float(normal.y());
        v.halfWidth = halfWidth;
        v.side = side;
        v.r = quint8(color);
        v.g = quint8(color >> 8);
        v.b = quint8(color >> 16);
        v.a = quint8(color >> 24);
        mesh.vertices.append(v);
        return quint32(mesh.vertices.size() - 1);
    }

    void triangle(quint32 a, quint32 b, quint32 c) {
        mesh.indices.append(a);
        mesh.indices.append(b);
        mesh.indices.append(c);
    }

    // A wedge of a disc around `center`, from angle `from` sweeping `sweep`.
    void fan(QPointF center, qreal from, qreal sweep) {
        const int steps = qMax(1, int(std::ceil(std::abs(sweep) / arcStep)));
        const quint32 middle = vertex(center, QPointF(1, 0), 0);
        quint32 previous = vertex(center, QPointF(std::cos(from), std::sin(from)), 1);
        for (int i = 1; i <= steps; ++i) {
            const qreal angle = from + sweep * i / steps;
            const quint32 next = vertex(center, QPointF(std::cos(angle), std::sin(angle)), 1);
            triangle(middle, previous, next);
            previous = next;
        }
    }
};

} // namespace

QVector<QPointF> simplify(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
    if (count < 3 || tolerance <= 0)
        return points;
    QVector<bool> keep(count, false);
    keep[0] = keep[count - 1] = true;
    QVector<QPair<int, int>> stack{{0, count - 1}};
    while (!stack.isEmpty()) {
        const auto [first, last] = stack.takeLast();
        qreal furthest = 0;
        int index = -1;
        for (int i = first + 1; i < last; ++i) {
            const qreal distance = distanceToSegment(points[i], points[first], points[last]);
            if (distance > furthest) {
                furthest = distance;
                index = i;
            }
        }
        if (index >= 0 && furthest > tolerance) {
            keep[index] = true;
            stack.append({first, index});
            stack.append({index, last});
        }
    }
    QVector<QPointF> kept;
    for (int i = 0; i < count; ++i)
        if (keep[i])
            kept.append(points[i]);
    return kept;
}

QVector<QPointF> smooth(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
    if (count < 3)
        return points;
    tolerance = qMax(tolerance, 1e-3);
    QVector<QPointF> curve;
    curve.reserve(count * 4);
    curve.append(points[0]);
    for (int i = 0; i + 1 < count; ++i) {
        const QPointF p0 = points[qMax(i - 1, 0)], p1 = points[i], p2 = points[i + 1], p3 = points[qMin(i + 2, count - 1)];
        // Centripetal: the knots are spaced by the square root of the
        // distance between points.
        const qreal d1 = std::sqrt(length(p1 - p0)), d2 = std::sqrt(length(p2 - p1)), d3 = std::sqrt(length(p3 - p2));
        if (d2 < 1e-6)
            continue;
        // The cubic Bezier between p1 and p2 that the spline is there.
        const QPointF b1 = d1 < 1e-6 ? p1 + (p2 - p1) / 3
                                     : (d1 * d1 * p2 - d2 * d2 * p0 + (2 * d1 * d1 + 3 * d1 * d2 + d2 * d2) * p1) /
                                           (3 * d1 * (d1 + d2));
        const QPointF b2 = d3 < 1e-6 ? p2 - (p2 - p1) / 3
                                     : (d3 * d3 * p1 - d2 * d2 * p3 + (2 * d3 * d3 + 3 * d3 * d2 + d2 * d2) * p2) /
                                           (3 * d3 * (d3 + d2));
        // Wang's bound on how many segments keep a cubic within tolerance.
        const qreal bend = qMax(length(p1 - 2 * b1 + b2), length(b1 - 2 * b2 + p2));
        const int steps = qBound(1, int(std::ceil(std::sqrt(0.75 * bend / tolerance))), 32);
        for (int step = 1; step <= steps; ++step) {
            const qreal t = qreal(step) / steps, u = 1 - t;
            curve.append(u * u * u * p1 + 3 * u * u * t * b1 + 3 * u * t * t * b2 + t * t * t * p2);
        }
    }
    return curve;
}

QRectF boundsOf(const QVector<QPointF> &points) {
    if (points.isEmpty())
        return {};
    qreal left = points[0].x(), right = left, top = points[0].y(), bottom = top;
    for (const QPointF &p : points) {
        left = qMin(left, p.x());
        right = qMax(right, p.x());
        top = qMin(top, p.y());
        bottom = qMax(bottom, p.y());
    }
    return QRectF(left, top, right - left, bottom - top);
}

quint32 packColor(const QColor &color, qreal opacity) {
    const qreal alpha = color.alphaF() * opacity;
    auto byte = [alpha](qreal channel) { return quint32(qBound(0, int(std::lround(channel * alpha * 255)), 255)); };
    return byte(color.redF()) | byte(color.greenF()) << 8 | byte(color.blueF()) << 16 |
           quint32(qBound(0, int(std::lround(alpha * 255)), 255)) << 24;
}

void strokePolyline(StrokeMesh &mesh, const QVector<QPointF> &source, QPointF offset, qreal halfWidth, quint32 color,
                    Cap cap, Join join) {
    // Points on top of each other have no direction to draw along.
    QVector<QPointF> points;
    points.reserve(source.size());
    for (const QPointF &p : source) {
        const QPointF moved = p + offset;
        if (points.isEmpty() || length(moved - points.last()) > 1e-4)
            points.append(moved);
    }
    if (points.isEmpty())
        return;

    Builder build{mesh, float(halfWidth), color};
    const int count = points.size();
    if (count == 1) {
        // A tap of the pen is a dot.
        build.fan(points[0], 0, 2 * M_PI);
        return;
    }

    const int segments = count - 1;
    QVector<QPointF> directions(segments), normals(segments);
    for (int i = 0; i < segments; ++i) {
        const QPointF delta = points[i + 1] - points[i];
        directions[i] = delta / length(delta);
        normals[i] = QPointF(-directions[i].y(), directions[i].x());
    }

    // Each segment is a quad between a pair of vertices at its start and a
    // pair at its end. Where the path bends gently the two segments share a
    // pair, mitered; where it turns sharply each keeps its own and a wedge
    // fills the outside of the corner.
    struct Pair {
        quint32 left, right;
    };
    auto pairAt = [&build](QPointF center, QPointF normal) {
        return Pair{build.vertex(center, normal, 1), build.vertex(center, normal, -1)};
    };
    Pair start = pairAt(points[0], normals[0]);
    for (int i = 0; i < segments; ++i) {
        Pair end, nextStart{};
        if (i + 1 < segments) {
            const qreal bend = dot(directions[i], directions[i + 1]);
            // A miter is as long as the corner is sharp, so past a point it
            // is cut off, as a bevel is from the start.
            const bool mitered = join == Join::Miter ? bend > -0.9 : join == Join::Bevel ? bend > 0.5 : bend > 0.9;
            if (mitered) {
                const QPointF sum = normals[i] + normals[i + 1];
                const QPointF miter = sum / length(sum);
                // The miter reaches further than the stroke is wide by the
                // inverse cosine of half the turn.
                end = nextStart = pairAt(points[i + 1], miter / dot(miter, normals[i]));
            } else {
                end = pairAt(points[i + 1], normals[i]);
                nextStart = pairAt(points[i + 1], normals[i + 1]);
                // The outside of the corner is away from the turn.
                const bool leftOutside = cross(directions[i], directions[i + 1]) <= 0;
                if (join == Join::Round) {
                    const qreal outside = leftOutside ? 1 : -1;
                    const QPointF from = outside * normals[i], to = outside * normals[i + 1];
                    const qreal sweep = std::atan2(cross(from, to), dot(from, to));
                    build.fan(points[i + 1], std::atan2(from.y(), from.x()), sweep);
                } else {
                    build.triangle(build.vertex(points[i + 1], QPointF(1, 0), 0), leftOutside ? end.left : end.right,
                                   leftOutside ? nextStart.left : nextStart.right);
                }
            }
        } else {
            end = pairAt(points[i + 1], normals[i]);
        }
        build.triangle(start.left, start.right, end.left);
        build.triangle(start.right, end.right, end.left);
        start = nextStart;
    }

    if (cap == Cap::Round) {
        // Half a disc behind the first point and ahead of the last.
        const QPointF first = normals[0], last = -normals[segments - 1];
        build.fan(points[0], std::atan2(first.y(), first.x()), M_PI);
        build.fan(points[count - 1], std::atan2(last.y(), last.x()), M_PI);
    }
}

void ShapeMesh::add(const QRectF &rect, Outline outline, qreal radius, qreal borderWidth, quint32 fill, quint32 border) {
    add(rect.center(), QSizeF(rect.width() / 2, rect.height() / 2), QPointF(1, 0), outline, radius, borderWidth, fill,
        border);
}

void ShapeMesh::add(QPointF center, QSizeF halfSize, QPointF axis, Outline outline, qreal radius, qreal borderWidth,
                    quint32 fill, quint32 border) {
    const QPointF across(-axis.y(), axis.x());
    const qreal w = halfSize.width(), h = halfSize.height();
    for (const auto &[sx, sy] : {std::pair{-1, -1}, std::pair{1, -1}, std::pair{-1, 1}, std::pair{1, 1}}) {
        const QPointF corner = center + axis * (sx * w) + across * (sy * h);
        ShapeVertex v;
        v.x = float(corner.x());
        v.y = float(corner.y());
        v.localX = float(sx * w);
        v.localY = float(sy * h);
        v.kind = float(int(outline));
        // A corner cannot be rounder than the shape is small.
        v.radius = float(qMin(radius, qMin(w, h)));
        v.borderWidth = float(borderWidth);
        v.unused = 0;
        v.axisX = float(axis.x());
        v.axisY = float(axis.y());
        for (int i = 0; i < 4; ++i) {
            v.fill[i] = quint8(fill >> (8 * i));
            v.border[i] = quint8(border >> (8 * i));
        }
        vertices.append(v);
    }
}

QVector<QPointF> frameEdge(QSizeF size, qreal gapFrom, qreal gapTo) {
    const qreal left = 0.5, top = 0.5, right = size.width() - 0.5, bottom = size.height() - 0.5;
    const bool gap = gapTo > gapFrom;
    const qreal from = gap ? qMin(gapTo, right) : size.width() / 2;
    const qreal to = gap ? qMax(gapFrom, left) : size.width() / 2;
    return {{from, top}, {right, top}, {right, bottom}, {left, bottom}, {left, top}, {to, top}};
}

QVector<QPointF> connectorPath(const Item &connector, qreal tolerance) {
    const QPointF a = connector.p1, b = connector.p2;
    QVector<QPointF> path{a};
    if (connector.route != Item::Route::Straight) {
        // Out from each end toward the other as far as halfway, sideways or
        // up and down: the two places an elbow turns at, and what a curve
        // is pulled toward.
        const QPointF middle = (a + b) / 2;
        const QPointF out = connector.sideways ? QPointF(middle.x(), a.y()) : QPointF(a.x(), middle.y());
        const QPointF in = connector.sideways ? QPointF(middle.x(), b.y()) : QPointF(b.x(), middle.y());
        if (connector.route == Item::Route::Elbow) {
            path << out << in;
        } else {
            // Wang's bound on how many edges keep a cubic within tolerance.
            const qreal bend = qMax(length(a - 2 * out + in), length(out - 2 * in + b));
            const int steps = qBound(1, int(std::ceil(std::sqrt(0.75 * bend / qMax(tolerance, 1e-3)))), 64);
            for (int step = 1; step < steps; ++step) {
                const qreal t = qreal(step) / steps, u = 1 - t;
                path << u * u * u * a + 3 * u * u * t * out + 3 * u * t * t * in + t * t * t * b;
            }
        }
    }
    path << b;
    // Ends in line with each other need no corners.
    QVector<QPointF> kept;
    for (const QPointF &point : std::as_const(path))
        if (kept.isEmpty() || length(point - kept.last()) > 1e-6)
            kept.append(point);
    // Nor does a corner that is not a turn.
    for (int i = 1; i + 1 < kept.size() && connector.route == Item::Route::Elbow;) {
        if (std::abs(cross(kept[i] - kept[i - 1], kept[i + 1] - kept[i])) < 1e-9)
            kept.remove(i);
        else
            ++i;
    }
    if (kept.size() < 2)
        kept = {a, b};
    return kept;
}

qreal pathLength(const QVector<QPointF> &path) {
    qreal total = 0;
    for (int i = 0; i + 1 < path.size(); ++i)
        total += length(path[i + 1] - path[i]);
    return total;
}

PathPoint alongPath(const QVector<QPointF> &path, qreal distance) {
    PathPoint found{path.isEmpty() ? QPointF() : path.first(), QPointF(1, 0)};
    for (int i = 0; i + 1 < path.size(); ++i) {
        const QPointF step = path[i + 1] - path[i];
        const qreal span = length(step);
        if (span < 1e-9)
            continue;
        found.heading = step / span;
        if (distance <= span || i + 2 == path.size()) {
            found.at = path[i] + found.heading * qBound(0.0, distance, span);
            return found;
        }
        distance -= span;
    }
    return found;
}

namespace {

// The part of a path between two distances along it.
QVector<QPointF> partOf(const QVector<QPointF> &path, qreal from, qreal to) {
    QVector<QPointF> part{alongPath(path, from).at};
    qreal walked = 0;
    for (int i = 0; i + 1 < path.size(); ++i) {
        walked += length(path[i + 1] - path[i]);
        if (walked > from + 1e-9 && walked < to - 1e-9)
            part << path[i + 1];
    }
    part << alongPath(path, to).at;
    return part;
}

} // namespace

ConnectorArt connectorArt(const Item &connector) {
    ConnectorArt art;
    const QVector<QPointF> path = connectorPath(connector);
    const qreal total = pathLength(path);
    if (total < 1e-3)
        return art;
    art.middle = alongPath(path, total / 2);
    const qreal width = connector.width;
    // An end grows with the line, and two of them never outgrow a short one.
    const qreal reach = qMin(6 + 2 * width, total * 0.4), spread = reach * 0.45;

    // What is at an end, and how far short of the end the line stops for it:
    // inside what is filled, at the back of what is hollow.
    auto end = [&](Item::Head kind, QPointF tip, QPointF out) {
        const QPointF across(-out.y(), out.x());
        auto shape = [&](Outline outline, qreal along, qreal half, bool hollow) {
            art.heads.append({tip - out * (along / 2), QSizeF(along / 2, half), out, outline, hollow});
        };
        switch (kind) {
        case Item::Head::None:
            return 0.0;
        case Item::Head::Arrow:
            shape(Outline::Arrowhead, reach, spread, false);
            return reach * 0.8;
        case Item::Head::Triangle:
            shape(Outline::Arrowhead, reach, spread, true);
            return reach - width / 2;
        case Item::Head::Open: {
            // Two strokes meeting at the tip. Their corner is as long as it
            // is sharp, so they meet that much short of the end.
            const qreal back = (width / 2) / std::sin(std::atan2(spread, reach));
            const QPointF point = tip - out * qMin(back, reach / 2);
            art.lines.append({tip - out * reach + across * spread, point, tip - out * reach - across * spread});
            return qMin(back, reach / 2);
        }
        case Item::Head::Dot:
            shape(Outline::Ellipse, spread * 2, spread, false);
            return spread;
        case Item::Head::Circle:
            shape(Outline::Ellipse, spread * 2, spread, true);
            return spread * 2 - width / 2;
        case Item::Head::Diamond:
            shape(Outline::Diamond, reach * 1.4, spread * 0.9, false);
            return reach * 0.7;
        case Item::Head::OpenDiamond:
            shape(Outline::Diamond, reach * 1.4, spread * 0.9, true);
            return reach * 1.4 - width;
        case Item::Head::Bar: {
            // Across the end, and no further out than the end is.
            const QPointF middle = tip - out * (width / 2);
            art.lines.append({middle + across * (spread + 1), middle - across * (spread + 1)});
            return 0.0;
        }
        }
        return 0.0;
    };
    const qreal from = end(connector.start, path.first(), -alongPath(path, 0).heading);
    const qreal short_ = end(connector.end, path.last(), alongPath(path, total).heading);
    const qreal to = total - short_;
    if (to - from < 1e-3)
        return art;

    if (connector.dash == Item::Dash::Solid) {
        art.lines.append(partOf(path, from, to));
        return art;
    }
    // Dashes are a few widths long with less between them; dots are as
    // long as the line is wide.
    const bool dots = connector.dash == Item::Dash::Dotted;
    const qreal on = dots ? qMax(width, 1.5) : qMax(6.0, 4 * width), off = dots ? qMax(2 * width, 3.0) : qMax(4.0, 3 * width);
    if ((to - from) / (on + off) > 4000) {
        // Long enough to be drawn with more dashes than anyone would count.
        art.lines.append(partOf(path, from, to));
        return art;
    }
    for (qreal at = from; at < to - 1e-6; at += on + off)
        art.lines.append(partOf(path, at, qMin(at + on, to)));
    return art;
}

qreal distanceToSegment(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a;
    const qreal squared = dot(ab, ab);
    if (squared < 1e-12)
        return length(p - a);
    const qreal t = qBound(0.0, dot(p - a, ab) / squared, 1.0);
    return length(p - (a + ab * t));
}

qreal distanceToPolyline(const QVector<QPointF> &points, QPointF p) {
    if (points.isEmpty())
        return std::numeric_limits<qreal>::max();
    if (points.size() == 1)
        return length(p - points[0]);
    qreal nearest = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < points.size(); ++i)
        nearest = qMin(nearest, distanceToSegment(p, points[i], points[i + 1]));
    return nearest;
}

bool segmentIntersectsRect(QPointF a, QPointF b, const QRectF &rect) {
    if (rect.contains(a) || rect.contains(b))
        return true;
    const QLineF line(a, b);
    const QPointF corners[4] = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
    for (int i = 0; i < 4; ++i)
        if (line.intersects(QLineF(corners[i], corners[(i + 1) % 4]), nullptr) == QLineF::BoundedIntersection)
            return true;
    return false;
}

bool polylineIntersectsRect(const QVector<QPointF> &points, const QRectF &rect) {
    if (points.size() == 1)
        return rect.contains(points[0]);
    for (int i = 0; i + 1 < points.size(); ++i)
        if (segmentIntersectsRect(points[i], points[i + 1], rect))
            return true;
    return false;
}

bool shapeContains(Item::Shape shape, const QRectF &rect, QPointF p) {
    if (!rect.contains(p))
        return false;
    if (shape == Item::Shape::Rect || rect.width() <= 0 || rect.height() <= 0)
        return true;
    if (shape == Item::Shape::Ellipse || shape == Item::Shape::Diamond) {
        const QPointF v = p - rect.center();
        const qreal x = v.x() / (rect.width() / 2), y = v.y() / (rect.height() / 2);
        return shape == Item::Shape::Ellipse ? x * x + y * y <= 1 : std::abs(x) + std::abs(y) <= 1;
    }
    return polygonContains(figure(shape, rect.size(), 0.25).edge, p - rect.topLeft());
}

QPointF shapeEdgeToward(Item::Shape shape, const QRectF &rect, QPointF target) {
    const QPointF center = rect.center(), v = target - center;
    const qreal rx = rect.width() / 2, ry = rect.height() / 2;
    if (length(v) < 1e-6 || rx <= 0 || ry <= 0)
        return center;
    if (shape != Item::Shape::Rect && shape != Item::Shape::Ellipse && shape != Item::Shape::Diamond) {
        // The furthest place the way to the target crosses the outline:
        // where it leaves the shape for good, whatever notches it has.
        const QVector<QPointF> edge = figure(shape, rect.size(), 0.25).edge;
        const QPointF from = center - rect.topLeft();
        qreal furthest = -1;
        for (int i = 0; i < edge.size(); ++i) {
            const QPointF a = edge[i], side = edge[(i + 1) % edge.size()] - a;
            const qreal across = cross(v, side);
            if (std::abs(across) < 1e-12)
                continue;
            const qreal along = cross(a - from, side) / across, on = cross(a - from, v) / across;
            if (along > 0 && on >= -1e-9 && on <= 1 + 1e-9)
                furthest = qMax(furthest, along);
        }
        if (furthest > 0)
            return center + v * furthest;
        // The middle of its box is not inside every shape, or is right on
        // its edge (a right triangle's is). Then it is met at the nearest
        // place on its outline.
        QPointF nearest = from;
        qreal least = -1;
        for (int i = 0; i < edge.size(); ++i) {
            const QPointF a = edge[i], side = edge[(i + 1) % edge.size()] - a;
            const qreal span = dot(side, side);
            const QPointF on = a + side * (span > 0 ? qBound(0.0, dot(from - a, side) / span, 1.0) : 0.0);
            const qreal distance = length(on - from);
            if (least < 0 || distance < least) {
                least = distance;
                nearest = on;
            }
        }
        return rect.topLeft() + nearest;
    }
    const qreal x = std::abs(v.x()) / rx, y = std::abs(v.y()) / ry;
    // How far along v the outline is, for each outline's own equation.
    const qreal reach = shape == Item::Shape::Ellipse   ? 1 / std::sqrt(x * x + y * y)
                        : shape == Item::Shape::Diamond ? 1 / (x + y)
                                                        : 1 / qMax(x, y);
    return center + v * reach;
}

// --- Shapes ----------------------------------------------------------------

namespace {

// An arc of an ellipse, from one angle to another (clockwise on screen, as
// angles are when y points down), cut finely enough to stay within tolerance.
void arc(QVector<QPointF> &path, QPointF center, qreal rx, qreal ry, qreal from, qreal to, qreal tolerance) {
    const qreal radius = qMax(qMax(rx, ry), 1e-6);
    const qreal step = 2 * std::acos(qBound(0.0, 1 - tolerance / radius, 1.0));
    const int steps = qBound(2, int(std::ceil(std::abs(to - from) / qMax(step, 1e-3))), 96);
    for (int i = 0; i <= steps; ++i) {
        const qreal angle = from + (to - from) * i / steps;
        path.append(center + QPointF(rx * std::cos(angle), ry * std::sin(angle)));
    }
}

void roundedBox(QVector<QPointF> &path, const QRectF &box, qreal radius, qreal tolerance) {
    const qreal r = qBound(0.0, radius, qMin(box.width(), box.height()) / 2);
    if (r < 1e-6) {
        path << box.topLeft() << box.topRight() << box.bottomRight() << box.bottomLeft();
        return;
    }
    arc(path, box.topLeft() + QPointF(r, r), r, r, M_PI, 1.5 * M_PI, tolerance);
    arc(path, box.topRight() + QPointF(-r, r), r, r, 1.5 * M_PI, 2 * M_PI, tolerance);
    arc(path, box.bottomRight() + QPointF(-r, -r), r, r, 0, 0.5 * M_PI, tolerance);
    arc(path, box.bottomLeft() + QPointF(r, -r), r, r, 0.5 * M_PI, M_PI, tolerance);
}

// A polygon with its corners on a circle, stretched to fill the box.
// `inner` below 1 pulls every other corner in, which makes a star.
QVector<QPointF> onCircle(int corners, qreal inner, QSizeF size) {
    QVector<QPointF> points;
    qreal left = 1, right = -1, top = 1, bottom = -1;
    for (int i = 0; i < corners; ++i) {
        const qreal angle = -M_PI / 2 + 2 * M_PI * i / corners, reach = i % 2 ? inner : 1;
        const QPointF point(reach * std::cos(angle), reach * std::sin(angle));
        points.append(point);
        left = qMin(left, point.x());
        right = qMax(right, point.x());
        top = qMin(top, point.y());
        bottom = qMax(bottom, point.y());
    }
    for (QPointF &point : points)
        point = QPointF((point.x() - left) / (right - left) * size.width(),
                        (point.y() - top) / (bottom - top) * size.height());
    return points;
}

// The head of a block arrow is as long as it can be without being most of
// the arrow, and no blunter than the arrow is tall.
qreal headOf(qreal w, qreal h, qreal most) { return qMin(w * most, h * 0.75); }
// A speech bubble's tail, a rounded box's corners, a cylinder's ends.
qreal tailOf(qreal h) { return qBound(qMin(6.0, h / 3), h * 0.25, 28.0); }
qreal capOf(qreal w, qreal h) { return qMin(h / 6, w / 4); }

void tidy(QVector<QPointF> &path) {
    // Neighbors on top of each other, the last and the first among them.
    QVector<QPointF> kept;
    for (const QPointF &point : std::as_const(path))
        if (kept.isEmpty() || length(point - kept.last()) > 1e-6)
            kept.append(point);
    while (kept.size() > 1 && length(kept.first() - kept.last()) <= 1e-6)
        kept.removeLast();
    path = kept;
}

} // namespace

bool isQuad(Item::Shape shape) {
    return shape == Item::Shape::Rect || shape == Item::Shape::Rounded || shape == Item::Shape::Pill ||
           shape == Item::Shape::Ellipse || shape == Item::Shape::Diamond;
}

Outline outlineOf(Item::Shape shape) {
    return shape == Item::Shape::Ellipse ? Outline::Ellipse : shape == Item::Shape::Diamond ? Outline::Diamond : Outline::Rect;
}

qreal cornerRadius(Item::Shape shape, QSizeF size, qreal desktopRadius) {
    const qreal least = qMin(size.width(), size.height());
    if (shape == Item::Shape::Pill)
        return least / 2;
    if (shape == Item::Shape::Rounded)
        return qMax(desktopRadius, qMin(16.0, least / 4));
    return shape == Item::Shape::Rect ? desktopRadius : 0;
}

Figure figure(Item::Shape shape, QSizeF size, qreal tolerance) {
    const qreal w = qMax(size.width(), 1e-3), h = qMax(size.height(), 1e-3);
    const QRectF box(0, 0, w, h);
    Figure f;
    QVector<QPointF> &e = f.edge;
    switch (shape) {
    case Item::Shape::Rect:
        e << QPointF(0, 0) << QPointF(w, 0) << QPointF(w, h) << QPointF(0, h);
        break;
    case Item::Shape::Rounded:
    case Item::Shape::Pill:
        roundedBox(e, box, cornerRadius(shape, size, 0), tolerance);
        break;
    case Item::Shape::Ellipse:
        arc(e, box.center(), w / 2, h / 2, 0, 2 * M_PI, tolerance);
        break;
    case Item::Shape::Diamond:
        e << QPointF(w / 2, 0) << QPointF(w, h / 2) << QPointF(w / 2, h) << QPointF(0, h / 2);
        break;
    case Item::Shape::Triangle:
        e << QPointF(w / 2, 0) << QPointF(w, h) << QPointF(0, h);
        break;
    case Item::Shape::RightTriangle:
        e << QPointF(0, 0) << QPointF(w, h) << QPointF(0, h);
        break;
    case Item::Shape::Parallelogram: {
        const qreal lean = qMin(w * 0.25, h * 0.6);
        e << QPointF(lean, 0) << QPointF(w, 0) << QPointF(w - lean, h) << QPointF(0, h);
        break;
    }
    case Item::Shape::Trapezoid: {
        const qreal lean = qMin(w * 0.2, h * 0.5);
        e << QPointF(lean, 0) << QPointF(w - lean, 0) << QPointF(w, h) << QPointF(0, h);
        break;
    }
    case Item::Shape::Pentagon:
        e = onCircle(5, 1, size);
        break;
    case Item::Shape::Hexagon: {
        // Sides at sixty degrees, as a regular one has, when there is room.
        const qreal point = qMin(w * 0.25, h * 0.2887);
        e << QPointF(point, 0) << QPointF(w - point, 0) << QPointF(w, h / 2) << QPointF(w - point, h)
          << QPointF(point, h) << QPointF(0, h / 2);
        break;
    }
    case Item::Shape::Octagon: {
        const qreal cut = qMin(w, h) * 0.2929;
        e << QPointF(cut, 0) << QPointF(w - cut, 0) << QPointF(w, cut) << QPointF(w, h - cut) << QPointF(w - cut, h)
          << QPointF(cut, h) << QPointF(0, h - cut) << QPointF(0, cut);
        break;
    }
    case Item::Shape::Star:
        e = onCircle(10, 0.382, size);
        break;
    case Item::Shape::Cross: {
        const qreal arm = qMin(w, h) / 3, l = (w - arm) / 2, r = (w + arm) / 2, t = (h - arm) / 2, b = (h + arm) / 2;
        e << QPointF(l, 0) << QPointF(r, 0) << QPointF(r, t) << QPointF(w, t) << QPointF(w, b) << QPointF(r, b)
          << QPointF(r, h) << QPointF(l, h) << QPointF(l, b) << QPointF(0, b) << QPointF(0, t) << QPointF(l, t);
        break;
    }
    case Item::Shape::Arrow:
    case Item::Shape::ArrowLeft: {
        const qreal head = headOf(w, h, 0.5);
        e << QPointF(0, h * 0.25) << QPointF(w - head, h * 0.25) << QPointF(w - head, 0) << QPointF(w, h / 2)
          << QPointF(w - head, h) << QPointF(w - head, h * 0.75) << QPointF(0, h * 0.75);
        if (shape == Item::Shape::ArrowLeft) {
            // The same arrow in a mirror, turned to go around the same way.
            for (QPointF &point : e)
                point.setX(w - point.x());
            std::reverse(e.begin(), e.end());
        }
        break;
    }
    case Item::Shape::DoubleArrow: {
        const qreal head = headOf(w, h, 0.3);
        e << QPointF(0, h / 2) << QPointF(head, 0) << QPointF(head, h * 0.25) << QPointF(w - head, h * 0.25)
          << QPointF(w - head, 0) << QPointF(w, h / 2) << QPointF(w - head, h) << QPointF(w - head, h * 0.75)
          << QPointF(head, h * 0.75) << QPointF(head, h);
        break;
    }
    case Item::Shape::Bubble: {
        // A rounded box with a tail under its left end.
        const qreal body = h - tailOf(h), r = qMin(12.0, qMin(w, body) / 4);
        const qreal from = qMin(qMax(r, w * 0.16), w / 2), to = qMin(from + qMax(w * 0.16, 12.0), w - r);
        arc(e, QPointF(r, r), r, r, M_PI, 1.5 * M_PI, tolerance);
        arc(e, QPointF(w - r, r), r, r, 1.5 * M_PI, 2 * M_PI, tolerance);
        arc(e, QPointF(w - r, body - r), r, r, 0, 0.5 * M_PI, tolerance);
        e << QPointF(to, body) << QPointF(qMax(0.0, from - qMin(6.0, w * 0.04)), h) << QPointF(from, body);
        arc(e, QPointF(r, body - r), r, r, 0.5 * M_PI, M_PI, tolerance);
        break;
    }
    case Item::Shape::Cylinder: {
        // Seen a little from above: the far edge of its top, its sides, the
        // near edge of its bottom; and inside, the near edge of its top.
        const qreal cap = capOf(w, h);
        arc(e, QPointF(w / 2, cap), w / 2, cap, M_PI, 2 * M_PI, tolerance);
        arc(e, QPointF(w / 2, h - cap), w / 2, cap, 0, M_PI, tolerance);
        arc(f.line, QPointF(w / 2, cap), w / 2, cap, M_PI, 0, tolerance);
        break;
    }
    }
    tidy(e);
    return f;
}

Figure simplified(const Figure &original, qreal tolerance) {
    Figure f;
    // Around and back to where it starts, so that the start is kept.
    QVector<QPointF> loop = original.edge;
    if (!loop.isEmpty())
        loop.append(loop.first());
    f.edge = simplify(loop, tolerance);
    if (!f.edge.isEmpty())
        f.edge.removeLast();
    if (f.edge.size() < 3)
        f.edge = original.edge;
    f.line = simplify(original.line, tolerance);
    return f;
}

QRectF textBox(Item::Shape shape, QSizeF size) {
    const qreal w = size.width(), h = size.height(), pad = 10;
    auto within = [&](qreal left, qreal top, qreal right, qreal bottom) {
        return QRectF(QPointF(left, top), QPointF(qMax(left + 1, right), qMax(top, bottom)));
    };
    switch (shape) {
    case Item::Shape::Rect:
    case Item::Shape::Rounded:
        return within(pad, 0, w - pad, h);
    case Item::Shape::Pill: {
        const qreal inset = qMax(pad, qMin(w, h) * 0.3);
        return within(inset, 0, w - inset, h);
    }
    case Item::Shape::Ellipse:
    case Item::Shape::Diamond: {
        // An ellipse and a diamond have less room across than their box.
        const qreal inset = qMax(pad, w * 0.14);
        return within(inset, 0, w - inset, h);
    }
    case Item::Shape::Triangle:
        return within(w * 0.26, h * 0.42, w * 0.74, h);
    case Item::Shape::RightTriangle:
        return within(pad, h * 0.5, w * 0.56, h);
    case Item::Shape::Parallelogram: {
        const qreal lean = qMin(w * 0.25, h * 0.6);
        return within(lean + 4, 0, w - lean - 4, h);
    }
    case Item::Shape::Trapezoid: {
        const qreal lean = qMin(w * 0.2, h * 0.5);
        return within(lean + 4, 0, w - lean - 4, h);
    }
    case Item::Shape::Pentagon:
        return within(w * 0.18, h * 0.3, w * 0.82, h);
    case Item::Shape::Hexagon: {
        const qreal point = qMin(w * 0.25, h * 0.2887);
        return within(point + 2, 0, w - point - 2, h);
    }
    case Item::Shape::Octagon:
        return within(w * 0.12, 0, w * 0.88, h);
    case Item::Shape::Star:
        return within(w * 0.3, h * 0.36, w * 0.7, h * 0.74);
    case Item::Shape::Cross:
        return within(w * 0.06, h / 3, w * 0.94, h * 2 / 3);
    case Item::Shape::Arrow:
        return within(pad, h * 0.25, w - headOf(w, h, 0.5) * 0.5, h * 0.75);
    case Item::Shape::ArrowLeft:
        return within(headOf(w, h, 0.5) * 0.5, h * 0.25, w - pad, h * 0.75);
    case Item::Shape::DoubleArrow: {
        const qreal head = headOf(w, h, 0.3) * 0.6;
        return within(head, h * 0.25, w - head, h * 0.75);
    }
    case Item::Shape::Bubble:
        return within(pad, 0, w - pad, h - tailOf(h));
    case Item::Shape::Cylinder: {
        const qreal cap = capOf(w, h);
        return within(pad, cap * 2, w - pad, h - cap * 0.5);
    }
    }
    return QRectF(0, 0, w, h);
}

bool polygonContains(const QVector<QPointF> &polygon, QPointF p) {
    // Crossings of a line from the point off to the right: an odd number
    // of them is inside.
    bool inside = false;
    for (int i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const QPointF a = polygon[i], b = polygon[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

QVector<quint32> triangulate(const QVector<QPointF> &polygon) {
    // Ears are clipped off one at a time: a corner that turns the way the
    // polygon goes around, with no other corner inside its triangle.
    QVector<quint32> triangles;
    const int count = polygon.size();
    if (count < 3)
        return triangles;
    QVector<int> ring(count);
    qreal area = 0;
    for (int i = 0; i < count; ++i) {
        ring[i] = i;
        area += cross(polygon[i], polygon[(i + 1) % count]);
    }
    if (area < 0)
        std::reverse(ring.begin(), ring.end());
    auto turn = [&](int a, int b, int c) { return cross(polygon[b] - polygon[a], polygon[c] - polygon[b]); };
    auto holds = [&](int a, int b, int c, int p) {
        const qreal slack = 1e-9;
        return turn(a, b, p) > slack && turn(b, c, p) > slack && turn(c, a, p) > slack;
    };
    triangles.reserve((count - 2) * 3);
    while (ring.size() > 3) {
        const int size = ring.size();
        int ear = -1, flat = -1;
        for (int i = 0; i < size && ear < 0; ++i) {
            const int a = ring[(i + size - 1) % size], b = ring[i], c = ring[(i + 1) % size];
            const qreal bend = turn(a, b, c);
            if (bend <= 1e-12) {
                // On a straight line between its neighbors, it is no corner.
                if (bend >= -1e-12 && flat < 0)
                    flat = i;
                continue;
            }
            bool empty = true;
            for (int k = 0; k < size && empty; ++k) {
                const int p = ring[k];
                empty = p == a || p == b || p == c || !holds(a, b, c, p);
            }
            if (empty)
                ear = i;
        }
        if (ear >= 0) {
            triangles << quint32(ring[(ear + size - 1) % size]) << quint32(ring[ear]) << quint32(ring[(ear + 1) % size]);
            ring.remove(ear);
        } else if (flat >= 0) {
            ring.remove(flat);
        } else {
            // A path that crosses itself has no ears. Something is drawn
            // of it all the same.
            for (int i = 1; i + 1 < size; ++i)
                triangles << quint32(ring[0]) << quint32(ring[i]) << quint32(ring[i + 1]);
            return triangles;
        }
    }
    triangles << quint32(ring[0]) << quint32(ring[1]) << quint32(ring[2]);
    return triangles;
}

void strokeFigure(StrokeMesh &mesh, const Figure &figure, QPointF offset, quint32 fill, quint32 border,
                  qreal borderWidth) {
    const QVector<QPointF> &edge = figure.edge;
    if (edge.size() < 3)
        return;
    // The inside, flat: its corners are in the middle of nothing as far as
    // the stroke shader goes, which leaves them where they are and solid.
    // Its edge is under the border, which is what is soft.
    Builder inside{mesh, 1e4f, fill};
    const quint32 first = quint32(mesh.vertices.size());
    for (const QPointF &point : edge)
        inside.vertex(point + offset, QPointF(1, 0), 0);
    const QVector<quint32> triangles = triangulate(edge);
    for (quint32 index : triangles)
        mesh.indices.append(first + index);

    // The border, around from the middle of one edge and back to it, so
    // that its two ends meet square on a straight run.
    QVector<QPointF> around;
    around.reserve(edge.size() + 2);
    around.append((edge[0] + edge[1]) / 2);
    for (int i = 1; i < edge.size(); ++i)
        around.append(edge[i]);
    around.append(edge[0]);
    around.append(around.first());
    strokePolyline(mesh, around, offset, borderWidth / 2, border, Cap::Butt, Join::Miter);
    if (figure.line.size() > 1)
        strokePolyline(mesh, figure.line, offset, borderWidth / 2, border, Cap::Butt, Join::Miter);
}

} // namespace Geo
