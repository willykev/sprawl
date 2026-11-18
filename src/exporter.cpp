#include "exporter.h"

#include <QBuffer>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSvgGenerator>
#include <algorithm>
#include <cmath>

#include "geometry.h"
#include "imagestore.h"
#include "textlayout.h"

namespace Exporter {

namespace {

// No picture is written bigger than this on a side: past it, viewers and
// encoders start to refuse.
const int longestSide = 16384;

QVector<quint32> backToFront(const Board &board, const QVector<quint32> &ids) {
    if (ids.isEmpty())
        return board.order();
    QVector<quint32> ordered = ids;
    std::sort(ordered.begin(), ordered.end(), [&board](quint32 a, quint32 b) { return board.depth(a) < board.depth(b); });
    return ordered;
}

QPolygonF polygon(const QVector<QPointF> &points, QPointF offset = {}) {
    QPolygonF shape;
    shape.reserve(points.size());
    for (const QPointF &point : points)
        shape.append(point + offset);
    return shape;
}

// Where an item's text goes on the board, as the window puts it: a
// connector's beside the middle of its line, any other's in its box.
QPointF textCorner(const Item &item, const TextBlock &text) {
    if (item.kind == Item::Kind::Connector)
        return TextMetrics::connectorLabelCorner(item, text.size());
    return item.rect.topLeft() + text.origin();
}

// What a block of text covers, from its corner: its lines, down to its foot.
// Lines set across the middle of a box wider than they are, or wider than a
// box too narrow for a single letter, are not where the block's size alone
// would put them: one wider than its box reaches out past both its sides.
QRectF covers(const TextBlock &text) {
    QRectF covered;
    for (const TextBlock::Paragraph &paragraph : text.paragraphs())
        for (int i = 0; i < paragraph.layout->lineCount(); ++i)
            covered |= paragraph.layout->lineAt(i).naturalTextRect().translated(0, paragraph.y);
    covered.setBottom(qMax(covered.bottom(), text.size().height()));
    return covered;
}

// The writer makes a page a whole number of points, and says how big it is
// in the page's boxes: "/MediaBox [0 0 163.000000 113.000000]" and the like.
// Each is made the size the page should be right here, in place, in no more
// characters than were there (spaces make up the rest), so that nothing
// after it moves and the file's index of where everything is stays true.
void fitPage(QByteArray &pdf, QSize whole, QSizeF exact) {
    const QByteArray from = "[0 0 " + QByteArray::number(qreal(whole.width()), 'f') + ' ' +
                            QByteArray::number(qreal(whole.height()), 'f') + ']';
    QByteArray to = "[0 0 " + QByteArray::number(exact.width(), 'g', 15) + ' ' +
                    QByteArray::number(exact.height(), 'g', 15) + ']';
    if (to.size() > from.size())
        return;
    to.insert(to.size() - 1, QByteArray(from.size() - to.size(), ' '));
    for (const QByteArray &box : {QByteArrayLiteral("/MediaBox "), QByteArrayLiteral("/TrimBox ")}) {
        const qsizetype at = pdf.indexOf(box + from);
        if (at >= 0)
            pdf.replace(at + box.size(), to.size(), to);
    }
}

} // namespace

QStringList formats() { return {"png", "svg", "pdf"}; }

QRectF area(const Board &board, const QVector<quint32> &ids, qreal margin) {
    // Sizes of type do not depend on colors.
    static const Palette palette;
    QRectF bounds;
    for (quint32 id : ids.isEmpty() ? board.order() : ids) {
        const Item *found = board.item(id);
        if (!found)
            continue;
        Item item = *found;
        // A text item is as big as its type makes it, which the window may
        // not have measured yet: until it has, the board has a guess.
        if (item.kind == Item::Kind::Text)
            TextMetrics::fit(item);
        // And text is laid out here as it is drawn, where the board can only
        // guess at it or it runs out of its item: a connector's label and a
        // frame's title beside them, and more than a note or a shape holds.
        QRectF text;
        if (item.hasText() && !item.text.isEmpty()) {
            const TextBlock block(item.text, TextMetrics::styleFor(item, palette));
            text = covers(block).translated(textCorner(item, block));
        }
        const QRectF covered = Board::boundsFor(item, text) | text;
        bounds = bounds.isNull() ? covered : bounds | covered;
    }
    if (bounds.isNull())
        return {};
    // On whole units, so nothing sits between pixels.
    return QRectF(QPointF(std::floor(bounds.left() - margin), std::floor(bounds.top() - margin)),
                  QPointF(std::ceil(bounds.right() + margin), std::ceil(bounds.bottom() + margin)));
}

void paint(QPainter &painter, const Board &board, const QVector<quint32> &ids, const Palette &palette,
           const QString &assetFolder, int radius) {
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    const qreal corner = qMin(radius, 12);
    const QColor dim = Theme::mix(palette.background, palette.foreground, .62);
    auto colorFor = [&palette](const QString &name, const QColor &neutral) {
        return name.isEmpty() ? neutral : palette.swatch(name, neutral);
    };
    QHash<QString, QImage> images;

    for (quint32 id : backToFront(board, ids)) {
        const Item *found = board.item(id);
        if (!found)
            continue;
        const Item &item = *found;
        const QRectF box(QPointF(0, 0), item.rect.size());
        painter.save();
        painter.translate(item.rect.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::NoBrush);

        std::unique_ptr<TextBlock> text;
        if (item.hasText() && !item.text.isEmpty())
            text = std::make_unique<TextBlock>(item.text, TextMetrics::styleFor(item, palette));

        switch (item.kind) {
        case Item::Kind::Note:
            painter.setBrush(palette.note(item.color));
            painter.drawRoundedRect(box, corner, corner);
            break;
        case Item::Kind::Shape: {
            // The border is the unit just inside the shape's edge.
            painter.setPen(QPen(colorFor(item.color, dim), 1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter.setBrush(palette.raised);
            const QRectF inner = box.adjusted(0.5, 0.5, -0.5, -0.5);
            if (item.shape == Item::Shape::Ellipse) {
                painter.drawEllipse(inner);
            } else if (item.shape == Item::Shape::Diamond) {
                const QPointF middle = inner.center();
                painter.drawPolygon(QPolygonF{{middle.x(), inner.top()},
                                              {inner.right(), middle.y()},
                                              {middle.x(), inner.bottom()},
                                              {inner.left(), middle.y()}});
            } else if (Geo::isQuad(item.shape)) {
                const qreal round = Geo::cornerRadius(item.shape, box.size(), corner);
                painter.drawRoundedRect(inner, round, round);
            } else {
                const Geo::Figure figure = Geo::figure(item.shape, inner.size());
                painter.drawPolygon(polygon(figure.edge, inner.topLeft()));
                if (figure.line.size() > 1) {
                    painter.setBrush(Qt::NoBrush);
                    painter.drawPolyline(polygon(figure.line, inner.topLeft()));
                }
            }
            break;
        }
        case Item::Kind::Text:
            break;
        case Item::Kind::Stroke: {
            const QColor color = colorFor(item.color, palette.bright);
            const QVector<QPointF> curve = Geo::smooth(item.points, Geo::strokeTolerance);
            if (curve.size() == 1) {
                painter.setBrush(color);
                painter.drawEllipse(curve[0], item.width / 2, item.width / 2);
            } else if (curve.size() > 1) {
                painter.setPen(QPen(color, item.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPolyline(polygon(curve));
            }
            break;
        }
        case Item::Kind::Connector: {
            const QPointF origin = item.rect.topLeft();
            const QColor color = colorFor(item.color, palette.foreground);
            const Geo::ConnectorArt art = Geo::connectorArt(item);
            const QPen line(color, item.width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
            painter.setPen(line);
            for (const QVector<QPointF> &run : art.lines)
                painter.drawPolyline(polygon(run, -origin));
            for (const Geo::ConnectorArt::Head &head : art.heads) {
                // Filled, or a line just inside its edge.
                const qreal inset = head.hollow ? item.width / 2 : 0;
                const qreal along = qMax(0.0, head.half.width() - inset), half = qMax(0.0, head.half.height() - inset);
                const QPointF at = head.center - origin, across(-head.axis.y(), head.axis.x());
                painter.setPen(head.hollow ? line : QPen(Qt::NoPen));
                painter.setBrush(head.hollow ? QBrush(Qt::NoBrush) : QBrush(color));
                if (head.outline == Geo::Outline::Ellipse)
                    painter.drawEllipse(at, along, half);
                else if (head.outline == Geo::Outline::Diamond)
                    painter.drawPolygon(QPolygonF({at + head.axis * along, at + across * half, at - head.axis * along,
                                                   at - across * half}));
                else
                    painter.drawPolygon(QPolygonF({at + head.axis * along, at - head.axis * along + across * half,
                                                   at - head.axis * along - across * half}));
            }
            break;
        }
        case Item::Kind::Image: {
            if (!images.contains(item.asset))
                images.insert(item.asset, ImageStore::read(assetFolder, item.asset));
            const QImage image = images.value(item.asset);
            if (image.isNull()) {
                // A picture that is missing is shown to be missing.
                painter.setPen(QPen(palette.swatch("red", palette.foreground), 1));
                painter.setBrush(palette.raised);
                painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
            } else {
                painter.drawImage(box, image);
            }
            break;
        }
        case Item::Kind::Frame: {
            const qreal title = text ? text->size().width() : 0;
            const qreal gapFrom = TextMetrics::frameTitleInset - TextMetrics::frameTitleGap;
            const qreal gapTo = TextMetrics::frameTitleInset + title + TextMetrics::frameTitleGap;
            painter.setPen(QPen(colorFor(item.color, palette.muted), 1, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
            painter.drawPolyline(polygon(Geo::frameEdge(box.size(), gapFrom, title > 0 ? gapTo : gapFrom)));
            break;
        }
        }

        if (text) {
            // Where the window puts it; the layout's own origin is already in
            // the block.
            painter.translate(textCorner(item, *text) - item.rect.topLeft() - text->origin());
            text->draw(&painter, QPointF(0, 0));
        }
        painter.restore();
    }
}

QString write(const QString &path, const Board &board, const QVector<quint32> &ids, const Palette &palette,
              const QString &assetFolder, const Options &options) {
    const QRectF bounds = area(board, ids, options.margin);
    if (bounds.isEmpty())
        return "There is nothing on the board to export.";
    const QString format = options.format.toLower();
    if (!formats().contains(format))
        return QString("Sprawl exports %1, not %2.").arg(formats().join(", "), options.format);

    // Written beside the target and renamed over it, like everything else.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString("Could not write %1: %2.").arg(path, file.errorString());

    if (format == "png") {
        qreal scale = qBound(0.1, options.scale, 8.0);
        const qreal longest = qMax(bounds.width(), bounds.height());
        if (longest * scale > longestSide)
            scale = longestSide / longest;
        QImage image((bounds.size() * scale).toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        image.fill(options.background ? palette.background : QColor(0, 0, 0, 0));
        QPainter painter(&image);
        painter.scale(scale, scale);
        painter.translate(-bounds.topLeft());
        paint(painter, board, ids, palette, assetFolder, options.radius);
        painter.end();
        QImageWriter writer(&file, "png");
        if (!writer.write(image))
            return QString("Could not write %1: %2.").arg(path, writer.errorString());
    } else if (format == "svg") {
        // A canvas unit to a CSS pixel, which is what a size with no unit is
        // in. The generator writes a size in millimeters, which is never
        // quite it, and a view box to six figures, which past a million units
        // is not: so it is asked for neither, and both go in after, in whole
        // units.
        QBuffer svg;
        svg.open(QIODevice::WriteOnly);
        QSvgGenerator generator;
        generator.setOutputDevice(&svg);
        generator.setTitle("Sprawl board");
        QPainter painter(&generator);
        if (options.background)
            painter.fillRect(QRectF(QPointF(0, 0), bounds.size()), palette.background);
        painter.translate(-bounds.topLeft());
        paint(painter, board, ids, palette, assetFolder, options.radius);
        painter.end();
        // They go into the root tag, after its name. The text before and
        // after them is written out as it is, rather than copied to make
        // room: a big board's SVG is big.
        const QByteArray &written = svg.data();
        const qsizetype root = written.indexOf("<svg");
        const qsizetype split = root < 0 ? 0 : root + 4;
        file.write(written.constData(), split);
        if (root >= 0)
            file.write(QString(" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\"")
                           .arg(qRound64(bounds.width()))
                           .arg(qRound64(bounds.height()))
                           .toLatin1());
        file.write(written.constData() + split, written.size() - split);
    } else {
        // One page the size of the board, a canvas unit to a CSS pixel:
        // three quarters of a point. The writer only makes pages a whole
        // number of points, so it is given the next whole size up, and then
        // the page is cut down to the board's size exactly. A page is
        // measured from its bottom left, so that is the corner the board is
        // painted in, in points (72 to the inch), with the canvas color over
        // all of the page to reach every edge.
        const QSizeF page = bounds.size() * 0.75;
        const QSize whole(std::ceil(page.width()), std::ceil(page.height()));
        QBuffer pdf;
        pdf.open(QIODevice::WriteOnly);
        QPdfWriter writer(&pdf);
        writer.setResolution(72);
        // That size exactly, not a paper size it is close to.
        writer.setPageSize(QPageSize(QSizeF(whole), QPageSize::Point, QString(), QPageSize::ExactMatch));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle("Sprawl board");
        QPainter painter(&writer);
        if (options.background)
            painter.fillRect(QRectF(QPointF(0, 0), QSizeF(whole)), palette.background);
        painter.translate(0, whole.height() - page.height());
        painter.scale(0.75, 0.75);
        painter.translate(-bounds.topLeft());
        paint(painter, board, ids, palette, assetFolder, options.radius);
        painter.end();
        fitPage(pdf.buffer(), whole, page);
        file.write(pdf.data());
    }
    if (!file.commit())
        return QString("Could not write %1: %2.").arg(path, file.errorString());
    return {};
}

} // namespace Exporter
