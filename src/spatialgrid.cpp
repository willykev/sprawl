#include "spatialgrid.h"

#include <algorithm>
#include <cmath>

namespace {
// About three notes across: small enough that a cell holds few items, big
// enough that most items sit in one to four cells.
const qreal cellSize = 512;
// An item covering more cells than this goes on the short list instead.
const int largeSpan = 64;

bool touches(const QRectF &a, const QRectF &b) {
    // Unlike QRectF::intersects, a line or a point (a rect with no width or
    // height) still counts.
    return a.left() <= b.right() && b.left() <= a.right() && a.top() <= b.bottom() && b.top() <= a.bottom();
}
} // namespace

SpatialGrid::Span SpatialGrid::spanOf(const QRectF &bounds) {
    return {int(std::floor(bounds.left() / cellSize)), int(std::floor(bounds.top() / cellSize)),
            int(std::floor(bounds.right() / cellSize)), int(std::floor(bounds.bottom() / cellSize))};
}

void SpatialGrid::clear() {
    m_entries.clear();
    m_cells.clear();
    m_large.clear();
}

void SpatialGrid::link(quint32 id, const Entry &entry) {
    if (entry.large) {
        m_large.append(id);
        return;
    }
    for (int y = entry.span.top; y <= entry.span.bottom; ++y)
        for (int x = entry.span.left; x <= entry.span.right; ++x)
            m_cells[key(x, y)].append(id);
}

void SpatialGrid::unlink(quint32 id, const Entry &entry) {
    if (entry.large) {
        m_large.removeOne(id);
        return;
    }
    for (int y = entry.span.top; y <= entry.span.bottom; ++y)
        for (int x = entry.span.left; x <= entry.span.right; ++x) {
            const auto cell = m_cells.find(key(x, y));
            if (cell == m_cells.end())
                continue;
            cell->removeOne(id);
            if (cell->isEmpty())
                m_cells.erase(cell);
        }
}

void SpatialGrid::insert(quint32 id, const QRectF &bounds) {
    remove(id);
    Entry entry;
    entry.bounds = bounds;
    entry.span = spanOf(bounds);
    entry.large = qint64(entry.span.right - entry.span.left + 1) * (entry.span.bottom - entry.span.top + 1) > largeSpan;
    link(id, entry);
    m_entries.insert(id, entry);
}

void SpatialGrid::remove(quint32 id) {
    const auto entry = m_entries.constFind(id);
    if (entry == m_entries.constEnd())
        return;
    unlink(id, *entry);
    m_entries.erase(entry);
}

void SpatialGrid::update(quint32 id, const QRectF &bounds) {
    const auto entry = m_entries.find(id);
    if (entry == m_entries.end()) {
        insert(id, bounds);
        return;
    }
    // Most moves stay within the same cells, and then only the bounds change.
    const Span span = spanOf(bounds);
    const bool large = qint64(span.right - span.left + 1) * (span.bottom - span.top + 1) > largeSpan;
    if (span == entry->span && large == entry->large) {
        entry->bounds = bounds;
        return;
    }
    insert(id, bounds);
}

void SpatialGrid::query(const QRectF &area, QVector<quint32> &found) const {
    found.clear();
    for (quint32 id : m_large) {
        const auto entry = m_entries.constFind(id);
        if (entry != m_entries.constEnd() && touches(entry->bounds, area))
            found.append(id);
    }

    const Span span = spanOf(area);
    const qint64 cells = qint64(span.right - span.left + 1) * (span.bottom - span.top + 1);
    if (cells > qint64(m_cells.size()) * 2) {
        // An area wider than the whole board: looking at each item once is
        // quicker than visiting mostly empty cells.
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
            if (!it->large && touches(it->bounds, area))
                found.append(it.key());
        return;
    }
    const bool oneCell = cells == 1;
    for (int y = span.top; y <= span.bottom; ++y)
        for (int x = span.left; x <= span.right; ++x) {
            const auto cell = m_cells.constFind(key(x, y));
            if (cell == m_cells.constEnd())
                continue;
            for (quint32 id : *cell) {
                const auto entry = m_entries.constFind(id);
                if (entry != m_entries.constEnd() && touches(entry->bounds, area))
                    found.append(id);
            }
        }
    if (!oneCell) {
        // An item in several of the cells was found once per cell.
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
    }
}
