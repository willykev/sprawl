#include "geometry.h"

#include <QColor>
#include <QLineF>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Geo {

namespace {

inline qreal length(QPointF v) { return std::hypot(v.x(), v.y()); }
inline qreal dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
inline qreal cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }

// An arc is drawn as one triangle per this many radians: fine enough that a
// thick stroke's cap stays round when zoomed all the way in.
const qreal arcStep = M_PI / 10;
// How far a line cut for one size on screen strays from true, at most, in
// screen pixels: a quarter of one, which its soft edge hides.
const qreal pixelTolerance = 0.25;

// How much of an arc one triangle may take, for a line with this half width
// seen at `scale` screen pixels to the unit: as much as keeps its rim within
// a quarter of a pixel of round, and never less than arcStep. The shader
// widens a line to at least a pixel, and half a pixel more for its soft
// edge, so a rim is never nearer the middle than a pixel; and that near, a
// whole disc is round enough in five triangles.
qreal arcStepFor(qreal halfWidth, qreal scale) {
    if (scale <= 0)
        return arcStep;
    const qreal rim = qMax(halfWidth * scale, 0.5) + 0.5;
    return qMax(arcStep, 2 * std::acos(1 - pixelTolerance / rim));
}

struct Builder {
    StrokeMesh &mesh;
    float halfWidth;
    quint32 color;
    // How much of an arc one triangle takes.
    qreal step = arcStep;

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
        const int steps = qMax(1, int(std::ceil(std::abs(sweep) / step)));
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

// Which of the points a path needs (see simplify()), of three or more.
QVector<bool> needed(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
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
    return keep;
}

} // namespace

QVector<QPointF> simplify(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
    if (count < 3 || tolerance <= 0)
        return points;
    const QVector<bool> keep = needed(points, tolerance);
    QVector<QPointF> kept;
    for (int i = 0; i < count; ++i)
        if (keep[i])
            kept.append(points[i]);
    return kept;
}

namespace {

// The stretch of the smooth curve from points[i] to points[i + 1], handed to
// `each` a point at a time, all but the first. It bends with the points
// either side as well, so it is only settled once there is a point after
// the next.
template <class Each> void curveBetween(const QVector<QPointF> &points, int i, qreal tolerance, Each each) {
    const int count = points.size();
    const QPointF p0 = points[qMax(i - 1, 0)], p1 = points[i], p2 = points[i + 1], p3 = points[qMin(i + 2, count - 1)];
    // Centripetal: the knots are spaced by the square root of the distance
    // between points.
    const qreal d1 = std::sqrt(length(p1 - p0)), d2 = std::sqrt(length(p2 - p1)), d3 = std::sqrt(length(p3 - p2));
    if (d2 < 1e-6)
        return;
    // The cubic Bezier between p1 and p2 that the spline is there.
    const QPointF b1 = d1 < 1e-6 ? p1 + (p2 - p1) / 3
                                 : (d1 * d1 * p2 - d2 * d2 * p0 + (2 * d1 * d1 + 3 * d1 * d2 + d2 * d2) * p1) /
                                       (3 * d1 * (d1 + d2));
    const QPointF b2 = d3 < 1e-6 ? p2 - (p2 - p1) / 3
                                 : (d3 * d3 * p1 - d2 * d2 * p3 + (2 * d3 * d3 + 3 * d3 * d2 + d2 * d2) * p2) /
                                       (3 * d3 * (d3 + d2));
    // Wang's bound on how many segments keep a cubic within tolerance.
    const qreal bend = qMax(length(p1 - 2 * b1 + b2), length(b1 - 2 * b2 + p2));
    const int steps = qBound(1, int(std::ceil(std::sqrt(0.75 * bend / qMax(tolerance, 1e-3)))), 32);
    for (int step = 1; step <= steps; ++step) {
        const qreal t = qreal(step) / steps, u = 1 - t;
        each(u * u * u * p1 + 3 * u * u * t * b1 + 3 * u * t * t * b2 + t * t * t * p2);
    }
}

} // namespace

