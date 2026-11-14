#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QTextLayout>
#include <memory>
#include <vector>

#include "item.h"
#include "theme.h"

class Board;
class QPainter;

// How an item's text is set: its type, its colors, and the box it sits in.
struct TextStyle {
    QFont font;
    // Notes and shapes write their first line as a title: bolder, a size up.
    bool titled = false;
    qreal titleSize = 0;
    QColor color, titleColor;
    Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignTop;
    // The box, relative to the item's corner. A width of 0 never wraps.
    QRectF box;

    bool wraps() const { return box.width() > 0; }
};

// Text laid out once and drawn by both the canvas (as scene graph glyphs)
// and the exporter (with a painter), so a board looks the same on screen and
// in a file. Lines break exactly as they do in the editor that opens over
// the item, which lays out with the same engine.
class TextBlock {
public:
    TextBlock(const QString &text, const TextStyle &style);

    bool isEmpty() const { return m_paragraphs.empty(); }
    QSizeF size() const { return m_size; }
    // Where the block's corner goes for it to sit in the style's box.
    QPointF origin() const { return m_origin; }
    void draw(QPainter *painter, QPointF itemCorner) const;

    struct Paragraph {
        std::unique_ptr<QTextLayout> layout;
        qreal y = 0;
    };
    const std::vector<Paragraph> &paragraphs() const { return m_paragraphs; }

private:
    std::vector<Paragraph> m_paragraphs;
    QSizeF m_size;
    QPointF m_origin;
};

namespace TextMetrics {

// The board's type: the Omarchy font, at a size in canvas units.
QFont font(qreal pixelSize, bool bold = false);

// How each kind of item sets its text.
TextStyle styleFor(const Item &item, const Palette &palette);

// Where a connector's label goes: beside the middle of the line.
QPointF connectorLabelCorner(const Item &connector, QSizeF labelSize);

// Makes an item big enough for its text: text items are exactly the size of
// their text, and notes and shapes grow taller rather than spill, a shape to
// the least height that holds it, to the hundredth a file keeps. Returns
// whether the item changed. `laidOut`, when given, is its text as it is set
// at the size the item is now, which is then not laid out again.
bool fit(Item &item, const TextBlock *laidOut = nullptr);
// Makes every item on a board big enough for its text, as the canvas makes
// each one as it prepares it: what a file says of the size of a note or a
// shape is how big it is at least, and of text only where it starts. That is
// not an edit, so it is not undone, and a board that was saved still is. A
// note or a shape that could never grow, by a reach() all measured, is not
// laid out.
void fitAll(Board &board);

// All an item could cover, from its corner, once fit() has made it big
// enough for its text: worked out from what is written on it without laying
// that out. Text could reach that far across and down, a note that far down,
// and a shape that far above and below its middle. `measured`, when given, is
// whether all of that was measured, rather than allowed for: a handful of
// signs can reach further than they are allowed.
QRectF reach(const Item &item, bool *measured = nullptr);

// Sizes on the board's type scale.
inline constexpr qreal small = 11, body = 12, subtitle = 13, heading = 16, display = 28;
inline constexpr qreal notePadding = 12, shapePadding = 10;
// Where a frame's title starts along its top edge, and the gap in the edge
// either side of it.
inline constexpr qreal frameTitleInset = 24, frameTitleGap = 8;

} // namespace TextMetrics
