#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

// One thing on a board. A plain value: the board, the undo history, the
// clipboard and the files all pass these around, and the heavy members
// (text, stroke points) are shared until written, so copies are cheap.
struct Item {
    enum class Kind : quint8 { Note, Text, Shape, Stroke, Connector, Image, Frame };
    // The outlines a shape can have, in the order they are offered in.
    enum class Shape : quint8 {
        Rect,
        Rounded,
        Pill,
        Ellipse,
        Diamond,
        Triangle,
        RightTriangle,
        Parallelogram,
        Trapezoid,
        Pentagon,
        Hexagon,
        Octagon,
        Star,
        Cross,
        Arrow,
        ArrowLeft,
        DoubleArrow,
        Bubble,
        Cylinder,
    };
    static constexpr int shapeCount = int(Shape::Cylinder) + 1;

    // How a connector gets from one end to the other: straight there; in a
    // curve that leaves each end square to it; or out, across, and in, with
    // square corners.
    enum class Route : quint8 { Straight, Curved, Elbow };
    // What a connector has at an end.
    enum class Head : quint8 { None, Arrow, Open, Triangle, Dot, Circle, Diamond, OpenDiamond, Bar };
    enum class Dash : quint8 { Solid, Dashed, Dotted };
    static constexpr int routeCount = 3, headCount = 9, dashCount = 3;

    quint32 id = 0;
    Kind kind = Kind::Note;
    Shape shape = Shape::Rect;
    // Text: the whole text is bold. Notes and shapes always write their
    // first line bold instead.
    bool bold = false;
    // Connector: its route, what is at each of its ends, and its line.
    Route route = Route::Straight;
    Head start = Head::None, end = Head::Arrow;
    Dash dash = Dash::Solid;
    // Connector: whether a curved or elbow one leaves its ends sideways
    // rather than up and down. Like where its ends are, this is worked out
    // from what it is attached to, and is not kept in a file.
    bool sideways = true;
    // Items sharing a group are selected and moved as one. 0 is no group.
    quint32 group = 0;
    // Where the item is on the canvas. A stroke's is the box around its
    // points; a connector's is derived from its two ends.
    QRectF rect;
    QString text;
    // A color of the theme by name ("yellow", "blue"); empty for the kind's
    // own neutral color. Boards recolor when the theme changes.
    QString color;
    // Text: the size of the type, and the width it wraps at (0: it doesn't).
    qreal fontSize = 12;
    qreal wrap = 0;
    // Stroke and connector: line width.
    qreal width = 2;
    // Stroke: the pen's path, relative to rect.topLeft(), so moving a stroke
    // never touches its points.
    QVector<QPointF> points;
    // Connector: the items its ends are attached to (0: a free end), and
    // where the ends are. An attached end follows its item.
    quint32 from = 0, to = 0;
    QPointF p1, p2;
    // Image: the file in the board's assets folder.
    QString asset;

    // The most text one item holds: three pages or so, which is more than a
    // board is for. It is also what keeps writing safe. The editor draws
    // every letter of what it is given, in view or not, so what an item may
    // hold is what one frame may be asked for on its account.
    static constexpr int longestText = 10'000;
    // As much of `text` as an item holds, never ending on half a character.
    static QString fitted(QString text) {
        if (text.size() > longestText) {
            text.truncate(longestText);
            if (text.back().isHighSurrogate())
                text.chop(1);
        }
        return text;
    }

    bool operator==(const Item &other) const = default;

    // The colors of the theme a person picks from, in the order the toolbar
    // shows them.
    static const QStringList &colorNames() {
        static const QStringList names{"yellow", "orange", "red", "magenta", "blue", "cyan", "green"};
        return names;
    }

    bool isBox() const { return kind == Kind::Note || kind == Kind::Shape || kind == Kind::Image || kind == Kind::Frame; }
    bool hasText() const { return kind != Kind::Stroke && kind != Kind::Image; }
};

// Sizes new items start with, in canvas units (pixels at 100%).
namespace Defaults {
inline constexpr qreal noteSize = 152;
inline constexpr qreal shapeWidth = 152, shapeHeight = 56;
// A shape dropped with a click is the size that suits its outline: the box
// of a flowchart for the ones that are boxes, and squarer for the rest.
inline QSizeF shapeSize(Item::Shape shape) {
    switch (shape) {
    case Item::Shape::Triangle:
        return {120, 104};
    case Item::Shape::RightTriangle:
    case Item::Shape::Octagon:
    case Item::Shape::Cross:
        return {104, 104};
    case Item::Shape::Pentagon:
    case Item::Shape::Star:
        return {112, 108};
    case Item::Shape::Bubble:
        return {152, 104};
    case Item::Shape::Cylinder:
        return {104, 120};
    default:
        return {shapeWidth, shapeHeight};
    }
}
inline constexpr qreal frameWidth = 320, frameHeight = 200;
inline constexpr qreal minimumSize = 16;
inline constexpr qreal textSize = 12;
inline constexpr qreal penWidth = 2;
inline constexpr qreal connectorWidth = 1.5;
} // namespace Defaults
