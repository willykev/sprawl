#include "board.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

#include "geometry.h"

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

QRectF Board::boundsFor(const Item &item) {
    switch (item.kind) {
    case Item::Kind::Stroke: {
        const qreal reach = item.width / 2 + 1;
        return item.rect.adjusted(-reach, -reach, reach, reach);
    }
    case Item::Kind::Connector: {
        // Room for what is at its ends, and for the label beside the middle.
        const qreal reach = 8 + 3 * item.width;
        QRectF bounds = item.rect.adjusted(-reach, -reach, reach, reach);
        if (!item.text.isEmpty()) {
            const qreal half = item.text.size() * 3.5 + 16;
            const QPointF middle = item.rect.center();
            bounds |= QRectF(middle.x() - half, middle.y() - 24, half * 2, 48);
        }
        return bounds;
    }
    case Item::Kind::Frame:
        // The title sits on the top edge.
        return item.rect.adjusted(-1, -10, 1, 1);
    default:
        return item.rect;
    }
}

QRectF Board::contentBounds() const {
    QRectF all;
    for (const Item &item : m_items)
        all = all.isNull() ? boundsFor(item) : all | boundsFor(item);
    return all;
}

QRectF Board::contentBounds(const QVector<quint32> &ids) const {
    QRectF all;
    for (quint32 id : ids)
        if (const Item *found = item(id))
            all = all.isNull() ? boundsFor(*found) : all | boundsFor(*found);
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
        return Geo::polylineIntersectsRect(item.points, area.translated(-item.rect.topLeft()));
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

quint32 Board::add(Item item) {
    const bool alone = !m_open;
    if (alone)
        begin();
    item.text = Item::fitted(item.text);
    item.id = ++m_lastId;
    m_created.insert(item.id);
    place(item);
    m_order.append(item.id);
    m_depthsStale = true;
    const quint32 id = item.id;
    emit itemChanged(id, Change::Added);
    refreshConnector(id);
    if (alone)
        commit();
    return id;
}

void Board::set(const Item &item) {
    if (item.text.size() > Item::longestText) {
        // No more text than an item holds, whoever is asking.
        Item fitted = item;
        fitted.text = Item::fitted(item.text);
        set(fitted);
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
    m_grid.update(item.id, boundsFor(item));
    m_lastGroup = qMax(m_lastGroup, item.group);
    emit itemChanged(item.id, movedOnly(before, item) ? Change::Moved : Change::Edited);
    refreshConnector(item.id);
    if (before.rect != item.rect || before.shape != item.shape)
        refreshConnectorsOf(item.id);
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
    const Command command = close();
    if (command.isEmpty())
        return false;
    m_undo.append(command);
    if (m_undo.size() > historyLimit)
        m_undo.removeFirst();
    m_redo.clear();
    changedSinceSave();
    emit historyChanged();
    return true;
}

void Board::rollback() {
    if (!m_open)
        return;
    const Command command = close();
    if (!command.isEmpty())
        apply(command, false);
}

void Board::apply(const Command &command, bool forward) {
    auto existsFrom = [forward](const Edit &edit) { return forward ? edit.existedBefore : edit.existsAfter; };
    auto existsTo = [forward](const Edit &edit) { return forward ? edit.existsAfter : edit.existedBefore; };

    // Items that go away, or change places, leave the stack first.
    QSet<quint32> leaving;
    for (const Edit &edit : command)
        if (existsFrom(edit) && (!existsTo(edit) || edit.restacked))
            leaving.insert(edit.id);
    if (!leaving.isEmpty())
        m_order.removeIf([&leaving](quint32 id) { return leaving.contains(id); });

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

    // Then those that arrive take their places, from the back forward, which
    // puts every one of them exactly where it was.
    QVector<QPair<int, quint32>> arriving;
    for (const Edit &edit : command)
        if (existsTo(edit) && (!existsFrom(edit) || edit.restacked))
            arriving.append({forward ? edit.afterDepth : edit.beforeDepth, edit.id});
    std::sort(arriving.begin(), arriving.end());
    for (const auto &[at, id] : std::as_const(arriving))
        m_order.insert(at < 0 ? m_order.size() : qMin<qsizetype>(at, m_order.size()), id);
    m_depthsStale = true;

    // Connectors follow whatever moved under them.
    for (const Edit &edit : command) {
        if (!existsTo(edit))
            continue;
        refreshConnector(edit.id);
        refreshConnectorsOf(edit.id);
    }
    if (!leaving.isEmpty() || !arriving.isEmpty())
        emit orderChanged();
}

void Board::setMeasuredRect(quint32 id, const QRectF &rect) {
    const auto found = m_items.find(id);
    if (found == m_items.end() || found->rect == rect)
        return;
    found->rect = rect;
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
    changedSinceSave();
    emit historyChanged();
}

void Board::redo() {
    if (m_open || m_redo.isEmpty())
        return;
    const Command command = m_redo.takeLast();
    apply(command, true);
    m_undo.append(command);
    changedSinceSave();
    emit historyChanged();
}

void Board::changedSinceSave() {
    ++m_revision;
    emit modified();
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
    case Item::Kind::Connector:
        if (item.from)
            object["from"] = qint64(item.from);
        if (item.to)
            object["to"] = qint64(item.to);
        object["x1"] = rounded(item.p1.x());
        object["y1"] = rounded(item.p1.y());
        object["x2"] = rounded(item.p2.x());
        object["y2"] = rounded(item.p2.y());
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
        if (!value.isDouble() || read < 0 || read > 4e9 || read != std::floor(read)) {
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
    const double far = 1e9;
    auto box = [&](double width, double height) {
        const double x = number("x", true, 0, -far, far), y = number("y", true, 0, -far, far);
        item.rect = QRectF(x, y, number("w", false, width, 1, far), number("h", false, height, 1, far));
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
            if ((!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > far || std::abs(y) > far) && problem.isEmpty())
                problem = "\"points\" must hold only numbers";
            item.points.append(QPointF(x, y));
        }
        // Points are kept relative to the corner of the box around them.
        const QPointF origin(number("x", false, 0, -far, far), number("y", false, 0, -far, far));
        const QRectF bounds = Geo::boundsOf(item.points);
        for (QPointF &point : item.points)
            point -= bounds.topLeft();
        item.rect = bounds.translated(origin);
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
    if (version.toInt() > formatVersion)
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

QVector<Item> Board::items() const {
    QVector<Item> items;
    items.reserve(m_order.size());
    for (quint32 id : m_order)
        items.append(m_items.value(id));
    return items;
}

QJsonObject Board::toJson(const QVector<Item> &items) {
    QJsonArray array;
    for (const Item &item : items)
        array.append(itemToJson(item));
    return {{"sprawl", formatVersion}, {"items", array}};
}

QStringList Board::fromJson(const QJsonObject &root) {
    QVector<Item> items;
    const QStringList problems = parse(root, items);
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
    m_open = false;
    m_lastId = m_lastGroup = 0;
    for (const Item &item : std::as_const(items))
        m_lastId = qMax(m_lastId, item.id);
    for (Item &item : items) {
        // An item written without an id (by hand, by an agent) gets one.
        if (item.id == 0)
            item.id = ++m_lastId;
        place(item);
        m_order.append(item.id);
    }
    m_depthsStale = true;
    for (quint32 id : std::as_const(m_order))
        refreshConnector(id);
    ++m_revision;
    emit reset();
    emit historyChanged();
    return {};
}

void Board::clear() { fromJson({{"sprawl", formatVersion}}); }