QVector<QPointF> smooth(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
    if (count < 3)
        return points;
    QVector<QPointF> curve;
    curve.reserve(count * 4);
    curve.append(points[0]);
    for (int i = 0; i + 1 < count; ++i)
        curveBetween(points, i, tolerance, [&curve](QPointF point) { curve.append(point); });
    return curve;
}

namespace {

// A line that turns by 75 degrees or more comes to a point: the corner of a
// box, the head of an arrow, the points of a zigzag. One that turns by 40 or
// more does too where it runs straight either side. A curve drawn by hand
// turns as sharply only where it was drawn with too few points to tell it
// from a corner, and runs straight either side of a turn only where it was
// drawn with too few to tell it from a polygon.
const qreal sharpTurn = 75, gentleTurn = 40;
// How far, in screen pixels, the pen has to go either side of a sharp turn
// for it to be a corner. Nearer, the turn can be the pixels it moves on: a
// line drawn a pixel at a time steps across them, a right angle at every
// step.
const qreal cornerReach = 3;
// How far it has to go either side of a turn split between two points, for
// where the lines either side meet to be told apart from how the pixels
// under them fall.
const qreal splitReach = 8;
// Rounded off, a corner bulges out by about a thirteenth of the shorter of
// the stretches either side of it times the sine of its turn: a gentle turn
// is only a corner where that would show, half a pixel or more.
const qreal roundingShows = 6.7;
// The pen moves over whole pixels, so where it went is up to half a pixel's
// diagonal off, and which way it went by that over each stretch.
const qreal pixelOff = 0.7;
// How far, in screen pixels, a point can be from the line through the points
// either side of it for the line to run straight there. The pixels put the
// point off one way and those either side as much the other, so a line
// straight on the screen can bend at a point by a pixel or more. Into a
// sharp corner it may bend by a pixel, as how fast the hand went by the
// corner tells a round one from a sharp one (see cornerPace). Either side of
// a gentle turn, where the way round a curve is all but as long as the way by
// a corner and only how straight the line runs tells them apart, by less.
const qreal sharpRunOff = 1, gentleRunOff = 0.8;
// A hand goes round a corner no faster than it goes either side of it: the
// way by a corner between two points is no more than a tenth longer than
// the stretches either side, and as much as where the corner is can be off.
// Further, it went round a curve that cut the corner short.
const qreal cornerPace = 1.1;

// How far a line turns going along `in` and then along `out`, in degrees to
// the left or to the right.
qreal turnFrom(QPointF in, QPointF out) { return qRadiansToDegrees(std::atan2(cross(in, out), dot(in, out))); }

// Where a hand that went along `in` and then along `out` went round a
// corner, on its way from the end of the one to the start of the other:
// where the two meet, ahead of the one and behind the other, by a way that
// keeps its pace, a point every `pace` (see cornerPace). None where they do
// not meet like that. `pixel` is how long a screen pixel is.
std::optional<QPointF> wentRound(const QLineF &in, const QLineF &out, qreal pace, qreal pixel) {
    QPointF corner;
    if (in.intersects(out, &corner) == QLineF::NoIntersection)
        return std::nullopt;
    const QPointF along = in.p2() - in.p1(), onwards = out.p2() - out.p1();
    const qreal ahead = dot(corner - in.p2(), along) / in.length(),
                behind = dot(out.p1() - corner, onwards) / out.length();
    // Each line is as far off as the points it goes through (see pixelOff),
    // which puts where they meet further off the more nearly head on they
    // meet: the sharper the corner, the more.
    const qreal half = qDegreesToRadians(180 - std::abs(turnFrom(along, onwards))) / 2;
    const qreal slack = qMin(pixelOff * pixel / std::tan(half), pace);
    if (ahead < 0 || behind < 0 || ahead + behind > cornerPace * pace + slack)
        return std::nullopt;
    return corner;
}

// The line nearest all of `run`, from where the first of its points is on it
// to where the last is: the line through them, if there are two. Through a
// few of the pen's points it is nearer true than the line through any two of
// them, as the pixels put some of them one way and some the other.
QLineF fitted(const QVector<QPointF> &run) {
    if (run.size() < 3)
        return QLineF(run.first(), run.last());
    QPointF middle;
    for (const QPointF &point : run)
        middle += point;
    middle /= run.size();
    // Which way they spread most (least squares, across the line).
    qreal xx = 0, xy = 0, yy = 0;
    for (const QPointF &point : run) {
        const QPointF from = point - middle;
        xx += from.x() * from.x();
        xy += from.x() * from.y();
        yy += from.y() * from.y();
    }
    const qreal angle = std::atan2(2 * xy, xx - yy) / 2;
    QPointF along(std::cos(angle), std::sin(angle));
    if (dot(along, run.last() - run.first()) < 0)
        along = -along;
    return QLineF(middle + along * dot(run.first() - middle, along), middle + along * dot(run.last() - middle, along));
}

// How far `point` is from the line through `count` points from `line`.
qreal distanceTo(const QPointF *line, int count, QPointF point) {
    if (count == 1)
        return length(point - line[0]);
    qreal nearest = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < count; ++i)
        nearest = qMin(nearest, distanceToSegment(point, line[i], line[i + 1]));
    return nearest;
}

