#include "board.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

#include "geometry.h"
#include "textlayout.h"

namespace {

// How many steps of undo a board keeps.
const int historyLimit = 500;

// What a file said, for saying what is wrong with it: a few words of it at
// most and all on one line, because it goes wherever the answer is read,
// a status line and a terminal among them.
QString said(const QString &value) {
    QString words = value.left(40);
    if (!words.isEmpty() && words.back().isHighSurrogate())
        words.chop(1);
    for (QChar &c : words)
        if (c.category() == QChar::Other_Control)
            c = ' ';
    return value.size() > words.size() ? words + "..." : words;
}

const QStringList kindNames{"note", "text", "shape", "stroke", "connector", "image", "frame"};
// As Item::Shape lists them.
const QStringList shapeNames{"rect",     "rounded",  "pill",    "ellipse", "diamond", "triangle",   "right-triangle",
                             "parallelogram", "trapezoid", "pentagon", "hexagon", "octagon", "star", "cross",
                             "arrow",    "arrow-left", "double-arrow", "bubble", "cylinder"};
static_assert(Item::shapeCount == 19, "every shape has a name in shapeNames");
// As Item::Route, Item::Head and Item::Dash list them.
const QStringList routeNames{"straight", "curved", "elbow"};
const QStringList headNames{"none", "arrow", "open", "triangle", "dot", "circle", "diamond", "open-diamond", "bar"};
const QStringList dashNames{"solid", "dashed", "dotted"};

// Two decimals is a hundredth of a pixel at 100%: plenty, and it keeps the
// files short and stable.
double rounded(qreal value) { return std::round(value * 100) / 100; }

// The least a box (a note, a shape, a picture, a frame) is across and down.
// Nothing smaller could be seen or picked, and Sprawl 0.1.0 opens no board
// with a smaller box in it, so none is ever saved.
const qreal smallestBox = 1;

// A side less than that, or one that is no number at all, which a file has no
// way to say either.
bool tooShort(qreal side) { return !(side >= smallestBox); }

// A box smaller than that, as a picture far wider than tall is once it is
// brought down to a size to arrange.
bool tooSmall(const Item &item) {
    return item.isBox() && (tooShort(item.rect.width()) || tooShort(item.rect.height()));
}

// A point farther out than a file has anything, either way, or one that is
// no number at all; and the nearest to it that a file can have.
bool beyond(QPointF point) { return !(qAbs(point.x()) <= Item::farthest && qAbs(point.y()) <= Item::farthest); }
QPointF within(QPointF point) {
    const qreal far = Item::farthest;
    return QPointF(qBound(-far, point.x(), far), qBound(-far, point.y(), far));
}

// An item with its corner out there, or an end of a connector, as one nudged,
// dragged or pasted past the edge of the board would have.
bool tooFar(const Item &item) {
    return item.kind == Item::Kind::Connector ? beyond(item.p1) || beyond(item.p2) : beyond(item.rect.topLeft());
}

// The item as a board holds it, whatever it was given as: no more text than
// an item holds, a box at least the smallest across and down, grown from its
// top left corner as one read from a file is, and nothing farther out than a
// file has anything, which would be saved as a board that could not be read
// back.
Item held(Item item) {
    item.text = Item::fitted(item.text);
    if (item.isBox()) {
        if (tooShort(item.rect.width()))
            item.rect.setWidth(smallestBox);
        if (tooShort(item.rect.height()))
            item.rect.setHeight(smallestBox);
    }
    if (item.kind == Item::Kind::Connector) {
        item.p1 = within(item.p1);
        item.p2 = within(item.p2);
    } else {
        item.rect.moveTo(within(item.rect.topLeft()));
    }
    return item;
}

Item::Shape outlineOf(const Item &item) { return item.kind == Item::Kind::Shape ? item.shape : Item::Shape::Rect; }

// Everything the same except where the item is.
bool movedOnly(const Item &before, const Item &after) {
    if (before.kind == Item::Kind::Connector || before.rect.size() != after.rect.size() ||
        before.rect.topLeft() == after.rect.topLeft())
        return false;
    Item moved = after;
    moved.rect = before.rect;
    return moved == before;
}

// An item as a file has it, less where the attached ends of a connector are:
// those are worked out from what they are attached to, a little differently
// by whatever measured its text. What one version of an item is told from
// another by.
QJsonObject asWritten(const Item &item) {
    QJsonObject object = Board::itemToJson(item);
    if (item.kind == Item::Kind::Connector) {
        if (item.from) {
            object.remove("x1");
            object.remove("y1");
        }
        if (item.to) {
            object.remove("x2");
            object.remove("y2");
        }
    }
    return object;
}

// A version of an item the history keeps, with what a file changed of the
// item since the board last had it, from `was` to `now` (as asWritten() has
// them), changed in it too. An item the file made another kind of thing is
// what the file has.
Item withChanges(const Item &version, const QJsonObject &was, const QJsonObject &now) {
    QJsonObject object = now;
    if (was.value("type") == now.value("type")) {
        object = Board::itemToJson(version);
        const QStringList keys = was.keys() + now.keys();
        for (const QString &key : keys) {
            if (was.value(key) == now.value(key))
                continue;
            if (now.contains(key))
                object.insert(key, now.value(key));
            else
                object.remove(key);
        }
    }
    Item item;
    return Board::itemFromJson(object, item).isEmpty() ? item : version;
}

// The most an id can be, as a file says it.
const quint32 mostId = 4000000000u;

// The last id a file says it gave, to an item it has since taken away: a
// whole number, as an id is. Said some other way, it is no help, and nothing
// to refuse the file for.
quint32 lastIdOf(const QJsonObject &root) {
    const QJsonValue value = root.value("lastId");
    const double read = value.toDouble(-1);
    return value.isDouble() && read >= 0 && read <= mostId && read == std::floor(read) ? quint32(read) : 0;
}

// The id to give next: the one after `last`, the last id given, which it
// then is. Past the most an id can be, which only a file that says it gave
// ids that far brings about, a board could not be read back: there it is
// the first id not among those `used` gives. A group id may be given before
// anything has it, so a `cursor` remembers how far the search got.
quint32 nextId(quint32 &last, const std::function<QSet<quint32>()> &used, quint32 *cursor = nullptr) {
    if (last < mostId)
        return ++last;
    const QSet<quint32> taken = used();
    quint32 id = cursor ? *cursor : 0;
    do {
        id = id == mostId ? 1 : id + 1;
    } while (taken.contains(id));
    if (cursor)
        *cursor = id;
    return id;
}

// A text item's size before its type has been measured: close for the
// monospace type boards are written in.
QSizeF estimatedTextSize(const Item &item) {
    const QStringList lines = item.text.split('\n');
    qsizetype longest = 1;
    for (const QString &line : lines)
        longest = qMax(longest, line.size());
    const qreal width = item.wrap > 0 ? item.wrap : longest * item.fontSize * 0.6;
    return QSizeF(width, qMax<qsizetype>(1, lines.size()) * std::ceil(item.fontSize * 1.32));
}

// The most room a connector's label can take, for the board to say what the
// connector covers without laying out its type. A letter of the board's
// monospace type is a little over 0.6 of its size across once hinted to
// whole pixels, so 0.7 is room enough, and a wide one (Chinese, Japanese,
// Korean, an emoji) twice that: every character from where the wide ones
// start is taken to be one. A line, in the scripts whose type is tallest, is
// near three times the size.
QSizeF mostLabelSize(const QString &text) {
    qsizetype widest = 0, lines = 0;
    for (const QStringView line : QStringView(text).split(u'\n')) {
        qsizetype letters = 0;
        for (const QChar c : line)
            letters += !c.isSurrogate() && c.unicode() >= 0x1100 ? 2 : 1;
        widest = qMax(widest, letters);
        ++lines;
    }
    return QSizeF(widest * TextMetrics::small * 0.7, lines * TextMetrics::small * 3);
}

} // namespace

