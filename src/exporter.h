#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include "board.h"
#include "theme.h"

class QPainter;

// Draws a board with a painter, for files: a PNG, an SVG, or a PDF. It draws
// what the canvas draws, from the same text layout and the same geometry, so
// a board looks the same in a file as on screen. It needs no window, which
// is what lets `sprawl export` run with no display.
namespace Exporter {

struct Options {
    QString format = QStringLiteral("png");
    // PNG: pixels per canvas unit. 2 is what a HiDPI screen shows at 100%.
    qreal scale = 2;
    // The canvas color behind the board, or nothing.
    bool background = true;
    qreal margin = 32;
    int radius = 0;
};

QStringList formats();

// What an export covers: the items (all of them when `ids` is empty) and a
// margin around them.
QRectF area(const Board &board, const QVector<quint32> &ids, qreal margin);

void paint(QPainter &painter, const Board &board, const QVector<quint32> &ids, const Palette &palette,
           const QString &assetFolder, int radius);

// Writes the file. Returns what went wrong, or nothing when it worked.
QString write(const QString &path, const Board &board, const QVector<quint32> &ids, const Palette &palette,
              const QString &assetFolder, const Options &options);

} // namespace Exporter