// How far apart two lines are: as far as any point of either is from the
// other. Measured only until that is past `enough`: how far past does not
// matter.
qreal apart(const QPointF *one, int ones, const QPointF *other, int others, qreal enough) {
    qreal furthest = 0;
    for (int i = 0; i < ones && furthest <= enough; ++i)
        furthest = qMax(furthest, distanceTo(other, others, one[i]));
    for (int i = 0; i < others && furthest <= enough; ++i)
        furthest = qMax(furthest, distanceTo(one, ones, other[i]));
    return furthest;
}

} // namespace

Corner cornerBefore(const QVector<QPointF> &points, QPointF next, qreal pixel) {
    // The end of the line, the newest first and a corner's two points as
    // one: `next`, the last point, the one before it and so on, each with
    // where it is in `points`, whether the line comes to a point there and
    // whether it starts there.
    struct End {
        QPointF at;
        int index = -1;
        bool corner = false, start = false;
    };
    std::array<End, 7> end;
    end[0].at = next;
    int count = 1;
    for (int i = int(points.size()) - 1; i >= 0 && count < int(end.size()); ++count) {
        End &point = end[count];
        point.at = points[i];
        point.corner = i > 0 && points[i - 1] == points[i];
        point.index = point.corner ? i - 1 : i;
        point.start = point.index == 0;
        i = point.index - 1;
    }
    if (count < 3 || end[1].corner)
        return {};
    // The line from the point k + 1 back to the kth, in screen pixels...
    auto stretch = [&](int k) { return length(end[k].at - end[k + 1].at) / pixel; };
    // ...the way it turns at the kth...
    auto turn = [&](int k) { return turnFrom(end[k].at - end[k + 1].at, end[k - 1].at - end[k].at); };
    // ...and as much as that can be, by the point and those either side of it
    // being `by` screen pixels off across it.
    auto off = [&](int k, qreal by = pixelOff) { return qRadiansToDegrees(by / stretch(k) + by / stretch(k - 1)); };
    // Whether it runs straight through the kth point next to a turn of
    // `turned`: turns there by no more than a third of that, nor than its
    // points `by` off make it.
    auto straight = [&](int k, qreal turned, qreal by) { return std::abs(turn(k)) <= qMin(turned / 3, off(k, by)); };
    // The line straight into the kth point, its points oldest first: as far
    // back as it runs straight, two points more at most, or to where it
    // starts or comes to a point (`whole`).
    struct Run {
        QVector<QPointF> points;
        int straight = 0;
        bool whole = false;
    };
    auto runInto = [&](int k, qreal turned, qreal by) {
        Run run{{end[k + 1].at, end[k].at}};
        for (int j = k + 1; j < k + 3; ++j) {
            if (end[j].start || end[j].corner || j + 1 >= count) {
                run.whole = true;
                break;
            }
            if (!straight(j, turned, by))
                break;
            run.points.prepend(end[j + 1].at);
            ++run.straight;
        }
        return run;
    };
    // How far the hand went from one point to the next, by the stretches
    // either side of a corner: only the one after it where the one before
    // starts where the line did or came to a point, as only part of a
    // stretch's way was gone by then.
    auto pace = [&](const Run &run, int k, const QLineF &out) {
        return run.whole && run.straight == 0 ? out.length() : qMin(stretch(k) * pixel, out.length());
    };
    const qreal last = turn(1);
    // A sharp turn split between the last two points, after a straight run:
    // the hand went fast by a corner between them, which is where the run
    // and the way on from the last point meet. That can be past a point the
    // pen took for the corner, as it turned sharply where the hand had all
    // but reached it. A run from where the line starts or comes to a point
    // can be a single stretch: the stroke of a tick goes into its corner
    // with a point or two.
    if (count >= 4 && !end[2].start) {
        const qreal before = turn(2);
        const Run run = runInto(2, std::abs(before + last), sharpRunOff);
        const QLineF in = fitted(run.points), out(end[1].at, end[0].at);
        if (before * last > 0 && stretch(2) >= splitReach && stretch(0) >= splitReach && std::abs(last) > off(1) &&
            std::abs(before) > off(2) && (run.straight >= 2 || run.whole) &&
            std::abs(turnFrom(in.p2() - in.p1(), out.p2() - out.p1())) >= sharpTurn)
            if (const std::optional<QPointF> corner = wentRound(in, out, pace(run, 2, out), pixel))
                return {int(points.size()) - end[2].index - 1, {*corner, *corner, end[1].at}};
    }
    // A gentle turn at the point before last, the line straight either side
    // of it, which would show rounded off.
    if (count >= 4 && !end[2].start && !end[2].corner) {
        const qreal turned = std::abs(turn(2));
        const Run run = runInto(2, turned, gentleRunOff);
        if (turned >= gentleTurn &&
            qMin(stretch(2), stretch(1)) * std::sin(qDegreesToRadians(turned)) >= roundingShows &&
            (run.straight >= 2 || (run.straight >= 1 && run.whole)) && straight(1, turned, gentleRunOff))
            return {1, {end[2].at, end[1].at}};
    }
    // A gentle turn split between the two points before that, the line
    // straight either side of them.
    if (count >= 5 && !end[3].start && !end[3].corner && !end[2].corner) {
        const qreal first = turn(3), second = turn(2), both = std::abs(first + second);
        const Run run = runInto(3, both, gentleRunOff);
        const QLineF in = fitted(run.points), out(end[2].at, end[1].at);
        const qreal turned = std::abs(turnFrom(in.p2() - in.p1(), out.p2() - out.p1()));
        if (first * second > 0 && stretch(3) >= splitReach && stretch(1) >= splitReach && std::abs(first) > off(3) &&
            std::abs(second) > off(2) && (run.straight >= 2 || (run.straight >= 1 && run.whole)) &&
            straight(1, both, gentleRunOff) && turned >= gentleTurn &&
            qMin(stretch(3), stretch(1)) * std::sin(qDegreesToRadians(turned)) >= roundingShows)
            if (const std::optional<QPointF> corner = wentRound(in, out, pace(run, 3, out), pixel))
                return {2, {*corner, *corner, end[2].at, end[1].at}};
    }
    // A sharp turn at the last point, the line coming from the start or
    // another corner, or straight into it: at the point before it turns by
    // no more than a third as much, even as far off as the pixels can put
    // that, and at the two before by no more than that together. The foot of
    // a loop drawn small turns at all three, if by little at one of them, and
    // so does a hand that shakes.
    auto ranStraight = [&] {
        if (end[2].start || end[2].corner)
            return true;
        if (std::abs(turn(2)) + off(2) > std::abs(last) / 3)
            return false;
        return end[3].start || end[3].corner || count < 5 || std::abs(turn(2) + turn(3)) <= std::abs(last) / 3;
    };
    if (std::abs(last) >= sharpTurn && stretch(1) >= cornerReach && stretch(0) >= cornerReach && ranStraight())
        return {0, {end[1].at}};
    return {};
}