Board::Board(QObject *parent) : QObject(parent) {}

QString Board::kindName(Item::Kind kind) { return kindNames.value(int(kind)); }
QString Board::shapeName(Item::Shape shape) { return shapeNames.value(int(shape)); }
QStringList Board::shapes() { return shapeNames; }
QStringList Board::routes() { return routeNames; }
QStringList Board::heads() { return headNames; }
QStringList Board::dashes() { return dashNames; }

// --- Reading -----------------------------------------------------------------

const Item *Board::item(quint32 id) const {
    const auto found = m_items.constFind(id);
    return found == m_items.constEnd() ? nullptr : &*found;
}

int Board::depth(quint32 id) const {
    if (m_depthsStale) {
        m_depths.clear();
        m_depths.reserve(m_order.size());
        for (int i = 0; i < m_order.size(); ++i)
            m_depths.insert(m_order[i], i);
        m_depthsStale = false;
    }
    return m_depths.value(id, -1);
}

QRectF Board::boundsFor(const Item &item, const QRectF &laidOut) {
    switch (item.kind) {
    case Item::Kind::Stroke: {
        // What is drawn is the curve through its points (see Geo::smooth()),
        // which can bend out past the box around them: round a corner given
        // only once, as a file or a stroke drawn before corners were kept
        // has them, and between the points of a bend.
        const qreal reach = item.width / 2 + 1;
        const QRectF curve = Geo::boundsOf(Geo::smooth(item.points, Geo::strokeTolerance));
        return (item.rect | curve.translated(item.rect.topLeft())).adjusted(-reach, -reach, reach, reach);
    }
    case Item::Kind::Connector: {
        // Room for what is at its ends, and for the label beside the middle.
        const qreal reach = 8 + 3 * item.width;
        QRectF bounds = item.rect.adjusted(-reach, -reach, reach, reach);
        if (!laidOut.isNull()) {
            bounds |= laidOut;
        } else if (!item.text.isEmpty()) {
            // A guess at it: where the window puts a label as big as its text
            // can make one, whichever way the line runs. A smaller one sits
            // nearer the line, so it is where the window puts one of any size
            // up to that, each corner rounded to a whole unit.
            const QSizeF most = mostLabelSize(item.text);
            for (const QSizeF size : {most, QSizeF(most.width(), 0), QSizeF(0, most.height())})
                bounds |= QRectF(TextMetrics::connectorLabelCorner(item, size), size).adjusted(-1, -1, 1, 1);
        }
        return bounds;
    }
    case Item::Kind::Frame:
        // The title sits on the top edge. One longer than the edge runs past
        // its end, which only `laidOut` knows.
        return item.rect.adjusted(-1, -10, 1, 1) | laidOut;
    default:
        return item.rect;
    }
}

// What each item covers was worked out as it was set (see bounds()).
QRectF Board::contentBounds() const {
    QRectF all;
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
        all = all.isNull() ? bounds(it.key()) : all | bounds(it.key());
    return all;
}

QRectF Board::contentBounds(const QVector<quint32> &ids) const {
    QRectF all;
    for (quint32 id : ids)
        if (item(id))
            all = all.isNull() ? bounds(id) : all | bounds(id);
    return all;
}

void Board::itemsIn(const QRectF &area, QVector<quint32> &found) const { m_grid.query(area, found); }

quint32 Board::itemAt(QPointF point, qreal slack, const std::function<bool(const Item &)> &accept) const {
    QVector<quint32> near;
    m_grid.query(QRectF(point.x() - slack, point.y() - slack, slack * 2, slack * 2), near);
    quint32 front = 0;
    int frontDepth = -1;
    for (quint32 id : near) {
        const Item *candidate = item(id);
        if (!candidate || (accept && !accept(*candidate)))
            continue;
        const int at = depth(id);
        if (at > frontDepth && hits(*candidate, point, slack)) {
            front = id;
            frontDepth = at;
        }
    }
    return front;
}

bool Board::hits(const Item &item, QPointF point, qreal slack) const {
    const QRectF &rect = item.rect;
    switch (item.kind) {
    case Item::Kind::Note:
    case Item::Kind::Image:
        return rect.contains(point);
    case Item::Kind::Text:
        return rect.adjusted(-2, -2, 2, 2).contains(point);
    case Item::Kind::Shape:
        return Geo::shapeContains(item.shape, rect, point);
    case Item::Kind::Stroke:
        return Geo::distanceToPolyline(item.points, point - rect.topLeft()) <= item.width / 2 + slack;
    case Item::Kind::Connector:
        return Geo::distanceToPolyline(Geo::connectorPath(item), point) <= item.width / 2 + slack;
    case Item::Kind::Frame: {
        // A frame is its edge and its title. Its inside belongs to whatever
        // is in it, so those items can be clicked and marquee-selected.
        if (!rect.adjusted(-slack, -10, slack, slack).contains(point))
            return false;
        const QRectF inside = rect.adjusted(slack, 10, -slack, -slack);
        return !inside.isValid() || !inside.contains(point);
    }
    }
    return false;
}

bool Board::touches(const Item &item, const QRectF &area) const {
    switch (item.kind) {
    case Item::Kind::Stroke:
        return Geo::polylineIntersectsRect(Geo::smooth(item.points, Geo::strokeTolerance),
                                           area.translated(-item.rect.topLeft()), item.width / 2);
    case Item::Kind::Connector:
        return Geo::polylineIntersectsRect(Geo::connectorPath(item), area);
    case Item::Kind::Frame:
        // Dragging a marquee inside a frame selects what is in it, not it.
        return area.contains(item.rect);
    default:
        return area.intersects(item.rect);
    }
}

QVector<quint32> Board::membersOf(quint32 group) const {
    QVector<quint32> members;
    if (group == 0)
        return members;
    for (quint32 id : m_order)
        if (m_items.value(id).group == group)
            members.append(id);
    return members;
}

quint32 Board::newGroup() {
    return nextId(m_lastGroup, [this] { return groupsInUse(); }, &m_wrappedGroup);
}

Board::Ends Board::connectorEnds(const Item &connector) const {
    const Item *from = item(connector.from), *to = item(connector.to);
    if (from && !canAttach(*from))
        from = nullptr;
    if (to && !canAttach(*to))
        to = nullptr;
    Ends ends{connector.p1, connector.p2};
    if (connector.route == Item::Route::Straight) {
        // Each attached end leaves its item toward the other end.
        const QPointF fromAim = from ? from->rect.center() : connector.p1;
        const QPointF toAim = to ? to->rect.center() : connector.p2;
        if (from)
            ends.from = Geo::shapeEdgeToward(outlineOf(*from), from->rect, toAim);
        if (to)
            ends.to = Geo::shapeEdgeToward(outlineOf(*to), to->rect, fromAim);
        ends.sideways = std::abs(ends.to.x() - ends.from.x()) >= std::abs(ends.to.y() - ends.from.y());
        return ends;
    }
    // A curved or an elbow one leaves the middle of a side: the sides that
    // face each other across the wider of the two gaps between the items,
    // the one from side to side or the one from top to bottom. (A free end
    // is an item of no size where the end is.)
    const QRectF a = from ? from->rect : QRectF(connector.p1, QSizeF(0, 0));
    const QRectF b = to ? to->rect : QRectF(connector.p2, QSizeF(0, 0));
    const qreal across = qMax(b.left() - a.right(), a.left() - b.right());
    const qreal down = qMax(b.top() - a.bottom(), a.top() - b.bottom());
    ends.sideways = across >= down;
    const qreal far = 1e6;
    const QPointF toward = ends.sideways ? QPointF(b.center().x() >= a.center().x() ? far : -far, 0)
                                         : QPointF(0, b.center().y() >= a.center().y() ? far : -far);
    if (from)
        ends.from = Geo::shapeEdgeToward(outlineOf(*from), a, a.center() + toward);
    if (to)
        ends.to = Geo::shapeEdgeToward(outlineOf(*to), b, b.center() - toward);
    return ends;
}

