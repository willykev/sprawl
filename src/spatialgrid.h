#pragma once

#include <QHash>
#include <QRectF>
#include <QVector>

// Finds the items near a point or inside an area without looking at every
// item on the board: the canvas is cut into square cells, and each cell
// remembers the items whose bounds touch it. Hit tests, marquee selection
// and working out what is on screen all stay quick on a board of any size.
class SpatialGrid {
public:
    void clear();
    void insert(quint32 id, const QRectF &bounds);
    void remove(quint32 id);
    void update(quint32 id, const QRectF &bounds);

    bool contains(quint32 id) const { return m_entries.contains(id); }
    QRectF bounds(quint32 id) const { return m_entries.value(id).bounds; }
    int count() const { return int(m_entries.size()); }

    // The ids of every item whose bounds intersect the area, in no order.
    void query(const QRectF &area, QVector<quint32> &found) const;

private:
    struct Span {
        int left = 0, top = 0, right = -1, bottom = -1;
        bool operator==(const Span &other) const = default;
    };
    struct Entry {
        QRectF bounds;
        Span span;
        bool large = false;
    };

    static Span spanOf(const QRectF &bounds);
    static quint64 key(int x, int y) { return quint64(quint32(x)) << 32 | quint32(y); }
    void link(quint32 id, const Entry &entry);
    void unlink(quint32 id, const Entry &entry);

    QHash<quint32, Entry> m_entries;
    QHash<quint64, QVector<quint32>> m_cells;
    // Items too big to list in every cell they cover are checked one by one.
    QVector<quint32> m_large;
};