QVector<QPointF> simplifyCurve(const QVector<QPointF> &points, qreal tolerance) {
    const int count = points.size();
    if (count < 3 || tolerance <= 0)
        return points;
    // Both curves are cut into pieces within a sixteenth of `tolerance` of
    // them to be measured against each other, so they can be an eighth of it
    // further apart than they measure: they may measure what is left. Cut
    // less finely, a line that waves by a little less than `tolerance` would
    // measure as waving by more, and be kept wave by wave.
    const qreal fine = tolerance / 16, near = tolerance - 2 * fine;
    // The curve through every point, and where on it each point is.
    QVector<QPointF> whole{points[0]};
    QVector<int> at(count, 0);
    for (int i = 0; i + 1 < count; ++i) {
        curveBetween(points, i, fine, [&whole](QPointF point) { whole.append(point); });
        at[i + 1] = int(whole.size()) - 1;
    }
    // From the points the path needs on to those the curve needs. A point
    // drawn twice is a corner (see smooth()), and is kept twice or not at all.
    QVector<bool> keep = needed(points, tolerance);
    for (int i = 1; i < count; ++i)
        if (points[i] == points[i - 1] && keep[i] != keep[i - 1])
            keep[i] = keep[i - 1] = true;
    // The points kept, each with the one kept before it and after it, and the
    // stretches of curve from one to the next still to look at, by where they
    // start.
    QVector<int> before(count, -1), after(count, -1), waiting;
    for (int i = 0, kept = -1; i < count; ++i)
        if (keep[i]) {
            before[i] = kept;
            if (kept >= 0) {
                after[kept] = i;
                waiting.append(kept);
            }
            kept = i;
        }
    // Keeps the ith point, which is between the kept points `from` and `to`,
    // and with it the other of a corner's two. A stretch of curve bends with
    // the points either side of it, so those it bends are looked at again.
    auto keepAt = [&](int i, int from, int to) {
        int first = i, last = i;
        while (first > from + 1 && points[first - 1] == points[i])
            --first;
        while (last < to - 1 && points[last + 1] == points[i])
            ++last;
        for (int j = first; j <= last; ++j) {
            keep[j] = true;
            before[j] = j == first ? from : j - 1;
            after[j] = j == last ? to : j + 1;
        }
        after[from] = first;
        before[to] = last;
        for (const int start : {before[from], from, first, last, to})
            if (start >= 0 && after[start] >= 0)
                waiting.append(start);
    };
    // The stretch of curve from one point kept to the next bends with the
    // points kept either side of it, and is only looked at again once one of
    // those is another.
    QVector<std::array<int, 3>> matched(count, {-1, -1, -1});
    QVector<QPointF> stretch;
    for (qsizetype next = 0; next < waiting.size(); ++next) {
        const int a = waiting[next], b = after[a];
        const std::array<int, 3> around{before[a], b, after[b]};
        if (matched[a] == around)
            continue;
        // From one point to the next, bent by the points either side of them,
        // it is the curve as drawn.
        if (b == a + 1 && around[0] == a - 1 && around[2] == (b + 1 < count ? b + 1 : -1)) {
            matched[a] = around;
            continue;
        }
        const QVector<QPointF> bending{points[around[0] >= 0 ? around[0] : a], points[a], points[b],
                                       points[around[2] >= 0 ? around[2] : b]};
        stretch = {points[a]};
        curveBetween(bending, 1, fine, [&stretch](QPointF point) { stretch.append(point); });
        if (apart(stretch.constData(), int(stretch.size()), whole.constData() + at[a], at[b] - at[a] + 1, near) <=
            near) {
            matched[a] = around;
            continue;
        }
        // Too far from the curve as drawn: the point between it strays
        // furthest from puts that right, of those at least a quarter of the
        // way from either end, so that a long stretch is cut well short each
        // time and looked at again only a few times. With none between, the
        // points either side that bend it.
        const int from = b - a > 4 ? a + (b - a) / 4 : a + 1, to = b - a > 4 ? b - (b - a) / 4 : b - 1;
        int furthest = -1;
        qreal most = -1;
        for (int i = from; i <= to; ++i)
            if (const qreal off = distanceTo(stretch.constData(), int(stretch.size()), points[i]); off > most) {
                most = off;
                furthest = i;
            }
        if (furthest >= 0) {
            keepAt(furthest, a, b);
        } else {
            if (a > 0 && !keep[a - 1])
                keepAt(a - 1, before[a], a);
            if (b + 1 < count && !keep[b + 1])
                keepAt(b + 1, b, after[b]);
        }
    }
    QVector<QPointF> kept;
    for (int i = 0; i < count; ++i)
        if (keep[i])
            kept.append(points[i]);
    return kept;
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
                    Cap cap, Join join, qreal scale) {
    StrokeLine line(halfWidth, color, cap, join, scale);
    for (const QPointF &point : source)
        line.add(mesh, point + offset);
    line.end(mesh);
}