// --- Storage ------------------------------------------------------------------

void Board::link(const Item &item) {
    if (item.kind != Item::Kind::Connector)
        return;
    if (item.from)
        m_links.insert(item.from, item.id);
    if (item.to && item.to != item.from)
        m_links.insert(item.to, item.id);
}

void Board::unlink(const Item &item) {
    if (item.kind != Item::Kind::Connector)
        return;
    m_links.remove(item.from, item.id);
    m_links.remove(item.to, item.id);
}

void Board::place(const Item &item) {
    m_items.insert(item.id, item);
    m_grid.insert(item.id, boundsFor(item));
    link(item);
    m_lastId = qMax(m_lastId, item.id);
    m_lastGroup = qMax(m_lastGroup, item.group);
}

void Board::erase(quint32 id) {
    const auto found = m_items.constFind(id);
    if (found == m_items.constEnd())
        return;
    unlink(*found);
    m_items.erase(found);
    m_grid.remove(id);
}

void Board::refreshConnector(quint32 id) {
    const auto found = m_items.find(id);
    if (found == m_items.end() || found->kind != Item::Kind::Connector)
        return;
    Item &connector = *found;
    // An end whose item is gone stays, free, where it last was.
    for (quint32 *end : {&connector.from, &connector.to}) {
        const Item *attached = *end ? item(*end) : nullptr;
        if (*end && (!attached || !canAttach(*attached))) {
            m_links.remove(*end, id);
            *end = 0;
        }
    }
    const Ends ends = connectorEnds(connector);
    const QRectF rect = QRectF(ends.from, ends.to).normalized();
    if (connector.p1 == ends.from && connector.p2 == ends.to && connector.rect == rect &&
        connector.sideways == ends.sideways)
        return;
    connector.p1 = ends.from;
    connector.p2 = ends.to;
    connector.sideways = ends.sideways;
    connector.rect = rect;
    m_grid.update(id, boundsFor(connector));
    emit itemChanged(id, Change::Edited);
}

void Board::refreshConnectorsOf(quint32 id) {
    const QVector<quint32> connectors = m_links.values(id);
    for (quint32 connector : connectors)
        refreshConnector(connector);
}

// --- Editing ------------------------------------------------------------------

void Board::begin() {
    if (m_open)
        commit();
    m_open = true;
    m_revisionBefore = m_revision;
}

void Board::touch(quint32 id) {
    if (!m_created.contains(id) && !m_before.contains(id))
        m_before.insert(id, m_items.value(id));
}

void Board::rememberOrder() {
    if (m_orderRemembered)
        return;
    m_orderRemembered = true;
    m_orderBefore = m_order;
}

void Board::hold(quint32 id) {
    if (m_open && m_items.contains(id))
        touch(id);
}

quint32 Board::add(Item item) {
    const bool alone = !m_open;
    if (alone)
        begin();
    item = held(item);
    item.id = nextId(m_lastId, [this] { return idsInUse(); });
    m_created.insert(item.id);
    place(item);
    m_order.append(item.id);
    m_depthsStale = true;
    const quint32 id = item.id;
    emit itemChanged(id, Change::Added);
    refreshConnector(id);
    revise();
    if (alone)
        commit();
    return id;
}

void Board::set(const Item &item) {
    if (item.text.size() > Item::longestText || tooSmall(item) || tooFar(item)) {
        // No more text than an item holds, no box smaller than a file holds,
        // and nothing farther out than it holds, whoever is asking.
        set(held(item));
        return;
    }
    const auto found = m_items.find(item.id);
    if (found == m_items.end() || *found == item)
        return;
    const bool alone = !m_open;
    if (alone)
        begin();
    touch(item.id);
    const Item before = *found;
    unlink(before);
    *found = item;
    link(item);
    // An item that only moved covers what it did, as far over. What a stroke
    // covers takes its whole curve to work out, which is not done again on
    // every move of a drag.
    const bool moved = movedOnly(before, item);
    m_grid.update(item.id, moved ? m_grid.bounds(item.id).translated(item.rect.topLeft() - before.rect.topLeft())
                                 : boundsFor(item));
    m_lastGroup = qMax(m_lastGroup, item.group);
    emit itemChanged(item.id, moved ? Change::Moved : Change::Edited);
    refreshConnector(item.id);
    if (before.rect != item.rect || before.shape != item.shape)
        refreshConnectorsOf(item.id);
    revise();
    if (alone)
        commit();
}

void Board::remove(quint32 id) {
    if (!m_items.contains(id))
        return;
    const bool alone = !m_open;
    if (alone)
        begin();
    // A connector with nothing at one end is litter, so it goes too, in the
    // same step of undo.
    const QVector<quint32> connectors = m_links.values(id);
    for (quint32 connector : connectors)
        remove(connector);
    if (m_created.contains(id)) {
        m_created.remove(id);
    } else {
        touch(id);
        rememberOrder();
    }
    erase(id);
    m_order.removeOne(id);
    m_depthsStale = true;
    emit itemChanged(id, Change::Removed);
    revise();
    if (alone)
        commit();
}

void Board::remove(const QVector<quint32> &ids) {
    // What goes, in the order removing each in turn takes it: an item's
    // connectors, then the item.
    QVector<quint32> going;
    QSet<quint32> gone;
    auto take = [&](quint32 id) {
        if (!gone.contains(id)) {
            gone.insert(id);
            going.append(id);
        }
    };
    for (quint32 id : ids) {
        if (!m_items.contains(id) || gone.contains(id))
            continue;
        const QVector<quint32> connectors = m_links.values(id);
        for (quint32 connector : connectors)
            if (m_items.contains(connector))
                take(connector);
        take(id);
    }
    if (going.isEmpty())
        return;
    const bool alone = !m_open;
    if (alone)
        begin();
    for (quint32 id : std::as_const(going)) {
        if (m_created.contains(id)) {
            m_created.remove(id);
        } else {
            touch(id);
            rememberOrder();
        }
        erase(id);
    }
    m_order.removeIf([&gone](quint32 id) { return gone.contains(id); });
    m_depthsStale = true;
    // Said once they are all gone, so that whoever hears of one finds the
    // board as it is from now on.
    for (quint32 id : std::as_const(going))
        emit itemChanged(id, Change::Removed);
    revise();
    if (alone)
        commit();
}

void Board::restack(const QVector<quint32> &ids, bool toFront) {
    QSet<quint32> moving;
    for (quint32 id : ids)
        if (m_items.contains(id))
            moving.insert(id);
    if (moving.isEmpty())
        return;
    const bool alone = !m_open;
    if (alone)
        begin();
    rememberOrder();
    // They keep the order they had among themselves.
    QVector<quint32> staying, moved;
    for (quint32 id : m_order)
        (moving.contains(id) ? moved : staying).append(id);
    const QVector<quint32> order = toFront ? staying + moved : moved + staying;
    if (order != m_order) {
        for (quint32 id : moved) {
            touch(id);
            m_restacked.insert(id);
        }
        m_order = order;
        m_depthsStale = true;
        emit orderChanged();
        revise();
    }
    if (alone)
        commit();
}

