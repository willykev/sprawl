#include "textlayout.h"

#include "geometry.h"

#include <QPainter>
#include <QTextOption>
#include <cmath>

TextBlock::TextBlock(const QString &text, const TextStyle &style) {
    if (text.isEmpty())
        return;

    QTextOption option;
    option.setWrapMode(style.wraps() ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setAlignment(style.wraps() ? style.alignment & Qt::AlignHorizontal_Mask : Qt::AlignLeft);
    // Glyphs advance by the font's own unhinted widths, so text is the same
    // width at every zoom.
    option.setUseDesignMetrics(true);
    const qreal lineWidth = style.wraps() ? style.box.width() : 1e9;

    QTextCharFormat title;
    title.setFontWeight(QFont::Bold);
    title.setProperty(QTextFormat::FontPixelSize, qMax(1, qRound(style.titleSize)));
    title.setForeground(style.titleColor);
    QTextCharFormat plain;
    plain.setForeground(style.color);

    // One layout per paragraph, stacked as the editor's document stacks its
    // blocks: every line a whole number of pixels tall.
    const QStringList paragraphs = text.split('\n');
    qreal y = 0, widest = 0;
    m_paragraphs.reserve(paragraphs.size());
    for (int i = 0; i < paragraphs.size(); ++i) {
        auto layout = std::make_unique<QTextLayout>(paragraphs[i], style.font);
        layout->setTextOption(option);
        if (!paragraphs[i].isEmpty())
            layout->setFormats(
                {QTextLayout::FormatRange{0, int(paragraphs[i].size()), style.titled && i == 0 ? title : plain}});
        layout->beginLayout();
        qreal height = 0;
        for (;;) {
            QTextLine line = layout->createLine();
            if (!line.isValid())
                break;
            line.setLeadingIncluded(true);
            line.setLineWidth(lineWidth);
            line.setPosition(QPointF(0, height));
            height += std::ceil(line.ascent() + line.descent() + line.leading());
            widest = qMax(widest, line.naturalTextWidth());
        }
        layout->endLayout();
        m_paragraphs.push_back({std::move(layout), y});
        y += height;
    }
    m_size = QSizeF(std::ceil(widest), y);

    const QRectF &box = style.box;
    qreal left = box.left(), top = box.top();
    if (!style.wraps() && (style.alignment & Qt::AlignHCenter))
        left = box.center().x() - m_size.width() / 2;
    if (style.alignment & Qt::AlignVCenter)
        top = box.center().y() - m_size.height() / 2;
    // Whole pixels at 100%, so type is as crisp as it is in a terminal.
    m_origin = QPointF(std::round(left), std::round(top));
}

void TextBlock::draw(QPainter *painter, QPointF itemCorner) const {
    for (const Paragraph &paragraph : m_paragraphs)
        paragraph.layout->draw(painter, itemCorner + m_origin + QPointF(0, paragraph.y));
}

namespace TextMetrics {

QFont font(qreal pixelSize, bool bold) {
    // "monospace" is the alias `omarchy font set` points at the chosen font.
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    font.setBold(bold);
    return font;
}

TextStyle styleFor(const Item &item, const Palette &palette) {
    TextStyle style;
    const QSizeF size = item.rect.size();
    const QColor own = item.color.isEmpty() ? QColor() : palette.swatch(item.color, palette.foreground);
    switch (item.kind) {
    case Item::Kind::Note:
        style.font = font(body);
        style.titled = true;
        style.titleSize = subtitle;
        style.color = style.titleColor = palette.ink;
        style.box = QRectF(notePadding, notePadding, qMax(1.0, size.width() - 2 * notePadding),
                           qMax(0.0, size.height() - 2 * notePadding));
        break;
    case Item::Kind::Shape: {
        style.font = font(small);
        style.titled = true;
        style.titleSize = body;
        style.color = palette.foreground;
        style.titleColor = palette.bright;
        style.alignment = Qt::AlignHCenter | Qt::AlignVCenter;
        // In the middle of the part of the shape that has room for it.
        style.box = Geo::textBox(item.shape, size);
        break;
    }
    case Item::Kind::Text:
        style.font = font(item.fontSize, item.bold);
        style.color = own.isValid() ? own : item.bold ? palette.bright : palette.foreground;
        style.box = QRectF(0, 0, qMax(0.0, item.wrap), 0);
        break;
    case Item::Kind::Frame:
        // The title sits across the top edge, in a gap of it.
        style.font = font(small);
        style.color = own.isValid() ? own : palette.foreground;
        style.alignment = Qt::AlignLeft | Qt::AlignVCenter;
        style.box = QRectF(frameTitleInset, 0, 0, 0);
        break;
    case Item::Kind::Connector:
        style.font = font(small);
        style.color = own.isValid() ? own : palette.foreground;
        break;
    case Item::Kind::Stroke:
    case Item::Kind::Image:
        break;
    }
    if (!style.titled)
        style.titleColor = style.color;
    return style;
}

QPointF connectorLabelCorner(const Item &connector, QSizeF labelSize) {
    // Beside the middle of its line, whatever way the line gets there.
    const QVector<QPointF> path = Geo::connectorPath(connector);
    const Geo::PathPoint halfway = Geo::alongPath(path, Geo::pathLength(path) / 2);
    const QPointF middle = halfway.at, along = halfway.heading;
    // Off to the side of the line that faces up, or right of an upright one.
    QPointF away(along.y(), -along.x());
    if (away.y() > 1e-6 || (std::abs(away.y()) <= 1e-6 && away.x() < 0))
        away = -away;
    // Far enough that the label's box clears the line whatever its angle.
    const qreal reach =
        std::abs(away.x()) * labelSize.width() / 2 + std::abs(away.y()) * labelSize.height() / 2 + 6;
    const QPointF center = middle + away * reach;
    return QPointF(std::round(center.x() - labelSize.width() / 2), std::round(center.y() - labelSize.height() / 2));
}

bool fit(Item &item) {
    static const Palette palette; // sizes do not depend on colors
    QRectF rect = item.rect;
    switch (item.kind) {
    case Item::Kind::Text: {
        const TextStyle style = styleFor(item, palette);
        // Empty text still has the height of a line, to put the cursor in.
        const QSizeF size = TextBlock(item.text.isEmpty() ? QStringLiteral(" ") : item.text, style).size();
        rect.setSize(QSizeF(item.wrap > 0 ? item.wrap : qMax(size.width(), 2.0), size.height()));
        break;
    }
    case Item::Kind::Note: {
        const qreal needed = TextBlock(item.text, styleFor(item, palette)).size().height() + 2 * notePadding;
        if (rect.height() < needed)
            rect.setHeight(needed);
        break;
    }
    case Item::Kind::Shape: {
        // What is written goes in a part of the shape (a triangle's foot,
        // an arrow's shaft), and it is that part that has to be tall
        // enough. As the shape grows the part does, though not always by
        // as much, nor always staying as wide: so, a few steps.
        Item grown = item;
        for (int step = 0; step < 8; ++step) {
            const TextStyle style = styleFor(grown, palette);
            const qreal needed = TextBlock(item.text, style).size().height() + 2 * 6;
            const qreal room = style.box.height();
            if (room >= needed - 1e-6)
                break;
            const qreal share = qBound(0.1, room / qMax(grown.rect.height(), 1.0), 1.0);
            grown.rect.setHeight(grown.rect.height() + (needed - room) / share);
        }
        if (grown.rect.height() > rect.height()) {
            // Grown about its middle, so its connectors stay level.
            const qreal middle = rect.center().y(), height = grown.rect.height();
            rect.setTop(std::round(middle - height / 2));
            rect.setHeight(height);
        }
        break;
    }
    default:
        break;
    }
    if (rect == item.rect)
        return false;
    item.rect = rect;
    return true;
}

} // namespace TextMetrics
