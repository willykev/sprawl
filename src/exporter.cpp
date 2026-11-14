#include "exporter.h"

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

} // namespace

QStringList formats() { return {"png", "svg", "pdf"}; }

QRectF area(const Board &board, const QVector<quint32> &ids, qreal margin) {
    const QRectF bounds = ids.isEmpty() ? board.contentBounds() : board.contentBounds(ids);
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
        QPointF textCorner;
        if (item.hasText() && !item.text.isEmpty()) {
            text = std::make_unique<TextBlock>(item.text, TextMetrics::styleFor(item, palette));
            textCorner = text->origin();
        }

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
            const QVector<QPointF> curve = Geo::smooth(item.points, 0.1);
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
            if (text)
                textCorner = TextMetrics::connectorLabelCorner(item, text->size()) - origin;
            break;
        }
        case Item::Kind::Image: {
            if (!images.contains(item.asset))
                images.insert(item.asset, ImageStore::read(assetFolder + "/" + item.asset));
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
            // The layout's own origin is already in the block.
            painter.translate(textCorner - text->origin());
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
        QSvgGenerator generator;
        generator.setOutputDevice(&file);
        generator.setSize(bounds.size().toSize());
        generator.setViewBox(QRectF(QPointF(0, 0), bounds.size()));
        generator.setTitle("Sprawl board");
        QPainter painter(&generator);
        if (options.background)
            painter.fillRect(QRectF(QPointF(0, 0), bounds.size()), palette.background);
        painter.translate(-bounds.topLeft());
        paint(painter, board, ids, palette, assetFolder, options.radius);
        painter.end();
    } else {
        QPdfWriter writer(&file);
        // One page the size of the board, a canvas unit to a CSS pixel.
        writer.setResolution(96);
        writer.setPageSize(QPageSize(bounds.size() * 0.75, QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle("Sprawl board");
        QPainter painter(&writer);
        if (options.background)
            painter.fillRect(QRectF(QPointF(0, 0), bounds.size()), palette.background);
        painter.translate(-bounds.topLeft());
        paint(painter, board, ids, palette, assetFolder, options.radius);
        painter.end();
    }
    if (!file.commit())
        return QString("Could not write %1: %2.").arg(path, file.errorString());
    return {};
}

} // namespace Exporter