StrokeLine::StrokeLine(qreal halfWidth, quint32 color, Cap cap, Join join, qreal scale)
    : m_halfWidth(float(halfWidth)), m_color(color), m_cap(cap), m_join(join), m_step(arcStepFor(halfWidth, scale)) {}

// The two vertices across the line at a point of it.
StrokeLine::Pair StrokeLine::pairAt(StrokeMesh &mesh, QPointF center, QPointF normal) const {
    Builder build{mesh, m_halfWidth, m_color, m_step};
    return Pair{build.vertex(center, normal, 1), build.vertex(center, normal, -1)};
}

void StrokeLine::add(StrokeMesh &mesh, QPointF point) {
    // Points on top of each other have no direction to draw along.
    if (m_count > 0 && length(point - m_last) <= 1e-4)
        return;
    if (++m_count == 1) {
        m_first = m_last = point;
        return;
    }
    const QPointF delta = point - m_last;
    const QPointF direction = delta / length(delta), normal(-direction.y(), direction.x());
    if (m_count == 2) {
        m_start = pairAt(mesh, m_last, normal);
        // Half a disc behind the first point.
        if (m_cap == Cap::Round)
            Builder{mesh, m_halfWidth, m_color, m_step}.fan(m_first, std::atan2(normal.y(), normal.x()), M_PI);
    } else {
        corner(mesh, direction, normal);
    }
    m_direction = direction;
    m_normal = normal;
    m_last = point;
}