Board::Command Board::close() {
    Command command;
    QHash<quint32, int> depthBefore;
    if (m_orderRemembered)
        for (int i = 0; i < m_orderBefore.size(); ++i)
            depthBefore.insert(m_orderBefore[i], i);

    auto consider = [&](quint32 id, bool existedBefore) {
        Edit edit;
        edit.id = id;
        edit.existedBefore = existedBefore;
        edit.existsAfter = m_items.contains(id);
        if (!edit.existedBefore && !edit.existsAfter)
            return;
        if (edit.existedBefore)
            edit.before = m_before.value(id);
        if (edit.existsAfter)
            edit.after = m_items.value(id);
        edit.restacked = edit.existedBefore && edit.existsAfter && m_restacked.contains(id);
        if (edit.existedBefore && edit.existsAfter && !edit.restacked && edit.before == edit.after)
            return;
        if (edit.existedBefore && (!edit.existsAfter || edit.restacked))
            edit.beforeDepth = depthBefore.value(id, -1);
        if (edit.existsAfter && (!edit.existedBefore || edit.restacked))
            edit.afterDepth = depth(id);
        command.append(edit);
    };
    for (quint32 id : std::as_const(m_created))
        consider(id, false);
    for (auto it = m_before.constBegin(); it != m_before.constEnd(); ++it)
        consider(it.key(), true);

    m_before.clear();
    m_created.clear();
    m_restacked.clear();
    m_orderBefore.clear();
    m_orderRemembered = false;
    m_open = false;
    return command;
}

bool Board::commit() {
    if (!m_open)
        return false;
    // Empty text written as it was while the step was under way, and kept
    // by it, is written as it is from now on: a change to what the file
    // holds.
    const bool standingIn = !writtenInstead().isEmpty();
    const Command command = close();
    if (standingIn)
        revise();
    if (command.isEmpty())
        return false;
    m_undo.append(command);
    if (m_undo.size() > historyLimit)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
    return true;
}

void Board::rollback() {
    if (!m_open)
        return;
    const Command command = close();
    if (!command.isEmpty())
        apply(command, false);
    // Back to just what it held when the step began, which is that revision
    // again: a file written before the step still holds it.
    if (m_revision != m_revisionBefore) {
        m_revision = m_revisionBefore;
        emit modified();
    }
}

Board::StackChange Board::stackChange(const Command &command, bool forward) {
    StackChange stack;
    for (const Edit &edit : command) {
        const bool existed = forward ? edit.existedBefore : edit.existsAfter;
        const bool exists = forward ? edit.existsAfter : edit.existedBefore;
        if (existed && (!exists || edit.restacked))
            stack.leaving.insert(edit.id);
        if (exists && (!existed || edit.restacked))
            stack.arriving.append({forward ? edit.afterDepth : edit.beforeDepth, edit.id});
    }
    std::sort(stack.arriving.begin(), stack.arriving.end());
    return stack;
}

void Board::StackChange::takeFrom(QVector<quint32> &order) const {
    if (!leaving.isEmpty())
        order.removeIf([this](quint32 id) { return leaving.contains(id); });
}

void Board::StackChange::putInto(QVector<quint32> &order) const {
    // They take their places from the back forward, each where it was.
    for (const auto &[at, id] : arriving)
        order.insert(at < 0 ? order.size() : qMin<qsizetype>(at, order.size()), id);
}

void Board::apply(const Command &command, bool forward) {
    auto existsTo = [forward](const Edit &edit) { return forward ? edit.existsAfter : edit.existedBefore; };

    // Items that go away, or change places, leave the stack first.
    const StackChange stack = stackChange(command, forward);
    stack.takeFrom(m_order);

    for (const Edit &edit : command) {
        if (!existsTo(edit)) {
            erase(edit.id);
            emit itemChanged(edit.id, Change::Removed);
            continue;
        }
        const Item &target = forward ? edit.after : edit.before;
        const auto found = m_items.find(edit.id);
        if (found == m_items.end()) {
            place(target);
            emit itemChanged(edit.id, Change::Added);
        } else {
            const Item current = *found;
            unlink(current);
            *found = target;
            link(target);
            m_grid.update(edit.id, boundsFor(target));
            emit itemChanged(edit.id, movedOnly(current, target) ? Change::Moved : Change::Edited);
        }
    }

    stack.putInto(m_order);
    m_depthsStale = true;

    // Connectors follow whatever moved under them. One still attached to an
    // item that went was attached to it by no step of the history, but from
    // outside (see merge()): it is let go of where it was, as it is when the
    // file takes the item away.
    for (const Edit &edit : command) {
        if (!existsTo(edit)) {
            refreshConnectorsOf(edit.id);
            continue;
        }
        refreshConnector(edit.id);
        refreshConnectorsOf(edit.id);
    }
    if (!stack.leaving.isEmpty() || !stack.arriving.isEmpty())
        emit orderChanged();
}

void Board::setMeasuredRect(quint32 id, const QRectF &rect) {
    // A shape grown about its middle can put its top edge past the file's
    // coordinate limit, just as an edit can.
    const QRectF measured(within(rect.topLeft()), rect.size());
    const auto found = m_items.find(id);
    if (found == m_items.end() || found->rect == measured)
        return;
    found->rect = measured;
    m_grid.update(id, boundsFor(*found));
    emit itemChanged(id, Change::Edited);
    refreshConnectorsOf(id);
}

void Board::undo() {
    if (m_open || m_undo.isEmpty())
        return;
    const Command command = m_undo.takeLast();
    apply(command, false);
    m_redo.append(command);
    revise();
    emit historyChanged();
}

void Board::redo() {
    if (m_open || m_redo.isEmpty())
        return;
    const Command command = m_redo.takeLast();
    apply(command, true);
    m_undo.append(command);
    revise();
    emit historyChanged();
}

// What it holds changed: a new revision, by a number it has never had before.
void Board::revise() {
    m_revision = ++m_lastRevision;
    emit modified();
}

// --- Changes from outside ---------------------------------------------------------

// Every id the board has some use for: its items', the ones the step under
// way and the history keep, and those their connectors' ends are on.
QSet<quint32> Board::idsInUse() const {
    QSet<quint32> ids;
    auto use = [&ids](const Item &item) {
        ids.insert(item.id);
        ids.insert(item.from);
        ids.insert(item.to);
    };
    for (const Item &item : m_items)
        use(item);
    for (const Item &item : m_before)
        use(item);
    for (quint32 id : m_created)
        ids.insert(id);
    for (const QVector<Command> *history : {&m_undo, &m_redo})
        for (const Command &command : *history)
            for (const Edit &edit : command) {
                use(edit.before);
                use(edit.after);
            }
    ids.remove(0);
    return ids;
}

