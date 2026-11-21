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
    // The same for any item. Where text beside an item goes (a connector's
    // label, a frame's title) only what lays out its type can say exactly:
    // `laidOut` is where that put it, and without it the board guesses.
    static QRectF boundsFor(const Item &item, const QRectF &laidOut = {});
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
    quint32 newGroup();
    // The last id given to an item, here or by whatever wrote the file,
    // whether the board still has that item or not: one taken away is never
    // given to another, which would be taken for it (see toJson()).
    quint32 lastId() const { return m_lastId; }
    // Changes whenever what the board holds does, in the middle of a step
    // too, and whenever what is written of it (itemsToWrite()) does, so a
    // save knows what it saved. The same number is the same board: a step
    // rolled back gives back the number it began with, and a number once
    // given is never given to anything else.
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
    // Puts a new item on top and returns the id it was given. Here and in
    // set(), the item is held as a file can have it, whatever it was given
    // as: with no more text than an item holds, no box (a note, a shape, a
    // picture, a frame) less than a unit across or down, and no corner or
    // end of a connector farther out than Item::farthest.
    quint32 add(Item item);
    void set(const Item &item);
    // Also removes the connectors attached to the item.
    void remove(quint32 id);
    // The same of many: as removing each in turn would, but out of the
    // stack in one go rather than one look through it for each.
    void remove(const QVector<quint32> &ids);
    void restack(const QVector<quint32> &ids, bool toFront);
    // The step under way is about this item, whether or not it has changed
    // it yet: it is being written in, or dragged. What comes in from outside
    // leaves it as it is here (see merge()).
    void hold(quint32 id);
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

    // --- Changes from outside ------------------------------------------------

    // What taking in a file came to.
    struct Merged {
        // Items the file changed that the board now has as the file does.
        int taken = 0;
        // Items changed both here and in the file, kept as they are here.
        int kept = 0;
        // The file stacked the items it shares with the board another way,
        // which the board now has too.
        bool restacked = false;
        // The board holds what the file does not, its own changes, which are
        // still to be written to it.
        bool ahead = false;
    };
    // Takes in what was written to the board's file from outside since
    // `base`, the file as this board last read or wrote it, to `theirs`, the
    // file now, which has given ids up to `lastId` (all as read() has them).
    // An item the file changed and the board did not comes as the file has
    // it; one both changed stays as it is here, as does whatever the step
    // under way is about, which goes on undisturbed. Items new in the file go
    // on top, in its order; the stack is otherwise as it is here, unless only
    // the file changed it. An item made here that is not in the file yet,
    // whose id the file now gives something else, is the one given a new id:
    // nothing outside knows it by its id, and the file's have been printed by
    // the commands that wrote them. None of it is a step of undo, and the
    // history goes on undoing what was done here.
    Merged merge(const QVector<Item> &base, const QVector<Item> &theirs, quint32 lastId = 0);
    // Gives an item the id `to`, one the board has no use for, everywhere it
    // is known by `from`: the board, the stack, the connectors attached to
    // it, the step under way, and the history.
    void renumber(quint32 from, quint32 to);
    // The same of a group: the items in group `from`, here, in the step
    // under way, and in the history, are in group `to` from now on.
    void regroup(quint32 from, quint32 to);

    // --- Files ---------------------------------------------------------------

    static constexpr int formatVersion = 1;
    // A file made by a newer Sprawl, in a format this one does not read.
    static bool newer(const QJsonObject &root) { return root.value("sprawl").toInt() > formatVersion; }
    QJsonObject toJson() const { return toJson(itemsToWrite(), m_lastId); }
    // The items back to front: a copy that is cheap to take and safe to hand
    // to another thread.
    QVector<Item> items() const;
    // The same, as a file is to have them, which is how boards are saved
    // without a stutter. Text being written that holds nothing is written as
    // it was before the step began: emptied, as it was, and new, not at all.
    QVector<Item> itemsToWrite() const;
    // A file of the items, which keeps `lastId` (see lastId()) when it is
    // later than every id they have: an item that had it was taken away.
    static QJsonObject toJson(const QVector<Item> &items, quint32 lastId = 0);
    // Replaces the board with the one in the file. Returns what is wrong
    // with the file, leaving the board alone, or nothing when it loaded.
    // `numbered`, when asked for, says whether it gave ids to items written
    // without one (see read()).
    QStringList fromJson(const QJsonObject &root, bool *numbered = nullptr);
    void clear();

    static QJsonObject itemToJson(const Item &item);
    static QString itemFromJson(const QJsonObject &object, Item &item);
    // Reads a board file, collecting everything wrong with it.
    static QStringList parse(const QJsonObject &root, QVector<Item> &items);
    // The same, the items as a board holds them: every one with an id, those
    // written without one numbered after every id the file has given, in the
    // order they come. `numbered`, when asked for, says whether there were
    // any, and `lastId` is the last id given then.
    static QStringList read(const QJsonObject &root, QVector<Item> &items, bool *numbered = nullptr,
                            quint32 *lastId = nullptr);
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
    // An item was given a new id (see renumber()). Said before anything else
    // is said of it: then it is removed under the old id and added under the
    // new one.
    void renumbered(quint32 from, quint32 to);
    void orderChanged();
    // The whole board was replaced.
    void reset();
    // What it holds changed, or what is written of it, and with it its
    // revision: as each change is made, not only when a step ends.
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
    struct StackChange {
        QSet<quint32> leaving;
        QVector<QPair<int, quint32>> arriving;

        void takeFrom(QVector<quint32> &order) const;
        void putInto(QVector<quint32> &order) const;
    };

    void touch(quint32 id);
    void rememberOrder();
    void place(const Item &item);
    void erase(quint32 id);
    void link(const Item &item);
    void unlink(const Item &item);
    void refreshConnector(quint32 id);
    void refreshConnectorsOf(quint32 id);
    Command close();
    static StackChange stackChange(const Command &command, bool forward);
    void apply(const Command &command, bool forward);
    void rebaseHistory(const QSet<quint32> &gone);
    void revise();
    QHash<quint32, const Item *> writtenInstead() const;
    QSet<quint32> idsInUse() const;
    QSet<quint32> groupsInUse() const;

    QHash<quint32, Item> m_items;
    QVector<quint32> m_order;
    mutable QHash<quint32, int> m_depths;
    mutable bool m_depthsStale = true;
    SpatialGrid m_grid;
    // Item -> the connectors attached to it.
    QMultiHash<quint32, quint32> m_links;
    quint32 m_lastId = 0, m_lastGroup = 0;
    quint32 m_wrappedGroup = 0;
    quint64 m_revision = 0, m_lastRevision = 0;

    bool m_open = false;
    quint64 m_revisionBefore = 0;
    QHash<quint32, Item> m_before;
    QSet<quint32> m_created, m_restacked;
    QVector<quint32> m_orderBefore;
    bool m_orderRemembered = false;

    QVector<Command> m_undo, m_redo;
};
