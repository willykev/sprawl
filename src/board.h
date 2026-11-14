#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>

#include "item.h"
#include "spatialgrid.h"

// A board: its items, the order they are stacked in, and the history of what
// was done to them. Everything that changes a board goes through begin() and
// commit(), which is one step of undo however many items it touched.
class Board : public QObject {
    Q_OBJECT

public:
    enum class Change { Added, Removed, Moved, Edited };
    Q_ENUM(Change)

    explicit Board(QObject *parent = nullptr);

    // --- Reading -------------------------------------------------------------

    int count() const { return int(m_order.size()); }
    const Item *item(quint32 id) const;
    // Back to front.
    const QVector<quint32> &order() const { return m_order; }
    int depth(quint32 id) const;
    // What the item covers when drawn, a little more than its rect.
    QRectF bounds(quint32 id) const { return m_grid.bounds(id); }
    QRectF contentBounds() const;
    QRectF contentBounds(const QVector<quint32> &ids) const;
    void itemsIn(const QRectF &area, QVector<quint32> &found) const;
    // The item in front at a point, among those `accept` allows. `slack` is
    // how far off a thin thing (a stroke, a connector, a frame's edge) may
    // be and still count.
    quint32 itemAt(QPointF point, qreal slack, const std::function<bool(const Item &)> &accept = {}) const;
    bool hits(const Item &item, QPointF point, qreal slack) const;
    bool touches(const Item &item, const QRectF &area) const;
    QVector<quint32> connectorsOf(quint32 id) const { return m_links.values(id); }
    QVector<quint32> membersOf(quint32 group) const;
    quint32 newGroup() { return ++m_lastGroup; }
    // Changes whenever the board does, so a save knows what it saved.
    quint64 revision() const { return m_revision; }

    struct Ends {
        QPointF from, to;
        bool sideways = true;
    };
    // Where a connector's two ends are right now: on the edges of the items
    // they are attached to, facing each other. A straight one leaves each
    // item toward the middle of the other; a curved or an elbow one leaves
    // the middle of the side that faces the other, sideways or up and down.
    Ends connectorEnds(const Item &connector) const;
    static bool canAttach(const Item &item) {
        return item.kind != Item::Kind::Stroke && item.kind != Item::Kind::Connector;
    }

    // --- Editing -------------------------------------------------------------

    void begin();
    bool isOpen() const { return m_open; }
    // Puts a new item on top and returns the id it was given.
    quint32 add(Item item);
    void set(const Item &item);
    // Also removes the connectors attached to the item.
    void remove(quint32 id);
    void restack(const QVector<quint32> &ids, bool toFront);
    // Ends the step. False when nothing actually changed.
    bool commit();
    void rollback();

    // A measured size (text fits its type) is not an edit: it is not undone
    // and does not make the board unsaved.
    void setMeasuredRect(quint32 id, const QRectF &rect);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();

    // --- Files ---------------------------------------------------------------

    static constexpr int formatVersion = 1;
    QJsonObject toJson() const { return toJson(items()); }
    // The items back to front: a copy that is cheap to take and safe to hand
    // to another thread, which is how boards are saved without a stutter.
    QVector<Item> items() const;
    static QJsonObject toJson(const QVector<Item> &items);
    // Replaces the board with the one in the file. Returns what is wrong
    // with the file, leaving the board alone, or nothing when it loaded.
    QStringList fromJson(const QJsonObject &root);
    void clear();

    static QJsonObject itemToJson(const Item &item);
    static QString itemFromJson(const QJsonObject &object, Item &item);
    // Reads a board file, collecting everything wrong with it.
    static QStringList parse(const QJsonObject &root, QVector<Item> &items);
    static QString kindName(Item::Kind kind);
    static QString shapeName(Item::Shape shape);
    // Every shape's name, in the order they are offered in; and the same
    // for the routes a connector can take, its ends, and its lines.
    static QStringList shapes();
    static QStringList routes();
    static QStringList heads();
    static QStringList dashes();

signals:
    void itemChanged(quint32 id, Board::Change change);
    void orderChanged();
    // The whole board was replaced.
    void reset();
    void modified();
    void historyChanged();

private:
    struct Edit {
        quint32 id = 0;
        bool existedBefore = false, existsAfter = false, restacked = false;
        int beforeDepth = -1, afterDepth = -1;
        Item before, after;
    };
    using Command = QVector<Edit>;

    void touch(quint32 id);
    void rememberOrder();
    void place(const Item &item);
    void erase(quint32 id);
    void link(const Item &item);
    void unlink(const Item &item);
    void refreshConnector(quint32 id);
    void refreshConnectorsOf(quint32 id);
    static QRectF boundsFor(const Item &item);
    Command close();
    void apply(const Command &command, bool forward);
    void changedSinceSave();

    QHash<quint32, Item> m_items;
    QVector<quint32> m_order;
    mutable QHash<quint32, int> m_depths;
    mutable bool m_depthsStale = true;
    SpatialGrid m_grid;
    // Item -> the connectors attached to it.
    QMultiHash<quint32, quint32> m_links;
    quint32 m_lastId = 0, m_lastGroup = 0;
    quint64 m_revision = 0;

    bool m_open = false;
    QHash<quint32, Item> m_before;
    QSet<quint32> m_created, m_restacked;
    QVector<quint32> m_orderBefore;
    bool m_orderRemembered = false;

    QVector<Command> m_undo, m_redo;
};