void Board::renumber(quint32 from, quint32 to) {
    if (from == 0 || to == 0 || from == to || m_items.contains(to))
        return;
    auto swap = [from, to](quint32 &id) {
        if (id == from)
            id = to;
    };
    // A copy of any item, as anything keeps one: its own id, and the ids of
    // what its ends are on.
    auto swapIn = [&swap](Item &item) {
        swap(item.id);
        swap(item.from);
        swap(item.to);
    };

    // The connectors attached to it, and it when it is one, are linked
    // again under the new id.
    QSet<quint32> joined;
    for (quint32 connector : m_links.values(from))
        joined.insert(connector);
    const bool here = m_items.contains(from);
    if (here && m_items.value(from).kind == Item::Kind::Connector)
        joined.insert(from);
    for (quint32 id : std::as_const(joined))
        unlink(m_items.value(id));
    if (here) {
        Item item = m_items.take(from);
        item.id = to;
        m_items.insert(to, item);
        m_grid.remove(from);
        m_grid.insert(to, boundsFor(item));
        std::replace(m_order.begin(), m_order.end(), from, to);
        m_depthsStale = true;
    }
    QSet<quint32> connectors;
    for (quint32 id : std::as_const(joined)) {
        swap(id);
        Item &connector = m_items[id];
        swapIn(connector);
        link(connector);
        connectors.insert(id);
    }

    // The step under way, and the history, have it by its new id too.
    if (m_before.contains(from))
        m_before.insert(to, m_before.take(from));
    for (Item &item : m_before)
        swapIn(item);
    if (m_created.remove(from))
        m_created.insert(to);
    if (m_restacked.remove(from))
        m_restacked.insert(to);
    std::replace(m_orderBefore.begin(), m_orderBefore.end(), from, to);
    for (QVector<Command> *history : {&m_undo, &m_redo})
        for (Command &command : *history)
            for (Edit &edit : command) {
                swap(edit.id);
                swapIn(edit.before);
                swapIn(edit.after);
            }
    m_lastId = qMax(m_lastId, to);

    emit renumbered(from, to);
    if (here) {
        emit itemChanged(from, Change::Removed);
        emit itemChanged(to, Change::Added);
        emit orderChanged();
    }
    for (quint32 id : std::as_const(connectors))
        if (id != to)
            emit itemChanged(id, Change::Edited);
    // What a step under way would go back to has the new id as well.
    if (m_open)
        m_revisionBefore = ++m_lastRevision;
    revise();
}

QSet<quint32> Board::groupsInUse() const {
    QSet<quint32> groups;
    auto use = [&groups](const Item &item) {
        if (item.group)
            groups.insert(item.group);
    };
    for (const Item &item : m_items)
        use(item);
    for (const Item &item : m_before)
        use(item);
    for (const QVector<Command> *history : {&m_undo, &m_redo})
        for (const Command &command : *history)
            for (const Edit &edit : command) {
                use(edit.before);
                use(edit.after);
            }
    return groups;
}

void Board::regroup(quint32 from, quint32 to) {
    if (from == 0 || to == 0 || from == to)
        return;
    auto move = [from, to](Item &item) {
        if (item.group != from)
            return false;
        item.group = to;
        return true;
    };
    QVector<quint32> changed;
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        if (move(*it))
            changed.append(it.key());
    for (Item &item : m_before)
        move(item);
    for (QVector<Command> *history : {&m_undo, &m_redo})
        for (Command &command : *history)
            for (Edit &edit : command) {
                move(edit.before);
                move(edit.after);
            }
    m_lastGroup = qMax(m_lastGroup, to);
    for (quint32 id : std::as_const(changed))
        emit itemChanged(id, Change::Edited);
    // What a step under way would go back to has the new group as well.
    if (m_open)
        m_revisionBefore = ++m_lastRevision;
    revise();
}

void Board::rebaseHistory(const QSet<quint32> &gone) {
    if (gone.isEmpty())
        return;
    auto removedDepths = [&gone](const QVector<quint32> &order) {
        QVector<int> depths;
        for (int depth = 0; depth < order.size(); ++depth)
            if (gone.contains(order[depth]))
                depths.append(depth);
        return depths;
    };
    auto shift = [](int depth, const QVector<int> &removed) {
        return depth - int(std::lower_bound(removed.cbegin(), removed.cend(), depth) - removed.cbegin());
    };
    // A missing item may have been at a different depth on either side of
    // a step. Walk each history as it was, shifting each saved depth by
    // the number of items that are now gone below it.
    for (QVector<Command> *history : {&m_undo, &m_redo}) {
        const bool forward = history == &m_redo;
        QVector<quint32> order = m_orderRemembered ? m_orderBefore : m_order;
        QVector<int> from = removedDepths(order);
        for (auto it = history->rbegin(); it != history->rend(); ++it) {
            const StackChange stack = stackChange(*it, forward);
            if (stack.leaving.isEmpty() && stack.arriving.isEmpty())
                continue;
            stack.takeFrom(order);
            stack.putInto(order);
            QVector<int> to = removedDepths(order);
            for (Edit &edit : *it) {
                edit.beforeDepth = shift(edit.beforeDepth, forward ? from : to);
                edit.afterDepth = shift(edit.afterDepth, forward ? to : from);
            }
            from = std::move(to);
        }
    }
}