// Each segment is a quad between a pair of vertices at its start and a pair
// at its end. Where the path bends gently the two segments share a pair,
// mitered; where it turns sharply each keeps its own and a wedge fills the
// outside of the corner. This is the corner at the last point, between the
// segment that ends there and the one that starts there, and with it the
// first of those, which it settles.
void StrokeLine::corner(StrokeMesh &mesh, QPointF direction, QPointF normal) {
    Builder build{mesh, m_halfWidth, m_color, m_step};
    Pair end, next;
    const qreal bend = dot(m_direction, direction);
    // A miter is as long as the corner is sharp, so past a point it is cut
    // off, as a bevel is from the start.
    const bool mitered = m_join == Join::Miter ? bend > -0.9 : m_join == Join::Bevel ? bend > 0.5 : bend > 0.9;
    if (mitered) {
        const QPointF sum = m_normal + normal;
        const QPointF miter = sum / length(sum);
        // The miter reaches further than the stroke is wide by the inverse
        // cosine of half the turn.
        end = next = pairAt(mesh, m_last, miter / dot(miter, m_normal));
    } else {
        end = pairAt(mesh, m_last, m_normal);
        next = pairAt(mesh, m_last, normal);
        // The outside of the corner is away from the turn.
        const bool leftOutside = cross(m_direction, direction) <= 0;
        if (m_join == Join::Round) {
            const qreal outside = leftOutside ? 1 : -1;
            const QPointF from = outside * m_normal, to = outside * normal;
            const qreal sweep = std::atan2(cross(from, to), dot(from, to));
            build.fan(m_last, std::atan2(from.y(), from.x()), sweep);
        } else {
            build.triangle(build.vertex(m_last, QPointF(1, 0), 0), leftOutside ? end.left : end.right,
                           leftOutside ? next.left : next.right);
        }
    }
    build.triangle(m_start.left, m_start.right, end.left);
    build.triangle(m_start.right, end.right, end.left);
    m_start = next;
}

