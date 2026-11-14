#include "textlayout.h"

#include "board.h"
#include "geometry.h"

#include <QGlyphRun>
#include <QHash>
#include <QPainter>
#include <QTextOption>
#include <cmath>
#include <optional>

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

namespace {

// The least height, in the hundredths of a unit a file keeps, above the one
// a shape has, at which the part of it that holds what is written is a given
// height tall. That part is no shorter for the shape being taller, so the
// height is narrowed down between one that is not tall enough, the shape's
// own, and one that is: three times as tall as the part is to be, as the
// part is never less than a third of the shape.
qreal leastHeight(Item::Shape shape, QSizeF size, qreal tall) {
    auto holds = [&](qint64 hundredths) {
        return Geo::textBox(shape, QSizeF(size.width(), hundredths / 100.0)).height() >= tall - 1e-6;
    };
    qint64 low = qint64(std::floor(size.height() * 100)), high = qMax(low + 1, qint64(std::ceil(tall * 300)));
    while (high - low > 1) {
        const qint64 middle = low + (high - low) / 2;
        (holds(middle) ? high : low) = middle;
    }
    return high / 100.0;
}

} // namespace

bool fit(Item &item, const TextBlock *laidOut) {
    static const Palette palette; // sizes do not depend on colors
    // A note or a shape with nothing written on it has nothing to hold, and
    // is as big as it was made, however small.
    if (item.text.isEmpty() && (item.kind == Item::Kind::Note || item.kind == Item::Kind::Shape))
        return false;
    QRectF rect = item.rect;
    switch (item.kind) {
    case Item::Kind::Text: {
        // Empty text still has the height of a line, to put the cursor in.
        const QSizeF size =
            laidOut && !item.text.isEmpty()
                ? laidOut->size()
                : TextBlock(item.text.isEmpty() ? QStringLiteral(" ") : item.text, styleFor(item, palette)).size();
        rect.setSize(QSizeF(item.wrap > 0 ? item.wrap : qMax(size.width(), 2.0), size.height()));
        break;
    }
    case Item::Kind::Note: {
        const QSizeF text = laidOut ? laidOut->size() : TextBlock(item.text, styleFor(item, palette)).size();
        const qreal needed = text.height() + 2 * notePadding;
        if (rect.height() < needed)
            rect.setHeight(needed);
        break;
    }
    case Item::Kind::Shape: {
        // What is written goes in a part of the shape (a triangle's foot,
        // an arrow's shaft), and it is that part that has to be tall
        // enough. The shape is made the least height that gives it that, in
        // the hundredths of a unit a file keeps: so it comes to the same
        // size however it got there, a letter at a time as it is typed in or
        // all at once as it is read, and is just as big when it is read back
        // from the file it was saved to. Some parts are narrower in a taller
        // shape, which can take another line: so, a few steps.
        Item grown = item;
        TextStyle style = styleFor(grown, palette);
        qreal needed = (laidOut ? laidOut->size() : TextBlock(item.text, style).size()).height() + 2 * 6;
        for (int step = 0; step < 8 && style.box.height() < needed - 1e-6; ++step) {
            grown.rect.setHeight(leastHeight(item.shape, grown.rect.size(), needed));
            const qreal width = style.box.width();
            style = styleFor(grown, palette);
            if (style.box.width() != width)
                needed = TextBlock(item.text, style).size().height() + 2 * 6;
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

void fitAll(Board &board) {
    for (quint32 id : board.order()) {
        Item item = *board.item(id);
        // A note or a shape that what is written on it could never make
        // bigger, as most are, is left as it is without laying that out: but
        // only where that was measured, as some signs reach past what is
        // allowed for them.
        bool measured = false;
        const bool holds = item.kind == Item::Kind::Note || item.kind == Item::Kind::Shape;
        if (holds && reach(item, &measured).size() == item.rect.size() && measured)
            continue;
        if (fit(item))
            board.setMeasuredRect(id, item.rect);
    }
}

namespace {

// All a letter takes of a line, as the font it comes from sets it in the
// board's type: how far across (with any of it that reaches past where the
// next one starts), how far above and below where the line sits, and how
// much more that font leaves before the next line.
struct Extent {
    qreal width = 0, ascent = 0, descent = 0, leading = 0;
};

Extent extentOf(const QTextLine &line) {
    return {line.naturalTextWidth(), line.ascent(), line.descent(), line.leading()};
}

// A line that takes in what a letter takes of one.
Extent taller(Extent line, const Extent &letter) {
    line.ascent = qMax(line.ascent, letter.ascent);
    line.descent = qMax(line.descent, letter.descent);
    line.leading = qMax(line.leading, letter.leading);
    return line;
}

// The most printable ASCII takes in the board's type at a size and weight:
// as wide as the widest of them is on its own, on a line with all of them on
// it. Measured once for each size and weight, as TextBlock sets them, in
// whatever font the type is.
Extent ascii(int size, bool bold) {
    static QHash<int, Extent> measured;
    const int key = 2 * size + int(bold);
    const auto found = measured.constFind(key);
    if (found != measured.constEnd())
        return *found;
    TextStyle style;
    style.font = font(size, bold);
    QString all;
    qreal widest = 0;
    for (char c = ' '; c <= '~'; ++c) {
        all += QLatin1Char(c);
        widest = qMax(widest, TextBlock(QString(QLatin1Char(c)), style).size().width());
    }
    Extent type = extentOf(TextBlock(all, style).paragraphs().front().layout->lineAt(0));
    type.width = widest;
    measured.insert(key, type);
    return type;
}

// Any other letter, as it is set in the board's type at a size and weight in
// a run of the script of another letter, or with none on its own. Which font
// a letter comes from goes by the script of the run it is in, which a sign or
// a space takes from the letters before it. Measured once for each letter,
// script, size and weight, on a line of its own after that other letter.
//
// Nothing is known of a letter in bold that the type's own font does not
// have. Before Qt loads another font for a letter, it looks only at what that
// font has in its regular weight, which can be less than it has in bold (Font
// Awesome draws hundreds of signs in bold only): so which font a letter in
// bold comes from can change once the board has had some other letter from
// that font.
std::optional<Extent> letter(char32_t c, char32_t after, int size, bool bold) {
    static QHash<quint64, std::optional<Extent>> measured;
    const quint64 key =
        quint64(c) << 32 | quint64(after ? QChar::script(after) : 0) << 24 | quint64(size) << 1 | quint64(bold);
    const auto found = measured.constFind(key);
    if (found != measured.constEnd())
        return *found;
    TextStyle style;
    style.font = font(size, bold);
    QString text;
    if (after)
        text = QString::fromUcs4(&after, 1) + QChar(QChar::LineSeparator);
    const qsizetype at = text.size();
    text += QString::fromUcs4(&c, 1);
    std::optional<Extent> set;
    bool own = true;
    if (bold) {
        TextStyle alone = style;
        alone.font.setStyleStrategy(QFont::NoFontMerging);
        const TextBlock block(text, alone);
        for (const QGlyphRun &run : block.paragraphs().front().layout->glyphRuns(at, text.size() - at))
            own = own && !run.glyphIndexes().contains(quint32(0));
    }
    if (own) {
        const TextBlock block(text, style);
        const QTextLayout &layout = *block.paragraphs().front().layout;
        set = extentOf(layout.lineAt(layout.lineCount() - 1));
    }
    measured.insert(key, set);
    return set;
}

// Whether a letter of a script and a kind has a script of its own, which the
// run it is in takes from it, rather than taking the run's, as spaces, signs
// and marks do.
bool scripted(QChar::Script script, QChar::Category kind) {
    return script > QChar::Script_Common && kind != QChar::Mark_NonSpacing && kind != QChar::Mark_SpacingCombining &&
           kind != QChar::Mark_Enclosing;
}

// Whether a letter of a kind, in a run of a script (Common, if of none), is
// set just as it is on its own, side by side with the next: a letter, digit,
// sign or space, not a mark set on another or what is not drawn, in a run of
// a script whose letters are not joined, stacked or reordered.
bool sideBySide(QChar::Category kind, QChar::Script run) {
    switch (kind) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return false;
    default:
        break;
    }
    switch (run) {
    case QChar::Script_Common:
    case QChar::Script_Latin:
    case QChar::Script_Greek:
    case QChar::Script_Cyrillic:
    case QChar::Script_Armenian:
    case QChar::Script_Georgian:
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
    case QChar::Script_Bopomofo:
    case QChar::Script_Hangul:
        return true;
    default:
        return false;
    }
}

// The letter at a place in a text, the two halves of a surrogate pair as
// one, moving the place on past it.
char32_t next(const QString &text, qsizetype &at) {
    const char16_t first = text[at++].unicode();
    if (QChar::isHighSurrogate(first) && at < text.size() && text[at].isLowSurrogate())
        return QChar::surrogateToUcs4(first, text[at++].unicode());
    return first;
}

// The first letter with a script of its own in the paragraph from a place in
// a text, whose script the run before it takes too; or 0, if none has.
char32_t firstScripted(const QString &text, qsizetype at) {
    while (at < text.size() && text[at] != u'\n') {
        const char32_t c = next(text, at);
        if (scripted(QChar::script(c), QChar::category(c)))
            return c;
    }
    return 0;
}

// How far text set in a style could reach from its corner, wrapped at a
// width (or, at 0, never), worked out from what is written without laying it
// out. Anything that is allowed for, rather than measured, clears `measured`.
QSizeF reachOf(const QString &text, const TextStyle &style, qreal wrap, bool &measured) {
    // A letter set side by side with the next, as those of most scripts are,
    // takes no more of a line than it does on its own, in the font its run
    // takes it from: printable ASCII in a run of no other script is as wide
    // as the widest of it in the type's own font, and any other letter, sign
    // or space as it is measured. Anything else, from the letters of scripts
    // that join, stack or reorder them to marks set on another letter, and
    // letters in bold that the type's own font does not have, could be set in
    // ways nothing measured of letters on their own says: no letter of that
    // more than three times as wide as the type is tall, or six past U+FFFF,
    // where a cuneiform sign can be four, and no line with any of it more
    // than three and a half times as tall. Of all the letters and signs of
    // every script Noto has, only a handful are wider than that (the widest,
    // U+FDFD, is twelve times as wide), and none of their lines is taller.
    // No tab goes past the next tab stop, which TextBlock leaves where
    // Qt puts them. And of wrapped text, a line and the next together hold
    // more than a line's width (what did not fit on the one starts the
    // other), so a line wraps into no more lines than twice its width over
    // the wrap.
    static const qreal tabStop = [] {
        TextStyle style;
        style.font = font(body);
        return TextBlock(QStringLiteral("\t"), style).size().width();
    }();
    // A note's or a shape's first paragraph is its title, in type of its own
    // (if empty, in the rest's).
    const int plainSize = qMax(1, style.font.pixelSize());
    const int titleSize = style.titled ? qMax(1, qRound(style.titleSize)) : plainSize;
    const bool plainBold = style.font.bold();
    const Extent plain = ascii(plainSize, plainBold);
    const Extent title = style.titled ? ascii(titleSize, true) : plain;
    const Extent titleLine = taller(title, plain);
    bool titled = style.titled, foreign = false;
    Extent line = titled ? titleLine : plain;
    qreal across = 0, widest = 0, height = 0;
    auto lineEnds = [&] {
        const qreal tall = qMax(std::ceil(line.ascent + line.descent + line.leading),
                                foreign ? 3.5 * (titled ? titleSize : plainSize) : 0.0);
        height += (wrap > 0 ? qMax(1.0, std::ceil(2 * across / wrap)) : 1) * tall;
        widest = qMax(widest, across);
        across = 0;
        foreign = false;
    };
    // The letter whose script the run is in, if any, and that script.
    char32_t run = 0;
    QChar::Script script = QChar::Script_Common;
    auto runOf = [&](char32_t letter) {
        run = letter;
        script = letter ? QChar::script(letter) : QChar::Script_Common;
    };
    runOf(firstScripted(text, 0));
    for (qsizetype at = 0; at < text.size();) {
        const char32_t c = next(text, at);
        const Extent &type = titled ? title : plain;
        if (c >= u' ' && c <= u'~') {
            if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) {
                run = c;
                script = QChar::Script_Latin;
            }
            if (script == QChar::Script_Latin || script == QChar::Script_Common) {
                across += type.width;
                continue;
            }
        }
        // The breaks that start a line: a paragraph's, and a line's own.
        if (c == u'\n' || c == QChar::LineSeparator) {
            lineEnds();
            if (c == u'\n') {
                titled = false;
                runOf(firstScripted(text, at));
            }
            line = titled ? titleLine : plain;
            continue;
        }
        if (c == u'\t') {
            across += qMax(type.width, tabStop);
            continue;
        }
        const QChar::Script of = QChar::script(c);
        const QChar::Category kind = QChar::category(c);
        const bool own = scripted(of, kind);
        if (own) {
            run = c;
            script = of;
        }
        const int size = titled ? titleSize : plainSize;
        const std::optional<Extent> set =
            sideBySide(kind, script) ? letter(c, own ? 0 : run, size, titled || plainBold) : std::nullopt;
        if (set) {
            across += set->width;
            line = taller(line, *set);
        } else {
            across += (QChar::requiresSurrogates(c) ? 6 : 3) * size;
            foreign = true;
            measured = false;
        }
    }
    lineEnds();
    // As wide as the widest line, to the whole unit TextBlock rounds it up to.
    return QSizeF(wrap > 0 ? wrap : qMax(std::ceil(widest), 2.0), height);
}

} // namespace

QRectF reach(const Item &item, bool *measured) {
    static const Palette palette; // sizes do not depend on colors
    const QRectF box(QPointF(0, 0), item.rect.size());
    QRectF covers = box;
    bool sure = true;
    switch (item.kind) {
    case Item::Kind::Text: {
        // Empty, it is a line of a space, for the cursor.
        const QString text = item.text.isEmpty() ? QStringLiteral(" ") : item.text;
        covers |= QRectF(QPointF(0, 0), reachOf(text, styleFor(item, palette), item.wrap, sure));
        break;
    }
    case Item::Kind::Note: {
        // With nothing written on it, it keeps its size; what is written is
        // wrapped inside its padding.
        if (item.text.isEmpty())
            break;
        const TextStyle style = styleFor(item, palette);
        const QSizeF text = reachOf(item.text, style, style.box.width(), sure);
        covers |= QRectF(0, 0, box.width(), text.height() + 2 * notePadding);
        break;
    }
    case Item::Kind::Shape: {
        // With nothing written on it, it keeps its size. Otherwise it grows
        // about its middle until the part of it that holds what is written is
        // tall enough for it. That part is no wider for the shape being
        // taller, so it is at its narrowest in a shape as tall as any can be;
        // and it is never less than a third of the shape's height (a cross's
        // middle third), so the shape grows to no more than three times what
        // the text needs, and a unit either way for its top being rounded.
        if (item.text.isEmpty())
            break;
        const qreal narrowest = Geo::textBox(item.shape, QSizeF(box.width(), 1e9)).width();
        const qreal needed = reachOf(item.text, styleFor(item, palette), narrowest, sure).height() + 2 * 6;
        const qreal tallest = 3 * needed + 2;
        covers |= QRectF(0, (box.height() - tallest) / 2, box.width(), tallest);
        break;
    }
    default:
        break;
    }
    if (measured)
        *measured = sure;
    return covers;
}

} // namespace TextMetrics