Board::Merged Board::merge(const QVector<Item> &base, const QVector<Item> &theirs, quint32 lastId) {
    Merged merged;
    // Nothing made here is given an id the file gave, to an item it still
    // has or one it took away again.
    m_lastId = qMax(m_lastId, lastId);
    // Groups are numbered as items are, by each that writes the board, from
    // the last it knows of: a group new in the file can have the number of
    // one made here meanwhile, of other items, and the two would be taken
    // for one. The one made here is numbered anew, after every group the
    // file has, as an item is (see renumber()). Were they the same items,
    // the group they end up in is the one here, as anything changed both
    // here and in the file is.
    QSet<quint32> hadGroups, incomingGroups;
    for (const Item &item : base)
        hadGroups.insert(item.group);
    for (const Item &item : theirs) {
        m_lastGroup = qMax(m_lastGroup, item.group);
        if (item.group)
            incomingGroups.insert(item.group);
    }
    const QSet<quint32> newGroups = incomingGroups - hadGroups;
    if (!newGroups.isEmpty()) {
        const QSet<quint32> used = groupsInUse();
        QSet<quint32> reserved = used + incomingGroups;
        QList<quint32> clashing = newGroups.values();
        std::sort(clashing.begin(), clashing.end());
        for (quint32 group : std::as_const(clashing)) {
            if (!used.contains(group))
                continue;
            const quint32 next = nextId(m_lastGroup, [&reserved] { return reserved; }, &m_wrappedGroup);
            reserved.insert(next);
            regroup(group, next);
        }
    }
    // Each item as the file had it and has it now, by id, and in its order.
    QHash<quint32, QJsonObject> was, now;
    QHash<quint32, Item> incoming;
    QVector<quint32> wasOrder, nowOrder;
    for (const Item &item : base) {
        was.insert(item.id, asWritten(item));
        wasOrder.append(item.id);
    }
    quint32 highest = m_lastId;
    for (const Item &item : theirs) {
        now.insert(item.id, asWritten(item));
        nowOrder.append(item.id);
        incoming.insert(item.id, item);
        highest = qMax(highest, item.id);
    }

    // The file's new items keep their ids. What the board has by one of them
    // is numbered anew: nothing it made reached the file under that id.
    // (Unless the file has it just as it is here: then it is no other item.)
    QVector<quint32> fresh;
    for (quint32 id : std::as_const(nowOrder))
        if (!was.contains(id))
            fresh.append(id);
    const QSet<quint32> inUse = fresh.isEmpty() ? QSet<quint32>() : idsInUse();
    for (quint32 id : std::as_const(fresh)) {
        if (!inUse.contains(id))
            continue;
        const Item *mine = item(id);
        if (mine && asWritten(*mine) == now.value(id))
            continue;
        renumber(id, nextId(highest, [&] { return idsInUse() + QSet<quint32>(nowOrder.cbegin(), nowOrder.cend()); }));
    }

    // What the board changed since the file was last read or written, and
    // whatever the step under way is about.
    auto changedHere = [&](quint32 id) {
        if (m_before.contains(id) || m_created.contains(id))
            return true;
        const Item *mine = item(id);
        const auto found = was.constFind(id);
        if (!mine || found == was.constEnd())
            return bool(mine) != (found != was.constEnd());
        return asWritten(*mine) != *found;
    };
    // Everything the file has or had: the new in its order, then the gone.
    QVector<quint32> ids = nowOrder;
    for (quint32 id : std::as_const(wasOrder))
        if (!now.contains(id))
            ids.append(id);
    // What the board takes of what the file changed: items it added, in its
    // order; items it changed, and how; and items it took away.
    QVector<quint32> arriving;
    QHash<quint32, Change> replaced;
    QSet<quint32> gone;
    for (quint32 id : std::as_const(ids)) {
        const auto before = was.constFind(id), after = now.constFind(id);
        const bool had = before != was.constEnd(), has = after != now.constEnd();
        if (had == has && (!has || *before == *after))
            continue;
        if (changedHere(id)) {
            const Item *mine = item(id);
            if (mine ? !has || asWritten(*mine) != *after : has)
                ++merged.kept;
            continue;
        }
        ++merged.taken;
        if (!has) {
            gone.insert(id);
            erase(id);
            continue;
        }
        Item target = incoming.value(id);
        const auto found = m_items.find(id);
        if (found == m_items.end()) {
            arriving.append(id);
            place(target);
            continue;
        }
        const Item current = *found;
        // Text set as it was here is as big as it was measured to be.
        if (target.kind == Item::Kind::Text && current.kind == Item::Kind::Text && target.text == current.text &&
            target.fontSize == current.fontSize && target.wrap == current.wrap && target.bold == current.bold)
            target.rect.setSize(current.rect.size());
        unlink(current);
        *found = target;
        link(target);
        m_grid.update(id, boundsFor(target));
        m_lastGroup = qMax(m_lastGroup, target.group);
        replaced.insert(id, movedOnly(current, target) ? Change::Moved : Change::Edited);
    }

    // The stack: as it is here, less what the file took away, and with what
    // it added on top. What all three have, the file restacked, and the
    // board did not, takes the file's order, each in a place one of them had
    // here.
    rebaseHistory(gone);
    QVector<quint32> order = m_order;
    order.removeIf([&gone](quint32 id) { return gone.contains(id); });
    QSet<quint32> shared;
    for (quint32 id : std::as_const(wasOrder))
        if (now.contains(id) && m_items.contains(id))
            shared.insert(id);
    auto sharedOf = [&shared](const QVector<quint32> &stack) {
        QVector<quint32> found;
        for (quint32 id : stack)
            if (shared.contains(id))
                found.append(id);
        return found;
    };
    const QVector<quint32> there = sharedOf(nowOrder);
    if (there != sharedOf(wasOrder) && sharedOf(order) == sharedOf(wasOrder)) {
        auto next = there.constBegin();
        for (quint32 &id : order)
            if (shared.contains(id))
                id = *next++;
        merged.restacked = true;
    }
    order += arriving;
    const bool restacked = order != m_order;
    if (restacked) {
        m_order = order;
        m_depthsStale = true;
    }
    if (m_orderRemembered) {
        m_orderBefore.removeIf([&gone](quint32 id) { return gone.contains(id); });
        m_orderBefore += arriving;
    }

    // Said once the board is whole again. Connectors follow what moved under
    // them, and one whose item went is left free where it was.
    for (quint32 id : std::as_const(gone))
        emit itemChanged(id, Change::Removed);
    for (quint32 id : std::as_const(arriving))
        emit itemChanged(id, Change::Added);
    for (auto it = replaced.constBegin(); it != replaced.constEnd(); ++it)
        emit itemChanged(it.key(), it.value());
    if (restacked)
        emit orderChanged();
    for (quint32 id : std::as_const(gone))
        refreshConnectorsOf(id);
    for (const QVector<quint32> &changes : {arriving, replaced.keys()})
        for (quint32 id : changes) {
            refreshConnector(id);
            refreshConnectorsOf(id);
        }

    // The history goes on undoing what was done here: what the file changed
    // of an item is in every version of it the history keeps, and what the
    // file took away is out of it.
    bool shortened = false;
    for (QVector<Command> *history : {&m_undo, &m_redo}) {
        for (Command &command : *history) {
            command.removeIf([&gone](const Edit &edit) { return gone.contains(edit.id); });
            for (Edit &edit : command) {
                if (!replaced.contains(edit.id))
                    continue;
                const QJsonObject &then = was[edit.id], &since = now[edit.id];
                if (edit.existedBefore)
                    edit.before = withChanges(edit.before, then, since);
                if (edit.existsAfter)
                    edit.after = withChanges(edit.after, then, since);
            }
        }
        shortened |= history->removeIf([](const Command &command) { return command.isEmpty(); }) > 0;
    }

    if (!arriving.isEmpty() || !replaced.isEmpty() || !gone.isEmpty() || restacked) {
        // What a step under way would go back to has the file's changes too.
        if (m_open)
            m_revisionBefore = ++m_lastRevision;
        revise();
    }
    if (shortened)
        emit historyChanged();

    const QVector<Item> written = itemsToWrite();
    merged.ahead = written.size() != nowOrder.size();
    for (qsizetype i = 0; !merged.ahead && i < written.size(); ++i)
        merged.ahead = written[i].id != nowOrder[i] || asWritten(written[i]) != now.value(nowOrder[i]);
    return merged;
}

// --- Files --------------------------------------------------------------------

QJsonObject Board::itemToJson(const Item &item) {
    QJsonObject object{{"id", qint64(item.id)}, {"type", kindName(item.kind)}};
    auto box = [&] {
        object["x"] = rounded(item.rect.x());
        object["y"] = rounded(item.rect.y());
        object["w"] = rounded(item.rect.width());
        object["h"] = rounded(item.rect.height());
    };
    auto origin = [&] {
        object["x"] = rounded(item.rect.x());
        object["y"] = rounded(item.rect.y());
    };
    switch (item.kind) {
    case Item::Kind::Note:
    case Item::Kind::Frame:
        box();
        break;
    case Item::Kind::Shape:
        box();
        object["shape"] = shapeName(item.shape);
        break;
    case Item::Kind::Image:
        box();
        object["asset"] = item.asset;
        break;
    case Item::Kind::Text:
        origin();
        object["size"] = rounded(item.fontSize);
        if (item.bold)
            object["bold"] = true;
        if (item.wrap > 0)
            object["wrap"] = rounded(item.wrap);
        break;
    case Item::Kind::Stroke: {
        origin();
        object["width"] = rounded(item.width);
        QJsonArray points;
        for (const QPointF &point : item.points) {
            points.append(rounded(point.x()));
            points.append(rounded(point.y()));
        }
        object["points"] = points;
        break;
    }
    case Item::Kind::Connector: {
        if (item.from)
            object["from"] = qint64(item.from);
        if (item.to)
            object["to"] = qint64(item.to);
        // An attached end can reach the edge of a box that extends past
        // the file's coordinate limit. On opening, it finds that edge
        // again. The points kept in the file must be within the limit.
        const QPointF first = within(item.p1), second = within(item.p2);
        object["x1"] = rounded(first.x());
        object["y1"] = rounded(first.y());
        object["x2"] = rounded(second.x());
        object["y2"] = rounded(second.y());
        object["width"] = rounded(item.width);
        // Only what is not what a connector is to begin with.
        if (item.route != Item::Route::Straight)
            object["route"] = routeNames.value(int(item.route));
        if (item.start != Item::Head::None)
            object["start"] = headNames.value(int(item.start));
        if (item.end != Item::Head::Arrow)
            object["end"] = headNames.value(int(item.end));
        if (item.dash != Item::Dash::Solid)
            object["dash"] = dashNames.value(int(item.dash));
        break;
    }
    }
    if (!item.color.isEmpty())
        object["color"] = item.color;
    if (item.hasText() && !item.text.isEmpty())
        object["text"] = item.text;
    if (item.group)
        object["group"] = qint64(item.group);
    return object;
}