void StrokeLine::end(StrokeMesh &mesh) const {
    Builder build{mesh, m_halfWidth, m_color, m_step};
    if (m_count == 1) {
        // A tap of the pen is a dot.
        build.fan(m_first, 0, 2 * M_PI);
    } else if (m_count > 1) {
        const Pair end = pairAt(mesh, m_last, m_normal);
        build.triangle(m_start.left, m_start.right, end.left);
        build.triangle(m_start.right, end.right, end.left);
        // Half a disc ahead of the last point.
        if (m_cap == Cap::Round) {
            const QPointF ahead = -m_normal;
            build.fan(m_last, std::atan2(ahead.y(), ahead.x()), M_PI);
        }
    }
}

Pen::Pen(qreal halfWidth, quint32 color, qreal scale)
    : m_halfWidth(halfWidth), m_color(color), m_scale(scale),
      m_tolerance(qMax(strokeTolerance, pixelTolerance / qMax(scale, 1e-6))),
      m_line(halfWidth, color, Cap::Round, Join::Round, scale) {}

void Pen::add(QPointF point) {
    m_points.append(point);
    if (m_points.size() == 1)
        m_line.add(m_mesh, point);
    cut();
}

void Pen::redraw(int back, const QVector<QPointF> &points) {
    // No more than the last two, and never the first: the line before them
    // is settled (see cut()).
    back = qBound(0, back, int(qMin<qsizetype>(2, m_points.size() - 1)));
    m_points.resize(m_points.size() - back);
    m_points += points;
    cut();
}

void Pen::cut() {
    const int count = int(m_points.size());
    // Back to where the line is settled: what was cut after that was only
    // for as long as the points after it had not come.
    m_mesh.vertices.resize(m_vertices);
    m_mesh.indices.resize(m_indices);
    // On through the stretches of curve that no point to come will bend,
    // nor taking back the last two (see redraw())...
    for (; m_settled + 4 < count; ++m_settled)
        curveBetween(m_points, m_settled, m_tolerance, [this](QPointF at) { m_line.add(m_mesh, at); });
    m_vertices = m_mesh.vertices.size();
    m_indices = m_mesh.indices.size();
    // ...and the rest as it is for now, to the end of the line.
    StrokeLine rest = m_line;
    for (int i = m_settled; i + 1 < count; ++i)
        curveBetween(m_points, i, m_tolerance, [this, &rest](QPointF at) { rest.add(m_mesh, at); });
    rest.end(m_mesh);
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

bool polylineIntersectsRect(const QVector<QPointF> &points, const QRectF &rect, qreal radius) {
    if (radius > 0) {
        // A line with round ends reaches the box wherever its middle meets
        // the box widened by its radius, with round corners.
        const QRectF across = rect.adjusted(-radius, 0, radius, 0);
        const QRectF down = rect.adjusted(0, -radius, 0, radius);
        const QPointF corners[] = {rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()};
        const auto touches = [&](QPointF a, QPointF b) {
            // Skip segments outside even the square-cornered box before
            // looking for intersections or measuring to the corners.
            if (qMax(a.x(), b.x()) < across.left() || qMin(a.x(), b.x()) > across.right() ||
                qMax(a.y(), b.y()) < down.top() || qMin(a.y(), b.y()) > down.bottom())
                return false;
            if (segmentIntersectsRect(a, b, across) || segmentIntersectsRect(a, b, down))
                return true;
            for (QPointF corner : corners)
                if (distanceToSegment(corner, a, b) <= radius)
                    return true;
            return false;
        };
        if (points.size() == 1)
            return touches(points.first(), points.first());
        for (int i = 0; i + 1 < points.size(); ++i)
            if (touches(points[i], points[i + 1]))
                return true;
        return false;
    }
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