QString Board::itemFromJson(const QJsonObject &object, Item &item) {
    const QString type = object.value("type").toString();
    const int kind = kindNames.indexOf(type);
    if (kind < 0)
        return type.isEmpty() ? QString("it has no type") : QString("there is no item type \"%1\"").arg(said(type));
    item = Item();
    item.kind = Item::Kind(kind);

    QString problem;
    // A number that must be finite and, when `least` is given, at least it.
    auto number = [&](const QString &key, bool required, double fallback, double least, double most) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined() && !required)
            return fallback;
        const double read = value.toDouble(std::nan(""));
        if (!value.isDouble() || !std::isfinite(read)) {
            if (problem.isEmpty())
                problem = QString("\"%1\" must be a number").arg(key);
            return fallback;
        }
        if (read < least || read > most) {
            if (problem.isEmpty())
                problem = QString("\"%1\" must be between %2 and %3").arg(key).arg(least).arg(most);
            return fallback;
        }
        return read;
    };
    auto identifier = [&](const QString &key) -> quint32 {
        const QJsonValue value = object.value(key);
        if (value.isUndefined())
            return 0;
        const double read = value.toDouble(-1);
        if (!value.isDouble() || read < 0 || read > mostId || read != std::floor(read)) {
            if (problem.isEmpty())
                problem = QString("\"%1\" must be a whole number").arg(key);
            return 0;
        }
        return quint32(read);
    };
    auto flag = [&](const QString &key, bool fallback) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined())
            return fallback;
        if (!value.isBool() && problem.isEmpty())
            problem = QString("\"%1\" must be true or false").arg(key);
        return value.toBool(fallback);
    };
    auto text = [&](const QString &key) {
        const QJsonValue value = object.value(key);
        if (!value.isUndefined() && !value.isString() && problem.isEmpty())
            problem = QString("\"%1\" must be text").arg(key);
        return value.toString();
    };
    // One of a list of names: which, or `fallback` when the key is not there.
    auto named = [&](const QString &key, const QStringList &names, const char *one, const char *many, int fallback) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined())
            return fallback;
        const int index = int(names.indexOf(value.toString()));
        if (index < 0 && problem.isEmpty())
            problem = QString("there is no %1 \"%2\" (the %3 are %4)")
                          .arg(one, said(value.toString()), many, names.join(", "));
        return qMax(0, index);
    };
    const double far = Item::farthest;
    auto box = [&](double width, double height) {
        const double x = number("x", true, 0, -far, far), y = number("y", true, 0, -far, far);
        // Less than the smallest box across or down is read as the smallest,
        // as a board holds it: 0.1.0 saved pictures far wider than tall with
        // no height at all, and then could not open their boards.
        item.rect = QRectF(x, y, qMax(number("w", false, width, 0, far), smallestBox),
                           qMax(number("h", false, height, 0, far), smallestBox));
    };

    item.id = identifier("id");
    item.group = identifier("group");
    item.color = text("color");
    if (!item.color.isEmpty() && !Item::colorNames().contains(item.color) && problem.isEmpty())
        problem = QString("there is no color \"%1\" (the colors are %2)")
                      .arg(said(item.color), Item::colorNames().join(", "));
    if (item.hasText()) {
        item.text = text("text");
        if (item.text.size() > Item::longestText && problem.isEmpty())
            problem = QString("\"text\" is %1 characters long, and an item holds %2 at most")
                          .arg(item.text.size())
                          .arg(Item::longestText);
    }

    switch (item.kind) {
    case Item::Kind::Note:
        box(Defaults::noteSize, Defaults::noteSize);
        if (item.color.isEmpty())
            item.color = "yellow";
        break;
    case Item::Kind::Frame:
        box(Defaults::frameWidth, Defaults::frameHeight);
        break;
    case Item::Kind::Shape: {
        const QString shape = object.value("shape").toString("rect");
        const int index = shapeNames.indexOf(shape);
        if (index < 0 && problem.isEmpty())
            problem = QString("there is no shape \"%1\" (the shapes are %2)").arg(said(shape), shapeNames.join(", "));
        item.shape = Item::Shape(qMax(0, index));
        const QSizeF size = Defaults::shapeSize(item.shape);
        box(size.width(), size.height());
        break;
    }
    case Item::Kind::Image:
        box(0, 0);
        if (object.value("w").isUndefined() || object.value("h").isUndefined()) {
            if (problem.isEmpty())
                problem = "an image needs \"w\" and \"h\"";
        }
        item.asset = text("asset");
        // Only ever a file of the board's own assets folder.
        if ((item.asset.isEmpty() || item.asset.contains('/') || item.asset.startsWith('.')) && problem.isEmpty())
            problem = "\"asset\" must be the name of a file in the board's assets folder";
        break;
    case Item::Kind::Text:
        item.fontSize = number("size", false, Defaults::textSize, 4, 512);
        item.bold = flag("bold", false);
        item.wrap = number("wrap", false, 0, 0, far);
        item.rect = QRectF(QPointF(number("x", true, 0, -far, far), number("y", true, 0, -far, far)),
                           estimatedTextSize(item));
        break;
    case Item::Kind::Stroke: {
        item.width = number("width", false, Defaults::penWidth, 0.1, 512);
        const QJsonValue points = object.value("points");
        const QJsonArray values = points.toArray();
        if ((!points.isArray() || values.isEmpty() || values.size() % 2 != 0) && problem.isEmpty())
            problem = "\"points\" must be a list of x, y pairs";
        item.points.reserve(values.size() / 2);
        for (int i = 0; i + 1 < values.size(); i += 2) {
            const double x = values[i].toDouble(std::nan("")), y = values[i + 1].toDouble(std::nan(""));
            if ((!std::isfinite(x) || !std::isfinite(y)) && problem.isEmpty())
                problem = "\"points\" must hold only numbers";
            if ((std::abs(x) > far || std::abs(y) > far) && problem.isEmpty())
                problem = QString("\"points\" must be between %1 and %2").arg(-far).arg(far);
            item.points.append(QPointF(x, y));
        }
        // Points are kept relative to the corner of the box around them, and
        // written so, "x" and "y" being that corner: a box that starts further
        // out than a file may say anything is, or is larger than it may say
        // any box is, would be written as a board that could not be read back.
        const QPointF origin(number("x", false, 0, -far, far), number("y", false, 0, -far, far));
        const QRectF bounds = Geo::boundsOf(item.points);
        for (QPointF &point : item.points)
            point -= bounds.topLeft();
        item.rect = bounds.translated(origin);
        if ((bounds.width() > far || bounds.height() > far) && problem.isEmpty())
            problem = QString("the box around \"points\" must be at most %1 across and down").arg(far);
        if ((std::abs(item.rect.x()) > far || std::abs(item.rect.y()) > far) && problem.isEmpty())
            problem = QString("the box around \"points\", moved by \"x\" and \"y\", must start between %1 and %2")
                          .arg(-far)
                          .arg(far);
        break;
    }
    case Item::Kind::Connector:
        item.from = identifier("from");
        item.to = identifier("to");
        item.p1 = QPointF(number("x1", item.from == 0, 0, -far, far), number("y1", item.from == 0, 0, -far, far));
        item.p2 = QPointF(number("x2", item.to == 0, 0, -far, far), number("y2", item.to == 0, 0, -far, far));
        item.width = number("width", false, Defaults::connectorWidth, 0.1, 512);
        item.route = Item::Route(named("route", routeNames, "route", "routes", int(Item::Route::Straight)));
        item.start = Item::Head(named("start", headNames, "end", "ends", int(Item::Head::None)));
        // Boards from before connectors had a choice of ends say only
        // whether there is an arrow.
        item.end = Item::Head(named("end", headNames, "end", "ends",
                                    flag("arrow", true) ? int(Item::Head::Arrow) : int(Item::Head::None)));
        item.dash = Item::Dash(named("dash", dashNames, "line", "lines", int(Item::Dash::Solid)));
        item.rect = QRectF(item.p1, item.p2).normalized();
        item.sideways = std::abs(item.p2.x() - item.p1.x()) >= std::abs(item.p2.y() - item.p1.y());
        break;
    }
    return problem;
}

QStringList Board::parse(const QJsonObject &root, QVector<Item> &items) {
    QStringList problems;
    const QJsonValue version = root.value("sprawl");
    if (!version.isDouble())
        return {"It is not a Sprawl board: there is no \"sprawl\" format number."};
    if (newer(root))
        return {QString("It was made by a newer Sprawl (format %1, and this one reads up to %2).")
                    .arg(version.toInt())
                    .arg(formatVersion)};
    const QJsonValue list = root.value("items");
    if (!list.isUndefined() && !list.isArray())
        return {"\"items\" must be a list."};

    const QJsonArray array = list.toArray();
    QHash<quint32, Item::Kind> kinds;
    items.clear();
    items.reserve(array.size());
    for (int i = 0; i < array.size(); ++i) {
        Item item;
        const QString problem =
            array[i].isObject() ? itemFromJson(array[i].toObject(), item) : QString("it must be an object");
        if (!problem.isEmpty()) {
            problems << QString("Item %1: %2.").arg(i + 1).arg(problem);
            continue;
        }
        if (item.id) {
            if (kinds.contains(item.id)) {
                problems << QString("Item %1: the id %2 is used twice.").arg(i + 1).arg(item.id);
                continue;
            }
            kinds.insert(item.id, item.kind);
        }
        items.append(item);
    }
    for (const Item &item : std::as_const(items)) {
        if (item.kind != Item::Kind::Connector)
            continue;
        for (quint32 end : {item.from, item.to}) {
            if (end == 0)
                continue;
            if (!kinds.contains(end))
                problems << QString("Connector %1: there is no item %2 to attach to.").arg(item.id).arg(end);
            else if (kinds.value(end) == Item::Kind::Stroke || kinds.value(end) == Item::Kind::Connector)
                problems << QString("Connector %1: item %2 is a %3, which a connector cannot attach to.")
                                .arg(item.id)
                                .arg(end)
                                .arg(kindName(kinds.value(end)));
        }
    }
    return problems;
}

// Text being written that holds nothing yet, or nothing any more, is
// written as it was before the step began. So a file is never given text
// with nothing to see in it, and nothing is lost if the app ends in the
// middle: emptied to be typed again, it is written as it was; made by the
// step, it is not written at all, nor is anything attached to it, which
// would point at nothing. Text that held nothing before the step is written
// as it is, as the file has it already. By id, what is written in place of
// each: the item as it was, or nothing.
QHash<quint32, const Item *> Board::writtenInstead() const {
    QHash<quint32, const Item *> instead;
    if (!m_open)
        return instead;
    auto blank = [](const Item *item) {
        return item && item->kind == Item::Kind::Text && item->text.trimmed().isEmpty();
    };
    for (auto it = m_before.constBegin(); it != m_before.constEnd(); ++it)
        if (blank(item(it.key())) && !blank(&*it))
            instead.insert(it.key(), &*it);
    for (quint32 id : m_created) {
        if (!blank(item(id)))
            continue;
        instead.insert(id, nullptr);
        for (quint32 connector : m_links.values(id))
            instead.insert(connector, nullptr);
    }
    return instead;
}

QVector<Item> Board::items() const {
    QVector<Item> items;
    items.reserve(m_order.size());
    for (quint32 id : m_order)
        items.append(m_items.value(id));
    return items;
}

QVector<Item> Board::itemsToWrite() const {
    const QHash<quint32, const Item *> instead = writtenInstead();
    if (instead.isEmpty())
        return items();
    QVector<Item> items;
    items.reserve(m_order.size());
    for (quint32 id : m_order) {
        const auto found = instead.constFind(id);
        if (found == instead.constEnd())
            items.append(m_items.value(id));
        else if (*found)
            items.append(**found);
    }
    return items;
}

QJsonObject Board::toJson(const QVector<Item> &items, quint32 lastId) {
    QJsonArray array;
    quint32 highest = 0;
    for (const Item &item : items) {
        array.append(itemToJson(item));
        highest = qMax(highest, item.id);
    }
    QJsonObject root{{"sprawl", formatVersion}, {"items", array}};
    // Only when the items do not say it themselves, so a board that never had
    // its last item taken away is written as it always was.
    if (lastId > highest)
        root.insert("lastId", qint64(lastId));
    return root;
}

QStringList Board::read(const QJsonObject &root, QVector<Item> &items, bool *numbered, quint32 *lastId) {
    if (numbered)
        *numbered = false;
    const QStringList problems = parse(root, items);
    if (!problems.isEmpty())
        return problems;
    quint32 last = lastIdOf(root);
    for (const Item &item : std::as_const(items))
        last = qMax(last, item.id);
    // An item written without an id (by hand, by an agent) gets one, and
    // never one an item taken away had.
    for (Item &item : items)
        if (item.id == 0) {
            item.id = nextId(last, [&items] {
                QSet<quint32> ids;
                for (const Item &each : items)
                    ids.insert(each.id);
                return ids;
            });
            if (numbered)
                *numbered = true;
        }
    if (lastId)
        *lastId = last;
    return {};
}

QStringList Board::fromJson(const QJsonObject &root, bool *numbered) {
    QVector<Item> items;
    quint32 last = 0;
    const QStringList problems = read(root, items, numbered, &last);
    if (!problems.isEmpty())
        return problems;

    m_items.clear();
    m_order.clear();
    m_grid.clear();
    m_links.clear();
    m_undo.clear();
    m_redo.clear();
    m_before.clear();
    m_created.clear();
    m_restacked.clear();
    m_orderBefore.clear();
    m_orderRemembered = false;
    m_open = false;
    m_lastId = m_lastGroup = m_wrappedGroup = 0;
    for (const Item &item : std::as_const(items)) {
        place(item);
        m_order.append(item.id);
    }
    m_lastId = qMax(m_lastId, last);
    m_depthsStale = true;
    for (quint32 id : std::as_const(m_order))
        refreshConnector(id);
    m_revision = ++m_lastRevision;
    emit reset();
    emit historyChanged();
    return {};
}

void Board::clear() { fromJson({{"sprawl", formatVersion}}); }
