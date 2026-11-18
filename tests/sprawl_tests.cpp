#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGGeometryNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QSignalSpy>
#include <QSvgRenderer>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtQuick/private/qquickitem_p.h>
#include <QtTest>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <qpa/qwindowsysteminterface.h>
#include <sys/stat.h>
#include <typeinfo>
#include <zlib.h>

#include "backend.h"
#include "board.h"
#include "canvasview.h"
#include "cli.h"
#include "exporter.h"
#include "filepicker.h"
#include "geometry.h"
#include "imagestore.h"
#include "launch.h"
#include "library.h"
#include "materials.h"
#include "measuring.h"
#include "offline.h"
#include "spatialgrid.h"
#include "textlayout.h"
#include "theme.h"
#include "translator.h"

namespace {

Item note(qreal x, qreal y, const QString &text = {}) {
    Item item;
    item.kind = Item::Kind::Note;
    item.rect = QRectF(x, y, Defaults::noteSize, Defaults::noteSize);
    item.color = "yellow";
    item.text = text;
    return item;
}

Item shape(Item::Shape outline, qreal x, qreal y, qreal w = 100, qreal h = 60) {
    Item item;
    item.kind = Item::Kind::Shape;
    item.shape = outline;
    item.rect = QRectF(x, y, w, h);
    return item;
}

Item connector(quint32 from, quint32 to) {
    Item item;
    item.kind = Item::Kind::Connector;
    item.from = from;
    item.to = to;
    item.width = Defaults::connectorWidth;
    return item;
}

// A stroke as the pen makes one: its points start from the corner of the box
// around them.
Item stroke(QVector<QPointF> points, qreal x = 0, qreal y = 0) {
    Item item;
    item.kind = Item::Kind::Stroke;
    const QRectF bounds = Geo::boundsOf(points);
    for (QPointF &point : points)
        point -= bounds.topLeft();
    item.points = points;
    item.rect = bounds.translated(x, y);
    return item;
}

void write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A picture of one color, written where a test asks for it.
QString picture(const QString &path, QSize size, const QColor &color = QColor("#7aa2f7")) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    image.save(path);
    return path;
}

// What `bytes` are, gzipped, as an .svgz file holds them.
QByteArray gzipped(const QByteArray &bytes) {
    z_stream stream{};
    deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray packed(int(deflateBound(&stream, uLong(bytes.size()))), '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.constData()));
    stream.avail_in = uInt(bytes.size());
    stream.next_out = reinterpret_cast<Bytef *>(packed.data());
    stream.avail_out = uInt(packed.size());
    deflate(&stream, Z_FINISH);
    packed.resize(qsizetype(stream.total_out));
    deflateEnd(&stream);
    return packed;
}

// The canvas, keeping the box around each thing it last drew in its own
// materials (the paper of a note, the outline of a shape, a line), in the
// order they were drawn. The software renderer the tests run with draws none
// of them, so this is how a test sees where they are drawn, how big, and
// over what. The corners of each start with where they are, and no other
// node's corners are the size of theirs. Nor does that renderer draw the dot
// grid, so the canvas keeps what the grid is drawn from too, and the view
// the board is drawn with.
class WatchedCanvas : public CanvasView {
public:
    QVector<QRectF> drawn;
    // The corners of the grid's quad, what its material asks of the scene
    // graph, and what it hands its shader (see grid.vert).
    struct Grid {
        QRectF corners;
        QSGMaterial::Flags flags;
        float zoom = 0, spacing = 0, radius = 0, dpr = 0;
        QVector2D origin;
    } grid;
    // What takes the board to the canvas.
    QMatrix4x4 view;

    // Where in that order a thing whose box is `box` was drawn, to within a
    // unit; -1 when it was not.
    int drawnAt(const QRectF &box) const {
        for (int i = 0; i < drawn.size(); ++i)
            if (qAbs(drawn[i].left() - box.left()) <= 1 && qAbs(drawn[i].top() - box.top()) <= 1 &&
                qAbs(drawn[i].right() - box.right()) <= 1 && qAbs(drawn[i].bottom() - box.bottom()) <= 1)
                return i;
        return -1;
    }

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *data) override {
        QSGNode *scene = CanvasView::updatePaintNode(old, data);
        drawn.clear();
        look(scene);
        // The board is drawn under the one transform among the scene's own
        // nodes.
        for (QSGNode *child = scene->firstChild(); child; child = child->nextSibling())
            if (child->type() == QSGNode::TransformNodeType)
                view = static_cast<QSGTransformNode *>(child)->matrix();
        return scene;
    }

private:
    void look(QSGNode *node) {
        // The canvas's own nodes are plain geometry nodes. Those of text and
        // pictures are the software renderer's, which hold a material and
        // corners that are not there.
        if (typeid(*node) == typeid(QSGGeometryNode)) {
            const auto *drawing = static_cast<QSGGeometryNode *>(node);
            const QSGGeometry *geometry = drawing->geometry();
            const int size = geometry->sizeOfVertex();
            if (const auto *material = dynamic_cast<const GridMaterial *>(drawing->material())) {
                QPolygonF corners;
                const QSGGeometry::Point2D *corner = geometry->vertexDataAsPoint2D();
                for (int i = 0; i < geometry->vertexCount(); ++i)
                    corners.append(QPointF(corner[i].x, corner[i].y));
                grid = {corners.boundingRect(), material->flags(), material->zoom, material->spacing,
                        material->radius, material->dpr, material->origin};
            } else if (size == int(sizeof(Geo::ShapeVertex)) || size == int(sizeof(Geo::StrokeVertex))) {
                QPolygonF corners;
                const char *vertex = static_cast<const char *>(geometry->vertexData());
                for (int i = 0; i < geometry->vertexCount(); ++i, vertex += size) {
                    const float *at = reinterpret_cast<const float *>(vertex);
                    corners.append(QPointF(at[0], at[1]));
                }
                if (!corners.isEmpty())
                    drawn.append(corners.boundingRect());
            }
        }
        for (QSGNode *child = node->firstChild(); child; child = child->nextSibling())
            look(child);
    }
};

// Where the middle of the dot for a point of the board is drawn, in pixels
// from where the point itself is, when the grid's shader is handed `handed`
// there. The shader takes what it is handed to a point of the board, with
// what the grid hands it besides (see grid.vert), and centers a dot where
// that is a multiple of the step. The point's own dot is where the shader
// sees the point, or a point a whole number of the coarser grid's steps
// away, as that grid's dots are on the board's multiples of its step.
QPointF dotFor(QPointF point, QPointF handed, QVector2D origin, qreal zoom, qreal spacing) {
    const QPointF seen = QPointF(origin.x(), origin.y()) + handed / zoom;
    auto wrapped = [period = 2 * spacing](qreal value) { return value - period * std::round(value / period); };
    return QPointF(wrapped(point.x() - seen.x()), wrapped(point.y() - seen.y())) * zoom;
}

// The desktop's file dialogs, answered without a desktop: a test says what
// would be picked, and reads back what was asked for.
class FakePicker : public FilePicker {
public:
    QList<QUrl> images;
    // Where an export is "saved". Empty is a dialog that was cancelled.
    QUrl target;
    QString failure;
    int opened = 0, asked = 0;
    QUrl suggested;
    QString format;

    void openImages() override {
        ++opened;
        if (!failure.isEmpty())
            emit failed(failure);
        else if (!images.isEmpty())
            emit imagesSelected(images);
    }
    void saveExport(const QUrl &suggestedUrl, const QString &wanted) override {
        ++asked;
        suggested = suggestedUrl;
        format = wanted;
        if (!target.isEmpty())
            emit exportSelected(target, wanted);
    }
};

// The whole app as main() puts it together, in a folder of its own: the
// real window from the real QML, a library nobody else uses, and a file
// picker that answers by itself.
struct Desk {
    using Setup = std::function<void(const QString &home)>;
    static int run(const Setup &before, const QString &home) {
        if (before && !home.isEmpty())
            before(home);
        return 0;
    }

    QTemporaryDir home;
    // What `before` put in the folder is there before anything reads it.
    int prepared;
    Library library;
    FakePicker picker;
    // Omarchy's, from the folder too: the current theme in theme/, and the
    // person's own settings for the shell beside it, in shell.toml.
    Theme theme;
    Translator translator;
    Backend backend;
    // Before the engine, which it has to outlast: as main() has it.
    Offline offline;
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    CanvasView *canvas = nullptr;

    explicit Desk(const QString &board = QStringLiteral("desk"), const Setup &before = {},
                  QSize size = QSize(1000, 700))
        : prepared(run(before, home.path())), library(home.path()),
          theme(home.path(), nullptr, {}, home.filePath("shell.toml")),
          translator(home.filePath("language"), ":/i18n"), backend(&library, &picker, &theme) {
        // Without a folder of its own a desk prepares nothing, opens no
        // board, and is not ready: handed an empty folder, the library is
        // the person's own, and so are the theme and the language.
        if (!home.isValid()) {
            qWarning("No folder for a desk: %s", qPrintable(home.errorString()));
            return;
        }
        backend.openInitial(board);
        engine.setNetworkAccessManagerFactory(&offline);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings << error.toString();
        });
        // Qt.quit() would end the tests with the app; they watch for it instead.
        QObject::disconnect(&engine, &QQmlEngine::quit, QCoreApplication::instance(), nullptr);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("startHidden", false);
        engine.rootContext()->setContextProperty("startSize", size);
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        canvas = window ? window->findChild<CanvasView *>() : nullptr;
        if (!window || !canvas)
            return;
        (void)QTest::qWaitForWindowExposed(window);
        window->requestActivate();
        (void)QTest::qWaitForWindowActive(window);
        // Every test starts from the same view: the board's corner in the
        // canvas's corner, at actual size.
        canvas->setView(QPointF(0, 0), 1);
    }

    Board &board() { return *backend.board(); }
    bool ready() const { return window && canvas; }

    // A point of the board, as a place in the window.
    QPoint at(QPointF canvasPoint) const { return canvas->mapToScene(canvas->toItem(canvasPoint)).toPoint(); }
    void hover(QPointF point) { QTest::mouseMove(window, at(point)); }
    void click(QPointF point, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseMove(window, at(point));
        QTest::mouseClick(window, Qt::LeftButton, modifiers, at(point));
    }
    void doubleClick(QPointF point) {
        QTest::mouseMove(window, at(point));
        QTest::mouseDClick(window, Qt::LeftButton, {}, at(point));
    }
    void press(QPointF point, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseMove(window, at(point));
        QTest::mousePress(window, Qt::LeftButton, modifiers, at(point));
    }
    void moveTo(QPointF point, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseEvent(QTest::MouseMove, window, Qt::NoButton, modifiers, at(point));
    }
    void release(QPointF point, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseRelease(window, Qt::LeftButton, modifiers, at(point));
    }
    // A drag as a hand makes one: down, across in steps, up.
    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = {}) {
        press(from, modifiers);
        for (int step = 1; step <= 4; ++step)
            moveTo(from + (to - from) * step / 4.0, modifiers);
        release(to, modifiers);
    }
    void key(Qt::Key code, Qt::KeyboardModifiers modifiers = {}) { QTest::keyClick(window, code, modifiers); }
    void type(const QString &text) {
        for (const QChar c : text) {
            if (c == '\n')
                QTest::keyClick(window, Qt::Key_Return);
            else
                QTest::keyClick(window, c.toLatin1());
        }
    }
    void wheel(QPointF point, QPoint angle, Qt::KeyboardModifiers modifiers = {}) {
        const QPointF local = at(point);
        QWheelEvent event(local, window->mapToGlobal(local), QPoint(), angle, Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
    }
    // A file dropped on the board, as a file manager drops one. True when
    // the window takes it, as the file manager is told.
    bool drop(QPointF point, const QString &file) {
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(file)});
        const QPointF where = at(point);
        QDragEnterEvent enter(where, Qt::CopyAction, &data, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &enter);
        QDropEvent dropped(where, Qt::CopyAction, &data, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &dropped);
        return dropped.isAccepted();
    }
    QString panel() const { return window->property("panel").toString(); }

    // Something in the window, by the name it was given.
    static QQuickItem *find(QQuickItem *root, const QString &name) {
        if (root->objectName() == name)
            return root;
        const auto children = root->childItems();
        for (QQuickItem *child : children)
            if (QQuickItem *found = find(child, name))
                return found;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const { return find(window->contentItem(), name); }
    // Every piece of text the window has in it right now.
    static void texts(QQuickItem *root, QVector<QQuickItem *> &found) {
        if (root->inherits("QQuickText"))
            found.append(root);
        const auto children = root->childItems();
        for (QQuickItem *child : children)
            texts(child, found);
    }
    bool shows(const QString &name) const {
        const QQuickItem *found = item(name);
        return found && found->isVisible();
    }
    // All of `text`, somewhere in the window: cut short nowhere, by its own
    // box or by anything it is in.
    bool readsWhole(const QString &text) const {
        window->grabWindow();
        QVector<QQuickItem *> found;
        texts(window->contentItem(), found);
        return std::any_of(found.cbegin(), found.cend(), [this, &text](const QQuickItem *shown) {
            const QRectF box = shown->mapRectToScene(QRectF(0, 0, shown->width(), shown->height()));
            QRectF seen = box & QRectF(QPointF(0, 0), window->size());
            for (const QQuickItem *in = shown->parentItem(); in; in = in->parentItem())
                if (in->clip())
                    seen &= in->mapRectToScene(QRectF(0, 0, in->width(), in->height()));
            return shown->isVisible() && shown->property("text").toString() == text &&
                   !shown->property("truncated").toBool() &&
                   shown->property("contentHeight").toReal() <= shown->height() && seen == box;
        });
    }
    // A click in the middle of it. False when it is not there to click.
    bool clickOn(const QString &name) {
        // A frame first: rows of things are laid out as one is drawn, and a
        // person only ever clicks what they have seen.
        window->grabWindow();
        QQuickItem *found = item(name);
        if (!found || !found->isVisible())
            return false;
        const QPoint middle = found->mapToScene(QPointF(found->width() / 2, found->height() / 2)).toPoint();
        QTest::mouseMove(window, middle);
        QTest::mouseClick(window, Qt::LeftButton, {}, middle);
        return true;
    }

    quint32 add(const Item &item) { return board().add(item); }
    // The items of one kind, back to front.
    QVector<const Item *> all(Item::Kind kind) {
        QVector<const Item *> found;
        for (quint32 id : board().order())
            if (board().item(id)->kind == kind)
                found.append(board().item(id));
        return found;
    }
    const Item *last() { return board().count() ? board().item(board().order().last()) : nullptr; }
    QJsonObject onDisk() const {
        QJsonObject root;
        Library::read(library.boardFile(backend.boardName()), root);
        return root;
    }
};

// The frames a desk's canvas draws while one of these is about: what each
// came to by the canvas's own count (see SceneLoad), and what the GPU was
// really handed, counted node by node from the scene graph itself. For a
// board of strokes and shapes the two are the same in every frame, or
// something is drawn that the budget does not know of. Writing is only
// counted by the piece, so a board with any is not for wrong().
struct Frames {
    struct Frame {
        SceneLoad load;
        // What the scene graph held of the canvas's strokes and shapes, in
        // bytes as scene.cpp counts them (strokeBytes and shapeBytes), and
        // the triangles it held of anything else but the grid.
        qint64 held = 0, others = 0;
        // The pieces of writing it held: a note's title, say, or the lines
        // under it.
        int texts = 0;
        // How many items the board had as the frame was drawn.
        int items = 0;
        // Of `held`, what was drawn in `marked`, and in how many nodes: each
        // stroke is one, at whatever level of detail, and so is the stroke
        // being drawn.
        qint64 inMarked = 0;
        int nodesInMarked = 0;
        // All that the frame asked of the GPU.
        qint64 total() const { return load.bytes + load.draft + load.editor + load.chrome; }
    };
    Desk &desk;
    QVector<Frame> all;
    // A color the strokes in which are counted apart, if any.
    QColor marked;
    QMetaObject::Connection watching;

    explicit Frames(Desk &on) : desk(on) {
        watching = QObject::connect(
            desk.window, &QQuickWindow::afterSynchronizing, desk.canvas, [this] { record(); },
            Qt::DirectConnection);
    }
    ~Frames() { QObject::disconnect(watching); }

    void record() {
        Frame frame;
        frame.load = desk.canvas->load();
        frame.items = desk.board().count();
        if (const QSGNode *root = QQuickItemPrivate::get(desk.canvas)->paintNode)
            count(root, frame);
        all.append(frame);
    }
    void count(const QSGNode *node, Frame &frame) const {
        // What a piece of writing is drawn with is the scene graph's own,
        // and drawn in software it has a material that is not one: the
        // piece is counted, and not looked into.
        if (dynamic_cast<const QSGTextNode *>(node)) {
            ++frame.texts;
            return;
        }
        if (node->type() == QSGNode::GeometryNodeType) {
            const auto *drawn = static_cast<const QSGGeometryNode *>(node);
            const QSGGeometry *geometry = drawn->geometry();
            const qint64 triangles =
                !geometry ? 0 : (geometry->indexCount() > 0 ? geometry->indexCount() : geometry->vertexCount()) / 3;
            if (dynamic_cast<const StrokeMaterial *>(drawn->material())) {
                frame.held += triangles * 120;
                if (marked.isValid() && geometry && geometry->vertexCount() > 0) {
                    // Its corners are uploaded as the mesh has them (see
                    // Geo::StrokeVertex), each in the color of the stroke.
                    const auto *corner = static_cast<const Geo::StrokeVertex *>(geometry->vertexData());
                    if (corner->r == marked.red() && corner->g == marked.green() && corner->b == marked.blue()) {
                        frame.inMarked += triangles * 120;
                        ++frame.nodesInMarked;
                    }
                }
            } else if (dynamic_cast<const ShapeMaterial *>(drawn->material())) {
                frame.held += triangles * 240;
            } else if (!dynamic_cast<const GridMaterial *>(drawn->material())) {
                frame.others += triangles;
            }
        }
        for (const QSGNode *child = node->firstChild(); child; child = child->nextSibling())
            count(child, frame);
    }

    // What was wrong with the first frame that came to more than `budget`,
    // or held what it did not count. Nothing, when none did.
    QString wrong(qint64 budget) const {
        for (qsizetype i = 0; i < all.size(); ++i) {
            const Frame &frame = all[i];
            const qint64 counted = frame.load.bytes + frame.load.draft + frame.load.chrome;
            if (frame.total() > budget)
                return QString("frame %1 came to %2 of %3").arg(i).arg(frame.total()).arg(budget);
            if (frame.held != counted || frame.others > 0)
                return QString("frame %1 held %2 bytes and %3 other triangles, and counted %4 + %5 + %6")
                    .arg(i)
                    .arg(frame.held)
                    .arg(frame.others)
                    .arg(frame.load.bytes)
                    .arg(frame.load.draft)
                    .arg(frame.load.chrome);
        }
        return {};
    }
};

// What the canvas has in the scene graph right now, in triangles, of what is
// drawn with the stroke material (the pen's strokes, lines, and shapes that
// are not boxes) in `area` of the board: every node of that whose middle is
// in there. Its middle, as the point of a sharp corner can reach a little
// past the box of what it is drawn for.
qint64 strokedWithin(CanvasView *canvas, const QRectF &area) {
    qint64 triangles = 0;
    std::function<void(const QSGNode *)> visit = [&](const QSGNode *node) {
        // Writing is not looked into (see Frames::count()).
        if (dynamic_cast<const QSGTextNode *>(node))
            return;
        if (node->type() == QSGNode::GeometryNodeType) {
            const auto *drawn = static_cast<const QSGGeometryNode *>(node);
            const QSGGeometry *geometry = drawn->geometry();
            if (geometry && geometry->vertexCount() > 0 && dynamic_cast<const StrokeMaterial *>(drawn->material())) {
                // Its corners are uploaded as the mesh has them (see
                // Geo::StrokeVertex).
                const auto *corners = static_cast<const Geo::StrokeVertex *>(geometry->vertexData());
                QVector<QPointF> points;
                for (int i = 0; i < geometry->vertexCount(); ++i)
                    points.append(QPointF(corners[i].x, corners[i].y));
                if (area.contains(Geo::boundsOf(points).center()))
                    triangles += geometry->indexCount() / 3;
            }
        }
        for (const QSGNode *child = node->firstChild(); child; child = child->nextSibling())
            visit(child);
    };
    if (const QSGNode *root = QQuickItemPrivate::get(canvas)->paintNode)
        visit(root);
    return triangles;
}

// The middle of each line the canvas has in the scene graph right now that
// is drawn with the stroke material: halfway between the two corners across
// it at each point of its middle, and the hub of each fan of its round ends
// and corners (see Geo::StrokeLine).
QVector<QPointF> strokeMiddles(CanvasView *canvas) {
    QVector<QPointF> middles;
    std::function<void(const QSGNode *)> visit = [&](const QSGNode *node) {
        // Writing is not looked into (see Frames::count()).
        if (dynamic_cast<const QSGTextNode *>(node))
            return;
        if (node->type() == QSGNode::GeometryNodeType) {
            const auto *drawn = static_cast<const QSGGeometryNode *>(node);
            const QSGGeometry *geometry = drawn->geometry();
            if (geometry && dynamic_cast<const StrokeMaterial *>(drawn->material())) {
                const auto *corners = static_cast<const Geo::StrokeVertex *>(geometry->vertexData());
                for (int i = 0; i < geometry->vertexCount(); ++i) {
                    const Geo::StrokeVertex &corner = corners[i];
                    if (corner.side == 0)
                        middles.append(QPointF(corner.x, corner.y));
                    else if (corner.side > 0 && i + 1 < geometry->vertexCount() && corners[i + 1].side < 0)
                        middles.append(QPointF(corner.x + corners[i + 1].x, corner.y + corners[i + 1].y) / 2);
                }
            }
        }
        for (const QSGNode *child = node->firstChild(); child; child = child->nextSibling())
            visit(child);
    };
    if (const QSGNode *root = QQuickItemPrivate::get(canvas)->paintNode)
        visit(root);
    return middles;
}

// Draws along `path` with the pen in hand, a point at a time as a hand does,
// and lets go: the middle of the stroke as it was being drawn, just before.
QVector<QPointF> drawWithThePen(Desk &desk, const QVector<QPointF> &path) {
    desk.press(path.first());
    for (qsizetype i = 1; i < path.size(); ++i)
        desk.moveTo(path[i]);
    desk.window->grabWindow();
    const QVector<QPointF> drawing = strokeMiddles(desk.canvas);
    desk.release(path.last());
    desk.window->grabWindow();
    return drawing;
}

// Where the pen really went drawing along `path`: the mouse is on whole
// pixels of the window.
QVector<QPointF> wentAlong(Desk &desk, const QVector<QPointF> &path) {
    QVector<QPointF> went;
    for (const QPointF &point : path)
        went.append(desk.canvas->toCanvas(desk.canvas->mapFromScene(QPointF(desk.at(point)))));
    return went;
}

// The curve a stroke on the board is drawn along, where it is on the board,
// cut far more finely than it is drawn.
QVector<QPointF> curveOf(const Item &stroke) {
    QVector<QPointF> curve = Geo::smooth(stroke.points, 0.001);
    for (QPointF &point : curve)
        point += stroke.rect.topLeft();
    return curve;
}

// How far from the line through `other` the farthest point of `one` is.
qreal farthest(const QVector<QPointF> &one, const QVector<QPointF> &other) {
    qreal most = 0;
    for (const QPointF &point : one)
        most = qMax(most, Geo::distanceToPolyline(other, point));
    return most;
}

// How sharply `curve` turns where it goes through `point`, in degrees: from
// the piece of it into the point to the piece out, as finely as it is cut.
// None where it does not go through the point.
qreal turnAt(const QVector<QPointF> &curve, QPointF point) {
    for (qsizetype i = 1; i + 1 < curve.size(); ++i)
        if (curve[i] == point) {
            const QPointF in = curve[i] - curve[i - 1], out = curve[i + 1] - curve[i];
            const qreal turn = std::atan2(in.x() * out.y() - in.y() * out.x(), in.x() * out.x() + in.y() * out.y());
            return std::abs(qRadiansToDegrees(turn));
        }
    return 0;
}

// What changed that shows, if anything, when a stroke drawn at `zoom` was
// let go of: the stroke being drawn just before (`drawing`, its middle)
// should be within a third of a pixel of the curve it went on the board as
// (`curve`, see curveOf()), and the stroke on the board as it is drawn
// (`drawn`) within half a pixel of what was.
QString jumped(const QVector<QPointF> &drawing, const QVector<QPointF> &drawn, const QVector<QPointF> &curve,
               qreal zoom = 1) {
    if (drawing.isEmpty() || drawn.isEmpty())
        return "it was not drawn";
    const qreal before = farthest(drawing, curve) * zoom, after = farthest(drawn, drawing) * zoom;
    if (before < 0.36 && after < 0.5)
        return {};
    return QString("what was drawn was %1 pixels from the curve on the board, which is drawn %2 from it")
        .arg(before)
        .arg(after);
}

// Points along the lines through `corners` every `spacing`, from half that
// on, as a hand going at one pace gives them: where each line is a whole
// number of spacings long, each corner falls halfway between two points.
QVector<QPointF> pacedAlong(const QVector<QPointF> &corners, qreal spacing) {
    QVector<QPointF> path{corners.first()};
    qreal from = spacing / 2;
    for (qsizetype c = 1; c < corners.size(); ++c) {
        const QLineF line(corners[c - 1], corners[c]);
        for (; from < line.length(); from += spacing)
            path.append(line.pointAt(from / line.length()));
        from -= line.length();
    }
    path.append(corners.last());
    return path;
}

// The points a stroke on the board comes to a point at, where they are on
// the board: those it has twice (see Geo::smooth()).
QVector<QPointF> cornersOf(const Item &stroke) {
    QVector<QPointF> corners;
    for (qsizetype i = 1; i < stroke.points.size(); ++i)
        if (stroke.points[i] == stroke.points[i - 1])
            corners.append(stroke.points[i] + stroke.rect.topLeft());
    return corners;
}

// How far `point` is from the nearest of `points`.
qreal nearest(const QVector<QPointF> &points, QPointF point) {
    qreal near = std::numeric_limits<qreal>::infinity();
    for (const QPointF &other : points)
        near = qMin(near, std::hypot(other.x() - point.x(), other.y() - point.y()));
    return near;
}

// `point` turned by `degrees` about the origin, clockwise on the screen.
QPointF turnedBy(QPointF point, qreal degrees) {
    const qreal angle = qDegreesToRadians(degrees);
    return QPointF(point.x() * std::cos(angle) - point.y() * std::sin(angle),
                   point.x() * std::sin(angle) + point.y() * std::cos(angle));
}

} // namespace

class SprawlTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<CanvasView>("Sprawl", 1, 0, "CanvasView");
        qmlRegisterUncreatableType<Board>("Sprawl", 1, 0, "Board", "Boards belong to the backend");
    }

    // --- Geometry ----------------------------------------------------------

    void simplifyDropsWhatAPathDoesNotNeed() {
        QVector<QPointF> line;
        for (int i = 0; i <= 100; ++i)
            line.append(QPointF(i, 0.01 * (i % 2)));
        const auto simple = Geo::simplify(line, 0.5);
        QCOMPARE(simple.size(), 2);
        QCOMPARE(simple.first(), line.first());
        QCOMPARE(simple.last(), line.last());

        // A corner survives.
        const QVector<QPointF> corner{{0, 0}, {50, 0.1}, {100, 0}, {100, 50}, {100, 100}};
        QCOMPARE(Geo::simplify(corner, 0.5), (QVector<QPointF>{{0, 0}, {100, 0}, {100, 100}}));
    }

    void smoothPassesThroughEveryPoint() {
        const QVector<QPointF> points{{0, 0}, {40, 30}, {90, 10}, {120, 80}, {121, 81}, {300, 90}};
        const auto curve = Geo::smooth(points, 0.1);
        QVERIFY(curve.size() > points.size());
        for (const QPointF &point : points)
            QVERIFY2(Geo::distanceToPolyline(curve, point) < 1e-6, "the curve left a point behind");
        // And it never strays far from the path it smooths.
        for (const QPointF &point : curve)
            QVERIFY(Geo::distanceToPolyline(points, point) < 12);
    }

    void strokeMeshIsWellFormed() {
        Geo::StrokeMesh mesh;
        const QVector<QPointF> zigzag{{0, 0}, {50, 0}, {50, 50}, {0, 50}, {40, 40}, {40, 40}};
        Geo::strokePolyline(mesh, zigzag, QPointF(10, 10), 2, 0xffffffff);
        QVERIFY(!mesh.vertices.isEmpty());
        QCOMPARE(mesh.indices.size() % 3, 0);
        for (quint32 index : std::as_const(mesh.indices))
            QVERIFY(index < quint32(mesh.vertices.size()));
        // Nothing lands further out than the stroke is wide.
        for (const auto &v : std::as_const(mesh.vertices)) {
            QVERIFY(std::isfinite(v.x) && std::isfinite(v.y));
            QVERIFY(Geo::distanceToPolyline(zigzag, QPointF(v.x - 10, v.y - 10)) <= 2 * 1.5);
        }

        // A tap is a dot, and no points are nothing.
        Geo::StrokeMesh dot, none;
        Geo::strokePolyline(dot, {{5, 5}}, {}, 2, 0xffffffff);
        QVERIFY(dot.indices.size() >= 3 * 8);
        Geo::strokePolyline(none, {}, {}, 2, 0xffffffff);
        QVERIFY(none.indices.isEmpty());
    }

    void mitersReachTheCorner() {
        Geo::StrokeMesh mesh;
        Geo::strokePolyline(mesh, {{0, 0}, {100, 0}, {100, 100}}, {}, 1, 0xffffffff, Geo::Cap::Butt, Geo::Join::Miter);
        // The outside of a right-angle corner is a point, a half width out
        // both ways.
        bool found = false;
        for (const auto &v : std::as_const(mesh.vertices))
            found |= qAbs(v.x - 101) < 1e-4 && qAbs(v.y + 1) < 1e-4;
        QVERIFY(found);
    }

    void plainCornersCostLittle() {
        // A zigzag: every corner sharp.
        QVector<QPointF> zigzag;
        for (int i = 0; i < 40; ++i)
            zigzag.append(QPointF(i * 10, i % 2 ? 30 : 0));
        Geo::StrokeMesh round, plain;
        Geo::strokePolyline(round, zigzag, {}, 2, 0xffffffff);
        Geo::strokePolyline(plain, zigzag, {}, 2, 0xffffffff, Geo::Cap::Butt, Geo::Join::Bevel);
        QCOMPARE(plain.indices.size() % 3, 0);
        for (quint32 index : std::as_const(plain.indices))
            QVERIFY(index < quint32(plain.vertices.size()));
        // Two triangles to the segment and one to the corner.
        QCOMPARE(plain.indices.size() / 3, 39 * 2 + 38);
        QVERIFY(plain.indices.size() * 2 < round.indices.size());
        // And nothing sticks out of the stroke.
        for (const auto &v : std::as_const(plain.vertices))
            QVERIFY(Geo::distanceToPolyline(zigzag, QPointF(v.x, v.y)) <= 2 * 1.2);

        // A gentle bend is shared between its two segments.
        Geo::StrokeMesh bend;
        Geo::strokePolyline(bend, {{0, 0}, {100, 0}, {200, 20}}, {}, 2, 0xffffffff, Geo::Cap::Butt, Geo::Join::Bevel);
        QCOMPARE(bend.indices.size() / 3, 4);
        QCOMPARE(bend.vertices.size(), 6);
    }

    void everyShapeIsOneOutline() {
        QCOMPARE(Board::shapes().size(), Item::shapeCount);
        auto areaOf = [](const QVector<QPointF> &polygon) {
            qreal twice = 0;
            for (int i = 0; i < polygon.size(); ++i) {
                const QPointF a = polygon[i], b = polygon[(i + 1) % polygon.size()];
                twice += a.x() * b.y() - a.y() * b.x();
            }
            return std::abs(twice) / 2;
        };
        for (int index = 0; index < Item::shapeCount; ++index) {
            const Item::Shape shape = Item::Shape(index);
            const QByteArray name = Board::shapeName(shape).toUtf8();
            QVERIFY2(!name.isEmpty(), "a shape has no name");
            // As it is dropped, stretched wide, stretched tall, and tiny.
            for (const QSizeF &size : {Defaults::shapeSize(shape), QSizeF(400, 48), QSizeF(48, 400), QSizeF(16, 16)}) {
                const Geo::Figure figure = Geo::figure(shape, size);
                QVERIFY2(figure.edge.size() >= 3, name);
                for (const QPointF &point : figure.edge) {
                    QVERIFY2(std::isfinite(point.x()) && std::isfinite(point.y()), name);
                    QVERIFY2(point.x() >= -1e-6 && point.x() <= size.width() + 1e-6, name);
                    QVERIFY2(point.y() >= -1e-6 && point.y() <= size.height() + 1e-6, name);
                }
                // It reaches every side of its box.
                const QRectF bounds = Geo::boundsOf(figure.edge);
                QVERIFY2(bounds.width() > size.width() - 0.2 && bounds.height() > size.height() - 0.2, name);

                // Cut into triangles, it is neither more nor less than it was.
                const QVector<quint32> triangles = Geo::triangulate(figure.edge);
                QCOMPARE(triangles.size() % 3, 0);
                qreal covered = 0;
                for (int i = 0; i < triangles.size(); i += 3) {
                    QVERIFY(triangles[i] < quint32(figure.edge.size()));
                    covered += areaOf({figure.edge[triangles[i]], figure.edge[triangles[i + 1]],
                                       figure.edge[triangles[i + 2]]});
                }
                const qreal area = areaOf(figure.edge);
                QVERIFY2(std::abs(covered - area) <= area * 1e-6 + 1e-6,
                         qPrintable(QString("%1: %2 of %3").arg(QString(name)).arg(covered).arg(area)));

                // And it is drawn with triangles that are all there.
                Geo::StrokeMesh mesh;
                Geo::strokeFigure(mesh, figure, QPointF(5, 5), 0xff202020, 0xffffffff);
                QVERIFY(!mesh.indices.isEmpty());
                QCOMPARE(mesh.indices.size() % 3, 0);
                for (quint32 corner : std::as_const(mesh.indices))
                    QVERIFY(corner < quint32(mesh.vertices.size()));
                for (const auto &v : std::as_const(mesh.vertices))
                    QVERIFY2(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.nx) && std::isfinite(v.ny), name);

                // With fewer points it is still the same shape, give or take.
                const Geo::Figure plain = Geo::simplified(figure, 1.6);
                QVERIFY2(plain.edge.size() >= 3 && plain.edge.size() <= figure.edge.size(), name);
                QVERIFY2(std::abs(areaOf(plain.edge) - area) <= area * 0.2 + 8, name);
            }

            // What is written on it is inside it; clicks inside it are on
            // it and clicks in the corners of its box, for most, are not;
            // and a connector from any side meets it at its outline.
            const QRectF rect(QPointF(300, 200), Defaults::shapeSize(shape));
            const QRectF writing = Geo::textBox(shape, rect.size());
            QVERIFY2(writing.width() >= 1 && QRectF(QPointF(0, 0), rect.size()).adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(writing), name);
            QVERIFY2(Geo::shapeContains(shape, rect, rect.topLeft() + writing.center()), name);
            QVERIFY2(!Geo::shapeContains(shape, rect, rect.topLeft() - QPointF(1, 1)), name);
            QVector<QPointF> outline = Geo::figure(shape, rect.size(), 0.02).edge;
            outline.append(outline.first());
            for (int step = 0; step < 24; ++step) {
                const qreal angle = step * M_PI / 12 + 0.01;
                const QPointF target = rect.center() + QPointF(std::cos(angle), std::sin(angle)) * 2000;
                const QPointF met = Geo::shapeEdgeToward(shape, rect, target);
                QVERIFY2(Geo::distanceToPolyline(outline, met - rect.topLeft()) < 0.3,
                         qPrintable(QString("%1 toward %2").arg(QString(name)).arg(step)));
            }
        }
        // A notch is not part of the shape: the corner of a cross's box.
        QVERIFY(!Geo::shapeContains(Item::Shape::Cross, QRectF(0, 0, 90, 90), QPointF(10, 10)));
        QVERIFY(Geo::shapeContains(Item::Shape::Cross, QRectF(0, 0, 90, 90), QPointF(45, 10)));
        QVERIFY(!Geo::shapeContains(Item::Shape::Star, QRectF(0, 0, 100, 100), QPointF(8, 8)));
        // A star's points are met at their tips, not at the box they are in.
        QCOMPARE(Geo::shapeEdgeToward(Item::Shape::Triangle, QRectF(0, 0, 100, 100), QPointF(50, -500)), QPointF(50, 0));
        // The boxes are still the shader's, corners and all.
        QVERIFY(Geo::isQuad(Item::Shape::Pill) && !Geo::isQuad(Item::Shape::Star));
        QCOMPARE(Geo::cornerRadius(Item::Shape::Pill, QSizeF(152, 56), 0), 28.0);
        QCOMPARE(Geo::cornerRadius(Item::Shape::Rect, QSizeF(152, 56), 6), 6.0);
        QVERIFY(Geo::cornerRadius(Item::Shape::Rounded, QSizeF(152, 56), 0) >= 8);
    }

    void connectorsTakeARoute() {
        Item line = connector(0, 0);
        line.p1 = QPointF(100, 100);
        line.p2 = QPointF(300, 220);
        // Straight there.
        QCOMPARE(Geo::connectorPath(line), (QVector<QPointF>{{100, 100}, {300, 220}}));
        QCOMPARE(Geo::pathLength(Geo::connectorPath(line)), std::hypot(200.0, 120.0));

        // Out, across, and in, with square corners: halfway along, sideways
        // or up and down.
        line.route = Item::Route::Elbow;
        QCOMPARE(Geo::connectorPath(line), (QVector<QPointF>{{100, 100}, {200, 100}, {200, 220}, {300, 220}}));
        line.sideways = false;
        QCOMPARE(Geo::connectorPath(line), (QVector<QPointF>{{100, 100}, {100, 160}, {300, 160}, {300, 220}}));
        // Ends that are level need no corners.
        line.sideways = true;
        line.p2 = QPointF(300, 100);
        QCOMPARE(Geo::connectorPath(line), (QVector<QPointF>{{100, 100}, {300, 100}}));

        // A curve leaves each end square to it, and stays between them.
        line.route = Item::Route::Curved;
        line.p2 = QPointF(300, 220);
        const QVector<QPointF> curve = Geo::connectorPath(line);
        QVERIFY(curve.size() > 8);
        QCOMPARE(curve.first(), QPointF(100, 100));
        QCOMPARE(curve.last(), QPointF(300, 220));
        QVERIFY(qAbs(Geo::alongPath(curve, 0).heading.y()) < 0.1);
        QVERIFY(qAbs(Geo::alongPath(curve, Geo::pathLength(curve)).heading.y()) < 0.1);
        for (const QPointF &point : curve)
            QVERIFY(QRectF(100, 100, 200, 120).adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(point));
        // Its middle is halfway between its ends, heading down as much as across.
        const Geo::PathPoint middle = Geo::alongPath(curve, Geo::pathLength(curve) / 2);
        QVERIFY(QLineF(middle.at, QPointF(200, 160)).length() < 1);
        QVERIFY(middle.heading.y() > 0.5);
        // Cut finer, it is the same curve with more edges.
        QVERIFY(Geo::connectorPath(line, 0.01).size() > curve.size());
    }

    void connectorsHaveEnds() {
        Item line = connector(0, 0);
        line.p1 = QPointF(0, 0);
        line.p2 = QPointF(200, 0);
        line.width = 2;
        line.end = Item::Head::None;
        auto reach = [](const QVector<QPointF> &run) { return Geo::boundsOf(run); };

        // With nothing at its ends it is one line from end to end.
        Geo::ConnectorArt art = Geo::connectorArt(line);
        QCOMPARE(art.lines.size(), 1);
        QCOMPARE(art.lines[0], (QVector<QPointF>{{0, 0}, {200, 0}}));
        QVERIFY(art.heads.isEmpty());
        QCOMPARE(art.middle.at, QPointF(100, 0));

        // The ends that are shapes face out along the line, with their far
        // side at the end of it, and the line stops short inside them.
        const QList<QPair<Item::Head, Geo::Outline>> shapes{{Item::Head::Arrow, Geo::Outline::Arrowhead},
                                                            {Item::Head::Triangle, Geo::Outline::Arrowhead},
                                                            {Item::Head::Dot, Geo::Outline::Ellipse},
                                                            {Item::Head::Circle, Geo::Outline::Ellipse},
                                                            {Item::Head::Diamond, Geo::Outline::Diamond},
                                                            {Item::Head::OpenDiamond, Geo::Outline::Diamond}};
        for (const auto &[head, outline] : shapes) {
            line.start = line.end = head;
            art = Geo::connectorArt(line);
            QCOMPARE(art.lines.size(), 1);
            QCOMPARE(art.heads.size(), 2);
            const bool hollow = head == Item::Head::Triangle || head == Item::Head::Circle || head == Item::Head::OpenDiamond;
            for (const Geo::ConnectorArt::Head &end : std::as_const(art.heads)) {
                QCOMPARE(end.outline, outline);
                QCOMPARE(end.hollow, hollow);
            }
            QCOMPARE(art.heads[0].axis, QPointF(-1, 0));
            QCOMPARE(art.heads[1].axis, QPointF(1, 0));
            QVERIFY(qAbs(art.heads[0].center.x() - art.heads[0].half.width()) < 1e-6);
            QVERIFY(qAbs(art.heads[1].center.x() + art.heads[1].half.width() - 200) < 1e-6);
            QVERIFY(reach(art.lines[0]).left() > 1 && reach(art.lines[0]).right() < 199);
            // What is hollow is not drawn through: the line stops at its back.
            if (hollow) {
                QVERIFY(reach(art.lines[0]).right() <= 200 - art.heads[1].half.width() * 2 + line.width + 1e-6);
            }
        }
        // An open arrow and a bar are strokes: one more run of line each.
        line.start = Item::Head::Bar;
        line.end = Item::Head::Open;
        art = Geo::connectorArt(line);
        QCOMPARE(art.lines.size(), 3);
        QVERIFY(art.heads.isEmpty());
        // The bar is across the start and does not stick out past it; the
        // arrow's point is at the end and no further.
        QVERIFY(qAbs(reach(art.lines[0]).width()) < 1e-6 && reach(art.lines[0]).height() > 6);
        QVERIFY(reach(art.lines[0]).left() >= 1 - 1e-6);
        QCOMPARE(art.lines[1].size(), 3);
        QVERIFY(reach(art.lines[1]).right() <= 200 + 1e-6);

        // Dashed, the line is runs with gaps between them, all of them on it
        // and none past its ends; dotted, more and shorter ones.
        line.start = line.end = Item::Head::None;
        line.dash = Item::Dash::Dashed;
        const Geo::ConnectorArt dashed = Geo::connectorArt(line);
        QVERIFY(dashed.lines.size() >= 10 && dashed.lines.size() <= 20);
        qreal drawn = 0;
        for (const QVector<QPointF> &run : dashed.lines) {
            QVERIFY(reach(run).left() >= -1e-6 && reach(run).right() <= 200 + 1e-6);
            drawn += Geo::pathLength(run);
        }
        QVERIFY(drawn > 90 && drawn < 150);
        line.dash = Item::Dash::Dotted;
        QVERIFY(Geo::connectorArt(line).lines.size() > dashed.lines.size() * 2);

        // Around the corners of an elbow, a dash that meets a corner turns it.
        line.route = Item::Route::Elbow;
        line.dash = Item::Dash::Dashed;
        line.p2 = QPointF(200, 120);
        bool turned = false;
        for (const QVector<QPointF> &run : Geo::connectorArt(line).lines)
            turned |= run.size() > 2;
        QVERIFY(turned);
        // And two ends never outgrow a connector too short for them.
        line = connector(0, 0);
        line.p1 = QPointF(0, 0);
        line.p2 = QPointF(6, 0);
        line.start = Item::Head::Diamond;
        art = Geo::connectorArt(line);
        for (const Geo::ConnectorArt::Head &end : std::as_const(art.heads))
            QVERIFY(end.half.width() * 2 <= 6);
    }

    void colorsArePremultiplied() {
        QCOMPARE(Geo::packColor(QColor(255, 0, 0)), quint32(0xff0000ff));
        QCOMPARE(Geo::packColor(QColor(255, 255, 255), 0.5) >> 24, quint32(128));
        QCOMPARE(Geo::packColor(QColor(255, 255, 255), 0.5) & 0xff, quint32(128));
    }

    void connectorsLeaveShapesAtTheirEdge() {
        const QRectF rect(0, 0, 100, 60);
        QCOMPARE(Geo::shapeEdgeToward(Item::Shape::Rect, rect, QPointF(500, 30)), QPointF(100, 30));
        QCOMPARE(Geo::shapeEdgeToward(Item::Shape::Rect, rect, QPointF(50, -500)), QPointF(50, 0));
        QCOMPARE(Geo::shapeEdgeToward(Item::Shape::Ellipse, rect, QPointF(500, 30)), QPointF(100, 30));
        QCOMPARE(Geo::shapeEdgeToward(Item::Shape::Diamond, rect, QPointF(50, 500)), QPointF(50, 60));
        // Along a diagonal an ellipse and a diamond are inside the box.
        const QPointF corner = Geo::shapeEdgeToward(Item::Shape::Rect, rect, QPointF(150, 90));
        const QPointF round = Geo::shapeEdgeToward(Item::Shape::Ellipse, rect, QPointF(150, 90));
        const QPointF pointed = Geo::shapeEdgeToward(Item::Shape::Diamond, rect, QPointF(150, 90));
        QCOMPARE(corner, QPointF(100, 60));
        QVERIFY(round.x() < corner.x() && pointed.x() < round.x());
        QVERIFY(Geo::shapeContains(Item::Shape::Ellipse, rect, round - QPointF(0.01, 0.01)));
        QVERIFY(!Geo::shapeContains(Item::Shape::Diamond, rect, QPointF(95, 55)));
    }

    // --- Spatial grid --------------------------------------------------------

    void gridFindsWhatIsNear() {
        SpatialGrid grid;
        grid.insert(1, QRectF(0, 0, 100, 100));
        grid.insert(2, QRectF(5000, 5000, 100, 100));
        grid.insert(3, QRectF(400, 400, 300, 300));        // spans four cells
        grid.insert(4, QRectF(-1e6, -1e6, 2e6, 2e6));      // far too big for cells
        QVector<quint32> found;
        grid.query(QRectF(-10, -10, 50, 50), found);
        std::sort(found.begin(), found.end());
        QCOMPARE(found, (QVector<quint32>{1, 4}));

        grid.query(QRectF(450, 450, 200, 200), found);
        std::sort(found.begin(), found.end());
        QCOMPARE(found, (QVector<quint32>{3, 4}));

        grid.update(1, QRectF(5050, 5050, 10, 10));
        grid.query(QRectF(5000, 5000, 200, 200), found);
        std::sort(found.begin(), found.end());
        QCOMPARE(found, (QVector<quint32>{1, 2, 4}));

        grid.remove(4);
        grid.remove(2);
        grid.query(QRectF(-1e7, -1e7, 2e7, 2e7), found);
        std::sort(found.begin(), found.end());
        QCOMPARE(found, (QVector<quint32>{1, 3}));
        QCOMPARE(grid.count(), 2);
    }

    void gridKeepsUpWithManyItems() {
        SpatialGrid grid;
        for (quint32 i = 0; i < 20000; ++i)
            grid.insert(i + 1, QRectF((i % 200) * 180, (i / 200) * 180, 152, 152));
        QVector<quint32> found;
        grid.query(QRectF(900, 900, 1728, 1085), found);
        // Ten columns and seven rows of notes touch that window.
        QCOMPARE(found.size(), 10 * 7);
    }

    // --- Board ---------------------------------------------------------------

    void addingAndUndoing() {
        Board board;
        QSignalSpy modified(&board, &Board::modified);
        const quint32 first = board.add(note(0, 0, "one"));
        const quint32 second = board.add(note(200, 0, "two"));
        QCOMPARE(board.count(), 2);
        QCOMPARE(board.order(), (QVector<quint32>{first, second}));
        QCOMPARE(modified.count(), 2);

        board.undo();
        QCOMPARE(board.count(), 1);
        QVERIFY(!board.item(second));
        board.redo();
        QCOMPARE(board.order(), (QVector<quint32>{first, second}));
        QCOMPARE(board.item(second)->text, QString("two"));
        board.undo();
        board.undo();
        QCOMPARE(board.count(), 0);
        QVERIFY(!board.canUndo());
        QVERIFY(board.canRedo());
    }

    void oneStepHoweverMuchChanged() {
        Board board;
        const quint32 a = board.add(note(0, 0)), b = board.add(note(200, 0));
        board.begin();
        for (int step = 1; step <= 20; ++step)
            for (quint32 id : {a, b}) {
                Item moved = *board.item(id);
                moved.rect.translate(5, 0);
                board.set(moved);
            }
        QVERIFY(board.commit());
        QCOMPARE(board.item(a)->rect.x(), 100.0);
        board.undo();
        QCOMPARE(board.item(a)->rect.x(), 0.0);
        QCOMPARE(board.item(b)->rect.x(), 200.0);

        // A step that changes nothing is no step.
        board.begin();
        board.set(*board.item(a));
        QVERIFY(!board.commit());
    }

    void removedItemsComeBackWhereTheyWere() {
        Board board;
        QVector<quint32> ids;
        for (int i = 0; i < 6; ++i)
            ids.append(board.add(note(i * 200, 0)));
        const QVector<quint32> before = board.order();
        board.begin();
        board.remove(ids[1]);
        board.remove(ids[4]);
        board.remove(ids[2]);
        board.commit();
        QCOMPARE(board.order(), (QVector<quint32>{ids[0], ids[3], ids[5]}));
        board.undo();
        QCOMPARE(board.order(), before);
        board.redo();
        QCOMPARE(board.order(), (QVector<quint32>{ids[0], ids[3], ids[5]}));
    }

    void restackingIsUndone() {
        Board board;
        QVector<quint32> ids;
        for (int i = 0; i < 5; ++i)
            ids.append(board.add(note(i * 10, 0)));
        board.restack({ids[1], ids[3]}, true);
        QCOMPARE(board.order(), (QVector<quint32>{ids[0], ids[2], ids[4], ids[1], ids[3]}));
        board.restack({ids[4]}, false);
        QCOMPARE(board.order(), (QVector<quint32>{ids[4], ids[0], ids[2], ids[1], ids[3]}));
        board.undo();
        QCOMPARE(board.order(), (QVector<quint32>{ids[0], ids[2], ids[4], ids[1], ids[3]}));
        board.undo();
        QCOMPARE(board.order(), ids);
        board.redo();
        board.redo();
        QCOMPARE(board.order(), (QVector<quint32>{ids[4], ids[0], ids[2], ids[1], ids[3]}));
    }

    void rollbackLeavesNoTrace() {
        Board board;
        const quint32 kept = board.add(note(0, 0, "kept"));
        const quint64 revision = board.revision();
        board.begin();
        Item changed = *board.item(kept);
        changed.text = "changed";
        board.set(changed);
        const quint32 extra = board.add(note(300, 0));
        board.rollback();
        QCOMPARE(board.item(kept)->text, QString("kept"));
        QVERIFY(!board.item(extra));
        QCOMPARE(board.revision(), revision);
        board.undo();
        QCOMPARE(board.count(), 0); // the only step is still adding `kept`
    }

    void connectorsStayAttached() {
        Board board;
        const quint32 left = board.add(shape(Item::Shape::Rect, 0, 0));
        const quint32 right = board.add(shape(Item::Shape::Ellipse, 300, 0));
        const quint32 line = board.add(connector(left, right));
        QCOMPARE(board.item(line)->p1, QPointF(100, 30));
        QCOMPARE(board.item(line)->p2, QPointF(300, 30));

        QSignalSpy changes(&board, &Board::itemChanged);
        Item moved = *board.item(right);
        moved.rect.moveTo(300, 400);
        board.set(moved);
        // The connector followed, without being edited itself.
        QVERIFY(board.item(line)->p2.y() > 300);
        QCOMPARE(board.connectorsOf(right), (QVector<quint32>{line}));
        bool told = false;
        for (const auto &change : changes)
            told |= change[0].toUInt() == line;
        QVERIFY(told);

        board.undo();
        QCOMPARE(board.item(line)->p2, QPointF(300, 30));
    }

    void curvedAndElbowConnectorsLeaveTheMiddleOfASide() {
        Board board;
        const quint32 a = board.add(shape(Item::Shape::Rect, 0, 0, 100, 60));
        const quint32 b = board.add(shape(Item::Shape::Ellipse, 300, 200, 100, 60));
        Item line = connector(a, b);
        line.route = Item::Route::Elbow;
        const quint32 id = board.add(line);
        // Further apart across than down: out of the right side, in at the left.
        QCOMPARE(board.item(id)->p1, QPointF(100, 30));
        QCOMPARE(board.item(id)->p2, QPointF(300, 230));
        QVERIFY(board.item(id)->sideways);
        QCOMPARE(Geo::connectorPath(*board.item(id)),
                 (QVector<QPointF>{{100, 30}, {200, 30}, {200, 230}, {300, 230}}));
        // It is clicked on its corners, and not on the straight way between
        // its ends, which it does not take.
        QVERIFY(board.hits(*board.item(id), QPointF(200, 130), 3));
        QVERIFY(!board.hits(*board.item(id), QPointF(150, 80), 3));
        QVERIFY(board.touches(*board.item(id), QRectF(190, 100, 20, 20)));
        QVERIFY(!board.touches(*board.item(id), QRectF(120, 60, 40, 40)));

        // Moved under the other, it is further apart down: out of the
        // bottom, in at the top, and it follows as straight ones do.
        Item moved = *board.item(b);
        moved.rect.moveTo(20, 300);
        board.set(moved);
        QCOMPARE(board.item(id)->p1, QPointF(50, 60));
        QCOMPARE(board.item(id)->p2, QPointF(70, 300));
        QVERIFY(!board.item(id)->sideways);
        // And to the left of it, out of the left side.
        moved.rect.moveTo(-400, 0);
        board.set(moved);
        QCOMPARE(board.item(id)->p1, QPointF(0, 30));
        QCOMPARE(board.item(id)->p2, QPointF(-300, 30));

        // A straight one still goes from middle toward middle.
        Item straight = *board.item(id);
        straight.route = Item::Route::Straight;
        board.set(straight);
        QCOMPARE(board.item(id)->p1, QPointF(0, 30));
        // A star is met at the point of it that is on that side.
        Item star = *board.item(b);
        star.shape = Item::Shape::Star;
        star.rect = QRectF(0, 300, 100, 100);
        board.set(star);
        Item curved = *board.item(id);
        curved.route = Item::Route::Curved;
        board.set(curved);
        QCOMPARE(board.item(id)->p1, QPointF(50, 60));
        QCOMPARE(board.item(id)->p2, QPointF(50, 300));
        // A free end is where it was put, and the other end faces it.
        Item loose = *board.item(id);
        loose.to = 0;
        loose.p2 = QPointF(400, 20);
        board.set(loose);
        QCOMPARE(board.item(id)->p1, QPointF(100, 30));
        QCOMPARE(board.item(id)->p2, QPointF(400, 20));
        // All of it undone, it is as it was made.
        while (board.canUndo() && board.count() > 2)
            board.undo();
        board.redo();
        QCOMPARE(board.item(id)->route, Item::Route::Elbow);
        QCOMPARE(board.item(id)->p2, QPointF(300, 230));
    }

    void connectorsKeepWhatTheyAreLikeInFiles() {
        Board board;
        const quint32 a = board.add(note(0, 0)), b = board.add(note(400, 300));
        Item plain = connector(a, b);
        const quint32 first = board.add(plain);
        Item fancy = connector(a, b);
        fancy.route = Item::Route::Curved;
        fancy.start = Item::Head::Diamond;
        fancy.end = Item::Head::Triangle;
        fancy.dash = Item::Dash::Dotted;
        const quint32 second = board.add(fancy);

        // What a connector is to begin with is not written out.
        const QJsonObject ordinary = Board::itemToJson(*board.item(first));
        for (const char *key : {"route", "start", "end", "dash", "arrow"})
            QVERIFY2(!ordinary.contains(key), key);
        const QJsonObject written = Board::itemToJson(*board.item(second));
        QCOMPARE(written.value("route").toString(), QString("curved"));
        QCOMPARE(written.value("start").toString(), QString("diamond"));
        QCOMPARE(written.value("end").toString(), QString("triangle"));
        QCOMPARE(written.value("dash").toString(), QString("dotted"));

        Board loaded;
        QCOMPARE(loaded.fromJson(QJsonDocument::fromJson(QJsonDocument(board.toJson()).toJson()).object()), QStringList());
        QCOMPARE(*loaded.item(first), *board.item(first));
        QCOMPARE(*loaded.item(second), *board.item(second));
        QCOMPARE(loaded.toJson(), board.toJson());

        // Every name there is comes back as it went.
        QCOMPARE(Board::routes().size(), Item::routeCount);
        QCOMPARE(Board::heads().size(), Item::headCount);
        QCOMPARE(Board::dashes().size(), Item::dashCount);
        for (const QString &head : Board::heads()) {
            Item read;
            QCOMPARE(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9},
                                          {"start", head}, {"end", head}}, read), QString());
            QCOMPARE(Board::itemToJson(read).value("start").toString(head), head);
            QCOMPARE(int(read.start), int(Board::heads().indexOf(head)));
            QCOMPARE(read.end, read.start);
        }
        // A board from before there was a choice says only whether there
        // is an arrow; and what there is no such thing as is said.
        Item old;
        QCOMPARE(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}, {"arrow", false}}, old),
                 QString());
        QCOMPARE(old.end, Item::Head::None);
        QCOMPARE(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}}, old), QString());
        QCOMPARE(old.end, Item::Head::Arrow);
        QCOMPARE(old.start, Item::Head::None);
        QVERIFY(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}, {"route", "wiggly"}}, old)
                    .contains("elbow"));
        QVERIFY(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}, {"end", "claw"}}, old)
                    .contains("open-diamond"));
        QVERIFY(Board::itemFromJson({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}, {"dash", "wavy"}}, old)
                    .contains("dotted"));
    }

    void removingAnItemTakesItsConnectors() {
        Board board;
        const quint32 left = board.add(shape(Item::Shape::Rect, 0, 0));
        const quint32 right = board.add(shape(Item::Shape::Rect, 300, 0));
        const quint32 line = board.add(connector(left, right));
        const QVector<quint32> before = board.order();
        board.remove(left);
        QVERIFY(!board.item(line));
        QCOMPARE(board.count(), 1);
        QVERIFY(board.connectorsOf(right).isEmpty());
        board.undo();
        QCOMPARE(board.order(), before);
        QCOMPARE(board.item(line)->from, left);
        QCOMPARE(board.item(line)->p1, QPointF(100, 30));
        QCOMPARE(board.connectorsOf(left), (QVector<quint32>{line}));
    }

    void theItemInFrontIsHit() {
        Board board;
        const quint32 back = board.add(note(0, 0));
        const quint32 front = board.add(note(100, 100));
        QCOMPARE(board.itemAt(QPointF(120, 120), 4), front);
        QCOMPARE(board.itemAt(QPointF(20, 20), 4), back);
        QCOMPARE(board.itemAt(QPointF(-50, -50), 4), quint32(0));
        board.restack({back}, true);
        QCOMPARE(board.itemAt(QPointF(120, 120), 4), back);
        // And only those the caller wants.
        QCOMPARE(board.itemAt(QPointF(120, 120), 4, [back](const Item &item) { return item.id != back; }), front);
    }

    void thinThingsAreHitWithSlack() {
        Board board;
        const quint32 line = board.add(stroke({{0, 0}, {100, 0}}, 50, 50));
        QCOMPARE(board.itemAt(QPointF(100, 53), 4), line);
        QCOMPARE(board.itemAt(QPointF(100, 60), 4), quint32(0));
        // Moving a stroke never touches its points.
        Item moved = *board.item(line);
        moved.rect.translate(0, 100);
        board.set(moved);
        QCOMPARE(board.itemAt(QPointF(100, 151), 4), line);
        QCOMPARE(board.item(line)->points, (QVector<QPointF>{{0, 0}, {100, 0}}));
    }

    void framesAreTheirEdges() {
        Board board;
        Item frame;
        frame.kind = Item::Kind::Frame;
        frame.rect = QRectF(0, 0, 400, 300);
        const quint32 id = board.add(frame);
        const quint32 inside = board.add(note(100, 100));
        QCOMPARE(board.itemAt(QPointF(200, 2), 4), id);       // the title edge
        QCOMPARE(board.itemAt(QPointF(398, 150), 4), id);     // the right edge
        QCOMPARE(board.itemAt(QPointF(50, 150), 4), quint32(0)); // the empty inside
        QCOMPARE(board.itemAt(QPointF(150, 150), 4), inside);
        // A marquee inside a frame picks what is in it; one around it, it too.
        QVERIFY(!board.touches(*board.item(id), QRectF(50, 50, 300, 200)));
        QVERIFY(board.touches(*board.item(inside), QRectF(50, 50, 300, 200)));
        QVERIFY(board.touches(*board.item(id), QRectF(-10, -10, 500, 400)));
    }

    void movesAndEditsAreToldApart() {
        Board board;
        const quint32 id = board.add(note(0, 0));
        QSignalSpy changes(&board, &Board::itemChanged);
        Item item = *board.item(id);
        item.rect.translate(10, 10);
        board.set(item);
        QCOMPARE(changes.last()[1].value<Board::Change>(), Board::Change::Moved);
        item.text = "now with text";
        board.set(item);
        QCOMPARE(changes.last()[1].value<Board::Change>(), Board::Change::Edited);
    }

    void groupsAreFound() {
        Board board;
        const quint32 a = board.add(note(0, 0)), b = board.add(note(200, 0)), c = board.add(note(400, 0));
        const quint32 group = board.newGroup();
        board.begin();
        for (quint32 id : {a, c}) {
            Item item = *board.item(id);
            item.group = group;
            board.set(item);
        }
        board.commit();
        QCOMPARE(board.membersOf(group), (QVector<quint32>{a, c}));
        QVERIFY(board.newGroup() != group);
        Q_UNUSED(b);
    }

    // --- Files ---------------------------------------------------------------

    void boardsSurviveTheirFiles() {
        Board board;
        const quint32 left = board.add(note(10.126, 20, "Infinite canvas\npan, zoom, fit"));
        Item round = shape(Item::Shape::Ellipse, 300, 0);
        round.color = "magenta";
        round.text = "theme switch";
        const quint32 right = board.add(round);
        Item arrow = connector(left, right);
        arrow.text = "watch";
        board.add(arrow);
        Item pen = stroke({{0, 0}, {10.5, 4.25}, {30, -2}}, 40, 400);
        pen.color = "yellow";
        board.add(pen);
        Item title;
        title.kind = Item::Kind::Text;
        title.text = "Infinite canvas for Omarchy";
        title.fontSize = 28;
        title.bold = true;
        title.rect = QRectF(0, -100, 400, 40);
        board.add(title);
        Item frame;
        frame.kind = Item::Kind::Frame;
        frame.rect = QRectF(-20, -20, 600, 300);
        frame.text = "v0.1 scope";
        frame.group = 3;
        board.add(frame);

        const QJsonObject saved = board.toJson();
        Board loaded;
        QCOMPARE(loaded.fromJson(QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object()), QStringList());
        QCOMPARE(loaded.count(), board.count());
        QCOMPARE(loaded.order(), board.order());
        // Saving what was loaded writes the same file.
        QCOMPARE(loaded.toJson(), saved);
        QCOMPARE(loaded.item(left)->rect.x(), 10.13);
        QCOMPARE(loaded.item(right)->shape, Item::Shape::Ellipse);
        QCOMPARE(loaded.connectorsOf(left).size(), 1);
        QVERIFY(!loaded.canUndo());
        QVERIFY(loaded.newGroup() > 3);
    }

    void everyShapeHasANameInAFile() {
        Board board;
        for (int index = 0; index < Item::shapeCount; ++index) {
            Item item = shape(Item::Shape(index), index * 200, 0, 140, 90);
            item.text = "Step";
            board.add(item);
        }
        Board loaded;
        QCOMPARE(loaded.fromJson(QJsonDocument::fromJson(QJsonDocument(board.toJson()).toJson()).object()), QStringList());
        QCOMPARE(loaded.count(), Item::shapeCount);
        QSet<QString> names;
        for (quint32 id : loaded.order()) {
            QCOMPARE(loaded.item(id)->shape, board.item(id)->shape);
            names.insert(Board::shapeName(loaded.item(id)->shape));
        }
        QCOMPARE(names.size(), Item::shapeCount);
        // The first three are spelled as they always were.
        QCOMPARE(Board::shapeName(Item::Shape::Rect), QString("rect"));
        QCOMPARE(Board::shapeName(Item::Shape::Ellipse), QString("ellipse"));
        QCOMPARE(Board::shapeName(Item::Shape::Diamond), QString("diamond"));

        // Without a size, a shape is the size that suits it; and a shape
        // there is not is named, with the ones there are.
        Item star;
        QCOMPARE(Board::itemFromJson({{"type", "shape"}, {"x", 0}, {"y", 0}, {"shape", "star"}}, star), QString());
        QCOMPARE(star.rect.size(), Defaults::shapeSize(Item::Shape::Star));
        const QString problem = Board::itemFromJson({{"type", "shape"}, {"x", 0}, {"y", 0}, {"shape", "blob"}}, star);
        QVERIFY(problem.contains("blob") && problem.contains("hexagon") && problem.contains("cylinder"));
    }

    void strokesAreStoredFromTheirCorner() {
        Board board;
        const QJsonObject file{
            {"sprawl", 1},
            {"items", QJsonArray{QJsonObject{{"type", "stroke"}, {"x", 100}, {"y", 100},
                                             {"points", QJsonArray{10, 20, 60, 20, 60, 90}}}}}};
        QCOMPARE(board.fromJson(file), QStringList());
        const Item *pen = board.item(board.order().first());
        QCOMPARE(pen->rect, QRectF(110, 120, 50, 70));
        QCOMPARE(pen->points, (QVector<QPointF>{{0, 0}, {50, 0}, {50, 70}}));
        QCOMPARE(pen->width, Defaults::penWidth);
    }

    void filesSayWhatIsWrongWithThem() {
        Board board;
        board.add(note(0, 0, "still here"));
        auto problems = [&board](const QByteArray &json) {
            return board.fromJson(QJsonDocument::fromJson(json).object()).join(" ");
        };
        QVERIFY(problems("{}").contains("not a Sprawl board"));
        QVERIFY(problems(R"({"sprawl": 99})").contains("newer Sprawl"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "sticker", "x": 0, "y": 0}]})").contains("no item type"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "note", "y": 0}]})").contains("\"x\" must be a number"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "note", "x": 0, "y": 0, "color": "purple"}]})")
                    .contains("no color"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "stroke", "points": [1, 2, 3]}]})").contains("pairs"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "connector", "id": 2, "from": 9, "x2": 0, "y2": 0}]})")
                    .contains("no item 9"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"type": "image", "x": 0, "y": 0, "w": 9, "h": 9, "asset": "../x.png"}]})")
                    .contains("assets folder"));
        QVERIFY(problems(R"({"sprawl": 1, "items": [{"id": 1, "type": "note", "x": 0, "y": 0}, {"id": 1, "type": "note", "x": 0, "y": 0}]})")
                    .contains("used twice"));
        // A file that would not load leaves the board as it was.
        QCOMPARE(board.count(), 1);
        QCOMPARE(board.item(board.order().first())->text, QString("still here"));
    }

    void itemsWithoutIdsGetThem() {
        Board board;
        const QByteArray file = R"({"sprawl": 1, "items": [
            {"type": "note", "x": 0, "y": 0, "text": "written by hand"},
            {"id": 7, "type": "shape", "x": 300, "y": 0, "shape": "diamond"},
            {"type": "connector", "from": 7, "x2": 500, "y2": 300}]})";
        QCOMPARE(board.fromJson(QJsonDocument::fromJson(file).object()), QStringList());
        QCOMPARE(board.count(), 3);
        QSet<quint32> ids(board.order().begin(), board.order().end());
        QCOMPARE(ids.size(), 3);
        QVERIFY(!ids.contains(0));
        QCOMPARE(board.item(board.order().first())->color, QString("yellow"));
        QCOMPARE(board.add(note(0, 0)), quint32(10));
    }

    // --- Saving, and changes from outside ---------------------------------

    void everyChangeIsARevisionAsItIsMade() {
        Board board;
        const quint32 a = board.add(note(0, 0)), b = board.add(note(200, 0));
        Item words;
        words.kind = Item::Kind::Text;
        words.text = "Measured";
        words.rect = QRectF(0, 300, 2, 16);
        const quint32 c = board.add(words);
        QSet<quint64> seen{board.revision()};
        // A number the board has not had before.
        auto fresh = [&board, &seen] {
            const bool unseen = !seen.contains(board.revision());
            seen.insert(board.revision());
            return unseen;
        };
        const quint64 before = board.revision();
        QSignalSpy modified(&board, &Board::modified);

        // Each change counts as it is made, with its step still open: what
        // is being typed or dragged is a board of its own, to be saved.
        board.begin();
        Item typed = *board.item(a);
        typed.text = "Half";
        board.set(typed);
        QVERIFY(fresh());
        board.restack({a}, true);
        QVERIFY(fresh());
        board.remove(b);
        QVERIFY(fresh());
        board.add(note(400, 0));
        QVERIFY(fresh());
        QCOMPARE(modified.count(), 4);
        // Rolled back, it is the board it began as, by the same number.
        board.rollback();
        QCOMPARE(board.revision(), before);
        QCOMPARE(modified.count(), 5);

        // Done again, it is a number never seen, not one it had along the
        // way; and ending a step, or measuring text, is no change at all.
        board.begin();
        board.set(typed);
        QVERIFY(fresh());
        const quint64 typing = board.revision();
        QVERIFY(board.commit());
        board.setMeasuredRect(c, QRectF(0, 300, 96, 21));
        QCOMPARE(board.revision(), typing);
        QCOMPARE(modified.count(), 6);
        // Undoing and redoing are changes; so is a whole new board.
        board.undo();
        QVERIFY(fresh());
        board.redo();
        QVERIFY(fresh());
        QCOMPARE(board.fromJson(board.toJson()), QStringList());
        QVERIFY(fresh());
    }

    void aFileReadInTheMiddleOfAStepLeavesNothingOfIt() {
        Board board;
        const quint32 a = board.add(note(0, 0)), b = board.add(note(200, 0)), c = board.add(note(400, 0));
        // The same board, and the same stacked the other way round, as
        // someone else might write it.
        const QJsonObject same = board.toJson();
        const QJsonArray items = same.value("items").toArray();
        QJsonArray reversed;
        for (const QJsonValue &value : items)
            reversed.prepend(value);
        const QJsonObject turned{{"sprawl", Board::formatVersion}, {"items", reversed}};

        // Read in the middle of a step that changed the order, as a change
        // from outside is: a step taken back after it leaves the board as it
        // was read, which is the number it had then.
        board.begin();
        board.restack({a}, true);
        QCOMPARE(board.fromJson(turned), QStringList());
        QCOMPARE(board.order(), (QVector<quint32>{c, b, a}));
        const quint64 read = board.revision();
        board.begin();
        board.remove(a);
        board.rollback();
        QCOMPARE(board.order(), (QVector<quint32>{c, b, a}));
        QCOMPARE(board.revision(), read);
        // And one that is kept is undone to just that, too.
        board.begin();
        board.restack({c}, true);
        QCOMPARE(board.fromJson(same), QStringList());
        board.remove(a);
        board.undo();
        QCOMPARE(board.order(), (QVector<quint32>{a, b, c}));
    }

    void textWithNothingInItIsWrittenAsItWas() {
        Board board;
        const quint32 card = board.add(note(400, 0, "Card"));
        Item words;
        words.kind = Item::Kind::Text;
        words.text = "Label";
        words.rect = QRectF(0, 0, 2, 16);
        const quint32 label = board.add(words);
        const quint32 line = board.add(connector(label, card));
        Item blank = words;
        blank.text.clear();
        blank.rect.moveTo(0, 200);
        const quint32 untouched = board.add(blank);
        auto written = [&board] {
            QVector<quint32> ids;
            for (const Item &item : board.itemsToWrite())
                ids << item.id;
            return ids;
        };
        // An item as it is written; nothing when it is not.
        auto writtenAs = [&board](quint32 id) {
            for (const Item &item : board.itemsToWrite())
                if (item.id == id)
                    return item;
            return Item();
        };
        QCOMPARE(written(), (QVector<quint32>{card, label, line, untouched}));

        // Text being written that holds nothing is written as it was before
        // the step: emptied to be typed again, as it was, connector and all;
        // new, not at all, and neither is what is attached to it. Nothing is
        // lost, no text is written with nothing in it to see, and the file
        // is one that opens.
        const Item labelAsItWas = *board.item(label);
        board.begin();
        Item emptied = labelAsItWas;
        emptied.text = " ";
        board.set(emptied);
        const quint32 started = board.add(blank);
        const quint32 tied = board.add(connector(started, card));
        QCOMPARE(written(), (QVector<quint32>{card, label, line, untouched}));
        QVERIFY(writtenAs(label) == labelAsItWas);
        QVector<Item> read;
        QCOMPARE(Board::parse(board.toJson(), read), QStringList());
        // Everything is still there for whatever else asks.
        QCOMPARE(board.items().size(), 6);
        // Something in them, and they are written as they are; nothing
        // again, and as they were.
        emptied.text = "Label, again";
        board.set(emptied);
        QCOMPARE(writtenAs(label).text, QString("Label, again"));
        Item typed = *board.item(started);
        typed.text = "Typed";
        board.set(typed);
        QCOMPARE(written(), (QVector<quint32>{card, label, line, untouched, started, tied}));
        typed.text.clear();
        board.set(typed);
        emptied.text.clear();
        board.set(emptied);
        QCOMPARE(written(), (QVector<quint32>{card, label, line, untouched}));
        QVERIFY(writtenAs(label) == labelAsItWas);
        // Empty text that was there before the step is written as it is,
        // moved or not: the file has it so already.
        Item moved = *board.item(untouched);
        moved.rect.translate(20, 0);
        board.set(moved);
        QVERIFY(writtenAs(untouched) == moved);

        // A step ended in a way that keeps the empty text writes it as it is
        // from then on, and that is a change, to be saved.
        const quint64 before = board.revision();
        QSignalSpy modified(&board, &Board::modified);
        QVERIFY(board.commit());
        QVERIFY(board.revision() != before);
        QCOMPARE(modified.count(), 1);
        QCOMPARE(written(), (QVector<quint32>{card, label, line, untouched, started, tied}));
        QCOMPARE(writtenAs(label).text, QString());
        // A step that ends with nothing written in place of anything is no
        // change at all.
        board.begin();
        moved.rect.translate(20, 0);
        board.set(moved);
        const quint64 dragged = board.revision();
        QVERIFY(board.commit());
        QCOMPARE(board.revision(), dragged);
    }

    void whatIsTypedIsSavedBeforeItIsFinished() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        desk.type("Half a thought");
        // A pause in the typing, and it is on disk, the note still open.
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->editing());
        QJsonArray items = desk.onDisk().value("items").toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value("text").toString(), QString("Half a thought"));
        // And so is what is typed after the pause.
        desk.type(", finished");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->editing());
        items = desk.onDisk().value("items").toArray();
        QCOMPARE(items.first().toObject().value("text").toString(), QString("Half a thought, finished"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void closingTheWindowKeepsWhatIsBeingTyped() {
        {
            Desk desk;
            QVERIFY(desk.ready());
            desk.hover(QPointF(300, 200));
            desk.key(Qt::Key_Space);
            desk.type("Last words");
            // While writing, Q is a letter: the window is closed the
            // desktop's way (Super+W), in the middle of the note.
            QVERIFY(desk.window->close());
            QVERIFY(!desk.canvas->editing());
            QCOMPARE(desk.backend.saveState(), QString("saved"));
            const QJsonArray items = desk.onDisk().value("items").toArray();
            QCOMPARE(items.size(), 1);
            QCOMPARE(items.first().toObject().value("text").toString(), QString("Last words"));
            QCOMPARE(desk.warnings, QStringList());
        }
        // Writing that never got going is finished as Esc would finish it:
        // it leaves nothing behind.
        Desk desk;
        QVERIFY(desk.ready());
        desk.doubleClick(QPointF(400, 300));
        QVERIFY(desk.canvas->editing());
        QVERIFY(desk.window->close());
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 0);
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void closingTheWindowKeepsTheStrokeBeingDrawn() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        QCOMPARE(desk.canvas->tool(), QString("pen"));
        desk.press(QPointF(100, 400));
        for (int x = 110; x <= 300; x += 10)
            desk.moveTo(QPointF(x, 400 + qRound(20 * std::sin((x - 100) / 30.0))));
        QVERIFY(desk.canvas->busy());
        QCOMPARE(desk.board().count(), 0);
        // The window is closed with the pen still down: the stroke is kept
        // as far as it got, as it is when the mouse is taken away.
        QVERIFY(desk.window->close());
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->kind, Item::Kind::Stroke);
        QCOMPARE(desk.last()->rect.left(), 100.0);
        QCOMPARE(desk.last()->rect.right(), 300.0);
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray items = desk.onDisk().value("items").toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject(), Board::itemToJson(*desk.last()));
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsBeingTypedOutlivesTheApp() {
        QByteArray file;
        {
            Desk desk;
            QVERIFY(desk.ready());
            desk.hover(QPointF(300, 200));
            desk.key(Qt::Key_Space);
            desk.type("Cut short");
            QVERIFY(desk.canvas->editing());
            // The app ends there, its window never closed. The file is read
            // the moment the backend that writes it is gone, and before its
            // folder goes too.
            QObject::connect(&desk.backend, &QObject::destroyed,
                             [&file, path = desk.library.boardFile("desk")] { file = read(path); });
        }
        const QJsonArray items = QJsonDocument::fromJson(file).object().value("items").toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value("text").toString(), QString("Cut short"));
    }

    void textWithNothingInItIsSavedAsItWas() {
        Desk desk;
        QVERIFY(desk.ready());
        // What is on disk: each item's type, and what it says.
        auto written = [&desk] {
            QStringList found;
            const QJsonArray items = desk.onDisk().value("items").toArray();
            for (const QJsonValue &value : items)
                found << value.toObject().value("type").toString() + ":" + value.toObject().value("text").toString();
            return found;
        };

        // Writing begun with a double click, and a pause before anything is
        // typed: the board is saved, and the text is not in it.
        desk.doubleClick(QPointF(400, 300));
        QVERIFY(desk.canvas->editing());
        QVERIFY(desk.backend.saveState() != "saved");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), QStringList());
        // What is typed is saved; taken out again, or left as spaces, it is
        // not, as Esc then would leave nothing.
        desk.type("Maybe");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), QStringList{"text:Maybe"});
        for (int i = 0; i < 5; ++i)
            desk.key(Qt::Key_Backspace);
        desk.type("  ");
        QVERIFY(desk.backend.saveState() != "saved");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->editing());
        QCOMPARE(written(), QStringList());
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 0);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), QStringList());

        // Text that was there, emptied to be written again: while it holds
        // nothing it is on disk as it was, with the connector attached to
        // it, so nothing is lost should the app end there.
        const quint32 card = desk.add(note(600, 432, "Card"));
        Item words;
        words.kind = Item::Kind::Text;
        words.fontSize = Defaults::textSize;
        words.text = "Label";
        words.rect = QRectF(300, 500, 2, 16);
        const quint32 label = desk.add(words);
        desk.add(connector(label, card));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"note:Card", "text:Label", "connector:"}));
        // Drawn, so it is as big as its type, and there to double click.
        desk.window->grabWindow();
        desk.doubleClick(desk.board().item(label)->rect.center());
        QVERIFY(desk.canvas->editing());
        for (int i = 0; i < 5; ++i)
            desk.key(Qt::Key_Backspace);
        QCOMPARE(desk.board().item(label)->text, QString());
        QVERIFY(desk.backend.saveState() != "saved");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"note:Card", "text:Label", "connector:"}));
        QVector<Item> read;
        QCOMPARE(Board::parse(desk.onDisk(), read), QStringList());
        desk.type("Again");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"note:Card", "text:Again", "connector:"}));
        // Emptied and finished, it is gone, and its connector with it.
        for (int i = 0; i < 5; ++i)
            desk.key(Qt::Key_Backspace);
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().count(), 1);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), QStringList{"note:Card"});
        QCOMPARE(desk.warnings, QStringList());
    }

    void aDragTakenBackWithEscIsSavedAsItWas() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Here"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // Something added a moment before is still to be written as the
        // drag begins.
        desk.add(note(600, 100, "There"));
        desk.press(QPointF(150, 150));
        desk.moveTo(QPointF(200, 200));
        desk.moveTo(QPointF(300, 320));
        QVERIFY(desk.canvas->busy());
        const QPointF dragged = desk.board().item(id)->rect.topLeft();
        QVERIFY(dragged != QPointF(100, 100));
        auto written = [&desk] {
            const QJsonObject first = desk.onDisk().value("items").toArray().first().toObject();
            return QPointF(first.value("x").toDouble(), first.value("y").toDouble());
        };
        // Held there for a while, it is written where it is.
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), dragged);
        // Esc takes the drag back, and the file goes back with it.
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(100, 100));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), QPointF(100, 100));
        desk.release(QPointF(300, 320));
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(100, 100));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aDragThatNeverPausesDoesNotPutOffSaving() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Here"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        auto written = [&desk] {
            QStringList texts;
            const QJsonArray items = desk.onDisk().value("items").toArray();
            for (const QJsonValue &value : items)
                texts << value.toObject().value("text").toString();
            return texts;
        };
        // Something added a moment before a drag that then keeps moving, a
        // step every tenth of a second: never still for as long as a save
        // waits for a pause.
        desk.add(note(600, 100, "There"));
        desk.press(QPointF(150, 150));
        QPointF at(200, 200);
        desk.moveTo(at);
        QVERIFY(desk.canvas->busy());
        for (int step = 0; step < 100 && written().size() < 2; ++step) {
            at += QPointF(4, 2);
            desk.moveTo(at);
            QTest::qWait(100);
        }
        // It is written all the same, the drag still going.
        QCOMPARE(written(), (QStringList{"Here", "There"}));
        QVERIFY(desk.canvas->busy());
        desk.release(at);
        QVERIFY(!desk.canvas->busy());
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonObject first = desk.onDisk().value("items").toArray().first().toObject();
        QCOMPARE(QPointF(first.value("x").toDouble(), first.value("y").toDouble()),
                 desk.board().item(id)->rect.topLeft());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardHoldsNoBoxTooSmallToReadBack() {
        Board board;
        // A picture far wider than tall, brought down to a size to arrange,
        // comes to no height at all. It is held a unit high, the least a file
        // has, from where it was put.
        Item rule;
        rule.kind = Item::Kind::Image;
        rule.asset = "rule.png";
        rule.rect = QRectF(268, 337, 420, 0);
        const quint32 picture = board.add(rule);
        QCOMPARE(board.item(picture)->rect, QRectF(268, 337, 420, 1));
        // So is every kind of box, either way, however it is changed.
        QVector<quint32> boxes{picture};
        for (const Item::Kind kind : {Item::Kind::Note, Item::Kind::Shape, Item::Kind::Frame}) {
            Item box = note(0, 0);
            box.kind = kind;
            box.rect = QRectF(10, 20, 0.3, 0);
            const quint32 id = board.add(box);
            QCOMPARE(board.item(id)->rect, QRectF(10, 20, 1, 1));
            box = *board.item(id);
            box.rect = QRectF(40, 20, 0, 0.3);
            board.set(box);
            QCOMPARE(board.item(id)->rect, QRectF(40, 20, 1, 1));
            box.rect = QRectF(40, 20, 16, 0.8);
            board.set(box);
            QCOMPARE(board.item(id)->rect, QRectF(40, 20, 16, 1));
            boxes << id;
        }
        // What is undone and redone is what was held.
        board.undo();
        QCOMPARE(board.item(boxes.last())->rect, QRectF(40, 20, 1, 1));
        board.redo();
        QCOMPARE(board.item(boxes.last())->rect, QRectF(40, 20, 16, 1));
        // Made too small again, it is what it is held as already: no change,
        // and nothing to save or undo.
        const quint64 revision = board.revision();
        QSignalSpy modified(&board, &Board::modified);
        Item again = *board.item(picture);
        again.rect.setHeight(0.4);
        board.begin();
        board.set(again);
        QVERIFY(!board.commit());
        QCOMPARE(board.revision(), revision);
        QCOMPARE(modified.count(), 0);
        // A side that is no number at all is no size either, and no file
        // could say it: it is held as a unit too.
        Item unmeasured = shape(Item::Shape::Ellipse, 0, 200);
        unmeasured.rect.setWidth(qQNaN());
        const quint32 blank = board.add(unmeasured);
        QCOMPARE(board.item(blank)->rect, QRectF(0, 200, 1, 60));
        unmeasured = *board.item(blank);
        unmeasured.rect.setHeight(qQNaN());
        board.set(unmeasured);
        QCOMPARE(board.item(blank)->rect, QRectF(0, 200, 1, 1));
        boxes << blank;
        // A stroke is no box: its size is where its points are, which is what
        // a file says of it, and a dead straight one has no height.
        const quint32 line = board.add(stroke({{0, 0}, {100, 0}}, 0, 400));
        QCOMPARE(board.item(line)->rect, QRectF(0, 400, 100, 0));

        // Saved and read back, all of it is as it was held.
        Board loaded;
        QCOMPARE(loaded.fromJson(QJsonDocument::fromJson(QJsonDocument(board.toJson()).toJson()).object()), QStringList());
        QCOMPARE(loaded.toJson(), board.toJson());
        for (quint32 id : std::as_const(boxes))
            QCOMPARE(loaded.item(id)->rect, board.item(id)->rect);
        QCOMPARE(loaded.item(line)->rect, board.item(line)->rect);
    }

    void boxesOfNoSizeInAFileAreReadAsTheSmallest() {
        // 0.1.0 saved a picture far wider than tall with no height at all,
        // and then could not open its board. Less than a unit across or down
        // is read as a unit, and the board opens.
        const QByteArray file = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "work worth keeping"},
            {"id": 2, "type": "image", "x": 268, "y": 337, "w": 420, "h": 0, "asset": "rule.png"},
            {"id": 3, "type": "shape", "x": 0, "y": 300, "w": 0.4, "h": 56},
            {"id": 4, "type": "frame", "x": 0, "y": 500, "w": 0, "h": 0.99, "text": "Thin"}]})";
        Board board;
        QCOMPARE(board.fromJson(QJsonDocument::fromJson(file).object()), QStringList());
        QCOMPARE(board.count(), 4);
        QCOMPARE(board.item(2)->rect, QRectF(268, 337, 420, 1));
        QCOMPARE(board.item(3)->rect, QRectF(0, 300, 1, 56));
        QCOMPARE(board.item(4)->rect, QRectF(0, 500, 1, 1));
        // Saved again, they are written as they are held, which any Sprawl
        // reads, 0.1.0 too.
        const QJsonArray written = board.toJson().value("items").toArray();
        QCOMPARE(written[1].toObject().value("h").toDouble(), 1.0);
        QCOMPARE(written[2].toObject().value("w").toDouble(), 1.0);
        QCOMPARE(written[3].toObject().value("w").toDouble(), 1.0);
        QCOMPARE(written[3].toObject().value("h").toDouble(), 1.0);
        Board again;
        QCOMPARE(again.fromJson(board.toJson()), QStringList());
        QCOMPARE(again.toJson(), board.toJson());

        // Less than nothing, or no number at all, is still no size.
        Item item;
        QCOMPARE(Board::itemFromJson({{"type", "note"}, {"x", 0}, {"y", 0}, {"w", -1}}, item),
                 QString("\"w\" must be between 0 and 1e+09"));
        QCOMPARE(Board::itemFromJson({{"type", "image"}, {"x", 0}, {"y", 0}, {"w", 420}, {"h", -0.5}, {"asset", "rule.png"}},
                                     item),
                 QString("\"h\" must be between 0 and 1e+09"));
        for (const double size : {qQNaN(), qInf(), -qInf()})
            QCOMPARE(Board::itemFromJson({{"type", "frame"}, {"x", 0}, {"y", 0}, {"h", size}}, item),
                     QString("\"h\" must be a number"));
        QCOMPARE(Board::itemFromJson({{"type", "shape"}, {"x", 0}, {"y", 0}, {"w", "wide"}}, item),
                 QString("\"w\" must be a number"));
    }

    void aPictureAddedByCommandComesAtASizeToArrange() {
        // Neither a screenful nor a sliver, as Defaults::pictureSize has it:
        // brought within 420 by 320, then grown in proportion until no side
        // is less than anything can be made, as far as that allows. A picture
        // far wider than tall is a long thin box, not a line a unit high that
        // can hardly be seen or picked, and one far taller than wide is
        // neither a dot nor taller than 320.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        auto file = [&home](QSize size) {
            return picture(home.filePath(QString("%1x%2.png").arg(size.width()).arg(size.height())), size);
        };
        QVector<int> codes{runCli({"sprawl", "new", "pictures"})};
        for (const QSize size :
             {QSize(2000, 2), QSize(2, 2000), QSize(4, 2000), QSize(333, 1000), QSize(800, 600), QSize(6, 3)})
            codes << runCli({"sprawl", "add", "pictures", "image", "--file", file(size)});
        // A width asked for takes the height along with it, in proportion,
        // and a unit at least.
        codes << runCli({"sprawl", "add", "pictures", "image", "--file", file({2000, 2}), "--width", "100"});
        codes << runCli({"sprawl", "add", "pictures", "image", "--file", file({800, 600}), "--width", "200"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(codes, QVector<int>(codes.size(), 0));
        QJsonObject root;
        QCOMPARE(Library::read(Library(home.path()).boardFile("pictures"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.item(board.order().first())->rect.topLeft(), QPointF(0, 0));
        QVector<QSizeF> sizes;
        for (quint32 id : board.order())
            sizes << board.item(id)->rect.size();
        QCOMPARE(sizes, (QVector<QSizeF>{{420, 16}, {16, 320}, {16, 320}, {107, 320}, {420, 315}, {32, 16}, {100, 1},
                                         {200, 150}}));
    }

    void aCommandRefusesABoxOfNoSize() {
        // A file may say a box is less than a unit across or down, from before
        // boxes were kept to one, and it is read as a unit. Asked for with a
        // command, it is a slip: nothing is added, not even the picture.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const QString photo = picture(home.filePath("photo.png"), QSize(800, 600));
        const QList<QStringList> slips{{"note", "--text", "Call Sam", "--width", "0"},
                                       {"frame", "--width", "0", "--height", "0"},
                                       {"shape", "--height", "0.5"},
                                       {"shape", "--width", "-1"},
                                       {"image", "--file", photo, "--width", "0.3"},
                                       // "nan" and "inf" read as numbers, but are no
                                       // number to a command, even one the item has
                                       // no use for.
                                       {"note", "--width", "nan"},
                                       {"text", "--text", "Plan", "--height", "inf"}};
        QVector<int> codes{runCli({"sprawl", "new", "sizes"})};
        for (const QStringList &slip : slips)
            codes << runCli(QStringList{"sprawl", "add", "sizes"} + slip);
        // A unit is enough.
        codes << runCli({"sprawl", "add", "sizes", "frame", "--width", "1", "--height", "1"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(codes, (QVector<int>{0, 1, 1, 1, 1, 1, 1, 1, 0}));
        const Library library(home.path());
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("sizes"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.count(), 1);
        QCOMPARE(board.item(board.order().first())->rect.size(), QSizeF(1, 1));
        QVERIFY(QDir(library.assetsFolder("sizes")).entryList(QDir::Files).isEmpty());
    }

    void aPictureThatCannotBeAddedIsNeverCopied() {
        // A command copies a picture into its board only once nothing can
        // refuse it. A copy is never taken back, for other items may show it,
        // so one made first would stay for good, shown by nothing.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const QString photo = picture(home.filePath("photo.png"), QSize(800, 600));
        const QString post = picture(home.filePath("post.png"), QSize(2, 2000));
        // No width at all: nothing to show, and no proportions to keep.
        const QString blank = home.filePath("blank.svg");
        write(blank, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"10\"/>");
        const QList<QStringList> refused{{"--file", photo, "--width", "nan"},
                                         {"--file", photo, "--height", "inf"},
                                         {"--file", photo, "--width", "2e9"},
                                         // The height that follows, 1e10, is more
                                         // than a file holds.
                                         {"--file", post, "--width", "1e7"},
                                         {"--file", photo, "--color", "purple"},
                                         {"--file", blank}};
        QVector<int> codes{runCli({"sprawl", "new", "refused"})};
        for (const QStringList &options : refused)
            codes << runCli(QStringList{"sprawl", "add", "refused", "image"} + options);
        const Library library(home.path());
        const QStringList left = QDir(library.assetsFolder("refused")).entryList(QDir::Files);
        // One that can be added is copied, and shown.
        codes << runCli({"sprawl", "add", "refused", "image", "--file", photo});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(codes, (QVector<int>{0, 1, 1, 1, 1, 1, 1, 0}));
        QCOMPARE(left, QStringList());
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("refused"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.count(), 1);
        QCOMPARE(QDir(library.assetsFolder("refused")).entryList(QDir::Files),
                 QStringList{board.item(board.order().first())->asset});
    }

    void aThinPictureOrFrameLeavesABoardThatOpens() {
        QByteArray saved, pixels;
        QString asset;
        {
            Desk desk;
            QVERIFY(desk.ready());
            desk.add(note(0, 0, "work worth keeping"));
            // Two thousand pixels by two, brought down to a size to arrange.
            desk.picker.images = {QUrl::fromLocalFile(picture(desk.home.filePath("rule.png"), QSize(2000, 2)))};
            desk.hover(QPointF(400, 300));
            desk.key(Qt::Key_I);
            QCOMPARE(desk.all(Item::Kind::Image).size(), 1);
            const Item *rule = desk.all(Item::Kind::Image).first();
            QVERIFY(rule->rect.width() >= 1 && rule->rect.height() >= 1);
            asset = rule->asset;
            // A long thin frame made smaller by its side, with Shift, which
            // keeps its proportions.
            Item band;
            band.kind = Item::Kind::Frame;
            band.rect = QRectF(100, 500, 800, 40);
            const quint32 frame = desk.add(band);
            desk.canvas->select({frame});
            desk.drag(QPointF(903, 520), QPointF(110, 520), Qt::ShiftModifier);
            const QRectF shrunk = desk.board().item(frame)->rect;
            QVERIFY(shrunk.width() < 800);
            QVERIFY(shrunk.width() >= 1 && shrunk.height() >= 1);
            QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
            saved = read(desk.library.boardFile("desk"));
            pixels = read(desk.backend.assetFolder() + "/" + asset);
            QCOMPARE(desk.warnings, QStringList());
        }
        // Next time, the board opens as it was left, to be worked on.
        Desk desk("desk", [&](const QString &home) {
            QDir(home).mkpath("boards/desk/assets");
            write(home + "/boards/desk/board.json", saved);
            write(home + "/boards/desk/assets/" + asset, pixels);
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.problem(), QString());
        QVERIFY(!desk.backend.readOnly());
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.all(Item::Kind::Image).size(), 1);
        QCOMPARE(desk.all(Item::Kind::Frame).size(), 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardSavedWithABoxOfNoSizeOpens() {
        // As 0.1.0 saved a picture far wider than tall, and then said it
        // could not open the board, and showed an empty one.
        const QByteArray old = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "work worth keeping"},
            {"id": 2, "type": "image", "x": 268, "y": 337, "w": 420, "h": 0, "asset": "rule.png"}]})";
        Desk desk("desk", [&old](const QString &home) {
            QDir(home).mkpath("boards/desk/assets");
            write(home + "/boards/desk/board.json", old);
            picture(home + "/boards/desk/assets/rule.png", QSize(2000, 2));
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.problem(), QString());
        QVERIFY(!desk.backend.readOnly());
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.board().item(2)->rect, QRectF(268, 337, 420, 1));
        // The next save writes the picture as it is held.
        desk.add(note(600, 0, "and more"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray items = desk.onDisk().value("items").toArray();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[1].toObject().value("h").toDouble(), 1.0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardTakesInWhatOnlyTheFileChanged() {
        Board board;
        const quint32 a = board.add(note(0, 0, "A")), b = board.add(note(200, 0, "B")),
                      c = board.add(note(400, 0, "C"));
        const quint32 line = board.add(connector(a, b));
        // The file as the board last saved it.
        const QVector<Item> base = board.items();
        // Here, A is moved.
        Item moved = *board.item(a);
        moved.rect.moveTo(0, 300);
        board.set(moved);
        // There, A is colored, B written on, C taken away, and D added.
        QVector<Item> theirs = base;
        theirs[0].color = "blue";
        theirs[1].text = "B, from outside";
        theirs.removeAt(2);
        Item d = note(600, 0, "D");
        d.id = 9;
        theirs.append(d);
        const quint64 before = board.revision();
        QSignalSpy modified(&board, &Board::modified);
        const Board::Merged merged = board.merge(base, theirs);

        // What only the file changed comes as it has it, and what both
        // changed is as it is here; the board has that to write still.
        QCOMPARE(merged.taken, 3);
        QCOMPARE(merged.kept, 1);
        QVERIFY(merged.ahead);
        QCOMPARE(board.item(a)->rect.topLeft(), QPointF(0, 300));
        QCOMPARE(board.item(a)->color, QString("yellow"));
        QCOMPARE(board.item(b)->text, QString("B, from outside"));
        QVERIFY(!board.item(c));
        QCOMPARE(board.item(9)->text, QString("D"));
        QCOMPARE(board.order(), (QVector<quint32>{a, b, line, 9}));
        QCOMPARE(board.connectorsOf(b), QVector<quint32>{line});
        QVERIFY(board.revision() != before);
        QVERIFY(modified.count() > 0);

        // None of it is a step of undo: undo takes back the move, and only
        // that. Further back, B is as the file has it whenever it is there,
        // and C, which the file took away, is out of the history.
        board.undo();
        QCOMPARE(board.item(a)->rect.topLeft(), QPointF(0, 0));
        QCOMPARE(board.item(b)->text, QString("B, from outside"));
        QCOMPARE(board.count(), 4);
        board.undo();
        QVERIFY(!board.item(line));
        board.undo();
        QVERIFY(!board.item(b));
        board.undo();
        QVERIFY(!board.canUndo());
        QCOMPARE(board.order(), QVector<quint32>{9});
        for (int i = 0; i < 4; ++i)
            board.redo();
        QCOMPARE(board.item(b)->text, QString("B, from outside"));
        QCOMPARE(board.item(a)->rect.topLeft(), QPointF(0, 300));
        QVERIFY(!board.item(c));
        QCOMPARE(board.count(), 4);

        // The file's stacking is taken when the board kept its own as it
        // was; what the board restacked itself stays as it has it.
        Board stack;
        const quint32 p = stack.add(note(0, 0)), q = stack.add(note(10, 10)), r = stack.add(note(20, 20));
        const QVector<Item> was = stack.items();
        const quint32 s = stack.add(note(30, 30));
        QVERIFY(stack.merge(was, {was[2], was[0], was[1]}).restacked);
        QCOMPARE(stack.order(), (QVector<quint32>{r, p, q, s}));
        const QVector<Item> now = stack.items();
        stack.restack({p}, true);
        QVERIFY(!stack.merge(now, {now[2], now[1], now[0], now[3]}).restacked);
        QCOMPARE(stack.order(), (QVector<quint32>{r, q, s, p}));
    }

    void anItemMadeHereGivesUpItsIdToTheFile() {
        Board board;
        const quint32 kept = board.add(note(0, 0, "Saved"));
        const QVector<Item> base = board.items();
        // Made here, and not saved yet: a note, and a connector joining it to
        // the one that is.
        const quint32 mine = board.add(note(200, 0, "Mine"));
        const quint32 line = board.add(connector(kept, mine));
        // Commands added two notes to the file meanwhile, which it numbered
        // as these are numbered here.
        QVector<Item> theirs = base;
        Item first = note(0, 300, "Theirs"), second = note(200, 300, "Theirs too");
        first.id = mine;
        second.id = line;
        theirs << first << second;
        QSignalSpy renumbered(&board, &Board::renumbered);
        QVector<quint32> said;
        connect(&board, &Board::renumbered, &board, [&said](quint32 from, quint32) { said << from; });
        connect(&board, &Board::itemChanged, &board, [&said](quint32 id, Board::Change) { said << id; });
        const Board::Merged merged = board.merge(base, theirs);
        QCOMPARE(merged.taken, 2);
        QVERIFY(merged.ahead);

        // The file's keep the ids they were printed with; what was made here
        // has new ones, everywhere, joined as it was.
        QCOMPARE(renumbered.count(), 2);
        const quint32 mineNow = renumbered[0][1].toUInt(), lineNow = renumbered[1][1].toUInt();
        QCOMPARE(renumbered[0][0].toUInt(), mine);
        QCOMPARE(renumbered[1][0].toUInt(), line);
        QVERIFY(mineNow > line && lineNow > line && mineNow != lineNow);
        QCOMPARE(board.item(mine)->text, QString("Theirs"));
        QCOMPARE(board.item(line)->kind, Item::Kind::Note);
        QCOMPARE(board.item(mineNow)->text, QString("Mine"));
        QCOMPARE(board.item(lineNow)->from, kept);
        QCOMPARE(board.item(lineNow)->to, mineNow);
        QCOMPARE(board.connectorsOf(mineNow), QVector<quint32>{lineNow});
        QVERIFY(board.connectorsOf(mine).isEmpty());
        QCOMPARE(board.order(), (QVector<quint32>{kept, mineNow, lineNow, mine, line}));
        // Said of the old id before anything else is said of it: then it is
        // gone by that id, and there by the new one.
        QCOMPARE(said.mid(0, 3), (QVector<quint32>{mine, mine, mineNow}));

        // Undo goes on undoing what was done here, under the new ids.
        board.undo();
        QVERIFY(!board.item(lineNow));
        QCOMPARE(board.item(line)->text, QString("Theirs too"));
        board.undo();
        QVERIFY(!board.item(mineNow));
        QCOMPARE(board.item(mine)->text, QString("Theirs"));
        board.redo();
        board.redo();
        QCOMPARE(board.item(lineNow)->to, mineNow);
        QCOMPARE(board.connectorsOf(mineNow), QVector<quint32>{lineNow});

        // One made by a step still under way, too: taken back, the step
        // leaves the file's, and the board it began from is not the board
        // it goes back to.
        const QVector<Item> saved = board.items();
        const quint64 begun = board.revision();
        board.begin();
        const quint32 open = board.add(note(600, 0, "Open"));
        QVector<Item> later = saved;
        Item third = note(600, 300, "Theirs as well");
        third.id = open;
        later << third;
        board.merge(saved, later);
        QVERIFY(board.isOpen());
        const quint32 openNow = renumbered.last()[1].toUInt();
        QCOMPARE(renumbered.last()[0].toUInt(), open);
        QCOMPARE(board.item(openNow)->text, QString("Open"));
        board.rollback();
        QVERIFY(!board.item(openNow));
        QCOMPARE(board.item(open)->text, QString("Theirs as well"));
        QVERIFY(board.revision() != begun);
        QVERIFY(!board.canRedo());
    }

    void whatAStepIsAboutStaysAsItIsHere() {
        Board board;
        const quint32 id = board.add(note(0, 0, "Being written in"));
        const QVector<Item> base = board.items();
        // A note opened to be written in, nothing typed yet, that the file
        // changes, and then takes away.
        board.begin();
        board.hold(id);
        QVector<Item> theirs = base;
        theirs[0].text = "Changed outside";
        Board::Merged merged = board.merge(base, theirs);
        QCOMPARE(merged.kept, 1);
        QCOMPARE(merged.taken, 0);
        QVERIFY(merged.ahead);
        QCOMPARE(board.item(id)->text, QString("Being written in"));
        merged = board.merge(theirs, {});
        QCOMPARE(merged.kept, 1);
        QVERIFY(board.item(id));
        QVERIFY(board.isOpen());
        // Holding it is no change of its own.
        QVERIFY(!board.commit());
        QCOMPARE(board.count(), 1);
    }

    void commandsRunSideBySideLoseNothing() {
        // As an agent batching its tool calls runs them: a dozen commands at
        // once, each a process of its own, on one board. Every one is kept,
        // with an id of its own, and the id each printed is the one the
        // file has for what it added.
        QTemporaryDir home;
        const Library library(home.path());
        QCOMPARE(Library(home.path()).create("race"), QString());
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SPRAWL_HOME", home.path());
        std::vector<std::unique_ptr<QProcess>> commands;
        auto start = [&commands, &environment](const QStringList &arguments) {
            commands.push_back(std::make_unique<QProcess>());
            commands.back()->setProcessEnvironment(environment);
            commands.back()->start(QCoreApplication::applicationFilePath(), QStringList{"--cli"} + arguments);
        };
        auto board = [&library](Board &read) {
            QJsonObject root;
            return Library::read(library.boardFile("race"), root).isEmpty() && read.fromJson(root).isEmpty();
        };
        // What each command that added printed, by the id it printed.
        QHash<quint32, QString> printed;
        auto finish = [&commands, &printed] {
            for (const auto &command : commands) {
                if (!command->waitForFinished(120000) || command->exitStatus() != QProcess::NormalExit ||
                    command->exitCode() != 0)
                    return false;
                const QJsonObject added = QJsonDocument::fromJson(command->readAllStandardOutput()).object();
                if (added.contains("id"))
                    printed.insert(quint32(added.value("id").toInteger()), added.value("text").toString());
            }
            commands.clear();
            return true;
        };

        for (int i = 0; i < 12; ++i)
            start({"add", "race", "note", "--text", QString("Note %1").arg(i), "--json"});
        QVERIFY(finish());
        QCOMPARE(printed.size(), 12);
        Board first;
        QVERIFY(board(first));
        QCOMPARE(first.count(), 12);
        for (auto it = printed.constBegin(); it != printed.constEnd(); ++it)
            QCOMPARE(first.item(it.key()) ? first.item(it.key())->text : QString(), it.value());

        // Taking some away while adding more, all at once, the same.
        QVector<quint32> ids(printed.keyBegin(), printed.keyEnd());
        std::sort(ids.begin(), ids.end());
        for (int i = 0; i < 6; ++i)
            start({"remove", "race", QString::number(ids[i]), "--json"});
        for (int i = 0; i < 6; ++i)
            start({"add", "race", "text", "--text", QString("More %1").arg(i), "--json"});
        QVERIFY(finish());
        QCOMPARE(printed.size(), 18);
        Board second;
        QVERIFY(board(second));
        QCOMPARE(second.count(), 12);
        for (int i = 0; i < 6; ++i)
            QVERIFY(!second.item(ids[i]));
        for (auto it = printed.constBegin(); it != printed.constEnd(); ++it)
            if (!ids.mid(0, 6).contains(it.key()))
                QCOMPARE(second.item(it.key()) ? second.item(it.key())->text : QString(), it.value());
    }

    void aCommandFromOutsideLeavesWhatIsBeingTyped() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "hello"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.doubleClick(QPointF(150, 150));
        QVERIFY(desk.canvas->editing());
        desk.type("abc");
        // A pause in the typing, long enough for it to be saved...
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->editing());
        // ...in which an agent adds a note with a command.
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "From outside", "--x", "500", "--y", "100"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        // It shows up, and the note is still being written in, with what was
        // typed, which goes on from where it was.
        QTRY_COMPARE(desk.board().count(), 2);
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("helloabc"));
        QCOMPARE(desk.item("editor")->property("text").toString(), QString("helloabc"));
        QVERIFY(desk.backend.notice().contains("from outside"));
        desk.type("def");
        QCOMPARE(desk.board().item(id)->text, QString("helloabcdef"));
        desk.key(Qt::Key_Escape);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QStringList written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written << value.toObject().value("text").toString();
        QCOMPARE(written, (QStringList{"helloabcdef", "From outside"}));
        // Undo takes back what was typed, and then the note it was typed in;
        // what came from outside was never a step of it.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->text, QString("hello"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!desk.board().item(id));
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("From outside"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNoteOpenToBeWrittenInIsNotTakenAwayFromUnderIt() {
        Desk desk;
        QVERIFY(desk.ready());
        // As main() has it, so what is counted is said as English says it.
        QCoreApplication::installTranslator(&desk.translator);
        const quint32 id = desk.add(note(100, 100, "Draft"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // Opened to be written in, and nothing typed yet, when an agent
        // removes it with a command.
        desk.doubleClick(QPointF(150, 150));
        QVERIFY(desk.canvas->editing());
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "remove", "desk", QString::number(id)});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        QVERIFY(desk.onDisk().value("items").toArray().isEmpty());
        // It is kept as it is here, and said to be, and the file is given it
        // back; the writing goes on.
        QTRY_VERIFY(!desk.backend.notice().isEmpty());
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Draft"));
        QCOMPARE(desk.backend.notice(), QString("Updated desk from outside, but kept 1 item as it is here"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 1);
        QVERIFY(desk.canvas->editing());
        desk.type(", and more");
        desk.key(Qt::Key_Escape);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray written = desk.onDisk().value("items").toArray();
        QCOMPARE(written.size(), 1);
        QCOMPARE(written.first().toObject().value("text").toString(), QString("Draft, and more"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNoteBeingWrittenKeepsGoingWhenItsIdIsTaken() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.hover(QPointF(300, 200));
        // A note made with Space and being written in, not saved yet...
        desk.key(Qt::Key_Space);
        desk.type("Mine");
        QVERIFY(desk.canvas->editing());
        const quint32 made = desk.canvas->selection().value(0);
        QVERIFY(desk.onDisk().value("items").toArray().isEmpty());
        // ...when a command adds a note of its own, which the file numbers as
        // this one is numbered here.
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "Theirs", "--x", "600", "--y", "0"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        const QJsonArray before = desk.onDisk().value("items").toArray();
        QCOMPARE(before.size(), 1);
        QCOMPARE(quint32(before.first().toObject().value("id").toInteger()), made);

        // The command's note keeps the id it printed. The one here takes a
        // new one, and is still the one selected and being written in.
        QTRY_COMPARE(desk.board().count(), 2);
        QCOMPARE(desk.board().item(made)->text, QString("Theirs"));
        QCOMPARE(desk.canvas->selection().size(), 1);
        const quint32 mine = desk.canvas->selection().first();
        QVERIFY(mine != made);
        QCOMPARE(desk.board().item(mine)->text, QString("Mine"));
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.item("editor")->property("text").toString(), QString("Mine"));
        desk.type(", still");
        QCOMPARE(desk.board().item(mine)->text, QString("Mine, still"));
        QCOMPARE(desk.board().item(made)->text, QString("Theirs"));
        desk.key(Qt::Key_Escape);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QHash<quint32, QString> written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written.insert(quint32(value.toObject().value("id").toInteger()), value.toObject().value("text").toString());
        QCOMPARE(written, (QHash<quint32, QString>{{made, "Theirs"}, {mine, "Mine, still"}}));
        // Undone, the note made here goes, by its new id; redone, it is back.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.board().item(made)->text, QString("Theirs"));
        desk.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk.board().item(mine)->text, QString("Mine, still"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aCommandRunJustBeforeASaveIsKept() {
        Desk desk;
        QVERIFY(desk.ready());
        // Added here a moment ago: a save waits for a pause.
        desk.add(note(100, 100, "Mine"));
        QCOMPARE(desk.backend.saveState(), QString("unsaved"));
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "Theirs"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        const QJsonObject added = desk.onDisk().value("items").toArray().first().toObject();
        QCOMPARE(added.value("text").toString(), QString("Theirs"));
        const quint32 theirs = quint32(added.value("id").toInteger());
        // The board is saved with both, the command's by the id it printed.
        QTRY_COMPARE(desk.board().count(), 2);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QHash<quint32, QString> written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written.insert(quint32(value.toObject().value("id").toInteger()), value.toObject().value("text").toString());
        QCOMPARE(written.size(), 2);
        QCOMPARE(written.value(theirs), QString("Theirs"));
        QVERIFY(written.values().contains("Mine"));
        QCOMPARE(desk.board().item(theirs)->text, QString("Theirs"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aDragGoesOnThroughAChangeFromOutside() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Dragged"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.press(QPointF(150, 150));
        desk.moveTo(QPointF(200, 200));
        desk.moveTo(QPointF(250, 230));
        QVERIFY(desk.canvas->busy());
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "From outside", "--x", "600", "--y", "100"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        // It shows up, and the drag goes on to where the note is let go.
        QTRY_COMPARE(desk.board().count(), 2);
        QVERIFY(desk.canvas->busy());
        desk.moveTo(QPointF(300, 260));
        desk.release(QPointF(300, 260));
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(250, 210));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray written = desk.onDisk().value("items").toArray();
        QCOMPARE(written.size(), 2);
        QCOMPARE(written[0].toObject().value("x").toDouble(), 250.0);
        QCOMPARE(written[1].toObject().value("text").toString(), QString("From outside"));
        // Undo takes back the drag, and only that.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(100, 100));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aFileThatCannotBeReadIsNeverWrittenOverWhileOpen() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "one"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QString file = desk.library.boardFile("desk");
        // Saved by hand, or by an agent, with one slip: a comma too many.
        const QByteArray slip = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "one"},
            {"id": 2, "type": "note", "x": 0, "y": 300, "color": "blue", "text": "an hour of hand editing"},
        ]})";
        write(file, slip);
        QTRY_VERIFY(desk.backend.problem().contains("can no longer be read"));
        QVERIFY(desk.backend.problem().contains("kept"));
        QCOMPARE(desk.backend.saveState(), QString("paused"));
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("not saved"));
        // Work goes on here, and is kept, but nothing is written over the
        // file, however long it waits; and quitting would ask first.
        desk.hover(QPointF(400, 100));
        desk.key(Qt::Key_Space);
        desk.type("two");
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.backend.saveState(), QString("paused"));
        QTest::qWait(1000);
        QCOMPARE(read(file), slip);
        // Put away, the problem stays away while the file is as it was, a
        // command that cannot read it either coming and going meanwhile.
        desk.backend.dismissProblem();
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "three"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 1);
        QTest::qWait(500);
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.backend.saveState(), QString("paused"));
        QCOMPARE(read(file), slip);
        // Asked to write the board now, it says why it cannot.
        QVERIFY(!desk.backend.flush());
        QCOMPARE(read(file), slip);
        QVERIFY(desk.backend.problem().contains("can no longer be read"));
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 0);
        QCOMPARE(desk.panel(), QString("quit"));
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(read(file), slip);

        // Mended, it is taken in: the hand editing and the work here, both,
        // and saving goes on.
        QByteArray mended = slip;
        mended.replace("},\n        ]", "}\n        ]");
        QVERIFY(mended != slip);
        write(file, mended);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.backend.problem(), QString());
        QStringList written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written << value.toObject().value("text").toString();
        written.sort();
        QCOMPARE(written, (QStringList{"an hour of hand editing", "one", "two"}));
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardWrittenWholeFromOutsideIsMergedIn() {
        Desk desk;
        QVERIFY(desk.ready());
        // As main() has it, so what is counted is said as English says it.
        QCoreApplication::installTranslator(&desk.translator);
        const quint32 plan = desk.add(note(0, 0, "Plan"));
        const quint32 step = desk.add(shape(Item::Shape::Diamond, 300, 0));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // The person moves the note; a moment later, before that is saved...
        Item moved = *desk.board().item(plan);
        moved.rect.moveTo(0, 200);
        desk.board().set(moved);
        // ...an agent writes the whole board, as the skill tells it to: read,
        // changed, written to a name of its own and renamed over the file.
        // It moves the note too, writes on the shape, and adds a heading
        // without an id and an arrow from the note to the shape.
        QJsonArray items = desk.onDisk().value("items").toArray();
        QJsonObject agentsPlan = items[0].toObject(), agentsStep = items[1].toObject();
        agentsPlan["x"] = 500;
        agentsStep["text"] = "Good?";
        items[0] = agentsPlan;
        items[1] = agentsStep;
        items.append(QJsonObject{{"type", "text"}, {"x", 0}, {"y", -60}, {"size", 28}, {"text", "Weekend"}});
        items.append(QJsonObject{{"type", "connector"}, {"from", qint64(plan)}, {"to", qint64(step)}});
        const QString file = desk.library.boardFile("desk");
        write(file + ".tmp", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        QVERIFY(std::rename(qPrintable(file + ".tmp"), qPrintable(file)) == 0);

        // Taken in: the note where the person put it, which the notice says
        // was kept as it is here, and the rest as the agent wrote it.
        QTRY_COMPARE(desk.board().count(), 4);
        QCOMPARE(desk.backend.notice(), QString("Updated desk from outside, but kept 1 item as it is here"));
        QCOMPARE(desk.board().item(plan)->rect.topLeft(), QPointF(0, 200));
        QCOMPARE(desk.board().item(step)->text, QString("Good?"));
        QCOMPARE(desk.all(Item::Kind::Text).first()->text, QString("Weekend"));
        const Item *arrow = desk.all(Item::Kind::Connector).first();
        QCOMPARE(arrow->from, plan);
        QCOMPARE(arrow->to, step);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QStringList written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written << value.toObject().value("type").toString() + ":" + value.toObject().value("text").toString();
        QCOMPARE(written, (QStringList{"note:Plan", "shape:Good?", "text:Weekend", "connector:"}));
        QCOMPARE(desk.onDisk().value("items").toArray().first().toObject().value("y").toDouble(), 200.0);
        // Undo takes back the move; further back, the shape that was added
        // here is taken away, and brought back as the agent wrote on it.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(plan)->rect.topLeft(), QPointF(0, 0));
        QCOMPARE(desk.board().item(step)->text, QString("Good?"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!desk.board().item(step));
        desk.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk.board().item(step)->text, QString("Good?"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aConnectorFromOutsideIsLetGoOfByWhatIsUndone() {
        // A connector a command attached to an item made here is in no step
        // of the history here. Whatever takes the item away again, undo, redo
        // or a step taken back, lets go of the connector's end on it where it
        // was, and the board is one a file can say all along.
        Board board;
        auto opens = [&board] {
            QVector<Item> read;
            return Board::read(board.toJson(), read);
        };
        // The board as it was saved, joined by a command from `from` to `to`
        // with a connector numbered after every id in the file.
        auto join = [&board](quint32 from, quint32 to) {
            const QVector<Item> saved = board.itemsToWrite();
            Item line = connector(from, to);
            for (const Item &item : saved)
                line.id = qMax(line.id, item.id);
            ++line.id;
            board.merge(saved, saved + QVector<Item>{line});
            return line.id;
        };

        // Undo: the note at the end of it was the last thing made.
        const quint32 a = board.add(note(0, 0, "A")), b = board.add(note(300, 0, "B"));
        const quint32 toB = join(a, b);
        QCOMPARE(board.connectorsOf(b), QVector<quint32>{toB});
        const QPointF end = board.item(toB)->p2;
        board.undo();
        QVERIFY(!board.item(b));
        QCOMPARE(opens(), QStringList());
        QCOMPARE(board.item(toB)->from, a);
        QCOMPARE(board.item(toB)->to, quint32(0));
        QCOMPARE(board.item(toB)->p2, end);
        QVERIFY(board.connectorsOf(b).isEmpty());
        // Redone, the note is back, and the connector stays as it was let go.
        board.redo();
        QVERIFY(board.item(b));
        QCOMPARE(opens(), QStringList());
        QCOMPARE(board.item(toB)->to, quint32(0));

        // Redo: a note taken away, that undone, and the note joined before
        // it is taken away again.
        const quint32 c = board.add(note(0, 300, "C"));
        board.remove(c);
        board.undo();
        const quint32 toC = join(a, c);
        board.redo();
        QVERIFY(!board.item(c));
        QCOMPARE(opens(), QStringList());
        QCOMPARE(board.item(toC)->to, quint32(0));
        QVERIFY(board.connectorsOf(c).isEmpty());

        // A step taken back: a note being made, saved as it is, and joined.
        board.begin();
        const quint32 d = board.add(note(300, 300, "D"));
        const quint32 toD = join(a, d);
        board.rollback();
        QVERIFY(!board.item(d));
        QCOMPARE(opens(), QStringList());
        QCOMPARE(board.item(toD)->to, quint32(0));
        QVERIFY(board.connectorsOf(d).isEmpty());

        // All the way back, and forward again.
        while (board.canUndo()) {
            board.undo();
            QCOMPARE(opens(), QStringList());
        }
        while (board.canRedo()) {
            board.redo();
            QCOMPARE(opens(), QStringList());
        }
        QCOMPARE(board.order(), (QVector<quint32>{a, b, toB, toC, toD}));
    }

    void undoingUnderAConnectorFromACommandSavesABoardThatOpens() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100, "A"));
        const quint32 b = desk.add(note(400, 100, "B"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // An agent joins the two with a command.
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int joined =
            runCli({"sprawl", "add", "desk", "connector", "--from", QString::number(a), "--to", QString::number(b)});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(joined, 0);
        QTRY_COMPARE(desk.board().count(), 3);
        const quint32 line = desk.all(Item::Kind::Connector).first()->id;
        // The person undoes the last thing they did, which was making B. What
        // is saved then is a board that opens, to the window and to every
        // command after it: the connector stays, let go of where B was.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!desk.board().item(b));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVector<Item> written;
        QCOMPARE(Board::read(desk.onDisk(), written), QStringList());
        QCOMPARE(written.size(), 2);
        QCOMPARE(desk.board().item(line)->from, a);
        QCOMPARE(desk.board().item(line)->to, quint32(0));
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int checked = runCli({"sprawl", "check", "desk"});
        const int added = runCli({"sprawl", "add", "desk", "note", "--text", "Next"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(checked, 0);
        QCOMPARE(added, 0);
        QTRY_COMPARE(desk.board().count(), 3);
        // Redone, B is back, and that is saved too.
        desk.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk.board().item(b)->text, QString("B"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(Board::read(desk.onDisk(), written), QStringList());
        QCOMPARE(written.size(), 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aChangeFromOutsideJustBeforeAResizeIsKept() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Resize me"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        // A corner of it is taken hold of; before the mouse has moved enough
        // to make a drag of it, an agent writes the note blue, with words of
        // its own, the whole file as the skill says.
        const QPointF corner = desk.board().item(id)->rect.bottomRight() + QPointF(3, 3);
        desk.press(corner);
        QJsonArray items = desk.onDisk().value("items").toArray();
        QJsonObject changed = items[0].toObject();
        changed["color"] = "blue";
        changed["text"] = "Written outside";
        items[0] = changed;
        const QString file = desk.library.boardFile("desk");
        write(file + ".tmp", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        QVERIFY(std::rename(qPrintable(file + ".tmp"), qPrintable(file)) == 0);
        QTRY_COMPARE(desk.board().item(id)->color, QString("blue"));
        // What is resized is the note as it is now: what the agent wrote
        // stays, and is saved with the new size.
        desk.moveTo(corner + QPointF(20, 20));
        desk.moveTo(corner + QPointF(40, 40));
        desk.release(corner + QPointF(40, 40));
        QCOMPARE(desk.board().item(id)->rect, QRectF(100, 100, 192, 192));
        QCOMPARE(desk.board().item(id)->color, QString("blue"));
        QCOMPARE(desk.board().item(id)->text, QString("Written outside"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonObject written = desk.onDisk().value("items").toArray().first().toObject();
        QCOMPARE(written.value("w").toDouble(), 192.0);
        QCOMPARE(written.value("color").toString(), QString("blue"));
        QCOMPARE(written.value("text").toString(), QString("Written outside"));
        // Undo takes back the resize, and only that.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect, QRectF(100, 100, 152, 152));
        QCOMPARE(desk.board().item(id)->color, QString("blue"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPictureThatNeverComesHoldsUpNothingElse() {
        // A pipe given as the picture to add sends nothing until something
        // writes to it. The command refuses it at once, before it asks for
        // the board: it never holds the board while others wait to write it,
        // nor waits for it itself, held here as the window holds it to save.
        QTemporaryDir home;
        QCOMPARE(Library(home.path()).create("pipes"), QString());
        const Library library(home.path());
        const QString pipe = home.filePath("photo.png");
        QCOMPARE(mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
        auto saving = Library::lock(library.boardFile("pipes"));
        QVERIFY(saving);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SPRAWL_HOME", home.path());
        QProcess command;
        command.setProcessEnvironment(environment);
        command.start(QCoreApplication::applicationFilePath(), {"--cli", "add", "pipes", "image", "--file", pipe});
        QVERIFY(command.waitForFinished(30000));
        QCOMPARE(command.exitCode(), 1);
        const QByteArray said = command.readAllStandardError();
        QVERIFY2(said.contains("is not a file"), said.constData());
        // The next command has the board to itself, and nothing was copied.
        saving.reset();
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "pipes", "note", "--text", "Next"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        QVERIFY(!QFile::exists(library.boardFile("pipes") + ".lock"));
        QVERIFY(QDir(library.assetsFolder("pipes")).entryList(QDir::Files).isEmpty());
    }

    void itemsWrittenWithoutAnIdAreGivenTheirsAtOnce() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "Plan"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // The whole file as an agent writes it, read just before.
        const QString file = desk.library.boardFile("desk");
        auto writeWhole = [&file](const QJsonArray &items) {
            write(file + ".tmp", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
            return std::rename(qPrintable(file + ".tmp"), qPrintable(file)) == 0;
        };
        // A heading added with no id, as the skill allows.
        QJsonArray items = desk.onDisk().value("items").toArray();
        items.append(QJsonObject{{"type", "text"}, {"x", 0}, {"y", -60}, {"size", 28}, {"text", "Weekend"}});
        QVERIFY(writeWhole(items));
        QTRY_COMPARE(desk.board().count(), 2);
        const quint32 heading = desk.all(Item::Kind::Text).first()->id;
        // Nothing changed here, and the file is written again all the same,
        // the heading with the id it goes by here and in what commands print.
        QTRY_COMPARE(desk.onDisk().value("items").toArray().last().toObject().value("id").toInteger(), qint64(heading));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // So the next whole file an agent writes, with a note of its own
        // numbered after the ids it finds there, numbers it after the
        // heading's. The person has moved the heading meanwhile: it stays
        // where it was put, and the note comes in beside it.
        Item moved = *desk.board().item(heading);
        moved.rect.moveTo(0, -120);
        desk.board().set(moved);
        items = desk.onDisk().value("items").toArray();
        qint64 last = 0;
        for (const QJsonValue &value : std::as_const(items))
            last = qMax(last, value.toObject().value("id").toInteger());
        items.append(QJsonObject{{"id", last + 1}, {"type", "note"}, {"x", 300}, {"y", 0}, {"text", "Packing"}});
        QVERIFY(writeWhole(items));
        QTRY_COMPARE(desk.board().count(), 3);
        QCOMPARE(desk.all(Item::Kind::Text).size(), 1);
        QCOMPARE(desk.board().item(heading)->rect.topLeft(), QPointF(0, -120));
        QCOMPARE(desk.board().item(quint32(last + 1))->text, QString("Packing"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 3);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aCommandRightAfterTheWindowSavesIsShown() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "Mine"));
        const QString file = desk.library.boardFile("desk");
        auto count = [&desk] { return desk.onDisk().value("items").toArray().size(); };
        // The window's save waits for the board, held here as a command holds
        // it, and writes it once it is let go of. The window is too busy to
        // look meanwhile, and an agent's command writes the board the moment
        // after the save does.
        auto held = Library::lock(file);
        QVERIFY(held);
        QTRY_COMPARE(desk.backend.saveState(), QString("saving"));
        held.reset();
        for (int i = 0; i < 3000 && count() != 1; ++i)
            QTest::qSleep(10);
        QCOMPARE(count(), 1);
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "Theirs", "--x", "300", "--y", "0"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        QCOMPARE(count(), 2);
        // Free again, the window shows it.
        QTRY_COMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->text, QString("Theirs"));
        QCOMPARE(desk.backend.notice(), QString("Updated desk from outside"));
        QCOMPARE(desk.backend.saveState(), QString("saved"));

        // The same for an agent that writes the file where it is, rather than
        // beside it and renamed over.
        desk.add(note(0, 300, "Mine too"));
        held = Library::lock(file);
        QVERIFY(held);
        QTRY_COMPARE(desk.backend.saveState(), QString("saving"));
        held.reset();
        for (int i = 0; i < 3000 && count() != 3; ++i)
            QTest::qSleep(10);
        QCOMPARE(count(), 3);
        QJsonArray items = desk.onDisk().value("items").toArray();
        items.append(QJsonObject{{"type", "note"}, {"x", 300}, {"y", 300}, {"text", "Written in place"}});
        write(file, QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        QTRY_COMPARE(desk.board().count(), 4);
        QCOMPARE(desk.last()->text, QString("Written in place"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(count(), 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aCommandAsABoardIsOpenedIsShown() {
        Desk desk;
        QVERIFY(desk.ready());
        QCOMPARE(desk.library.create("plans"), QString());
        // Another board is opened, and an agent's command adds a note to it
        // the moment the window has read it, before it is done opening it.
        int code = -1;
        const auto during = connect(&desk.board(), &Board::reset, &desk.board(), [&desk, &code] {
            if (code >= 0)
                return;
            qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
            code = runCli({"sprawl", "add", "plans", "note", "--text", "Theirs"});
            qunsetenv("SPRAWL_HOME");
        });
        QVERIFY(desk.backend.openBoard("plans"));
        disconnect(during);
        QCOMPARE(code, 0);
        QCOMPARE(desk.board().count(), 0);
        // The window shows it all the same.
        QTRY_COMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("Theirs"));
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void anIdIsNeverGivenTwice() {
        // The file keeps the last id given when the item that had it is taken
        // away. Whatever reads it gives the next item a later one, so the
        // next is never taken for the one taken away.
        Board board;
        board.add(note(0, 0));
        const quint32 gone = board.add(note(200, 0));
        board.remove(gone);
        const QJsonObject file = board.toJson();
        QCOMPARE(file.value("lastId").toInteger(), qint64(gone));
        Board loaded;
        QCOMPARE(loaded.fromJson(file), QStringList());
        QCOMPARE(loaded.lastId(), gone);
        const quint32 next = loaded.add(note(0, 300));
        QCOMPARE(next, gone + 1);
        // Said only when the items do not say it: a board that never had its
        // last item taken away is written as it always was.
        QVERIFY(!loaded.toJson().contains("lastId"));
        // An item written without an id is numbered after it too, as is
        // whatever a board that takes the file in makes next.
        QJsonObject handWritten = file;
        QJsonArray items = file.value("items").toArray();
        items.append(QJsonObject{{"type", "note"}, {"x", 0}, {"y", 300}});
        handWritten["items"] = items;
        QVector<Item> numbered;
        quint32 last = 0;
        QCOMPARE(Board::read(handWritten, numbered, nullptr, &last), QStringList());
        QCOMPARE(numbered.last().id, gone + 1);
        QCOMPARE(last, gone + 1);
        Board window;
        window.add(note(0, 0));
        const QVector<Item> base = window.items();
        window.merge(base, base, 7);
        QCOMPARE(window.lastId(), quint32(7));
        QCOMPARE(window.add(note(0, 300)), quint32(8));
        // Something else where an id is said is no help, and no reason to
        // refuse the file either.
        handWritten["lastId"] = "seven";
        QCOMPARE(Board::read(handWritten, numbered, nullptr, &last), QStringList());
        QCOMPARE(last, numbered.last().id);
    }

    void aNoteAnAgentPutsInPlaceOfAnotherIsNotUndoneHere() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 mine = desk.add(note(100, 100, "The person's"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // An agent puts a note of its own in place of it, with two commands
        // run before the window has looked.
        auto replace = [&desk](quint32 id, const QString &text) {
            qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
            const bool done = runCli({"sprawl", "remove", "desk", QString::number(id)}) == 0 &&
                              runCli({"sprawl", "add", "desk", "note", "--text", text}) == 0;
            qunsetenv("SPRAWL_HOME");
            return done;
        };
        QVERIFY(replace(mine, "The agent's"));
        const QJsonArray file = desk.onDisk().value("items").toArray();
        QCOMPARE(file.size(), 1);
        const quint32 theirs = quint32(file.first().toObject().value("id").toInteger());
        // It is a note of its own, by an id of its own: the person's goes,
        // and undoing their making it takes nothing of the agent's away.
        QVERIFY(theirs != mine);
        QTRY_VERIFY(!desk.board().item(mine));
        QCOMPARE(desk.board().item(theirs)->text, QString("The agent's"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.board().item(theirs)->text, QString("The agent's"));

        // A note being written in, put in place of the same way, stays as it
        // always does, and the agent's comes in beside it.
        const quint32 open = desk.add(note(400, 100, "Being written"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.doubleClick(QPointF(450, 150));
        QVERIFY(desk.canvas->editing());
        QVERIFY(replace(open, "Its replacement"));
        QTRY_COMPARE(desk.board().count(), 3);
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.board().item(open)->text, QString("Being written"));
        QCOMPARE(desk.last()->text, QString("Its replacement"));
        desk.key(Qt::Key_Escape);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QStringList written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written << value.toObject().value("text").toString();
        written.sort();
        QCOMPARE(written, (QStringList{"Being written", "Its replacement", "The agent's"}));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardIsWrittenOverOnlyAsItIsAtTheLastMoment() {
        QTemporaryDir home;
        const QString file = home.filePath("board.json");
        write(file, R"({"sprawl": 1, "items": []})");
        const QByteArray base = Library::fingerprint(file);
        const QByteArray mine = R"({"sprawl": 1, "items": [{"id": 1, "type": "note", "x": 0, "y": 0}]})";
        const QByteArray theirs = R"({"sprawl": 1, "items": [{"id": 1, "type": "text", "x": 0, "y": 0}]})";
        // What this process has open in the folder: what is to replace the
        // board while it is written, which has no name there until then.
        auto beingWritten = [&home] {
            QByteArrayList found;
            const QDir open("/proc/self/fd");
            for (const QFileInfo &each : open.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot))
                if (each.symLinkTarget().startsWith(home.path() + "/"))
                    found << read(each.filePath());
            return found;
        };
        // Asked, as the window's save asks, whether the board is still as it
        // was. By then what is to replace it is all written: only the rename
        // is left. An agent renames a whole board of its own over it just
        // then.
        QByteArrayList beside;
        QByteArray fingerprint;
        const QString problem = Library::write(file, mine, &fingerprint, [&] {
            beside = beingWritten();
            write(file + ".agent", theirs);
            std::rename(qPrintable(file + ".agent"), qPrintable(file));
            return Library::fingerprint(file) == base;
        });
        QCOMPARE(problem, QString());
        QCOMPARE(beside, QByteArrayList{mine});
        // What the agent wrote is left as it wrote it, and nothing else is.
        QCOMPARE(read(file), theirs);
        QVERIFY(fingerprint.isEmpty());
        QVERIFY(beingWritten().isEmpty());
        QCOMPARE(QDir(home.path()).entryList(QDir::Files), QStringList{"board.json"});
        // Still as it was, it is replaced.
        const QByteArray now = Library::fingerprint(file);
        QCOMPARE(Library::write(file, mine, &fingerprint, [&] { return Library::fingerprint(file) == now; }), QString());
        QCOMPARE(read(file), mine);
        QCOMPARE(fingerprint, Library::fingerprint(file));
    }

    void aWholeFileWrittenAsTheWindowLetsGoOfTheBoardIsKept() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "Saved"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QString file = desk.library.boardFile("desk");
        // The window writes the board then and there, as it does when it
        // quits: a note of its own to write, and one a command added to take
        // in first. As it takes that in, an agent renames a whole board over
        // the file, with a note of its own.
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "The command's", "--x", "300", "--y", "0"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        desk.add(note(0, 300, "The person's"));
        bool written = false;
        const auto during = connect(&desk.board(), &Board::itemChanged, &desk.board(), [&] {
            if (written)
                return;
            written = true;
            QJsonArray items = desk.onDisk().value("items").toArray();
            items.append(QJsonObject{{"type", "note"}, {"x", 300}, {"y", 300}, {"text", "The agent's"}});
            write(file + ".tmp", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
            std::rename(qPrintable(file + ".tmp"), qPrintable(file));
        });
        QVERIFY(desk.backend.flush());
        disconnect(during);
        QVERIFY(written);
        // All of it is on disk, and in the window.
        QStringList texts;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            texts << value.toObject().value("text").toString();
        texts.sort();
        QCOMPARE(texts, (QStringList{"Saved", "The agent's", "The command's", "The person's"}));
        QCOMPARE(desk.board().count(), 4);
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardRestackedFromOutsideSaysSo() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 back = desk.add(note(0, 0, "Back")), front = desk.add(note(50, 50, "Front"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // An agent writes the board whole, the two stacked the other way.
        const QJsonArray items = desk.onDisk().value("items").toArray();
        const QString file = desk.library.boardFile("desk");
        write(file + ".tmp",
              QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", QJsonArray{items[1], items[0]}}}).toJson());
        QVERIFY(std::rename(qPrintable(file + ".tmp"), qPrintable(file)) == 0);
        // So they are here, and the window says where that came from.
        QTRY_COMPARE(desk.board().order(), (QVector<quint32>{front, back}));
        QCOMPARE(desk.backend.notice(), QString("Updated desk from outside"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardThatCannotBeReadSaysSoForAsLongAsItIsOpen() {
        // Written by hand, or by an agent, with one slip: a comma too many.
        // Opened in a window as wide as the one it was found in.
        const QByteArray slip = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "a week of planning"},
        ]})";
        auto writeSlip = [&slip](const QString &home) {
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", slip);
        };
        QByteArray left;
        {
            Desk desk("trip", writeSlip, QSize(1200, 760));
            QVERIFY(desk.ready());
            const QString file = desk.library.boardFile("trip");
            // An empty board in its place, which is read only, and said to
            // be: never saved, and never in the color of a board that is.
            QVERIFY(desk.backend.readOnly());
            QVERIFY(desk.canvas->readOnly());
            QCOMPARE(desk.board().count(), 0);
            QCOMPARE(desk.backend.saveState(), QString("readonly"));
            QCOMPARE(desk.item("saveState")->property("text").toString(), QString("read only"));
            QVERIFY(desk.item("saveMark")->property("color").value<QColor>() != desk.theme.good());
            // What is wrong, and what can be done about it, is more than the
            // line has room for. It has what fits, and the whole of it is
            // there to be read. Which file to fix is said once, by what is
            // wrong with it.
            const QString problem = desk.backend.problem();
            QVERIFY2(problem.contains("is not valid JSON") && problem.contains("Nothing will be saved over it") &&
                         problem.contains("Fix the file") && problem.contains("Ctrl+O"),
                     qPrintable(problem));
            QCOMPARE(problem.count(file), 1);
            QVERIFY(desk.shows("problem"));
            QVERIFY(desk.item("problem")->property("truncated").toBool());
            QVERIFY(desk.readsWhole(problem));
            // A click puts it away. The board is read only still, and says
            // so, however long it is left and whatever the person starts over
            // with on it; a click on that says why again.
            QVERIFY(desk.clickOn("wholeProblem"));
            QCOMPARE(desk.backend.problem(), QString());
            QVERIFY(!desk.shows("wholeProblem"));
            desk.add(note(0, 0, "starting over"));
            QTest::qWait(700);
            QCOMPARE(desk.backend.saveState(), QString("readonly"));
            QCOMPARE(desk.item("saveState")->property("text").toString(), QString("read only"));
            QVERIFY(desk.clickOn("saveState"));
            QCOMPARE(desk.backend.problem(), problem);
            QVERIFY(desk.readsWhole(problem));
            // Q quits at once: nothing on the stand-in was ever to be kept,
            // so there is nothing to save, and nothing to ask.
            QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
            desk.key(Qt::Key_Q);
            QCOMPARE(quit.count(), 1);
            QCOMPARE(desk.panel(), QString());
            QVERIFY(desk.window->close());
            QCOMPARE(desk.warnings, QStringList());
            // The file is read the moment the backend that writes boards is
            // gone, and before its folder goes too.
            QObject::connect(&desk.backend, &QObject::destroyed, [&left, file] { left = read(file); });
        }
        QCOMPARE(left, slip);
    }

    void aBoardThatCannotBeReadOpensOnceItsFileIsMended() {
        // Far from where a board begins, with a color there is not.
        auto board = [](const QString &color) {
            return QString(R"({"sprawl": 1, "items": [
                {"id": 1, "type": "note", "x": 3000, "y": 2000, "color": "yellow", "text": "a week of planning"},
                {"id": 2, "type": "note", "x": 3200, "y": 2000, "color": "%1", "text": "the way back"}]})")
                .arg(color)
                .toUtf8();
        };
        Desk desk("trip", [&board](const QString &home) {
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", board("purple"));
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.readOnly());
        const QString file = desk.library.boardFile("trip");
        // What is wrong does not say which file it is in, so the problem does.
        QVERIFY(desk.backend.problem().contains("purple"));
        QVERIFY(desk.backend.problem().contains("/boards/trip/board.json and Sprawl opens it by itself"));
        // The person looks around the empty board meanwhile, and puts the
        // problem away. Anything put on that board is never kept: it only
        // stands in for the one that could not be read.
        desk.canvas->setView(QPointF(-300, -200), 1.5);
        desk.backend.dismissProblem();
        desk.add(note(0, 0, "on the stand-in"));
        // Mended by hand, with a slip on the way: still not a board, and
        // what is wrong with it now is said. So it is when the slip is
        // undone, and the file is as it was.
        QByteArray slipped = board("blue");
        slipped.replace("\"the way back\"}", "\"the way back\"},");
        write(file, slipped);
        QTRY_VERIFY(desk.backend.problem().contains("is not valid JSON"));
        QVERIFY(desk.backend.problem().contains("Nothing will be saved over it"));
        write(file, board("purple"));
        QTRY_VERIFY(desk.backend.problem().contains("purple"));
        QVERIFY(desk.backend.readOnly());
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        // Mended, it opens by itself, as it would have at first: what it
        // holds in view, and nothing left to say of the file.
        write(file, board("blue"));
        QTRY_VERIFY(!desk.backend.readOnly());
        QVERIFY(!desk.canvas->readOnly());
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.board().item(1)->text, QString("a week of planning"));
        QCOMPARE(desk.board().item(2)->color, QString("blue"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.backend.notice(), QString("Reloaded trip: it changed on disk"));
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("saved"));
        QCOMPARE(desk.item("saveMark")->property("color").value<QColor>(), desk.theme.good());
        const QRectF view(QPointF(0, 0), desk.canvas->size());
        QVERIFY(view.contains(desk.canvas->toItem(desk.board().contentBounds().center())));
        // A board like any other: what is done to it is saved.
        desk.add(note(3400, 2000, "and home"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 3);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aProblemTooLongForTheLineCanBeReadWhole() {
        Desk desk("desk", {}, QSize(800, 600));
        QVERIFY(desk.ready());
        // One that fits is on the line alone.
        QVERIFY(!desk.backend.openBoard("trip"));
        QCOMPARE(desk.backend.problem(), QString("There is no board \"trip\"."));
        QVERIFY(desk.readsWhole(desk.backend.problem()));
        QVERIFY(desk.shows("problem"));
        QVERIFY(!desk.shows("wholeProblem"));
        // The board's file changes into one that cannot be read while it is
        // open. What is said of that is more than the line has room for, and
        // the whole of it is there to be read.
        desk.add(note(0, 0, "Mine"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        write(desk.library.boardFile("desk"), "{ this is not a board");
        QTRY_VERIFY(desk.backend.problem().contains("can no longer be read"));
        const QString problem = desk.backend.problem();
        QVERIFY(desk.item("problem")->property("truncated").toBool());
        QVERIFY(desk.readsWhole(problem));
        // All of it in view, nothing beside it says there is more.
        QVERIFY(!desk.shows("wholeProblemInView"));
        // The line looks like something to click, and a click puts it away,
        // there and over the board; then it no longer does. The board is
        // still not saved, and a click on that says why again.
        const QQuickItem *line = desk.item("problem");
        QTest::mouseMove(desk.window, line->mapToScene(QPointF(line->width() / 2, line->height() / 2)).toPoint());
        QCOMPARE(desk.window->cursor().shape(), Qt::PointingHandCursor);
        QVERIFY(desk.clickOn("problem"));
        QCOMPARE(desk.backend.problem(), QString());
        QVERIFY(!desk.shows("wholeProblem"));
        QCOMPARE(desk.window->cursor().shape(), Qt::ArrowCursor);
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("not saved"));
        QVERIFY(desk.clickOn("saveState"));
        QCOMPARE(desk.window->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(desk.backend.problem(), problem);
        QVERIFY(desk.readsWhole(problem));
        // Saved, it has nothing to say: it no longer looks like something to
        // click, and a click there says nothing.
        write(desk.library.boardFile("desk"), QJsonDocument(desk.board().toJson()).toJson());
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.window->cursor().shape(), Qt::ArrowCursor);
        QVERIFY(desk.clickOn("saveState"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardThatCannotBeReadNamesItsFileAnewWhenRenamed() {
        // A comma too many: the reason given names the file by where it is.
        const QByteArray slip = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "a week of planning"},
        ]})";
        Desk desk("trip", [&slip](const QString &home) {
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", slip);
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.readOnly());
        desk.backend.dismissProblem();
        // Renamed in the switcher: what is said of it names it as it is now,
        // and the file to fix where it is now. That is the file watched.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_F2);
        desk.type("journey");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("journey"));
        const QString file = desk.library.boardFile("journey");
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.startsWith("Could not open journey. " + file + " is not valid JSON") &&
                     problem.contains("Fix the file and Sprawl opens it by itself"),
                 qPrintable(problem));
        QVERIFY2(!problem.contains("/boards/trip/"), qPrintable(problem));
        QVERIFY(desk.backend.readOnly());
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        QCOMPARE(read(file), slip);
        // That is what a click on "read only" says again.
        desk.backend.dismissProblem();
        QVERIFY(desk.clickOn("saveState"));
        QCOMPARE(desk.backend.problem(), problem);
        // Mended there, it opens.
        write(file, QByteArray(slip).replace("},\n", "}\n"));
        QTRY_VERIFY(!desk.backend.readOnly());
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.backend.notice(), QString("Reloaded journey: it changed on disk"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardMadeByANewerSprawlSaysToUpdateNotToFixIt() {
        const QByteArray newer = R"({"sprawl": 2, "items": [{"id": 1, "type": "note", "x": 0, "y": 0}]})";
        Desk desk("next", [&newer](const QString &home) {
            QDir(home).mkpath("boards/next");
            write(home + "/boards/next/board.json", newer);
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.readOnly());
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        // Nothing is wrong with the file: a newer Sprawl is what opens it.
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.contains("made by a newer Sprawl (format 2") && problem.contains("Update Sprawl to open it") &&
                     problem.contains("Ctrl+O"),
                 qPrintable(problem));
        QVERIFY2(!problem.contains("Fix"), qPrintable(problem));
        QVERIFY(desk.backend.flush());
        QCOMPARE(read(desk.library.boardFile("next")), newer);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aWindowWithNoBoardToKeepAnythingInIsReadOnly() {
        // The very first time, in a library nothing can be written to:
        // neither the welcome board nor an untitled one can be made.
        Desk desk(
            QString(),
            [](const QString &home) {
                QDir(home).mkpath("boards");
                QFile::setPermissions(home + "/boards", QFile::ReadOwner | QFile::ExeOwner);
            },
            QSize(1200, 760));
        QVERIFY(desk.ready());
        const QString boards = desk.library.boardsFolder();
        // An empty board, and no other: read only, and said to be, since
        // nothing done on it could be kept anywhere.
        QCOMPARE(desk.backend.boardName(), QString());
        QVERIFY(desk.backend.readOnly());
        QVERIFY(desk.canvas->readOnly());
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("read only"));
        QVERIFY(desk.item("saveMark")->property("color").value<QColor>() != desk.theme.good());
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.startsWith("Could not make the folder " + boards + "/untitled.") &&
                     problem.contains("Ctrl+O") && problem.contains("Ctrl+N"),
                 qPrintable(problem));
        QVERIFY(desk.readsWhole(problem));
        // Whatever ends up on it, it stays so, and Q quits at once: there is
        // nothing to save, and nothing to ask.
        desk.add(note(0, 0, "nowhere to go"));
        QTest::qWait(700);
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(desk.panel(), QString());
        // Once boards can be made, Ctrl+N makes one like any other.
        QVERIFY(QFile::setPermissions(boards, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        desk.key(Qt::Key_N, Qt::ControlModifier);
        desk.type("plans");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("plans"));
        QVERIFY(!desk.backend.readOnly());
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.board().count(), 0);
        desk.add(note(0, 0, "kept"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theLastBoardTrashedWithNowhereToMakeAnotherLeavesNone() {
        Desk desk("trip");
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "a week of planning"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->canUndo());
        // Where a board made in its place would go, no folder can be made,
        // as on a full disk, where moving one to the trash still works.
        QVERIFY(QFile::link(desk.home.filePath("nowhere"), desk.library.boardFolder("untitled")));
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_Delete, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QVERIFY(!desk.library.exists("trip"));
        // What it held went to the trash with it: nothing of it is left here
        // to be changed, or undone, where it could not be kept.
        QCOMPARE(desk.backend.boardName(), QString());
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());
        QVERIFY(desk.backend.readOnly());
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("read only"));
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.startsWith("Could not make the folder " + desk.library.boardFolder("untitled") + ".") &&
                     problem.contains("Ctrl+N"),
                 qPrintable(problem));
        QVERIFY(desk.readsWhole(problem));
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.warnings, QStringList());
    }

    void itemsWrittenWithoutAnIdAreGivenTheirsWhenTheBoardOpens() {
        // Written whole by an agent, the heading with no id, as the skill
        // allows; and the same board with a color there is not.
        auto board = [](const QString &color) {
            return QString(R"({"sprawl": 1, "items": [
                {"id": 1, "type": "note", "x": 0, "y": 0, "color": "%1", "text": "Plan"},
                {"type": "text", "x": 0, "y": -60, "size": 28, "text": "Weekend"}]})")
                .arg(color)
                .toUtf8();
        };
        auto idOnDisk = [](const QString &file) {
            QJsonObject root;
            Library::read(file, root);
            return root.value("items").toArray().last().toObject().value("id").toInteger();
        };
        Desk desk("desk", [&board](const QString &home) {
            QDir(home).mkpath("boards/desk");
            write(home + "/boards/desk/board.json", board("yellow"));
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", board("purple"));
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.board().count(), 2);
        // Nothing is done here, and the file is written all the same, the
        // heading with the id it goes by here and in what commands print.
        quint32 heading = desk.all(Item::Kind::Text).first()->id;
        QTRY_COMPARE(idOnDisk(desk.library.boardFile("desk")), qint64(heading));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // So it is for a board that could not be read, once it is mended.
        QVERIFY(desk.backend.openBoard("trip"));
        QVERIFY(desk.backend.readOnly());
        write(desk.library.boardFile("trip"), board("blue"));
        QTRY_VERIFY(!desk.backend.readOnly());
        heading = desk.all(Item::Kind::Text).first()->id;
        QTRY_COMPARE(idOnDisk(desk.library.boardFile("trip")), qint64(heading));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aProblemTooLongEvenForTheBoardCanBeReadToItsEnd() {
        // The smallest window, in a large font, as main() sets the theme's;
        // and a board with a lot wrong with it: shapes there are not.
        const QFont font = QGuiApplication::font();
        QFont large("monospace");
        large.setStyleHint(QFont::Monospace);
        large.setPixelSize(20);
        QGuiApplication::setFont(large);
        const auto restore = qScopeGuard([&font] { QGuiApplication::setFont(font); });
        const QString name = "a-weekend-by-the-lake-with-everyone";
        Desk desk(
            name,
            [&name](const QString &home) {
                QDir(home).mkpath("theme");
                write(home + "/theme/shell.toml", "[font]\nbase-size = 20\n");
                QStringList items;
                for (int i = 1; i <= 5; ++i)
                    items << QString(R"({"id": %1, "type": "shape", "x": %2, "y": 0, "shape": "one-there-is-not-%1"})")
                                 .arg(i)
                                 .arg(i * 200);
                QDir(home).mkpath("boards/" + name);
                write(home + "/boards/" + name + "/board.json",
                      QString(R"({"sprawl": 1, "items": [%1]})").arg(items.join(", ")).toUtf8());
            },
            QSize(560, 460));
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.readOnly());
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.contains("And 2 more.") && problem.endsWith("or open another board with Ctrl+O."),
                 qPrintable(problem));
        // It is shown over the board and nowhere else: "read only" is in view
        // beside it, on the line.
        auto box = [](const QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        QVERIFY(desk.shows("wholeProblem"));
        QQuickItem *card = desk.item("wholeProblem");
        desk.window->grabWindow();
        QVERIFY(box(desk.canvas).contains(box(card)));
        QVERIFY(desk.shows("saveState"));
        QVERIFY(!box(card).intersects(box(desk.item("saveState"))));
        // All of it is there, cut short nowhere. More of it than the board has
        // room for is out of view, and the wheel brings it in, every line of
        // it on the way, to the end: what can be done about it. None of it is
        // ever seen outside the card, and a mark beside it says how much of it
        // is in view, and where.
        QVector<QQuickItem *> texts;
        Desk::texts(card, texts);
        const auto found = std::find_if(texts.cbegin(), texts.cend(), [&problem](const QQuickItem *text) {
            return text->property("text").toString() == problem;
        });
        QVERIFY(found != texts.cend());
        const QQuickItem *text = *found;
        QVERIFY(!text->property("truncated").toBool());
        const QQuickItem *mark = desk.item("wholeProblemInView");
        QVERIFY(mark);
        // The part of it in view, as far down it as it is.
        auto inView = [&desk, &box, text] {
            desk.window->grabWindow();
            QRectF seen = box(text) & QRectF(QPointF(0, 0), desk.window->size());
            for (const QQuickItem *in = text->parentItem(); in; in = in->parentItem())
                if (in->clip())
                    seen &= box(in);
            return text->mapRectFromScene(seen);
        };
        QRectF seen = inView();
        QVERIFY2(seen.height() < text->height(), "the problem is to be more than the board has room for");
        QVERIFY(qAbs(seen.top()) < 0.5);
        QVERIFY(box(card).contains(text->mapRectToScene(seen)));
        QVERIFY(mark->isVisible());
        QVERIFY(qAbs(box(mark).top() - text->mapRectToScene(seen).top()) <= 1);
        const QPointF over = desk.canvas->toCanvas(desk.canvas->mapFromScene(box(card).center()));
        const QPointF origin = desk.canvas->origin();
        for (int notch = 0; notch < 100 && seen.bottom() < text->height() - 0.5; ++notch) {
            const qreal reached = seen.bottom();
            desk.wheel(over, QPoint(0, -120));
            seen = inView();
            QVERIFY(box(card).contains(text->mapRectToScene(seen)));
            QVERIFY(seen.top() <= reached && seen.bottom() > reached);
        }
        QVERIFY(seen.bottom() >= text->height() - 0.5);
        QVERIFY(qAbs(box(mark).bottom() - text->mapRectToScene(seen).bottom()) <= 1);
        // The board under it never moved; and the wheel brings back the start.
        QCOMPARE(desk.canvas->origin(), origin);
        for (int notch = 0; notch < 100 && seen.top() >= 0.5; ++notch) {
            desk.wheel(over, QPoint(0, 120));
            seen = inView();
        }
        QVERIFY(qAbs(seen.top()) < 0.5);
        QVERIFY(qAbs(box(mark).top() - text->mapRectToScene(seen).top()) <= 1);
        // A click puts it away, part read; the board is read only still, and
        // says so. Said again, it is read from the start.
        desk.wheel(over, QPoint(0, -120));
        QVERIFY(inView().top() > 0);
        QVERIFY(desk.clickOn("wholeProblem"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.item("saveState")->property("text").toString(), QString("read only"));
        QVERIFY(desk.clickOn("saveState"));
        QCOMPARE(desk.backend.problem(), problem);
        QVERIFY(qAbs(inView().top()) < 0.5);
        QCOMPARE(desk.warnings, QStringList());
    }

    void whyAFileCannotBeReadAtAllStaysPutAway() {
        // A file this app may not read: nothing tells it apart from what it
        // was, so it is read again at every change beside it. It cannot be
        // watched itself either, which is not tried.
        QTest::failOnWarning(QRegularExpression("inotify_add_watch"));
        const QByteArray plain = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "text": "Plan"}]})";
        Desk desk("trip", [&plain](const QString &home) {
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", plain);
            QFile::setPermissions(home + "/boards/trip/board.json", QFileDevice::Permissions());
        });
        QVERIFY(desk.ready());
        const QString file = desk.library.boardFile("trip");
        QVERIFY(desk.backend.readOnly());
        const QString problem = desk.backend.problem();
        QVERIFY2(problem.startsWith("Could not open trip. Could not read " + file), qPrintable(problem));
        // Put away, it stays away while the file is so, a command that cannot
        // read it either coming and going meanwhile.
        auto command = [&desk] {
            qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
            const int code = runCli({"sprawl", "add", "trip", "note", "--text", "Packing"});
            qunsetenv("SPRAWL_HOME");
            return code;
        };
        desk.backend.dismissProblem();
        QSignalSpy said(&desk.backend, &Backend::problemChanged);
        QCOMPARE(command(), 1);
        QTest::qWait(500);
        QCOMPARE(said.count(), 0);
        QCOMPARE(desk.backend.saveState(), QString("readonly"));
        // Asked for, it is said again. Once the file can be read, the board
        // opens.
        QVERIFY(desk.clickOn("saveState"));
        QCOMPARE(desk.backend.problem(), problem);
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner));
        QTRY_VERIFY(!desk.backend.readOnly());
        QCOMPARE(desk.board().count(), 1);
        // So with the file of an open board that comes to be one this app may
        // not read: nothing is written over it, and what is said of that, put
        // away, stays away too.
        QVERIFY(QFile::setPermissions(file, QFileDevice::Permissions()));
        QTRY_COMPARE(desk.backend.saveState(), QString("paused"));
        QVERIFY(desk.backend.problem().contains("can no longer be read"));
        desk.backend.dismissProblem();
        said.clear();
        QCOMPARE(command(), 1);
        QTest::qWait(500);
        QCOMPARE(said.count(), 0);
        QCOMPARE(desk.backend.saveState(), QString("paused"));
        // Readable again, as it was: saved.
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(read(file), plain);
        QCOMPARE(desk.warnings, QStringList());
    }

    void idsThatCannotBeWrittenBackLeaveNothingUnsaved() {
        // Written by an agent, a heading with no id, in a folder this app
        // cannot write to: a board shared to be looked at.
        const QByteArray shared = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "Plan"},
            {"type": "text", "x": 0, "y": -60, "size": 28, "text": "Weekend"}]})";
        // Each save state the window comes to. Made before the desk, which
        // can come to one more as it is torn down, a test that failed
        // included.
        QStringList states;
        Desk desk("desk", [&shared](const QString &home) {
            QDir(home).mkpath("boards/shared");
            write(home + "/boards/shared/board.json", shared);
            QFile::setPermissions(home + "/boards/shared", QFile::ReadOwner | QFile::ExeOwner);
        });
        QVERIFY(desk.ready());
        const QString folder = desk.library.boardFolder("shared");
        const QString file = desk.library.boardFile("shared");
        const auto writable = qScopeGuard(
            [&folder] { QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
        connect(&desk.backend, &Backend::saveStateChanged, &desk.backend,
                [&desk, &states] { states << desk.backend.saveState(); });
        // Opened, it is saved: the file reads as the board does. Writing the
        // heading's id into it is tried, and that it cannot be is said
        // nowhere, as nothing done here is lost.
        QVERIFY(desk.backend.openBoard("shared"));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QTRY_COMPARE(states, (QStringList{"saving", "saved"}));
        QCOMPARE(desk.backend.problem(), QString());
        // Nor at the last moment, when the lock cannot be had: an agent
        // writes the file in place, a note with no id added, and the board,
        // taken in, is let go of before those ids are written.
        QByteArray more = shared;
        more.replace("\"Weekend\"}]}", R"("Weekend"}, {"type": "note", "x": 300, "y": 0, "text": "Packing"}]})");
        write(file, more);
        QTRY_COMPARE(desk.board().count(), 3);
        QVERIFY(desk.backend.flush());
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.backend.problem(), QString());
        // Q quits at once, and the file is as the agent left it.
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(read(file), more);
        // Something done here is another matter: that it is not saved is
        // said, and quitting asks.
        desk.add(note(600, 0, "Maps"));
        QTRY_COMPARE(desk.backend.saveState(), QString("failed"));
        QVERIFY2(desk.backend.problem().startsWith("The board is not saved."), qPrintable(desk.backend.problem()));
        desk.key(Qt::Key_Q);
        QCOMPARE(desk.panel(), QString("quit"));
        QCOMPARE(quit.count(), 1);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        // Once it can be, the next save writes it all, the ids with it.
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        desk.add(note(900, 0, "Tickets"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QHash<qint64, QString> written;
        for (const QJsonValue &value : desk.onDisk().value("items").toArray())
            written.insert(value.toObject().value("id").toInteger(), value.toObject().value("text").toString());
        QHash<qint64, QString> here;
        for (quint32 id : desk.board().order())
            here.insert(id, desk.board().item(id)->text);
        QCOMPARE(written, here);
        QCOMPARE(written.size(), 5);
        // An agent adds a note with no id again. Its id is written when the
        // board is let go of, if not before; and then nothing more is.
        auto addedByAnAgent = [&desk, &file](const QString &text) {
            QJsonObject root = desk.board().toJson();
            QJsonArray items = root.value("items").toArray();
            items.append(QJsonObject{{"type", "note"}, {"x", 1200}, {"y", 0}, {"text", text}});
            root.insert("items", items);
            write(file, QJsonDocument(root).toJson());
        };
        addedByAnAgent("Snacks");
        QTRY_COMPARE(desk.board().count(), 6);
        QVERIFY(desk.backend.flush());
        QCOMPARE(desk.onDisk().value("items").toArray().last().toObject().value("id").toInteger(),
                 qint64(desk.last()->id));
        states.clear();
        QTest::qWait(700);
        QCOMPARE(states, QStringList());
        // Nor when the file changes into one that cannot be read before the
        // ids it was to be given are written: the board can be let go of,
        // and the file is left as it is.
        addedByAnAgent("Water");
        QTRY_COMPARE(desk.board().count(), 7);
        write(file, "{ this is not a board");
        QTRY_COMPARE(desk.backend.saveState(), QString("paused"));
        QVERIFY(desk.backend.flush());
        QCOMPARE(read(file), QByteArray("{ this is not a board"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void anItemAddedByCommandIsWhereItsPlaceSays() {
        // Its corner where --x and --y say; and where either does not say,
        // where it would go with neither: below everything, lined up with
        // its left edge.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        auto add = [](const QStringList &item) { return runCli(QStringList{"sprawl", "add", "places"} + item); };
        // More to say than a diamond has room for: it grows to hold it.
        const QString question = "Ready?\\nAsk Sam first\\nthen the others\\nthen ship it";
        QVector<int> codes{runCli({"sprawl", "new", "places"})};
        codes << add({"note", "--text", "First", "--x", "40", "--y", "100"});
        // Below that, out to the side.
        codes << add({"note", "--text", "Aside", "--x", "600"});
        // Above them both, at their left edge.
        codes << add({"text", "--text", "Heading", "--y", "-300"});
        // Grown about its middle, as a shape is, and its corner then put
        // back where it was asked to be.
        codes << add({"shape", "--shape", "diamond", "--text", question, "--x", "300", "--y", "500"});
        codes << add({"shape", "--shape", "diamond", "--text", question, "--y", "900"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(codes, QVector<int>(codes.size(), 0));
        QJsonObject root;
        QCOMPARE(Library::read(Library(home.path()).boardFile("places"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QVector<QPointF> corners;
        for (quint32 id : board.order())
            corners << board.item(id)->rect.topLeft();
        QCOMPARE(corners, (QVector<QPointF>{{40, 100}, {600, 276}, {40, -300}, {300, 500}, {40, 900}}));
        for (const quint32 id : {board.order()[3], board.order()[4]})
            QVERIFY(board.item(id)->rect.height() > Defaults::shapeSize(Item::Shape::Diamond).height());
    }

    void aBoardAskedForThatIsNotThereIsSaidToBeMissing() {
        // Opened with a slip in its path, there is no such board, and the
        // one that was open last opens in its place. That the one asked for
        // is not there is said, and stays said once the other is open.
        const QByteArray trip = R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "a week of planning"}]})";
        auto writeTrip = [&trip](const QString &home) {
            QDir(home).mkpath("boards/trip");
            write(home + "/boards/trip/board.json", trip);
        };
        const QString slip = "/nonexistent/boards/trip/bord.json";
        {
            Desk desk(slip, writeTrip);
            QVERIFY(desk.ready());
            QCOMPARE(desk.backend.boardName(), QString("trip"));
            QCOMPARE(desk.board().count(), 1);
            QVERIFY(!desk.backend.readOnly());
            const QString problem = desk.backend.problem();
            QCOMPARE(problem, "There is no board \"" + slip + "\".");
            QVERIFY(desk.shows("problem"));
            QVERIFY(desk.readsWhole(problem));
            // Worked on and saved, it is said still, until a click puts it
            // away.
            desk.add(note(300, 0, "and the tickets"));
            QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
            QCOMPARE(desk.backend.problem(), problem);
            QVERIFY(desk.clickOn("problem"));
            QCOMPARE(desk.backend.problem(), QString());
            QCOMPARE(desk.warnings, QStringList());
        }
        // Named as no board can be, in place of a board that cannot be read:
        // both are said, and a click on "read only" says again why it is.
        {
            Desk desk("Holiday Plans", [](const QString &home) {
                QDir(home).mkpath("boards/trip");
                write(home + "/boards/trip/board.json", "{ this is not a board");
            });
            QVERIFY(desk.ready());
            QCOMPARE(desk.backend.boardName(), QString("trip"));
            QVERIFY(desk.backend.readOnly());
            const QString problem = desk.backend.problem();
            QVERIFY2(problem.startsWith("There is no board \"Holiday Plans\". Could not open trip.") &&
                         problem.contains("Ctrl+O"),
                     qPrintable(problem));
            QVERIFY(desk.readsWhole(problem));
            QVERIFY(desk.clickOn("wholeProblem"));
            QCOMPARE(desk.backend.problem(), QString());
            QVERIFY(desk.clickOn("saveState"));
            QVERIFY2(desk.backend.problem().startsWith("Could not open trip."), qPrintable(desk.backend.problem()));
            QCOMPARE(desk.warnings, QStringList());
        }
        // A name that is free, in a library where no board can be made: why
        // it could not be made is said, with the last board open.
        {
            Desk desk("plans", [&writeTrip](const QString &home) {
                writeTrip(home);
                QFile::setPermissions(home + "/boards", QFile::ReadOwner | QFile::ExeOwner);
            });
            const QString boards = desk.library.boardsFolder();
            const auto writable = qScopeGuard(
                [&boards] { QFile::setPermissions(boards, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
            QVERIFY(desk.ready());
            QCOMPARE(desk.backend.boardName(), QString("trip"));
            QVERIFY(!desk.backend.readOnly());
            QCOMPARE(desk.backend.problem(), "Could not make the folder " + boards + "/plans.");
            QVERIFY(desk.shows("problem"));
            QCOMPARE(desk.warnings, QStringList());
        }
    }

    void anExportGivenItsEndingAsksBeforeReplacingAFile() {
        // The save dialog asks before it lets a file be written over, about
        // the name it was given. Given "plan", the export is plan.png, a
        // name it never asked about.
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "Export me"));
        desk.add(note(500, 400));
        const QString plan = desk.home.filePath("plan.png");
        const QByteArray before = "the picture that was here before";
        write(plan, before);
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("plan"));
        auto exportPng = [&desk] {
            desk.key(Qt::Key_E, Qt::ControlModifier);
            desk.key(Qt::Key_Return);
        };
        // Asked, and nothing written yet; nor is anything done on the board
        // while it is.
        exportPng();
        QCOMPARE(desk.panel(), QString("replace"));
        QVERIFY(desk.readsWhole("Replace plan.png?"));
        QCOMPARE(read(plan), before);
        desk.key(Qt::Key_Space);
        QCOMPARE(desk.board().count(), 2);
        // Esc keeps it: nothing is written, and there is nothing to say.
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(read(plan), before);
        QVERIFY(!QFile::exists(desk.home.filePath("plan")));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.backend.notice(), QString());
        // So does a click beside the question.
        exportPng();
        QCOMPARE(desk.panel(), QString("replace"));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(20, 20));
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(read(plan), before);
        QCOMPARE(desk.backend.notice(), QString());
        // Enter replaces it.
        exportPng();
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(QImage(plan).size(), QSize(2 * (552 + 64), 2 * (452 + 64)));
        QVERIFY(desk.backend.notice().contains("plan.png"));
        // What is selected is what is written, once the question is answered.
        desk.click(QPointF(150, 150));
        const QString drawing = desk.home.filePath("plan.svg");
        write(drawing, before);
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_S);
        QCOMPARE(desk.panel(), QString("replace"));
        QVERIFY(desk.readsWhole("Replace plan.svg?"));
        desk.key(Qt::Key_Return);
        QVERIFY(read(drawing).contains("viewBox=\"0 0 216 216\""));
        // A name chosen with its ending is one the dialog asked about: it is
        // not asked again.
        write(plan, before);
        desk.picker.target = QUrl::fromLocalFile(plan);
        exportPng();
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(QImage(plan).size(), QSize(2 * 216, 2 * 216));
        // Nor is a folder of that name, which is never written over: the
        // export says it cannot be written.
        QVERIFY(QDir(desk.home.path()).mkpath("album.png"));
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("album"));
        exportPng();
        QCOMPARE(desk.panel(), QString());
        QVERIFY2(desk.backend.problem().startsWith("Could not write " + desk.home.filePath("album.png")),
                 qPrintable(desk.backend.problem()));
        QVERIFY(QFileInfo(desk.home.filePath("album.png")).isDir());
        QCOMPARE(desk.picker.asked, 6);
        QCOMPARE(desk.warnings, QStringList());
    }

    void anExportAsksNothingAboutAFileItCannotReplace() {
        // An export is written beside the file it replaces, and renamed over
        // it. A file it may not change, or one in a folder it may not write
        // in, it cannot replace, and a yes to replacing it would come to
        // nothing: it is not asked about. The export says why it cannot be
        // written, and the file is as it was.
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "Export me"));
        const QByteArray before = "the picture that was here before";
        auto exportPng = [&desk](const QString &name) {
            desk.picker.target = QUrl::fromLocalFile(name);
            desk.key(Qt::Key_E, Qt::ControlModifier);
            desk.key(Qt::Key_Return);
        };
        const QString locked = desk.home.filePath("locked.png");
        write(locked, before);
        QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner));
        // A folder that may not be written in, with a file in it and a link
        // out of it to a file here; and a link here to the file in it.
        const QString shut = desk.home.filePath("shut"), here = desk.home.filePath("here.png");
        QVERIFY(QDir(desk.home.path()).mkpath("shut"));
        write(shut + "/plan.png", before);
        write(here, before);
        QVERIFY(QFile::link(here, shut + "/out.png"));
        QVERIFY(QFile::link(shut + "/plan.png", desk.home.filePath("in.png")));
        QVERIFY(QFile::setPermissions(shut, QFile::ReadOwner | QFile::ExeOwner));
        const auto writable = qScopeGuard(
            [&shut] { QFile::setPermissions(shut, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
        // Through a link, what would be replaced is the file at its end.
        for (const QString &file : QStringList{locked, shut + "/plan.png", desk.home.filePath("in.png")}) {
            exportPng(file.chopped(4));
            QCOMPARE(desk.panel(), QString());
            QVERIFY2(desk.backend.problem().startsWith("Could not write " + file), qPrintable(desk.backend.problem()));
            QCOMPARE(read(file), before);
            desk.backend.dismissProblem();
        }
        // Out of that folder, to a file that can be replaced: that is asked
        // about, and replaced once agreed to.
        exportPng(shut + "/out");
        QCOMPARE(desk.panel(), QString("replace"));
        QVERIFY(desk.readsWhole("Replace out.png?"));
        QCOMPARE(read(here), before);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(QImage(here).size(), QSize(2 * 216, 2 * 216));
        QVERIFY(QFileInfo(shut + "/out.png").isSymLink());
        QCOMPARE(desk.picker.asked, 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void anItemWithNoRoomBelowEverythingIsNotAdded() {
        // Below everything else, on a board that reaches as far down as a
        // board can, is further out than a file may say anything is: added
        // there, the board could not be read back. The command says so, and
        // adds nothing, not even the picture it was to copy in.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        const Library library(home.path());
        auto add = [](const QStringList &item) { return runCli(QStringList{"sprawl", "add", "far"} + item); };
        QCOMPARE(runCli({"sprawl", "new", "far"}), 0);
        QCOMPARE(add({"note", "--text", "As far down as can be", "--y", "1e9"}), 0);
        const QByteArray before = read(library.boardFile("far"));
        const QString photo = picture(home.filePath("photo.png"), QSize(800, 600));
        const QList<QStringList> below{
            {"note", "--text", "Below it"}, {"shape", "--x", "300"}, {"image", "--file", photo}};
        for (const QStringList &item : below)
            QVERIFY2(add(item) == 1, qPrintable(item.join(' ')));
        QCOMPARE(read(library.boardFile("far")), before);
        QVERIFY(QDir(library.assetsFolder("far")).entryList(QDir::Files).isEmpty());
        // What to do instead is said.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SPRAWL_HOME", home.path());
        QProcess command;
        command.setProcessEnvironment(environment);
        command.start(QCoreApplication::applicationFilePath(), {"--cli", "add", "far", "text", "--text", "Below it"});
        QVERIFY(command.waitForFinished(30000));
        QCOMPARE(command.exitCode(), 1);
        const QByteArray said = command.readAllStandardError();
        QVERIFY2(said.contains("There is no room for it below everything else: place it with --x and --y."),
                 said.constData());
        // Put somewhere, it goes there, and the board is read back.
        QCOMPARE(add({"note", "--text", "Beside it", "--x", "300", "--y", "1e9"}), 0);
        QCOMPARE(add({"text", "--text", "Above it", "--y", "-1e9"}), 0);
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("far"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.count(), 3);
    }

    void aBoardsNameWrittenAnotherWayIsNotThatBoard() {
        // "Weekend Trip" is what the board weekend-trip was made from, not
        // its name. A board is had by its name as it is, or a path to it, in
        // the window as by every command: the window opens the board open
        // last in its place and says the one asked for is not there, in the
        // words the commands say it in. What was asked for is quoted, as it
        // can be anything, ".." included.
        auto boards = [](const QString &home) {
            for (const QString &name : QStringList{"weekend-trip", "work"}) {
                QDir(home).mkpath("boards/" + name);
                write(home + "/boards/" + name + "/board.json",
                      QString(R"({"sprawl": 1, "items": [{"id": 1, "type": "note", "x": 0, "y": 0, "text": "%1"}]})")
                          .arg(name)
                          .toUtf8());
            }
            write(home + "/state.json", R"({"last": "work"})");
        };
        // Run from a folder with no board in it, nor in the one above it.
        QTemporaryDir here;
        QVERIFY(QDir(here.path()).mkpath("inside"));
        const QString inside = here.filePath("inside"), was = QDir::currentPath();
        QVERIFY(QDir::setCurrent(inside));
        const auto back = qScopeGuard([was] { QDir::setCurrent(was); });
        for (const QString &asked : QStringList{"Weekend Trip", "WEEKEND-TRIP", "weekend trip!", ".", ".."}) {
            Desk desk(asked, boards);
            QVERIFY(desk.ready());
            const QString missing = "There is no board \"" + asked + "\".";
            QCOMPARE(desk.backend.boardName(), QString("work"));
            QCOMPARE(desk.last()->text, QString("work"));
            QCOMPARE(desk.backend.problem(), missing);
            QVERIFY(desk.readsWhole(missing));
            QCOMPARE(desk.library.boards().size(), 2);
            QCOMPARE(desk.warnings, QStringList());
            // As the commands say it, run from the same folder.
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("SPRAWL_HOME", desk.home.path());
            const QList<QStringList> commands{{"show", asked}, {"check", asked}, {"add", asked, "note"}};
            for (const QStringList &command : commands) {
                QProcess process;
                process.setProcessEnvironment(environment);
                process.setWorkingDirectory(inside);
                process.start(QCoreApplication::applicationFilePath(), QStringList{"--cli"} + command);
                QVERIFY(process.waitForFinished(30000));
                QCOMPARE(process.exitCode(), 1);
                const QByteArray said = process.readAllStandardError();
                QVERIFY2(said.startsWith(missing.toUtf8() + " The boards are: "), said.constData());
            }
        }
    }

    void aNewBoardThatCannotHaveTheNameAskedForSaysSo() {
        // `sprawl open plans`, a name that is free, makes the board plans. A
        // folder there with no board in it (its file taken out by hand) keeps
        // the name, and is left as it is: the new board is plans-2, which is
        // said, with where that folder is.
        Desk desk("plans", [](const QString &home) { QDir(home).mkpath("boards/plans/assets"); });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.boardName(), QString("plans-2"));
        QVERIFY(!desk.backend.readOnly());
        const QString problem = desk.backend.problem();
        QCOMPARE(problem, desk.library.boardFolder("plans") +
                              " is there already and is not a board, so the new board is plans-2.");
        QVERIFY(desk.shows("problem"));
        QVERIFY(desk.readsWhole(problem));
        QVERIFY(!QFile::exists(desk.library.boardFile("plans")));
        // Worked on and saved, it is said still, until a click puts it away.
        desk.add(note(0, 0, "kept"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.backend.problem(), problem);
        QVERIFY(desk.clickOn("problem"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNameTakenByWhatIsNotABoardIsSaidToBeSoEverywhere() {
        // Among the boards, a folder with no board in it (its file taken out
        // by hand) and a file. Neither is a board, or shown as one, or made
        // one: a board that cannot be given the name one of them has says
        // so, however it is made or renamed, as `sprawl open plans` does.
        {
            Desk desk("work", [](const QString &home) {
                QDir(home).mkpath("boards/plans/assets");
                write(home + "/boards/notes", "not a board\n");
            });
            QVERIFY(desk.ready());
            const QString plans = desk.library.boardFolder("plans"), notes = desk.library.boardFolder("notes");
            // Made by a name typed in the switcher, which no board has.
            desk.key(Qt::Key_O, Qt::ControlModifier);
            desk.type("plans");
            desk.key(Qt::Key_Return);
            QCOMPARE(desk.panel(), QString());
            QCOMPARE(desk.backend.boardName(), QString("plans-2"));
            QCOMPARE(desk.backend.problem(),
                     plans + " is there already and is not a board, so the new board is plans-2.");
            QVERIFY(desk.shows("problem"));
            // By Ctrl+N, a file in the way.
            desk.key(Qt::Key_N, Qt::ControlModifier);
            desk.type("notes");
            desk.key(Qt::Key_Return);
            QCOMPARE(desk.panel(), QString());
            QCOMPARE(desk.backend.boardName(), QString("notes-2"));
            QCOMPARE(desk.backend.problem(),
                     notes + " is there already and is not a board, so the new board is notes-2.");
            // A board's own name is numbered as ever, with nothing to say:
            // the board that has it is there among the others.
            desk.backend.dismissProblem();
            desk.key(Qt::Key_N, Qt::ControlModifier);
            desk.type("work");
            desk.key(Qt::Key_Return);
            QCOMPARE(desk.backend.boardName(), QString("work-2"));
            QCOMPARE(desk.backend.problem(), QString());
            // Renamed to it, a board keeps its own name, and why is said as
            // it is.
            desk.key(Qt::Key_O, Qt::ControlModifier);
            desk.key(Qt::Key_F2);
            desk.type("plans");
            desk.key(Qt::Key_Return);
            QCOMPARE(desk.backend.boardName(), QString("work-2"));
            QCOMPARE(desk.backend.problem(), plans + " is there already and is not a board.");
            desk.key(Qt::Key_Escape);
            desk.key(Qt::Key_Escape);
            QCOMPARE(desk.panel(), QString());
            // `sprawl new` says the same, and makes nothing; of a board's own
            // name, that there is a board called that.
            qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
            const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
            QCOMPARE(runCli({"sprawl", "new", "plans"}), 1);
            QCOMPARE(desk.library.create("plans"), plans + " is there already and is not a board.");
            QCOMPARE(desk.library.create("notes"), notes + " is there already and is not a board.");
            QCOMPARE(desk.library.create("work"), QString("There is already a board called work."));
            QCOMPARE(desk.library.rename("work", "notes"), notes + " is there already and is not a board.");
            QVERIFY(!QFile::exists(desk.library.boardFile("plans")));
            QCOMPARE(read(notes), QByteArray("not a board\n"));
            QCOMPARE(desk.warnings, QStringList());
        }
        // The board made in place of the last one, moved to the trash, is an
        // untitled one, and when it cannot be called that, that is said.
        Desk desk("trip", [](const QString &home) { QDir(home).mkpath("boards/untitled"); });
        QVERIFY(desk.ready());
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_Delete, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QVERIFY(!desk.library.exists("trip"));
        QCOMPARE(desk.backend.boardName(), QString("untitled-2"));
        QCOMPARE(desk.backend.problem(), desk.library.boardFolder("untitled") +
                                             " is there already and is not a board, so the new board is untitled-2.");
        // So is a new board given no name at all.
        desk.backend.dismissProblem();
        desk.key(Qt::Key_N, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("untitled-3"));
        QCOMPARE(desk.backend.problem(), desk.library.boardFolder("untitled") +
                                             " is there already and is not a board, so the new board is untitled-3.");
        QCOMPARE(desk.warnings, QStringList());
    }

    void aStrokeThatCouldNotBeWrittenBackIsNotRead() {
        // A file may give a stroke's points as they are on the board, or from
        // an "x" and "y" of its own. They are kept, and written, from the
        // corner of the box around them, which has to be where a file may say
        // a box is, and no larger than one may be: read otherwise, the board
        // would be saved as one that could not be read back.
        auto file = [](const QString &stroke) {
            return QJsonDocument::fromJson(
                       QString(R"({"sprawl": 1, "items": [{"id": 1, "type": "stroke", %1}]})").arg(stroke).toUtf8())
                .object();
        };
        // As far apart and as far out as can be: read, and written as it is
        // read.
        for (const QString &stroke : QStringList{R"("points": [-1e9, -1e9, 0, 0])",
                                                  R"("x": 1e9, "y": -1e9, "points": [0, 0, 1e9, 1e9])",
                                                  R"("x": -5e8, "points": [-5e8, 0, 5e8, 10])"}) {
            Board board;
            QCOMPARE(board.fromJson(file(stroke)), QStringList());
            Board again;
            QCOMPARE(again.fromJson(board.toJson()), QStringList());
            QCOMPARE(again.item(1)->rect, board.item(1)->rect);
            QCOMPARE(again.item(1)->points, board.item(1)->points);
        }
        const QString wider = "Item 1: the box around \"points\" must be at most 1e+09 across and down.",
                      further = "Item 1: the box around \"points\", moved by \"x\" and \"y\", must start between "
                                "-1e+09 and 1e+09.";
        const QList<QPair<QString, QString>> refused{
            {R"("points": [-9e8, 0, 9e8, 10])", wider},
            {R"("points": [0, -6e8, 10, 6e8])", wider},
            {R"("x": 9e8, "points": [5e8, 0, 500000010, 10])", further},
            {R"("y": -9e8, "points": [0, -5e8, 10, -5e8])", further},
            {R"("points": [0, 0, 2e9, 10])", "Item 1: \"points\" must be between -1e+09 and 1e+09."},
            {R"("points": [0, 0, "far", 10])", "Item 1: \"points\" must hold only numbers."}};
        for (const auto &[stroke, problem] : refused) {
            Board board;
            QCOMPARE(board.fromJson(file(stroke)), QStringList{problem});
        }
        // A board holding one is said to be one that cannot be read: by the
        // window, which opens it read only, and by the commands, which write
        // nothing to it.
        const QByteArray wide =
            R"({"sprawl": 1, "items": [{"id": 1, "type": "stroke", "points": [-9e8, 0, 9e8, 10]}]})";
        Desk desk("wide", [&wide](const QString &home) {
            QDir(home).mkpath("boards/wide");
            write(home + "/boards/wide/board.json", wide);
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.readOnly());
        QVERIFY2(desk.backend.problem().contains(wider), qPrintable(desk.backend.problem()));
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        QCOMPARE(runCli({"sprawl", "check", "wide"}), 1);
        QCOMPARE(runCli({"sprawl", "add", "wide", "note", "--x", "0", "--y", "0"}), 1);
        QCOMPARE(read(desk.library.boardFile("wide")), wide);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aChangeFromOutsideComesInThroughAStepTakenBack() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Mine"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // What each item says, on disk and on the board.
        auto written = [&desk] {
            QStringList texts;
            for (const QJsonValue &value : desk.onDisk().value("items").toArray())
                texts << value.toObject().value("text").toString();
            return texts;
        };
        auto shown = [&desk] {
            QStringList texts;
            for (quint32 each : desk.board().order())
                texts << desk.board().item(each)->text;
            return texts;
        };
        auto agentAdds = [&desk](const QString &text, int x) {
            qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
            const int code =
                runCli({"sprawl", "add", "desk", "note", "--text", text, "--x", QString::number(x), "--y", "400"});
            qunsetenv("SPRAWL_HOME");
            return code == 0;
        };

        // An agent adds a note while a drag is under way here, and the
        // window has taken it in when Esc takes the drag back: the board is
        // then as it was before the drag, but for the note, which stays and
        // is written with the rest.
        desk.press(QPointF(150, 150));
        desk.moveTo(QPointF(200, 200));
        desk.moveTo(QPointF(300, 320));
        QVERIFY(desk.canvas->busy());
        QVERIFY(agentAdds("While dragged", 100));
        QTRY_COMPARE(desk.board().count(), 2);
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(100, 100));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"Mine", "While dragged"}));
        QCOMPARE(desk.onDisk().value("items").toArray().first().toObject().value("y").toDouble(), 100.0);
        desk.release(QPointF(300, 320));

        // The same with Esc before the window has looked.
        desk.press(QPointF(150, 150));
        desk.moveTo(QPointF(200, 200));
        desk.moveTo(QPointF(300, 320));
        QVERIFY(agentAdds("Before it looked", 400));
        desk.key(Qt::Key_Escape);
        desk.release(QPointF(300, 320));
        QTRY_COMPARE(desk.board().count(), 3);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), shown());

        // And with text just begun, nothing in it yet, which Esc takes back.
        desk.doubleClick(QPointF(700, 100));
        QVERIFY(desk.canvas->editing());
        QVERIFY(agentAdds("While written", 700));
        QTRY_COMPARE(desk.board().count(), 5);
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().count(), 4);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), shown());
        // What is done here next is saved with all of it.
        desk.add(note(1000, 100, "Next"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"Mine", "While dragged", "Before it looked", "While written", "Next"}));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardOpenInTwoWindowsKeepsWhatIsDoneInEither() {
        // The board opened in a second window too, as `sprawl open desk
        // --theme gruvbox` opens it beside the window it is open in, and
        // worked on in both before either has saved.
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 shared = desk.add(note(0, 0, "Shared"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        Backend other(&desk.library, nullptr, &desk.theme);
        other.openInitial("desk");
        Board &there = *other.board();
        QCOMPARE(there.count(), 1);
        // What each item says, sorted: on a board, and on disk.
        auto texts = [](const QVector<Item> &items) {
            QStringList found;
            for (const Item &item : items)
                found << item.text;
            found.sort();
            return found;
        };
        auto written = [&desk] {
            Board read;
            return read.fromJson(desk.onDisk()).isEmpty() ? read.items() : QVector<Item>();
        };
        // A note added in each, which each gives the same id, and the shared
        // note dragged in the second, the drag still under way.
        desk.add(note(300, 0, "Here"));
        there.add(note(600, 0, "There"));
        there.begin();
        Item dragged = *there.item(shared);
        dragged.rect.moveTo(0, 300);
        there.set(dragged);
        // Each saves what the other wrote, taken in, with its own.
        const QStringList all{"Here", "Shared", "There"};
        QTRY_COMPARE(texts(written()), all);
        QTRY_COMPARE(texts(desk.board().items()), all);
        QTRY_COMPARE(texts(there.items()), all);
        QTRY_COMPARE(desk.board().item(shared)->rect.topLeft(), QPointF(0, 300));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QTRY_COMPARE(other.saveState(), QString("saved"));
        // The drag taken back there is taken back here as well, and nothing
        // else is.
        there.rollback();
        QTRY_COMPARE(desk.board().item(shared)->rect.topLeft(), QPointF(0, 0));
        QTRY_COMPARE(other.saveState(), QString("saved"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(texts(written()), all);
        QCOMPARE(texts(desk.board().items()), all);
        QCOMPARE(texts(there.items()), all);
        QCOMPARE(written().first().rect.topLeft(), QPointF(0, 0));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aCommandChangesTheBoardAsItIsAtTheLastMoment() {
        // An agent writes the board whole, as the skill tells it to, renamed
        // over the file with no lock, just as a command reads it. The
        // command does what it does to what the agent wrote, and writes
        // nothing over it. The board is handed to the command through a
        // pipe, so that the agent goes just then: as the command reads.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        const Library library(home.path());
        QCOMPARE(Library(home.path()).create("race"), QString());
        const QString file = library.boardFile("race");
        // What each item on a board says, as its file has them.
        auto written = [](const QString &path) {
            QStringList texts;
            for (const QJsonValue &value : QJsonDocument::fromJson(read(path)).object().value("items").toArray())
                texts << value.toObject().value("text").toString();
            return texts;
        };
        // Written beside the file and renamed over it, as an agent writes a
        // board: the board as it was, and a note of its own added.
        auto renamedOver = [](const QString &path, const QByteArray &board, const QString &note = {}) {
            QJsonObject root = QJsonDocument::fromJson(board).object();
            QJsonArray items = root.value("items").toArray();
            if (!note.isEmpty())
                items.append(QJsonObject{{"type", "note"}, {"x", 600}, {"y", 0}, {"text", note}});
            root.insert("items", items);
            QFile beside(path + ".agent");
            if (!beside.open(QIODevice::WriteOnly))
                return;
            beside.write(QJsonDocument(root).toJson());
            beside.close();
            std::rename(qPrintable(path + ".agent"), qPrintable(path));
        };
        // The command, run with the board handed to it through a pipe, and
        // `agent` done with what the board was, once the command is reading.
        auto racing = [&file](const QStringList &command, const std::function<void(const QByteArray &)> &agent) {
            const QByteArray board = read(file);
            if (!QFile::remove(file) || mkfifo(QFile::encodeName(file).constData(), 0600) != 0)
                return -1;
            QThread *handing = QThread::create([&] {
                QFile pipe(file);
                if (!pipe.open(QIODevice::WriteOnly))
                    return;
                agent(board);
                pipe.write(board);
            });
            handing->start();
            const int code = runCli(QStringList{"sprawl"} + command);
            // Had the command not read the board, the pipe is let go of all
            // the same.
            QFile letGo(file);
            (void)letGo.open(QIODevice::ReadOnly);
            handing->wait();
            delete handing;
            return code;
        };

        // An item added is added to what the agent wrote.
        QCOMPARE(racing({"add", "race", "note", "--text", "The command's"},
                        [&](const QByteArray &board) { renamedOver(file, board, "The agent's"); }),
                 0);
        QCOMPARE(written(file), (QStringList{"The agent's", "The command's"}));
        // And one taken away is taken away from it.
        QCOMPARE(racing({"remove", "race", "1"},
                        [&](const QByteArray &board) { renamedOver(file, board, "The agent's too"); }),
                 0);
        QCOMPARE(written(file), (QStringList{"The command's", "The agent's too"}));
        // A board renamed meanwhile, as the window renames one, is not made
        // again where it was: the command says it is not there, and the
        // board is as it was, by its new name.
        const QString renamed = library.boardFile("renamed");
        QCOMPARE(racing({"add", "race", "note", "--text", "Nowhere"},
                        [&](const QByteArray &board) {
                            QDir().rename(library.boardFolder("race"), library.boardFolder("renamed"));
                            renamedOver(renamed, board);
                        }),
                 1);
        QVERIFY(!QFile::exists(library.boardFolder("race")));
        QCOMPARE(written(renamed), (QStringList{"The command's", "The agent's too"}));
    }

    void aConnectorAskedToBeSomewhereIsNotAddedUnsaid() {
        // A connector goes from one item to another, and is where they are:
        // asked to be somewhere, or of a size, it is not added with that
        // left out unsaid. What to do instead is said.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        const Library library(home.path());
        QCOMPARE(runCli({"sprawl", "new", "plan"}), 0);
        QCOMPARE(runCli({"sprawl", "add", "plan", "note", "--text", "From"}), 0);
        QCOMPARE(runCli({"sprawl", "add", "plan", "note", "--text", "To"}), 0);
        const QByteArray before = read(library.boardFile("plan"));
        for (const char *option : {"--x", "--y", "--width", "--height"})
            QVERIFY2(runCli({"sprawl", "add", "plan", "connector", "--from", "1", "--to", "2", option, "500"}) == 1,
                     option);
        QCOMPARE(read(library.boardFile("plan")), before);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SPRAWL_HOME", home.path());
        QProcess command;
        command.setProcessEnvironment(environment);
        command.start(QCoreApplication::applicationFilePath(),
                      {"--cli", "add", "plan", "connector", "--from", "1", "--to", "2", "--x", "5000"});
        QVERIFY(command.waitForFinished(30000));
        QCOMPARE(command.exitCode(), 1);
        const QByteArray said = command.readAllStandardError();
        QVERIFY2(said.contains("--x is not for a connector, which goes from one item to another: place those items "
                               "instead."),
                 said.constData());
        // Without, it is added.
        QCOMPARE(runCli({"sprawl", "add", "plan", "connector", "--from", "1", "--to", "2"}), 0);
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("plan"), root), QString());
        QCOMPARE(root.value("items").toArray().size(), 3);
    }

    void aPictureOfNoSizeIsNotCopiedIntoTheBoard() {
        Desk desk;
        QVERIFY(desk.ready());
        // No width at all, as an SVG can say: nothing to show, and no
        // proportions to keep. It is refused, as a file that is no picture
        // is, and nothing is left in the board's pictures, shown by nothing.
        const QString blank = desk.home.filePath("blank.svg");
        write(blank, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"10\"/>");
        desk.backend.addFiles({QUrl::fromLocalFile(blank)});
        QCOMPARE(desk.backend.problem(), QString("blank.svg is not a picture Sprawl can show."));
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(QDir(desk.backend.assetFolder()).entryList(QDir::Files), QStringList());
        // One with a size is added as ever.
        desk.backend.dismissProblem();
        desk.backend.addFiles({QUrl::fromLocalFile(picture(desk.home.filePath("photo.png"), QSize(80, 60)))});
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.all(Item::Kind::Image).size(), 1);
        QCOMPARE(QDir(desk.backend.assetFolder()).entryList(QDir::Files).size(), 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void anExportAsksNothingWhenThereIsNothingToExport() {
        // Named without its ending, an export asks before it writes over a
        // file that has the name: but not when there is nothing to export,
        // and a yes would come to nothing.
        Desk desk;
        QVERIFY(desk.ready());
        const QString plan = desk.home.filePath("plan.png");
        const QByteArray before = "the picture that was here before";
        write(plan, before);
        const QString nothing = "There is nothing on the board to export.";
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("plan"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.problem(), nothing);
        QCOMPARE(read(plan), before);
        // Nor when what was asked for is gone by the time the dialog
        // answers, taken away meanwhile from outside.
        desk.backend.dismissProblem();
        const quint32 asked = desk.add(note(100, 100, "Export me"));
        desk.add(note(500, 400, "Not me"));
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{asked});
        desk.picker.target = QUrl();
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.picker.asked, 2);
        desk.board().remove(asked);
        emit desk.picker.exportSelected(QUrl::fromLocalFile(desk.home.filePath("plan")), "png");
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.problem(), nothing);
        QCOMPARE(read(plan), before);
        QCOMPARE(desk.warnings, QStringList());
    }

    void anExportIsOfTheBoardItWasAskedFor() {
        // The save dialog leaves the window free to be used until it answers.
        // A board opened meanwhile is not exported in the place of the one
        // asked for, under its name: nothing is, and that is said.
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "Desk"));
        QCOMPARE(desk.library.create("other"), QString());
        auto exportPng = [&desk] {
            desk.key(Qt::Key_E, Qt::ControlModifier);
            desk.key(Qt::Key_Return);
        };
        exportPng();
        QCOMPARE(desk.picker.suggested.fileName(), QString("desk.png"));
        QVERIFY(desk.backend.openBoard("other"));
        desk.add(note(0, 0, "Other"));
        const QString named = desk.home.filePath("desk.png");
        emit desk.picker.exportSelected(QUrl::fromLocalFile(named), "png");
        QVERIFY(!QFile::exists(named));
        QCOMPARE(desk.backend.problem(),
                 QString("Nothing was exported: another board was opened before the export was given a name."));
        QCOMPARE(desk.backend.notice(), QString());
        // Renamed meanwhile, it is the board asked for still, and exported.
        desk.backend.dismissProblem();
        exportPng();
        QVERIFY(desk.backend.renameBoard("renamed"));
        emit desk.picker.exportSelected(QUrl::fromLocalFile(desk.home.filePath("other.png")), "png");
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(QImage(desk.home.filePath("other.png")).size(), QSize(2 * 216, 2 * 216));
        QCOMPARE(desk.warnings, QStringList());
    }

    void noIdIsGivenThatAFileCannotSay() {
        // A file may say it gave ids as far as the most an id can be, as one
        // written by hand or by an agent can. What is numbered after that,
        // here, by a command, or as a file is read, has the first id nothing
        // has, and the board is one that can be read back.
        auto file = [](const QByteArray &json) { return QJsonDocument::fromJson(json).object(); };
        auto readBack = [](const Board &board) {
            Board again;
            return again.fromJson(board.toJson());
        };
        Board saidLast;
        QCOMPARE(saidLast.fromJson(file(R"({"sprawl": 1, "lastId": 4000000000, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0}]})")),
                 QStringList());
        QCOMPARE(saidLast.add(note(0, 300)), quint32(2));
        QCOMPARE(readBack(saidLast), QStringList());
        Board farthest;
        QCOMPARE(farthest.fromJson(file(R"({"sprawl": 1, "items": [
            {"id": 4000000000, "type": "note", "x": 0, "y": 0}, {"type": "note", "x": 0, "y": 300}]})")),
                 QStringList());
        QCOMPARE(farthest.order(), (QVector<quint32>{4000000000u, 1}));
        const QVector<Item> base = farthest.items();
        QCOMPARE(farthest.add(note(0, 600, "Mine")), quint32(2));
        QCOMPARE(readBack(farthest), QStringList());
        // Made here, and given the id the file then gives an item of its own,
        // it is given another the same way.
        QVector<Item> theirs = base;
        Item added = note(300, 0, "Theirs");
        added.id = 2;
        theirs.append(added);
        QCOMPARE(farthest.merge(base, theirs, 4000000000u).taken, 1);
        QCOMPARE(farthest.order(), (QVector<quint32>{4000000000u, 1, 3, 2}));
        QCOMPARE(farthest.item(3)->text, QString("Mine"));
        QCOMPARE(farthest.item(2)->text, QString("Theirs"));
        QCOMPARE(readBack(farthest), QStringList());

        // A command adds to such a board, and leaves one that opens.
        QTemporaryDir home;
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        const Library library(home.path());
        QVERIFY(QDir(home.path()).mkpath("boards/far"));
        write(library.boardFile("far"), R"({"sprawl": 1, "lastId": 4000000000, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0}]})");
        QCOMPARE(runCli({"sprawl", "add", "far", "note", "--text", "Next"}), 0);
        QCOMPARE(runCli({"sprawl", "check", "far"}), 0);
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("far"), root), QString());
        Board written;
        QCOMPARE(written.fromJson(root), QStringList());
        QCOMPARE(written.item(2)->text, QString("Next"));
    }

    // --- Theme ---------------------------------------------------------------

    void themeFallsBackToTokyoNight() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.background(), QColor("#1a1b26"));
        QCOMPARE(theme.accent(), QColor("#7aa2f7"));
        QCOMPARE(theme.swatch("yellow"), QColor("#e0af68"));
        QCOMPARE(theme.radius(), 0);
        QVERIFY(theme.dark());
        QCOMPARE(theme.size("body"), 12);
        QCOMPARE(theme.size("heading"), 16);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#fafafa\"\nforeground = '#222222'\n"
              "yellow = \"#aa8800\" # a comment\n");
        write(dir.path() + "/theme/shell.toml",
              "[font]\nbase-size = 15\n[menu]\nborder = \"hyprland.active-border\"\n[hyprland]\nactive-border = "
              "\"rgba(aabbccff) rgba(112233ff) 45deg\"\n[controls]\nselected-fill-alpha = 0.5\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.swatch("yellow"), QColor("#aa8800"));
        QVERIFY(!theme.dark());
        QCOMPARE(theme.ink(), QColor("#222222"));
        QCOMPARE(theme.size("body"), 15);
        QCOMPARE(theme.size("heading"), 20);
        QCOMPARE(theme.menuBorder(), QColor("#aabbcc"));
        QVERIFY(qAbs(theme.selectedFill().alphaF() - 0.5) < 0.01);
    }

    void themeFollowsASwitch() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QSignalSpy changed(&theme, &Theme::changed);
        // A switch replaces the file rather than writing into it.
        QFile::remove(dir.path() + "/theme/colors.toml");
        write(dir.path() + "/theme/colors.toml", "accent = \"#445566\"\n");
        QTRY_COMPARE(theme.accent(), QColor("#445566"));
        QVERIFY(changed.count() >= 1);
    }

    void notePaperTakesDarkInk() {
        const Palette palette = Palette::fallback();
        for (const QString &name : Theme::swatchOrder()) {
            const QColor paper = palette.note(name);
            const double contrast = (Theme::luminance(paper) + .05) / (Theme::luminance(palette.ink) + .05);
            QVERIFY2(contrast >= 4.5, qPrintable(name + " paper is too dark to write on"));
            QVERIFY(Theme::luminance(paper) <= .29);
        }
        // Cyan is calm enough already, and stays the theme's own.
        QCOMPARE(palette.note("cyan"), QColor("#449dab"));
        // Light themes get pastels.
        const QColor pastel = Theme::notePaper(QColor("#aa8800"), QColor("#fafafa"), false);
        QVERIFY(Theme::luminance(pastel) >= .61);
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_VERIFY_WITH_TIMEOUT(theme.radius() == 6, 6000);
    }

    // --- Following the theme ------------------------------------------------

    void theColorsToPickFromFollowASwitch() {
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/colors.toml", "red = \"#f7768e\"\n");
        });
        QVERIFY(desk.ready());
        // Every color offered is one the theme has, as itself and as paper,
        // though its file names only red: no chip is ever left without one.
        for (const QString &name : desk.theme.swatchNames())
            QVERIFY2(desk.theme.swatches().contains(name) && desk.theme.notes().contains(name), qPrintable(name));
        auto colorOf = [&desk](const QString &name) {
            const QQuickItem *found = desk.item(name);
            return found ? found->property("color").value<QColor>() : QColor();
        };
        // As Omarchy switches: the file is replaced, not written into.
        auto switchTo = [&desk](const QByteArray &colors) {
            QFile::remove(desk.home.filePath("theme/colors.toml"));
            write(desk.home.filePath("theme/colors.toml"), colors);
        };
        const quint32 box = desk.add(shape(Item::Shape::Rect, 400, 100));
        const quint32 paper = desk.add(note(100, 100));

        // A shape is offered the theme's own colors...
        desk.click(QPointF(450, 130));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{box});
        QVERIFY(desk.shows("color-red"));
        QCOMPARE(colorOf("color-red"), QColor("#f7768e"));
        switchTo("red = \"#c62828\"\n");
        QTRY_COMPARE(desk.theme.swatch("red"), QColor("#c62828"));
        QCOMPARE(colorOf("color-red"), QColor("#c62828"));

        // ...a note, the paper made of them...
        desk.click(QPointF(176, 176));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{paper});
        const QColor before = desk.theme.note("red");
        QCOMPARE(colorOf("color-red"), before);
        switchTo("red = \"#f7768e\"\n");
        QTRY_COMPARE(desk.theme.swatch("red"), QColor("#f7768e"));
        QVERIFY(desk.theme.note("red") != before);
        QCOMPARE(colorOf("color-red"), desk.theme.note("red"));

        // ...and the pen, the colors themselves.
        desk.key(Qt::Key_P);
        QVERIFY(desk.shows("pen-red"));
        QCOMPARE(colorOf("pen-red"), QColor("#f7768e"));
        switchTo("red = \"#c62828\"\n");
        QTRY_COMPARE(desk.theme.swatch("red"), QColor("#c62828"));
        QCOMPARE(colorOf("pen-red"), QColor("#c62828"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStatusLineFollowsASwitch() {
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/colors.toml", "background = \"#1a1b26\"\nbright_foreground = \"#c0caf5\"\n"
                                               "magenta = \"#ad8ee6\"\nred = \"#f7768e\"\n");
        });
        QVERIFY(desk.ready());
        auto colorOf = [&desk](const QString &name) {
            const QQuickItem *found = desk.item(name);
            return found ? found->property("color").value<QColor>() : QColor();
        };
        auto switchTo = [&desk](const QByteArray &colors) {
            QFile::remove(desk.home.filePath("theme/colors.toml"));
            write(desk.home.filePath("theme/colors.toml"), colors);
        };

        // The pen in red: its dot beside its width, and the pen's badge, a
        // pale magenta written on in the canvas's own dark.
        desk.key(Qt::Key_P);
        QVERIFY(desk.clickOn("pen-red"));
        QCOMPARE(desk.canvas->penColor(), QString("red"));
        QVERIFY(desk.shows("pen-color"));
        QCOMPARE(colorOf("pen-color"), QColor("#f7768e"));
        QCOMPARE(colorOf("mode"), QColor("#ad8ee6"));
        QCOMPARE(colorOf("mode-name"), QColor("#1a1b26"));

        // The same magenta on another canvas: the badge stays, and what is
        // written on it is the new canvas's dark.
        switchTo("background = \"#0b0c10\"\nbright_foreground = \"#ffffff\"\n"
                 "magenta = \"#ad8ee6\"\nred = \"#c62828\"\n");
        QTRY_COMPARE(desk.theme.background(), QColor("#0b0c10"));
        QCOMPARE(colorOf("pen-color"), QColor("#c62828"));
        QCOMPARE(colorOf("mode"), QColor("#ad8ee6"));
        QCOMPARE(colorOf("mode-name"), QColor("#0b0c10"));
        QCOMPARE(colorOf("zoom"), QColor("#0b0c10"));

        // A deep magenta on a light canvas is written on in its light.
        switchTo("background = \"#fafafa\"\nforeground = \"#222222\"\nmagenta = \"#6a1b9a\"\n");
        QTRY_COMPARE(desk.theme.background(), QColor("#fafafa"));
        QCOMPARE(colorOf("mode"), QColor("#6a1b9a"));
        QCOMPARE(colorOf("mode-name"), QColor("#fafafa"));
        QCOMPARE(colorOf("zoom"), QColor("#fafafa"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void thePensDotIsTheColorItDrawsIn() {
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            // A bright that is not the foreground, so the two are told apart.
            write(home + "/theme/colors.toml", "foreground = \"#a9b1d6\"\nbright_foreground = \"#c0caf5\"\n");
        });
        QVERIFY(desk.ready());
        auto dot = [&desk] {
            const QQuickItem *found = desk.item("pen-color");
            return found ? found->property("color").value<QColor>() : QColor();
        };
        desk.key(Qt::Key_P);
        QVERIFY(desk.shows("pen-color"));
        // The pen's own ink is the theme's bright...
        QCOMPARE(desk.canvas->penColor(), QString());
        QCOMPARE(dot(), desk.theme.bright());
        // ...and so is a name the theme has no color for, as the board draws
        // a stroke of that name...
        desk.canvas->setPenColor("purple");
        QCOMPARE(dot(), desk.theme.bright());
        // ...while a swatch is the theme's own.
        desk.canvas->setPenColor("blue");
        QCOMPARE(dot(), desk.theme.swatch("blue"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void theTypeFollowsASwitch() {
        // The application's font is the theme's, as main() has it: from before
        // the window is made, and again after every change. A text takes it
        // only as it is made. The tests after this one get their own back.
        const auto putBack = qScopeGuard([font = QGuiApplication::font()] { QGuiApplication::setFont(font); });
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/shell.toml", "[font]\nbase-size = 12\n");
            QGuiApplication::setFont(Theme(home).font());
        });
        QVERIFY(desk.ready());
        QObject::connect(&desk.theme, &Theme::changed, &desk.theme,
                         [&desk] { QGuiApplication::setFont(desk.theme.font()); });
        // Every piece of text in the window, shown or not, and its size.
        auto sizes = [&desk] {
            QHash<QQuickItem *, int> found;
            QVector<QQuickItem *> items{desk.window->contentItem()};
            while (!items.isEmpty()) {
                QQuickItem *item = items.takeLast();
                if (item->inherits("QQuickText") || item->inherits("QQuickTextInput"))
                    found.insert(item, item->property("font").value<QFont>().pixelSize());
                items += item->childItems();
            }
            return found;
        };
        auto sizeOf = [&sizes](const QString &text) {
            const QHash<QQuickItem *, int> found = sizes();
            for (auto it = found.constBegin(); it != found.constEnd(); ++it)
                if (it.key()->property("text").toString() == text)
                    return it.value();
            return 0;
        };
        const QHash<QQuickItem *, int> before = sizes();
        QVERIFY(before.size() > 20);
        // A heading, small print, and everything else.
        QCOMPARE(sizeOf("Keys"), 16);
        QCOMPARE(sizeOf("? or Esc closes"), 11);
        QCOMPARE(sizeOf("SELECT"), 12);

        QFile::remove(desk.home.filePath("theme/shell.toml"));
        write(desk.home.filePath("theme/shell.toml"), "[font]\nbase-size = 15\n");
        QTRY_COMPARE(desk.theme.fontSize(), 15);
        QCOMPARE(QGuiApplication::font().pixelSize(), 15);
        QCOMPARE(sizeOf("Keys"), 20);
        QCOMPARE(sizeOf("? or Esc closes"), 14);
        QCOMPARE(sizeOf("SELECT"), 15);
        // Every size of the scale is another now, so every text is another size.
        const QHash<QQuickItem *, int> after = sizes();
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            QVERIFY(after.contains(it.key()));
            const QQmlContext *madeIn = qmlContext(it.key());
            QVERIFY2(after.value(it.key()) != it.value(),
                     qPrintable(QString("\"%1\" in %2 kept its size")
                                    .arg(it.key()->property("text").toString(),
                                         madeIn ? madeIn->baseUrl().fileName() : QString("the window"))));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void theRevisionCountsEveryChange() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "background = \"#1a1b26\"\n");
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        // Hyprland with sharp corners, no border and no animations.
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QTRY_COMPARE_WITH_TIMEOUT(theme.borderWidth(), 0, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(theme.reduceMotion(), 6000);
        auto switchTo = [&dir](const QByteArray &colors) {
            QFile::remove(dir.filePath("theme/colors.toml"));
            write(dir.filePath("theme/colors.toml"), colors);
        };
        // A badge handed the revision as a component would be, in a property
        // of its own, and asking for its ink with that.
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("theme", &theme);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nQtObject {\n    property int revision: theme.revision\n"
                          "    property color ink: theme.inkOn(\"#ffffff\", revision)\n}\n",
                          QUrl());
        const std::unique_ptr<QObject> badge(component.create());
        QVERIFY2(badge, qPrintable(component.errorString()));
        QCOMPARE(badge->property("ink").value<QColor>(), QColor("#1a1b26"));
        const int was = theme.revision();
        QSignalSpy changed(&theme, &Theme::changed);

        // A switch to another canvas is one more...
        switchTo("background = \"#0b0c10\"\n");
        QTRY_COMPARE(theme.background(), QColor("#0b0c10"));
        QCOMPARE(theme.revision(), was + changed.count());
        // ...and so another number, which is what a property holding it passes
        // on: the change of a value, never a signal alone.
        QCOMPARE(badge->property("revision").toInt(), theme.revision());
        QCOMPARE(badge->property("ink").value<QColor>(), QColor("#0b0c10"));

        // Hyprland's corners, border and motion count too, asked again with a
        // switch.
        write(response, "{\"int\":6}");
        switchTo("background = \"#1a1b26\"\n");
        QTRY_COMPARE(theme.background(), QColor("#1a1b26"));
        QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 6, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(theme.borderWidth(), 6, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(!theme.reduceMotion(), 6000);
        QCOMPARE(theme.revision(), was + changed.count());
        QCOMPARE(badge->property("revision").toInt(), theme.revision());
        QCOMPARE(badge->property("ink").value<QColor>(), QColor("#1a1b26"));
    }

    void everyThemeFunctionTheWindowCallsIsHandedTheRevision() {
        // A binding is evaluated again when something it read changes, and a
        // function's answer is not something it read. So the window reads the
        // theme's properties, and hands the revision to any function of the
        // theme it calls. Every file of it, as it is built in, and not only
        // the parts a test happens to open.
        static const QRegularExpression call("\\btheme\\.\\w+\\s*\\(");
        const QStringList files = QDir(":/").entryList({"*.qml", "*.js"}, QDir::Files);
        QVERIFY(files.contains("StatusLine.qml"));
        QStringList unfollowed;
        for (const QString &file : files) {
            const QString source = QString::fromUtf8(read(":/" + file));
            for (auto calls = call.globalMatch(source); calls.hasNext();) {
                const QRegularExpressionMatch found = calls.next();
                // The call, as far as the parenthesis that closes it.
                qsizetype end = found.capturedEnd();
                for (int depth = 1; end < source.size() && depth > 0; ++end)
                    depth += source[end] == '(' ? 1 : source[end] == ')' ? -1 : 0;
                const QString asked = source.sliced(found.capturedStart(), end - found.capturedStart());
                if (!asked.sliced(found.capturedLength()).contains("revision"))
                    unfollowed << QString("%1:%2 %3")
                                      .arg(file)
                                      .arg(source.first(found.capturedStart()).count('\n') + 1)
                                      .arg(asked);
            }
        }
        QVERIFY2(unfollowed.isEmpty(), qPrintable(unfollowed.join("; ")));
    }

    void thePersonsShellSettingsGoOverTheTheme() {
        // A theme that gives the shell 12, and `omarchy display text size 14`
        // over it, with a border and a color of the person's own as well.
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QDir(dir.path()).mkpath("omarchy");
        write(dir.filePath("theme/shell.toml"),
              "[font]\nbase-size = 12\n[menu]\nborder = \"hyprland.active-border\"\n[hyprland]\n"
              "active-border = \"#112233\"\n[controls]\nnormal-border-width = 2\nselected-fill-alpha = 0.5\n");
        write(dir.filePath("omarchy/shell.toml"),
              "[font]\nbase-size = 14\n[hyprland]\nactive-border = \"#aabbcc\"\n[controls]\nnormal-border-width = 0\n");
        Theme theme(dir.path(), nullptr, {}, dir.filePath("omarchy/shell.toml"));
        // Every key the person sets is theirs: the size, and the whole scale
        // made from it...
        QCOMPARE(theme.fontSize(), 14);
        QCOMPARE(theme.font().pixelSize(), 14);
        QCOMPARE(theme.size("body"), 14);
        QCOMPARE(theme.size("small"), 13);
        QCOMPARE(theme.size("heading"), 19);
        QCOMPARE(theme.controlBorderWidth(), 0);
        // ...even where a key of the theme's points at it.
        QCOMPARE(theme.menuBorder(), QColor("#aabbcc"));
        // Every other key stays the theme's.
        QVERIFY(qAbs(theme.selectedFill().alphaF() - 0.5) < 0.01);

        // With no settings of the person's, or none written yet, the theme's.
        for (const QString &own : {QString(), dir.filePath("nowhere/shell.toml")}) {
            Theme alone(dir.path(), nullptr, {}, own);
            QCOMPARE(alone.fontSize(), 12);
            QCOMPARE(alone.size("heading"), 16);
            QCOMPARE(alone.controlBorderWidth(), 2);
            QCOMPARE(alone.menuBorder(), QColor("#112233"));
        }
    }

    void thePersonsShellSettingsAreFollowedLive() {
        // The theme and the person's settings far apart, as in a home folder.
        QTemporaryDir home;
        const QString current = home.filePath(".local/state/omarchy/current");
        QDir(home.path()).mkpath(".local/state/omarchy/current/theme");
        write(current + "/theme/shell.toml", "[font]\nbase-size = 12\n");
        // Before a size is ever set, not even the folder is there.
        const QString own = home.filePath(".config/omarchy/shell.toml");
        Theme theme(current, nullptr, {}, own);
        QCOMPARE(theme.fontSize(), 12);

        // The first size makes the folder and the file...
        QVERIFY(QDir(home.path()).mkpath(".config/omarchy"));
        write(own, "[font]\nbase-size = 14\n");
        QTRY_COMPARE(theme.fontSize(), 14);
        // ...every one after puts a new file in its place, as `omarchy display
        // text size` does...
        QSaveFile next(own);
        QVERIFY(next.open(QIODevice::WriteOnly));
        next.write("[font]\nbase-size = 16\n");
        QVERIFY(next.commit());
        QTRY_COMPARE(theme.fontSize(), 16);
        // ...and an editor writes into it.
        write(own, "[font]\nbase-size = 18\n");
        QTRY_COMPARE(theme.fontSize(), 18);

        // A theme switch brings the theme's own keys, and the person's size
        // stays.
        QFile::remove(current + "/theme/shell.toml");
        write(current + "/theme/shell.toml", "[font]\nbase-size = 15\n[controls]\nnormal-border-width = 3\n");
        QTRY_COMPARE(theme.controlBorderWidth(), 3);
        QCOMPARE(theme.fontSize(), 18);

        // Taken away, the theme's size is back.
        QVERIFY(QFile::remove(own));
        QTRY_COMPARE(theme.fontSize(), 15);
    }

    void thePersonsShellSettingsAreWhereOmarchyKeepsThem() {
        // A home of the test's own, with the current theme where Omarchy keeps
        // it and the person's size where `omarchy display text size` keeps it;
        // and no Hyprland to ask.
        QTemporaryDir home;
        const QString current = home.filePath(".local/state/omarchy/current");
        QDir(home.path()).mkpath(".local/state/omarchy/current/theme");
        QDir(home.path()).mkpath(".config/omarchy");
        write(current + "/theme/shell.toml", "[font]\nbase-size = 12\n");
        write(home.filePath(".config/omarchy/shell.toml"), "[font]\nbase-size = 14\n");
        const QByteArray realHome = qgetenv("HOME"), hyprland = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
        const auto putBack = qScopeGuard([&] {
            qputenv("HOME", realHome);
            if (!hyprland.isEmpty())
                qputenv("HYPRLAND_INSTANCE_SIGNATURE", hyprland);
        });
        qputenv("HOME", home.path().toUtf8());
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");

        // The desktop's theme comes with the person's settings...
        const Theme desktop;
        QCOMPARE(Theme::userShellFile(), home.filePath(".config/omarchy/shell.toml"));
        QCOMPARE(desktop.fontSize(), 14);
        // ...a folder standing in for it, as a test's does, without them...
        const Theme stand(current);
        QCOMPARE(stand.fontSize(), 12);
        // ...unless they are handed over with it, as main() does with every
        // theme it shows.
        const Theme shown(current, nullptr, {}, Theme::userShellFile());
        QCOMPARE(shown.fontSize(), 14);
    }

    void theWindowIsTheSizeThePersonSet() {
        // `omarchy display text size 14` over a theme that gives the shell 12:
        // the shell and its bar are drawn at 14, and so is the window, with
        // the status line as tall as the bar.
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/shell.toml", "[font]\nbase-size = 12\n");
            write(home + "/shell.toml", "[font]\nbase-size = 14\n");
        });
        QVERIFY(desk.ready());
        auto sizeOf = [&desk](const QString &text) {
            QVector<QQuickItem *> found;
            Desk::texts(desk.window->contentItem(), found);
            for (const QQuickItem *item : std::as_const(found))
                if (item->property("text").toString() == text)
                    return item->property("font").value<QFont>().pixelSize();
            return 0;
        };
        // The mode's badge is as tall as the line it is in.
        const QQuickItem *badge = desk.item("mode");
        QVERIFY(badge);
        QCOMPARE(badge->height(), 30.0);
        QCOMPARE(sizeOf("SELECT"), 14);
        QCOMPARE(sizeOf("Keys"), 19);

        // With the person's size taken away, the theme's is back.
        QVERIFY(QFile::remove(desk.home.filePath("shell.toml")));
        QTRY_COMPARE(desk.theme.fontSize(), 12);
        QCOMPARE(badge->height(), 26.0);
        QCOMPARE(sizeOf("SELECT"), 12);
        QCOMPARE(sizeOf("Keys"), 16);

        // A bar of the person's own, taller than the theme's, at their size:
        // the line is as tall as the shell draws the bar.
        write(desk.home.filePath("shell.toml"), "[font]\nbase-size = 14\n[bar]\nsize-horizontal = 32\n");
        QTRY_COMPARE(desk.theme.barHeight(), 37);
        QCOMPARE(badge->height(), 37.0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void thePersonsShellSettingsAreReadAsTheShellReadsThem() {
        // The shell keeps only some of what a hand can write in the file, and
        // what it drops leaves the theme's value standing.
        QFETCH(QByteArray, own);
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/shell.toml"), "[font]\nbase-size = 16\n[controls]\nnormal-border-width = 2\n");
        write(dir.filePath("shell.toml"), own);
        const Theme theme(dir.path(), nullptr, {}, dir.filePath("shell.toml"));
        QTEST(theme.fontSize(), "size");
        QTEST(theme.controlBorderWidth(), "border");
    }
    void thePersonsShellSettingsAreReadAsTheShellReadsThem_data() {
        // What the shell's own Color.parseShell and Style.applyShellValues
        // make of each, over that theme.
        QTest::addColumn<QByteArray>("own");
        QTest::addColumn<int>("size");
        QTest::addColumn<int>("border");
        QTest::newRow("nothing") << QByteArray() << 16 << 2;
        QTest::newRow("a number") << QByteArray("[font]\nbase-size = 14\n") << 14 << 2;
        QTest::newRow("quoted") << QByteArray("[font]\nbase-size = \"14\"\n") << 14 << 2;
        QTest::newRow("with a comment") << QByteArray("[font] # mine\nbase-size = 14 # mine\n") << 14 << 2;
        QTest::newRow("Windows line ends") << QByteArray("[font]\r\nbase-size = 14\r\n") << 14 << 2;
        QTest::newRow("a byte order mark") << QByteArray("\xef\xbb\xbf[font]\nbase-size = 14\n") << 14 << 2;
        QTest::newRow("quoted, with a unit") << QByteArray("[font]\nbase-size = \"14px\"\n") << 14 << 2;
        QTest::newRow("quoted, with a plus") << QByteArray("[font]\nbase-size = \"+14\"\n") << 14 << 2;
        QTest::newRow("a fraction") << QByteArray("[font]\nbase-size = 13.6\n") << 13 << 2;
        QTest::newRow("two numbers") << QByteArray("[font]\nbase-size = 14 15\n") << 14 << 2;
        QTest::newRow("zero") << QByteArray("[font]\nbase-size = 0\n") << 1 << 2;
        // A word is kept, and is no size: the shell's own, not the theme's.
        QTest::newRow("a word") << QByteArray("[font]\nbase-size = big\n") << 12 << 2;
        QTest::newRow("with a unit") << QByteArray("[font]\nbase-size = 14px\n") << 16 << 2;
        QTest::newRow("empty") << QByteArray("[font]\nbase-size = \"\"\n") << 16 << 2;
        QTest::newRow("with a plus") << QByteArray("[font]\nbase-size = +14\n") << 16 << 2;
        QTest::newRow("before any section") << QByteArray("base-size = 14\n") << 16 << 2;
        // A header the shell does not read leaves its keys in the section
        // before.
        QTest::newRow("a dotted table") << QByteArray("[font]\nbase-size = 14\n[font.extra]\nbase-size = 15\n") << 15
                                        << 2;
        QTest::newRow("a table in brackets twice")
            << QByteArray("[font]\nbase-size = 14\n[[controls]]\nnormal-border-width = 0\n") << 14 << 2;
        // [style] is the shell's old name for [controls]...
        QTest::newRow("[style]") << QByteArray("[style]\nnormal-border-width = 0\n") << 16 << 0;
        // ...and of the two, the one later in the shell's dictionary counts,
        // where the theme's [controls] comes first.
        QTest::newRow("[style] and [controls]")
            << QByteArray("[style]\nnormal-border-width = 0\n[controls]\nnormal-border-width = 3\n") << 16 << 0;
        // A width for each side is no one width.
        QTest::newRow("widths") << QByteArray("[controls]\nnormal-border-width = 0 2\n") << 16 << 1;
    }

    void theTypeScaleAndTheBarAreTheShells() {
        QFETCH(QByteArray, shell);
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/shell.toml"), shell);
        const Theme theme(dir.path());
        const QList<int> sizes{theme.size("caption"), theme.size("small"),   theme.size("body"),
                               theme.size("subtitle"), theme.size("title"), theme.size("heading"),
                               theme.size("display")};
        QTEST(sizes, "sizes");
        QTEST(theme.barHeight(), "bar");
    }
    void theTypeScaleAndTheBarAreTheShells_data() {
        // As the shell's own Style.font and Style.bar have them: caption,
        // small, body, subtitle, title, heading and display, and the bar's
        // height.
        QTest::addColumn<QByteArray>("shell");
        QTest::addColumn<QList<int>>("sizes");
        QTest::addColumn<int>("bar");
        // The stops of the shell's text size slider, and two between them.
        QTest::newRow("9") << QByteArray("[font]\nbase-size = 9\n") << QList<int>{7, 8, 9, 10, 11, 12, 18} << 20;
        QTest::newRow("12") << QByteArray("[font]\nbase-size = 12\n") << QList<int>{10, 11, 12, 13, 14, 16, 24}
                            << 26;
        QTest::newRow("14") << QByteArray("[font]\nbase-size = 14\n") << QList<int>{12, 13, 14, 15, 16, 19, 28}
                            << 30;
        QTest::newRow("15") << QByteArray("[font]\nbase-size = 15\n") << QList<int>{12, 14, 15, 16, 18, 20, 30}
                            << 33;
        QTest::newRow("18") << QByteArray("[font]\nbase-size = 18\n") << QList<int>{15, 17, 18, 19, 21, 24, 36}
                            << 39;
        QTest::newRow("20") << QByteArray("[font]\nbase-size = 20\n") << QList<int>{17, 18, 20, 22, 23, 27, 40}
                            << 43;
        // One size pinned, as the shell reads it, and not pinned at all.
        QTest::newRow("a heading") << QByteArray("[font]\nbase-size = 14\nheading = 30\n")
                                   << QList<int>{12, 13, 14, 15, 16, 30, 28} << 30;
        QTest::newRow("a heading with a unit") << QByteArray("[font]\nbase-size = 14\nheading = \"20px\"\n")
                                               << QList<int>{12, 13, 14, 15, 16, 20, 28} << 30;
        QTest::newRow("a heading in part") << QByteArray("[font]\nbase-size = 14\nheading = 17.8\n")
                                           << QList<int>{12, 13, 14, 15, 16, 17, 28} << 30;
        QTest::newRow("no heading") << QByteArray("[font]\nbase-size = 14\nheading = 0\n")
                                    << QList<int>{12, 13, 14, 15, 16, 19, 28} << 30;
        // A bar of its own, which grows with the type unless told not to.
        QTest::newRow("a taller bar") << QByteArray("[font]\nbase-size = 14\n[bar]\nsize-horizontal = 32\n")
                                      << QList<int>{12, 13, 14, 15, 16, 19, 28} << 37;
        QTest::newRow("a bar that stays")
            << QByteArray("[font]\nbase-size = 14\n[bar]\nsize-horizontal = 32\nscale-with-font = false\n")
            << QList<int>{12, 13, 14, 15, 16, 19, 28} << 32;
        QTest::newRow("a bar that stays as it is")
            << QByteArray("[font]\nbase-size = 18\n[bar]\nscale-with-font = off\n")
            << QList<int>{15, 17, 18, 19, 21, 24, 36} << 26;
    }

    void aSizeSetWhileTheLastIsTakenInIsFollowed() {
        // The shell's text size slider runs `omarchy display text size` for
        // every stop it passes, so the next size can come while the last is
        // still being taken in: here, at the very moment the last is told.
        QFETCH(bool, replaced);
        // Where nothing else changes, as in a home folder: no other change
        // gets the window to read the files again.
        QTemporaryDir home;
        const QString current = home.filePath(".local/state/omarchy/current");
        QDir(home.path()).mkpath(".local/state/omarchy/current/theme");
        QDir(home.path()).mkpath(".config/omarchy");
        write(current + "/theme/shell.toml", "[font]\nbase-size = 12\n");
        const QString own = home.filePath(".config/omarchy/shell.toml");
        auto setSize = [&own, replaced](int size) {
            const QByteArray settings = "[font]\nbase-size = " + QByteArray::number(size) + "\n";
            if (replaced) {
                QSaveFile next(own);
                QVERIFY(next.open(QIODevice::WriteOnly));
                next.write(settings);
                QVERIFY(next.commit());
            } else {
                write(own, settings);
            }
        };
        setSize(13);
        Theme theme(current, nullptr, {}, own);
        QCOMPARE(theme.fontSize(), 13);
        bool told = false;
        QObject::connect(&theme, &Theme::changed, &theme, [&] {
            if (!std::exchange(told, true))
                setSize(16);
        });
        setSize(14);
        QTRY_VERIFY(told);
        QTRY_COMPARE(theme.fontSize(), 16);
    }
    void aSizeSetWhileTheLastIsTakenInIsFollowed_data() {
        QTest::addColumn<bool>("replaced");
        QTest::newRow("a new file in its place") << true;
        QTest::newRow("written into") << false;
    }

    void theCardsMakeRoomForTheType() {
        // At every size of the shell's slider, the cards keep their words to
        // themselves: no label runs under a key, and nothing off the card. In
        // the Omarchy font, as main() has it; the tests after this one get
        // their own back.
        const auto putBack = qScopeGuard([font = QGuiApplication::font()] { QGuiApplication::setFont(font); });
        Desk desk(
            QStringLiteral("desk"),
            [](const QString &home) {
                QDir(home).mkpath("theme");
                write(home + "/theme/shell.toml", "[font]\nbase-size = 12\n");
                QGuiApplication::setFont(Theme(home).font());
            },
            QSize(1600, 1000));
        QVERIFY(desk.ready());
        auto box = [](const QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        auto crowded = [&desk, &box](const QString &panel) {
            QMetaObject::invokeMethod(desk.window, "openPanel", Q_ARG(QVariant, panel));
            // Rows are laid out as a frame is drawn.
            desk.window->grabWindow();
            QQuickItem *card = desk.item(panel) ? Desk::find(desk.item(panel), "card") : nullptr;
            if (!card || !card->isVisible())
                return QStringList{panel + " is not open"};
            QStringList found;
            QVector<QQuickItem *> texts, caps, items{card};
            while (!items.isEmpty()) {
                QQuickItem *item = items.takeLast();
                if (QString(item->metaObject()->className()).startsWith("KeyCap"))
                    caps << item;
                else if (item->inherits("QQuickText") && item->isVisible())
                    texts << item;
                items += item->childItems();
            }
            for (const QQuickItem *text : std::as_const(texts)) {
                const QString words = text->property("text").toString();
                if (!box(card).contains(box(text)))
                    found << words + " runs off the card";
                for (const QQuickItem *cap : std::as_const(caps))
                    if (!cap->isAncestorOf(text) && box(cap).intersects(box(text)))
                        found << words + " runs under " + cap->property("label").toString();
            }
            QMetaObject::invokeMethod(desk.window, "closePanel");
            return found;
        };
        for (const int size : {9, 10, 11, 12, 14, 16, 20}) {
            write(desk.home.filePath("shell.toml"), "[font]\nbase-size = " + QByteArray::number(size) + "\n");
            QTRY_COMPARE(desk.theme.fontSize(), size);
            const QStringList found = crowded("keys") + crowded("export");
            QVERIFY2(found.isEmpty(), qPrintable(QString("At %1: %2").arg(size).arg(found.join("; "))));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aToolsKeyKeepsClearOfItsIcon() {
        // At every size `omarchy display text size` sets, 9 to 20, the key in
        // the corner of a tool on the rail is drawn beside the tool's icon,
        // with room between them, and within the tool. In the Omarchy font,
        // as main() has it; the tests after this one get their own back.
        const auto putBack = qScopeGuard([font = QGuiApplication::font()] { QGuiApplication::setFont(font); });
        Desk desk(QStringLiteral("desk"), [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/shell.toml", "[font]\nbase-size = 12\n");
            QGuiApplication::setFont(Theme(home).font());
        });
        QVERIFY(desk.ready());
        auto box = [](const QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QStringList tools{"select", "pan", "note", "text", "shape", "pen", "connector", "image", "frame"};
        for (int size = 9; size <= 20; ++size) {
            write(desk.home.filePath("shell.toml"), "[font]\nbase-size = " + QByteArray::number(size) + "\n");
            QTRY_COMPARE(desk.theme.fontSize(), size);
            desk.window->grabWindow();
            QStringList crowded;
            for (const QString &tool : tools) {
                const QQuickItem *button = desk.item("tool-" + tool);
                QVERIFY(button);
                const QQuickItem *icon = nullptr, *key = nullptr;
                const auto parts = button->childItems();
                for (const QQuickItem *part : parts) {
                    if (QString(part->metaObject()->className()).startsWith("Icon"))
                        icon = part;
                    else if (part->inherits("QQuickText") && !part->property("text").toString().isEmpty())
                        key = part;
                }
                QVERIFY(icon && key);
                // The letter where it is drawn, at the size it is drawn.
                const QRectF letter = key->mapRectToScene(key->boundingRect());
                if (letter.left() < box(icon).right() + 2)
                    crowded << key->property("text").toString() + " is up against its icon";
                if (!box(button).contains(letter))
                    crowded << key->property("text").toString() + " runs off its tool";
            }
            QVERIFY2(crowded.isEmpty(), qPrintable(QString("At %1: %2").arg(size).arg(crowded.join("; "))));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aKeysLettersStayInsideItsCap() {
        // At every size of the shell's slider, the letters of each key on the
        // cards are drawn inside the cap around them, clear of its border:
        // the p of Space as much as an S. In the Omarchy font, as main() has
        // it; the tests after this one get their own back.
        const auto putBack = qScopeGuard([font = QGuiApplication::font()] { QGuiApplication::setFont(font); });
        Desk desk(
            QStringLiteral("desk"),
            [](const QString &home) {
                QDir(home).mkpath("theme");
                write(home + "/theme/shell.toml", "[font]\nbase-size = 12\n");
                QGuiApplication::setFont(Theme(home).font());
            },
            QSize(1600, 1000));
        QVERIFY(desk.ready());
        auto box = [](const QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        auto spilled = [&desk, &box](const QString &panel) {
            QMetaObject::invokeMethod(desk.window, "openPanel", Q_ARG(QVariant, panel));
            // Rows are laid out as a frame is drawn.
            desk.window->grabWindow();
            QQuickItem *card = desk.item(panel) ? Desk::find(desk.item(panel), "card") : nullptr;
            if (!card || !card->isVisible())
                return QStringList{panel + " is not open"};
            QStringList found;
            int caps = 0;
            const qreal edge = desk.theme.controlBorderWidth();
            QVector<QQuickItem *> items{card};
            while (!items.isEmpty()) {
                QQuickItem *item = items.takeLast();
                items += item->childItems();
                if (!QString(item->metaObject()->className()).startsWith("KeyCap"))
                    continue;
                ++caps;
                const QRectF inside = box(item).adjusted(edge, edge, -edge, -edge);
                const auto parts = item->childItems();
                for (const QQuickItem *letters : parts)
                    if (letters->inherits("QQuickText") && !inside.contains(box(letters)))
                        found << item->property("label").toString() + " runs into its cap";
            }
            if (caps == 0)
                found << panel + " shows no keys";
            QMetaObject::invokeMethod(desk.window, "closePanel");
            return found;
        };
        for (const int size : {9, 10, 11, 12, 14, 16, 20}) {
            write(desk.home.filePath("shell.toml"), "[font]\nbase-size = " + QByteArray::number(size) + "\n");
            QTRY_COMPARE(desk.theme.fontSize(), size);
            const QStringList found = spilled("keys") + spilled("export");
            QVERIFY2(found.isEmpty(), qPrintable(QString("At %1: %2").arg(size).arg(found.join("; "))));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void thePersonsShellSettingsGoOverEveryThemeButOmarchysDefault() {
        // What `sprawl open` can be asked of its theme, read by a parser that
        // knows it, as main()'s does; and the person's settings somewhere of
        // the test's own.
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/shell.toml"), "[font]\nbase-size = 12\n");
        const QString own = dir.filePath("shell.toml");
        write(own, "[font]\nbase-size = 14\n");
        auto settingsFor = [&own](const QStringList &arguments) {
            QCommandLineParser args;
            args.addPositionalArgument("open", "Open the window", "open");
            args.addOption(QCommandLineOption("default-theme"));
            args.addOption(QCommandLineOption("theme", "theme", "theme"));
            return args.parse(QStringList{"sprawl", "open"} + arguments) ? userShellFor(args, own)
                                                                         : QString("not understood");
        };
        // The person's settings go over the desktop's theme, and over another
        // asked for by its name or by its folder, as the shell would show it
        // here...
        for (const QStringList &arguments : QList<QStringList>{{}, {"--theme", "nord"}, {"--theme", dir.path()}})
            QCOMPARE(settingsFor(arguments), own);
        QCOMPARE(Theme(dir.path(), nullptr, {}, settingsFor({"--theme", "nord"})).fontSize(), 14);
        // ...and none over Omarchy's default, which is shown as it ships, the
        // same on every machine, whatever else is asked.
        QCOMPARE(settingsFor({"--default-theme"}), QString());
        QCOMPARE(settingsFor({"--default-theme", "--theme", "nord"}), QString());
        QCOMPARE(Theme(dir.path(), nullptr, {}, settingsFor({"--default-theme"})).fontSize(), 12);
    }

    // --- The window ------------------------------------------------------------

    void theWindowOpensOnABoard() {
        Desk desk;
        QVERIFY(desk.ready());
        QCOMPARE(desk.warnings, QStringList());
        QCOMPARE(desk.backend.boardName(), QString("desk"));
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QCOMPARE(desk.window->title(), QString("desk · Sprawl"));
    }

    void spaceWritesANoteUnderThePointer() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        QVERIFY(desk.canvas->editing());
        desk.type("Call Sam\ntoday");
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->editing());

        QCOMPARE(desk.board().count(), 1);
        const Item *note = desk.last();
        QCOMPARE(note->kind, Item::Kind::Note);
        QCOMPARE(note->text, QString("Call Sam\ntoday"));
        QCOMPARE(note->rect, QRectF(224, 124, 152, 152));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{note->id});

        // Making it and writing on it was one thing to undo.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        desk.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("Call Sam\ntoday"));

        // Another, without moving the mouse, does not hide the first.
        desk.key(Qt::Key_Space);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->rect, QRectF(240, 140, 152, 152));
        QCOMPARE(desk.warnings, QStringList());
    }

    void draggingMovesWhatIsSelected() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Move me"));
        desk.click(QPointF(300, 200));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        // A click never nudges what it clicks.
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(224, 124));

        desk.drag(QPointF(300, 200), QPointF(350, 230));
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(274, 154));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(224, 124));

        // Escape in the middle of a drag puts the thing back and leaves
        // nothing to undo.
        desk.press(QPointF(300, 200));
        desk.moveTo(QPointF(340, 240));
        desk.moveTo(QPointF(380, 260));
        QVERIFY(desk.canvas->busy());
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(304, 184));
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->busy());
        desk.release(QPointF(380, 260));
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(224, 124));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void handlesResize() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124));
        desk.click(QPointF(300, 200));
        // The handle at the bottom right corner sits just clear of the note.
        desk.drag(QPointF(379, 279), QPointF(419, 309));
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 192, 182));
        // The one in the middle of the right side changes only the width.
        desk.drag(QPointF(419, 215), QPointF(399, 300));
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 172, 182));
        // With Shift a thing keeps its proportions.
        const qreal ratio = 172.0 / 182.0;
        desk.drag(QPointF(399, 309), QPointF(499, 329), Qt::ShiftModifier);
        const QRectF kept = desk.board().item(id)->rect;
        QCOMPARE(kept.topLeft(), QPointF(224, 124));
        QVERIFY(kept.width() > 172);
        QVERIFY(qAbs(kept.width() / kept.height() - ratio) < 0.01);
        // And nothing is dragged smaller than it can be, or inside out.
        desk.drag(kept.bottomRight() + QPointF(3, 3), QPointF(20, 20));
        const QRectF least = desk.board().item(id)->rect;
        QCOMPARE(least.topLeft(), QPointF(224, 124));
        QVERIFY(least.width() >= Defaults::minimumSize && least.width() < 40);
        QVERIFY(least.height() >= Defaults::minimumSize && least.height() < 40);
        // Each resize was a step of its own.
        for (int i = 0; i < 4; ++i)
            desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 152, 152));
    }

    void theMarqueeSelectsWhatItTouches() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100)), b = desk.add(note(400, 100)), c = desk.add(note(100, 400));
        auto selected = [&desk] {
            const QVector<quint32> ids = desk.canvas->selection();
            return QSet<quint32>(ids.begin(), ids.end());
        };
        desk.drag(QPointF(50, 50), QPointF(600, 300));
        QCOMPARE(selected(), (QSet<quint32>{a, b}));
        // Shift takes one away, and adds one.
        desk.click(QPointF(150, 150), Qt::ShiftModifier);
        QCOMPARE(selected(), (QSet<quint32>{b}));
        desk.click(QPointF(150, 450), Qt::ShiftModifier);
        QCOMPARE(selected(), (QSet<quint32>{b, c}));
        // A click on one of several picks just that one.
        desk.click(QPointF(450, 150));
        QCOMPARE(selected(), (QSet<quint32>{b}));
        // And a click on the empty board lets go of everything.
        desk.click(QPointF(800, 600));
        QCOMPARE(desk.canvas->selectionCount(), 0);

        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selected(), (QSet<quint32>{a, b, c}));
        desk.key(Qt::Key_Delete);
        QCOMPARE(desk.board().count(), 0);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 3);
    }

    void thePenDrawsStrokes() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        QCOMPARE(desk.canvas->tool(), QString("pen"));
        desk.press(QPointF(100, 400));
        for (int x = 110; x <= 300; x += 10)
            desk.moveTo(QPointF(x, 400 + qRound(20 * std::sin((x - 100) / 30.0))));
        desk.release(QPointF(300, 400 + qRound(20 * std::sin(200 / 30.0))));

        QCOMPARE(desk.board().count(), 1);
        const Item *stroke = desk.last();
        QCOMPARE(stroke->kind, Item::Kind::Stroke);
        QCOMPARE(stroke->width, 2.0);
        QVERIFY(stroke->points.size() >= 4);
        QCOMPARE(stroke->rect.left(), 100.0);
        QCOMPARE(stroke->rect.right(), 300.0);
        // Its points are kept from its own corner.
        QCOMPARE(stroke->points.first() + stroke->rect.topLeft(), QPointF(100, 400));

        // The pen stays in the hand: ] makes the next stroke thicker.
        QCOMPARE(desk.canvas->tool(), QString("pen"));
        desk.key(Qt::Key_BracketRight);
        QCOMPARE(desk.canvas->penWidth(), 4.0);
        desk.drag(QPointF(100, 500), QPointF(300, 500));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->width, 4.0);
        // A tap is a dot.
        desk.click(QPointF(500, 500));
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->points.size(), 1);

        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.canvas->tool(), QString("select"));
        // A stroke can be picked up by its line, and each was its own step.
        desk.click(QPointF(200, 500));
        QCOMPARE(desk.canvas->selectionKind(), QString("stroke"));
        for (int i = 0; i < 3; ++i)
            desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsJoinThingsAndFollowThem() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100)), b = desk.add(note(400, 100));
        desk.key(Qt::Key_C);
        desk.drag(QPointF(176, 176), QPointF(476, 176));
        QCOMPARE(desk.board().count(), 3);
        const quint32 joined = desk.last()->id;
        QCOMPARE(desk.board().item(joined)->kind, Item::Kind::Connector);
        QCOMPARE(desk.board().item(joined)->from, a);
        QCOMPARE(desk.board().item(joined)->to, b);
        // From the edge of one to the edge of the other.
        QCOMPARE(desk.board().item(joined)->p1, QPointF(252, 176));
        QCOMPARE(desk.board().item(joined)->p2, QPointF(400, 176));
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{joined});

        // Moving a thing takes its end of the connector along. The thing is
        // clicked below the bar by the selected connector, which covers the
        // top of it once a frame has been drawn.
        desk.click(QPointF(120, 220));
        desk.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(desk.board().item(a)->rect.top(), 110.0);
        QCOMPARE(desk.board().item(joined)->p1.x(), 252.0);
        QVERIFY(desk.board().item(joined)->p1.y() > 176 && desk.board().item(joined)->p1.y() < 186);
        // Deleting it takes the connector with it, and undo brings both back.
        desk.key(Qt::Key_Delete);
        QCOMPARE(desk.board().count(), 1);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.board().connectorsOf(a), QVector<quint32>{joined});

        // An arrow from nowhere to nowhere is a free one...
        desk.key(Qt::Key_C);
        desk.drag(QPointF(100, 500), QPointF(300, 560));
        const quint32 loose = desk.last()->id;
        QCOMPARE(desk.board().item(loose)->from, quint32(0));
        QCOMPARE(desk.board().item(loose)->to, quint32(0));
        QCOMPARE(desk.board().item(loose)->p1, QPointF(100, 500));
        QCOMPARE(desk.board().item(loose)->p2, QPointF(300, 560));
        // ...until an end of it is dropped on something.
        desk.drag(QPointF(300, 560), QPointF(450, 200));
        QCOMPARE(desk.board().item(loose)->to, b);
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsAreMadeAsTheyAreWanted() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100)), b = desk.add(note(500, 300));
        // The connector tool brings out what it is about to draw: nothing
        // at the start, straight, solid, an arrow at the end.
        QVERIFY(!desk.shows("draw-route"));
        desk.key(Qt::Key_C);
        desk.window->grabWindow();
        for (const char *kind : {"start", "route", "dash", "end"})
            QVERIFY2(desk.shows(QString("draw-") + kind), kind);
        QCOMPARE(desk.canvas->line().value("route").toString(), QString("straight"));
        QCOMPARE(desk.canvas->line().value("end").toString(), QString("arrow"));
        // C again is the next route, around and back.
        desk.key(Qt::Key_C);
        QCOMPARE(desk.canvas->line().value("route").toString(), QString("curved"));
        desk.key(Qt::Key_C);
        desk.key(Qt::Key_C);
        QCOMPARE(desk.canvas->line().value("route").toString(), QString("straight"));
        desk.key(Qt::Key_C);

        // Each of the four opens the choices there are of it; one is picked,
        // and they are put away.
        QVERIFY(!desk.shows("draw-end-dot"));
        QVERIFY(desk.clickOn("draw-end"));
        desk.window->grabWindow();
        for (const QString &head : Board::heads())
            QVERIFY2(desk.shows("draw-end-" + head), qPrintable(head));
        QVERIFY(desk.clickOn("draw-end-dot"));
        QCOMPARE(desk.canvas->line().value("end").toString(), QString("dot"));
        QVERIFY(!desk.shows("draw-end-dot"));
        QVERIFY(desk.clickOn("draw-start"));
        QVERIFY(desk.clickOn("draw-start-bar"));
        QVERIFY(desk.clickOn("draw-dash"));
        QVERIFY(desk.clickOn("draw-dash-dashed"));
        QCOMPARE(desk.canvas->tool(), QString("connector"));

        // What is drawn is as was asked: curved, from the side of one note
        // to the side of the other.
        desk.drag(QPointF(176, 176), QPointF(576, 376));
        QCOMPARE(desk.board().count(), 3);
        const quint32 id = desk.last()->id;
        const Item *made = desk.board().item(id);
        QCOMPARE(made->route, Item::Route::Curved);
        QCOMPARE(made->start, Item::Head::Bar);
        QCOMPARE(made->end, Item::Head::Dot);
        QCOMPARE(made->dash, Item::Dash::Dashed);
        QCOMPARE(made->from, a);
        QCOMPARE(made->to, b);
        QCOMPARE(made->p1, QPointF(252, 176));
        QCOMPARE(made->p2, QPointF(500, 376));
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QVERIFY(!desk.shows("draw-route"));

        // Selected, it shows what it is like, and each of the four can be
        // changed, each a step that undo takes back.
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        const QVariantMap like = desk.canvas->selectionLine();
        QCOMPARE(like.value("route").toString(), QString("curved"));
        QCOMPARE(like.value("start").toString(), QString("bar"));
        QCOMPARE(like.value("end").toString(), QString("dot"));
        QCOMPARE(like.value("dash").toString(), QString("dashed"));
        desk.window->grabWindow();
        QVERIFY(desk.shows("selected-route"));
        QVERIFY(desk.clickOn("selected-route"));
        QVERIFY(desk.clickOn("selected-route-elbow"));
        QCOMPARE(desk.board().item(id)->route, Item::Route::Elbow);
        QVERIFY(!desk.shows("selected-route-elbow"));
        QVERIFY(desk.clickOn("selected-start"));
        QVERIFY(desk.clickOn("selected-start-open-diamond"));
        QCOMPARE(desk.board().item(id)->start, Item::Head::OpenDiamond);
        QVERIFY(desk.clickOn("selected-end"));
        QVERIFY(desk.clickOn("selected-end-none"));
        QCOMPARE(desk.board().item(id)->end, Item::Head::None);
        QVERIFY(desk.clickOn("selected-dash"));
        QVERIFY(desk.clickOn("selected-dash-solid"));
        QCOMPARE(desk.board().item(id)->dash, Item::Dash::Solid);
        QCOMPARE(desk.canvas->selectionLine().value("start").toString(), QString("open-diamond"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->dash, Item::Dash::Dashed);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->end, Item::Head::Dot);
        QCOMPARE(desk.canvas->selectionLine().value("end").toString(), QString("dot"));
        // The next one drawn is like the last one changed.
        QCOMPARE(desk.canvas->line().value("route").toString(), QString("elbow"));

        // Several at once, and the bar says nothing of what they differ in.
        const quint32 other = desk.add(connector(b, a));
        desk.canvas->select({id, other});
        QCOMPARE(desk.canvas->selectionLine().value("route").toString(), QString());
        QCOMPARE(desk.canvas->selectionLine().value("dash").toString(), QString());
        desk.canvas->setSelectionLine("route", "curved");
        QCOMPARE(desk.board().item(id)->route, Item::Route::Curved);
        QCOMPARE(desk.board().item(other)->route, Item::Route::Curved);
        QCOMPARE(desk.canvas->selectionLine().value("route").toString(), QString("curved"));
        desk.canvas->setSelectionLine("route", "loopy");
        desk.canvas->setSelectionLine("shape", "star");
        QCOMPARE(desk.board().item(id)->route, Item::Route::Curved);

        // A note has no ends to choose.
        desk.key(Qt::Key_Escape);
        desk.click(QPointF(120, 120));
        QCOMPARE(desk.canvas->selectionKind(), QString("note"));
        desk.window->grabWindow();
        QVERIFY(!desk.shows("selected-route"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void shapesAreDrawnOrDropped() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_S);
        QCOMPARE(desk.canvas->tool(), QString("shape"));
        QCOMPARE(desk.canvas->shape(), QString("rect"));
        desk.drag(QPointF(400, 300), QPointF(560, 380));
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->kind, Item::Kind::Shape);
        QCOMPARE(desk.last()->rect, QRectF(400, 300, 160, 80));
        // Drawn, it is ready to be written on.
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.canvas->tool(), QString("select"));
        desk.type("Idea");
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.last()->text, QString("Idea"));

        // S again is the next shape, and a click drops one of the usual size.
        desk.key(Qt::Key_S);
        desk.key(Qt::Key_S);
        QCOMPARE(desk.canvas->shape(), QString("rounded"));
        desk.click(QPointF(500, 500));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->shape, Item::Shape::Rounded);
        QCOMPARE(desk.last()->rect, QRectF(424, 472, 152, 56));
        desk.key(Qt::Key_Escape);
        // A shape with nothing written on it is still a shape.
        QCOMPARE(desk.board().count(), 2);
        // Dragged backwards, it is the same box.
        desk.key(Qt::Key_S);
        desk.drag(QPointF(800, 200), QPointF(700, 120));
        QCOMPARE(desk.last()->rect, QRectF(700, 120, 100, 80));
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.warnings, QStringList());
    }

    void shapesArePickedFromAPopout() {
        Desk desk;
        QVERIFY(desk.ready());
        const QStringList shapes = desk.canvas->shapes();
        QCOMPARE(shapes.size(), Item::shapeCount);
        // Nothing to pick a shape from until the shape tool is held. Then
        // all of them are out beside it.
        QVERIFY(!desk.shows("draw-star"));
        desk.key(Qt::Key_S);
        desk.window->grabWindow();
        for (const QString &shape : shapes)
            QVERIFY2(desk.shows("draw-" + shape), qPrintable(shape));
        // One is clicked: it is what the tool draws, and they are put away.
        QVERIFY(desk.clickOn("draw-star"));
        QCOMPARE(desk.canvas->shape(), QString("star"));
        QCOMPARE(desk.canvas->tool(), QString("shape"));
        QVERIFY(!desk.shows("draw-star"));
        desk.click(QPointF(500, 300));
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 1);
        const quint32 id = desk.last()->id;
        QCOMPARE(desk.last()->shape, Item::Shape::Star);
        // Dropped at the size that suits it, not the size of a box.
        QCOMPARE(desk.last()->rect, QRectF(444, 246, 112, 108));

        // On the rail, the shape tool brings them out, and again puts them
        // away, leaving the shape as it is.
        QVERIFY(desk.clickOn("tool-shape"));
        QCOMPARE(desk.canvas->tool(), QString("shape"));
        QVERIFY(desk.shows("draw-star"));
        QVERIFY(desk.clickOn("tool-shape"));
        QVERIFY(!desk.shows("draw-star"));
        QCOMPARE(desk.canvas->tool(), QString("shape"));
        QCOMPARE(desk.canvas->shape(), QString("star"));

        // From the keys: S brings them out and then steps on, Shift+S steps
        // back, and the arrows move about the grid.
        desk.key(Qt::Key_S);
        desk.window->grabWindow();
        QVERIFY(desk.shows("draw-star"));
        QCOMPARE(desk.canvas->shape(), QString("star"));
        desk.key(Qt::Key_S);
        QCOMPARE(desk.canvas->shape(), QString("cross"));
        desk.key(Qt::Key_S, Qt::ShiftModifier);
        QCOMPARE(desk.canvas->shape(), QString("star"));
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.canvas->shape(), QString("cross"));
        desk.key(Qt::Key_Up);
        QCOMPARE(desk.canvas->shape(), shapes[shapes.indexOf("cross") - 5]);
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_Left);
        QCOMPARE(desk.canvas->shape(), QString("star"));
        // The view stays where it is: the arrows are the grid's for now.
        QCOMPARE(desk.canvas->origin(), QPointF(0, 0));
        // There is no row above the first, and the last is followed by the first.
        desk.canvas->setShape("pill");
        desk.key(Qt::Key_Up);
        QCOMPARE(desk.canvas->shape(), QString("pill"));
        desk.canvas->setShape(shapes.last());
        desk.key(Qt::Key_S);
        QCOMPARE(desk.canvas->shape(), shapes.first());
        // Enter is "this one": they are put away, and the tool is in hand.
        desk.canvas->setShape("hexagon");
        desk.key(Qt::Key_Return);
        QVERIFY(!desk.shows("draw-star"));
        QCOMPARE(desk.canvas->tool(), QString("shape"));
        // S brings them back out as they are, before it steps on again.
        desk.key(Qt::Key_S);
        QCOMPARE(desk.canvas->shape(), QString("hexagon"));
        desk.window->grabWindow();
        QVERIFY(desk.shows("draw-hexagon"));
        // Drawing puts them away too; and Escape puts the tool down.
        desk.drag(QPointF(600, 450), QPointF(760, 530));
        QCOMPARE(desk.last()->shape, Item::Shape::Hexagon);
        desk.key(Qt::Key_Escape);
        desk.key(Qt::Key_S);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QVERIFY(!desk.shows("draw-star"));
        desk.key(Qt::Key_Escape);

        // A shape that is on the board can be made another, from the bar by
        // the selection, in a step that undo takes back.
        desk.click(QPointF(500, 320));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        QCOMPARE(desk.canvas->selectionShape(), QString("star"));
        QVERIFY(!desk.shows("selected-octagon"));
        QVERIFY(desk.clickOn("selected-shape"));
        desk.window->grabWindow();
        for (const QString &shape : shapes)
            QVERIFY2(desk.shows("selected-" + shape), qPrintable(shape));
        QVERIFY(desk.clickOn("selected-octagon"));
        QCOMPARE(desk.board().item(id)->shape, Item::Shape::Octagon);
        QCOMPARE(desk.canvas->selectionShape(), QString("octagon"));
        QVERIFY(!desk.shows("selected-octagon"));
        // And it is what the tool draws next.
        QCOMPARE(desk.canvas->shape(), QString("octagon"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->shape, Item::Shape::Star);
        QCOMPARE(desk.canvas->selectionShape(), QString("star"));

        // Several at once; and its connectors meet the new outline.
        const quint32 other = desk.add(shape(Item::Shape::Rect, 800, 272, 152, 56));
        const quint32 line = desk.add(connector(id, other));
        desk.canvas->select({id, other});
        QCOMPARE(desk.canvas->selectionShape(), QString());
        desk.canvas->setSelectionShape("diamond");
        QCOMPARE(desk.board().item(id)->shape, Item::Shape::Diamond);
        QCOMPARE(desk.board().item(other)->shape, Item::Shape::Diamond);
        QCOMPARE(desk.board().item(line)->p1, QPointF(556, 300));
        QCOMPARE(desk.board().item(line)->p2, QPointF(800, 300));
        desk.canvas->setSelectionShape("blob");
        QCOMPARE(desk.board().item(id)->shape, Item::Shape::Diamond);

        // A note is not a shape, and is not offered any.
        desk.add(note(100, 450));
        desk.key(Qt::Key_Escape);
        desk.click(QPointF(150, 500));
        QCOMPARE(desk.canvas->selectionKind(), QString("note"));
        QVERIFY(!desk.shows("selected-shape"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void framesAreNamedAndCarryWhatIsInside() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_F);
        desk.drag(QPointF(50, 50), QPointF(450, 300));
        QCOMPARE(desk.board().count(), 1);
        const quint32 frame = desk.last()->id;
        QCOMPARE(desk.board().item(frame)->kind, Item::Kind::Frame);
        QCOMPARE(desk.board().item(frame)->rect, QRectF(50, 50, 400, 250));
        QVERIFY(desk.canvas->editing());
        desk.type("Later");
        // A title is one line, so Enter finishes it.
        desk.key(Qt::Key_Return);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(frame)->text, QString("Later"));

        const quint32 inside = desk.add(note(100, 100)), outside = desk.add(note(600, 100));
        // The middle of a frame is not the frame: things in it can be reached.
        desk.click(QPointF(300, 280));
        QCOMPARE(desk.canvas->selectionCount(), 0);
        // Taken by its edge, it carries what sits inside it.
        desk.drag(QPointF(50, 200), QPointF(80, 240));
        QCOMPARE(desk.board().item(frame)->rect.topLeft(), QPointF(80, 90));
        QCOMPARE(desk.board().item(inside)->rect.topLeft(), QPointF(130, 140));
        QCOMPARE(desk.board().item(outside)->rect.topLeft(), QPointF(600, 100));
        // All in one step.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(frame)->rect.topLeft(), QPointF(50, 50));
        QCOMPARE(desk.board().item(inside)->rect.topLeft(), QPointF(100, 100));
    }

    void textIsWrittenWhereItIsClicked() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_T);
        desk.click(QPointF(300, 300));
        QVERIFY(desk.canvas->editing());
        desk.type("hello");
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->kind, Item::Kind::Text);
        QCOMPARE(desk.last()->text, QString("hello"));
        QCOMPARE(desk.last()->rect.topLeft(), QPointF(300, 292));
        // As big as its type, and no bigger.
        Item measured = *desk.last();
        QVERIFY(!TextMetrics::fit(measured));
        QVERIFY(desk.last()->rect.width() > 20 && desk.last()->rect.height() >= 12);

        // Text with nothing in it leaves no trace, not even a step to undo.
        desk.key(Qt::Key_T);
        desk.click(QPointF(500, 500));
        QVERIFY(desk.canvas->editing());
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 1);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);

        // A double click on the empty board starts writing there.
        desk.doubleClick(QPointF(600, 300));
        QVERIFY(desk.canvas->editing());
        desk.type("x");
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("x"));
        // Selected, it takes bold and a bigger size.
        desk.key(Qt::Key_B, Qt::ControlModifier);
        QVERIFY(desk.last()->bold);
        const qreal height = desk.last()->rect.height();
        desk.key(Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(desk.last()->fontSize, 13.0);
        desk.key(Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(desk.last()->fontSize, 16.0);
        QVERIFY(desk.last()->rect.height() > height);
        desk.key(Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(desk.last()->fontSize, 13.0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aDoubleClickEditsWhatIsWritten() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Call"));
        desk.doubleClick(QPointF(300, 200));
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.canvas->editorText(), QString("Call"));
        desk.type(" Sam");
        // Clicking somewhere else is done, too.
        desk.click(QPointF(700, 500));
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam"));
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 152, 152));

        // Enter edits what is selected, and Q is a letter while it does.
        desk.click(QPointF(300, 200));
        desk.key(Qt::Key_Return);
        QVERIFY(desk.canvas->editing());
        desk.type(" quick");
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam quick"));

        // A note grows to hold what is written on it.
        desk.key(Qt::Key_Return);
        desk.type("\n1\n2\n3\n4\n5\n6\n7\n8\n9");
        desk.key(Qt::Key_Escape);
        QVERIFY(desk.board().item(id)->rect.height() > 152);
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(224, 124));
        // And all of that typing is one step back.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam quick"));
        QCOMPARE(desk.board().item(id)->rect.height(), 152.0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void copiesLandUnderThePointer() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "One"));
        desk.click(QPointF(150, 150));
        desk.key(Qt::Key_C, Qt::ControlModifier);
        // Anything else that takes text gets what was written.
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("One"));
        desk.hover(QPointF(600, 400));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->text, QString("One"));
        QCOMPARE(desk.last()->rect.center(), QPointF(600, 400));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{desk.last()->id});

        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->rect.center(), QPointF(616, 416));
        desk.key(Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 2);
        // Pasted where a copy already is, it steps aside rather than hide it,
        // and the next one steps aside again.
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->rect.center(), QPointF(616, 416));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 4);
        QCOMPARE(desk.last()->rect.center(), QPointF(632, 432));
        desk.key(Qt::Key_Z, Qt::ControlModifier);

        // Text from anywhere else becomes text on the board...
        QGuiApplication::clipboard()->setText("from a terminal");
        desk.hover(QPointF(300, 500));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 4);
        QCOMPARE(desk.last()->kind, Item::Kind::Text);
        QCOMPARE(desk.last()->text, QString("from a terminal"));
        // ...and a picture becomes a picture, kept with the board.
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(QColor("#9ece6a"));
        QGuiApplication::clipboard()->setImage(image);
        desk.hover(QPointF(700, 200));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 5);
        QCOMPARE(desk.last()->kind, Item::Kind::Image);
        QCOMPARE(desk.last()->rect, QRectF(668, 176, 64, 48));
        QVERIFY(QFile::exists(desk.backend.assetFolder() + "/" + desk.last()->asset));
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsAreCopiedWithOrWithoutWhatTheyJoin() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(shape(Item::Shape::Rect, 100, 100));
        const quint32 b = desk.add(shape(Item::Shape::Rect, 400, 100));
        const quint32 line = desk.add(connector(a, b));
        const QSizeF reach = desk.board().item(line)->rect.size();

        // Copied on its own, it is a line on nothing, where it is pasted.
        desk.canvas->select({line});
        desk.key(Qt::Key_C, Qt::ControlModifier);
        desk.hover(QPointF(300, 400));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.all(Item::Kind::Connector).size(), 2);
        QCOMPARE(desk.last()->from, quint32(0));
        QCOMPARE(desk.last()->to, quint32(0));
        QCOMPARE(desk.last()->rect.size(), reach);
        QVERIFY(QLineF(desk.last()->rect.center(), QPointF(300, 400)).length() < 2);
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{desk.last()->id});

        // Duplicated on its own, the same: a step aside from the one it was.
        desk.canvas->select({line});
        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.all(Item::Kind::Connector).size(), 3);
        QCOMPARE(desk.last()->from, quint32(0));
        QCOMPARE(desk.last()->to, quint32(0));
        QCOMPARE(desk.last()->rect.topLeft(), desk.board().item(line)->rect.topLeft() + QPointF(16, 16));

        // Duplicated with one of the things it joins, it joins the copy to
        // what the original is joined to.
        desk.canvas->select({a, line});
        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.all(Item::Kind::Shape).size(), 3);
        const quint32 copyOfA = desk.all(Item::Kind::Shape).last()->id;
        QCOMPARE(desk.last()->kind, Item::Kind::Connector);
        QCOMPARE(desk.last()->from, copyOfA);
        QCOMPARE(desk.last()->to, b);

        // Copied with both, it joins the copies, and nothing it was not
        // copied with.
        desk.canvas->select({a, b, line});
        desk.key(Qt::Key_C, Qt::ControlModifier);
        desk.hover(QPointF(500, 500));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        const auto shapes = desk.all(Item::Kind::Shape);
        QCOMPARE(shapes.size(), 5);
        QCOMPARE(desk.last()->kind, Item::Kind::Connector);
        QCOMPARE(desk.last()->from, shapes[3]->id);
        QCOMPARE(desk.last()->to, shapes[4]->id);

        // Copied with one of the two, it is pasted with that one, its other
        // end where it was and on nothing.
        desk.canvas->select({b, line});
        desk.key(Qt::Key_C, Qt::ControlModifier);
        desk.hover(QPointF(700, 300));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.all(Item::Kind::Shape).size(), 6);
        QCOMPARE(desk.last()->kind, Item::Kind::Connector);
        QCOMPARE(desk.last()->from, quint32(0));
        QCOMPARE(desk.last()->to, desk.all(Item::Kind::Shape).last()->id);

        // Cut and pasted back, the things it joined being gone by then, it
        // still joins what it came back with.
        const int connectors = int(desk.all(Item::Kind::Connector).size());
        desk.canvas->select({a, b, line});
        desk.key(Qt::Key_X, Qt::ControlModifier);
        QVERIFY(!desk.board().item(a) && !desk.board().item(line));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        const auto back = desk.all(Item::Kind::Shape);
        QCOMPARE(desk.last()->kind, Item::Kind::Connector);
        QCOMPARE(desk.last()->from, back[back.size() - 2]->id);
        QCOMPARE(desk.last()->to, back[back.size() - 1]->id);
        QVERIFY(desk.all(Item::Kind::Connector).size() <= connectors);
        // And follows them, as the first one did.
        const QPointF before = desk.last()->p1;
        Item moved = *back[back.size() - 2];
        moved.rect.translate(0, 120);
        desk.board().set(moved);
        QVERIFY(desk.last()->p1 != before);
        QVERIFY(moved.rect.adjusted(-1, -1, 1, 1).contains(desk.last()->p1));
        QCOMPARE(desk.warnings, QStringList());
    }

    void groupsMoveAsOne() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100)), b = desk.add(note(400, 100));
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.key(Qt::Key_G, Qt::ControlModifier);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.canvas->selectionCount(), 0);
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selectionCount(), 2);
        desk.drag(QPointF(150, 150), QPointF(170, 190));
        QCOMPARE(desk.board().item(a)->rect.topLeft(), QPointF(120, 140));
        QCOMPARE(desk.board().item(b)->rect.topLeft(), QPointF(420, 140));

        desk.key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        desk.key(Qt::Key_Escape);
        desk.click(QPointF(170, 190));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
    }

    void arrowsNudgeOrPan() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100));
        // With nothing selected they move the view.
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.canvas->origin(), QPointF(48, 0));
        desk.key(Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(desk.canvas->origin(), QPointF(48, -240));
        desk.canvas->setView(QPointF(0, 0), 1);

        desk.click(QPointF(150, 150));
        desk.key(Qt::Key_Right);
        desk.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(101, 110));
        QCOMPARE(desk.canvas->origin(), QPointF(0, 0));

        // ] and [ change what is in front.
        const quint32 other = desk.add(note(120, 120));
        QCOMPARE(desk.board().order(), (QVector<quint32>{id, other}));
        desk.key(Qt::Key_BracketRight);
        QCOMPARE(desk.board().order(), (QVector<quint32>{other, id}));
        desk.key(Qt::Key_BracketLeft);
        QCOMPARE(desk.board().order(), (QVector<quint32>{id, other}));
    }

    void zoomingKeepsItsPlace() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100));
        desk.add(note(1500, 900));
        auto near = [](qreal a, qreal b) { return qAbs(a - b) < 1e-6; };
        // What is under the mouse stays under the mouse.
        auto anchored = [&desk] {
            const QPointF under = desk.canvas->toCanvas(QPointF(400, 300));
            return qAbs(under.x() - 400) < 0.01 && qAbs(under.y() - 300) < 0.01;
        };
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_Plus);
        QTRY_VERIFY(near(desk.canvas->zoom(), 1.25));
        QVERIFY(anchored());
        desk.key(Qt::Key_Minus);
        desk.key(Qt::Key_Minus);
        QTRY_VERIFY(near(desk.canvas->zoom(), 0.75));
        QVERIFY(anchored());
        desk.key(Qt::Key_0);
        QTRY_VERIFY(near(desk.canvas->zoom(), 1));
        QVERIFY(anchored());

        // 1 shows the whole board, and never magnifies it.
        desk.key(Qt::Key_1);
        QTRY_VERIFY(desk.canvas->visibleArea().contains(desk.board().contentBounds()));
        QVERIFY(desk.canvas->zoom() < 1);

        desk.canvas->setView(QPointF(0, 0), 1);
        desk.wheel(QPointF(400, 300), QPoint(0, 120), Qt::ControlModifier);
        QVERIFY(near(desk.canvas->zoom(), 1.2));
        QVERIFY(anchored());
        // A plain wheel pans; with Shift it pans sideways.
        desk.canvas->setView(QPointF(0, 0), 1);
        desk.wheel(QPointF(400, 300), QPoint(0, -120));
        QCOMPARE(desk.canvas->origin(), QPointF(0, 48));
        desk.wheel(QPointF(400, 300), QPoint(0, -120), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->origin(), QPointF(48, 48));

        // There is an end to zooming, both ways.
        for (int i = 0; i < 40; ++i)
            desk.canvas->zoomBy(1.5, QPointF(100, 100));
        QCOMPARE(desk.canvas->zoom(), 8.0);
        for (int i = 0; i < 40; ++i)
            desk.canvas->zoomBy(0.5, QPointF(100, 100));
        QCOMPARE(desk.canvas->zoom(), 0.1);
    }

    void boardsAreMadeFoundRenamedAndTrashed() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "on desk"));
        desk.key(Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(desk.panel(), QString("newboard"));
        desk.type("Weekend Trip");
        // Typing a name is not using the board's keys.
        QCOMPARE(desk.canvas->tool(), QString("select"));
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("weekend-trip"));
        QCOMPARE(desk.board().count(), 0);
        // The board that was open went to disk before it was let go of.
        QJsonObject left;
        QCOMPARE(Library::read(desk.library.boardFile("desk"), left), QString());
        QCOMPARE(left.value("items").toArray().size(), 1);
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        desk.type("Pack");
        desk.key(Qt::Key_Escape);

        // Ctrl+O finds a board by a few letters of its name.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        QCOMPARE(desk.panel(), QString("boards"));
        desk.type("dsk");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("desk"));
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("on desk"));
        // A new board's history is its own: there is nothing of the other's to undo.
        QVERIFY(!desk.canvas->canUndo());

        // F2 renames the open board.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_F2);
        desk.type("Planning");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.backend.boardName(), QString("planning"));
        QVERIFY(desk.library.exists("planning"));
        QVERIFY(!desk.library.exists("desk"));
        QCOMPARE(desk.panel(), QString());

        // Ctrl+Delete asks first; Esc keeps the board.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_Delete, Qt::ControlModifier);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString("boards"));
        QCOMPARE(desk.library.boards().size(), 2);
        desk.key(Qt::Key_Delete, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.library.boards().size(), 1);
        QCOMPARE(desk.backend.boardName(), QString("weekend-trip"));
        QCOMPARE(desk.last()->text, QString("Pack"));
        QVERIFY(desk.backend.notice().contains("planning"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void exportsGoWhereThePickerSays() {
        Desk desk;
        QVERIFY(desk.ready());
        // Nothing to export is said, not written.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("empty.png"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QVERIFY(!QFile::exists(desk.home.filePath("empty.png")));
        QVERIFY(desk.backend.problem().contains("nothing"));
        desk.backend.dismissProblem();

        desk.add(note(100, 100, "Export me"));
        desk.add(note(500, 400));
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("out.png"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(desk.panel(), QString("export"));
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.picker.format, QString("png"));
        QCOMPARE(desk.picker.suggested.fileName(), QString("desk.png"));
        const QImage image(desk.home.filePath("out.png"));
        // Both notes and a margin around them, two pixels to the unit.
        QCOMPARE(image.size(), QSize(2 * (552 + 64), 2 * (452 + 64)));
        QCOMPARE(image.pixel(4, 4), QColor("#1a1b26").rgb());
        QCOMPARE(image.pixel(2 * (32 + 140), 2 * (32 + 140)), desk.theme.note("yellow").rgb());
        QVERIFY(desk.backend.notice().contains("out.png"));

        // S is an SVG, of only what is selected when something is.
        desk.click(QPointF(150, 150));
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("out.svg"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_S);
        QCOMPARE(desk.picker.format, QString("svg"));
        const QByteArray svg = read(desk.home.filePath("out.svg"));
        QVERIFY(svg.contains("<svg"));
        QVERIFY(svg.contains("viewBox=\"0 0 216 216\""));
        // S chose a format here; it did not pick up the shape tool.
        QCOMPARE(desk.canvas->tool(), QString("select"));

        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("out.pdf"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_D);
        QVERIFY(read(desk.home.filePath("out.pdf")).startsWith("%PDF"));

        // A name without its ending is given one, and a dialog that was
        // closed writes nothing.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("plain"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QVERIFY(QFile::exists(desk.home.filePath("plain.png")));
        desk.picker.target = QUrl();
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.picker.asked, 6);
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.warnings, QStringList());
    }

    void picturesComeInThroughThePicker() {
        Desk desk;
        QVERIFY(desk.ready());
        const QString photo = picture(desk.home.filePath("photo.png"), QSize(840, 420));
        const QByteArray original = read(photo);
        desk.picker.images = {QUrl::fromLocalFile(photo)};
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.picker.opened, 1);
        QCOMPARE(desk.board().count(), 1);
        const quint32 id = desk.last()->id;
        QCOMPARE(desk.board().item(id)->kind, Item::Kind::Image);
        // Brought down to a size to arrange, under the pointer.
        QCOMPARE(desk.board().item(id)->rect, QRectF(190, 195, 420, 210));
        const QString assets = desk.backend.assetFolder();
        QVERIFY(QFile::exists(assets + "/" + desk.board().item(id)->asset));
        // The board has a copy of its own; the original is only ever read.
        QCOMPARE(read(photo), original);

        // It is selected as it lands, and keeps its proportions however it
        // is resized.
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        const QRectF before = desk.board().item(id)->rect;
        desk.drag(before.bottomRight() + QPointF(3, 3), before.bottomRight() + QPointF(103, 3));
        const QRectF after = desk.board().item(id)->rect;
        QVERIFY(after.width() > before.width());
        QVERIFY(qAbs(after.width() / after.height() - 2.0) < 0.02);
        desk.key(Qt::Key_Z, Qt::ControlModifier);

        // The same picture twice is one file, and the second does not hide
        // the first.
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(QDir(assets).entryList(QDir::Files).size(), 1);
        QCOMPARE(desk.last()->rect, QRectF(206, 211, 420, 210));

        // What is not a picture is said, and not added.
        write(desk.home.filePath("notes.txt"), "not a picture");
        desk.picker.images = {QUrl::fromLocalFile(desk.home.filePath("notes.txt"))};
        desk.key(Qt::Key_I);
        QCOMPARE(desk.board().count(), 2);
        QVERIFY(desk.backend.problem().contains("notes.txt"));
        // And so is a dialog that could not be shown.
        desk.picker.failure = "The desktop has no file dialog.";
        desk.key(Qt::Key_I);
        QCOMPARE(desk.backend.problem(), QString("The desktop has no file dialog."));
        QCOMPARE(desk.warnings, QStringList());
    }

    void boardsSaveThemselves() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        desk.type("Saved");
        desk.key(Qt::Key_Escape);
        QVERIFY(desk.backend.saveState() != "saved");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 1);
        QCOMPARE(desk.onDisk().value("items").toArray().first().toObject().value("text").toString(), QString("Saved"));

        // Something else writes the file (an agent, `sprawl add`), and the
        // window follows.
        Board other;
        QCOMPARE(other.fromJson(desk.onDisk()), QStringList());
        other.add(note(500, 100, "From outside"));
        QCOMPARE(Library::write(desk.library.boardFile("desk"), other.toJson()), QString());
        QTRY_COMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->text, QString("From outside"));
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.backend.notice().contains("desk"));

        // A file that stops making sense is not taken: the board stays as it is.
        write(desk.library.boardFile("desk"), "{ this is not a board");
        QTRY_VERIFY(desk.backend.problem().contains("changed on disk"));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardThatCannotBeReadIsNeverWrittenOver() {
        const QByteArray broken = R"({"sprawl": 1, "items": [{"type": "sticker", "x": 0, "y": 0}]})";
        Desk desk("precious", [&broken](const QString &home) {
            QDir(home).mkpath("boards/precious");
            write(home + "/boards/precious/board.json", broken);
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.boardName(), QString("precious"));
        QVERIFY(desk.backend.problem().contains("precious"));
        QVERIFY(desk.backend.problem().contains("sticker"));
        // Whatever is done in the window, the file is as it was.
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        desk.type("Lost?");
        desk.key(Qt::Key_Escape);
        QTest::qWait(700);
        QVERIFY(desk.backend.flush());
        QCOMPARE(read(desk.library.boardFile("precious")), broken);
    }

    void theViewIsRememberedForEachBoard() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100));
        desk.canvas->setView(QPointF(320, 240), 2);
        QVERIFY(desk.backend.newBoard("other"));
        // A board not seen before opens whole, at actual size when it is empty.
        QCOMPARE(desk.canvas->zoom(), 1.0);
        QCOMPARE(desk.canvas->origin(), QPointF(0, 0));
        QVERIFY(desk.backend.openBoard("desk"));
        QCOMPARE(desk.canvas->origin(), QPointF(320, 240));
        QCOMPARE(desk.canvas->zoom(), 2.0);
    }

    void theLastBoardOpensNextTime() {
        QTemporaryDir home;
        {
            Library library(home.path());
            Theme theme(home.path());
            Backend backend(&library, nullptr, &theme);
            backend.openInitial({});
            // The very first time there is a welcome board, with things on it.
            QCOMPARE(backend.boardName(), QString("welcome"));
            QVERIFY(backend.board()->count() > 10);
            QVERIFY(backend.newBoard("second"));
            backend.rememberView(QPointF(12, 34), 0.5);
        }
        Library library(home.path());
        Theme theme(home.path());
        Backend backend(&library, nullptr, &theme);
        backend.openInitial({});
        QCOMPARE(backend.boardName(), QString("second"));
        QCOMPARE(backend.rememberedView().value("zoom").toDouble(), 0.5);
        QCOMPARE(backend.rememberedView().value("x").toDouble(), 12.0);
        // Asked for by name, a board is opened, or made when the name is free.
        backend.openInitial("welcome");
        QCOMPARE(backend.boardName(), QString("welcome"));
        backend.openInitial("brand-new");
        QCOMPARE(backend.boardName(), QString("brand-new"));
        QCOMPARE(library.boards().size(), 3);
    }

    void theKeysAreListedAndStayOutOfTheWay() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_Question, Qt::ShiftModifier);
        QCOMPARE(desk.panel(), QString("keys"));
        // While they are up, the board's own keys do nothing.
        desk.key(Qt::Key_P);
        desk.key(Qt::Key_Space);
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QCOMPARE(desk.board().count(), 0);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        desk.key(Qt::Key_Question, Qt::ShiftModifier);
        desk.key(Qt::Key_Question, Qt::ShiftModifier);
        QCOMPARE(desk.panel(), QString());
        // Back on the board, they work again.
        desk.key(Qt::Key_P);
        QCOMPARE(desk.canvas->tool(), QString("pen"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void quittingWaitsForTheBoardToBeSaved() {
        Desk desk;
        QVERIFY(desk.ready());
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        // While writing, Q is a letter.
        desk.type("Quiet");
        QCOMPARE(quit.count(), 0);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.last()->text, QString("Quiet"));
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 1);
        // It did not wait for the autosave: the board is on disk already.
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 1);
    }

    void quittingAsksWhenTheBoardWillNotSave() {
        Desk desk;
        QVERIFY(desk.ready());
        QSignalSpy quit(&desk.engine, &QQmlEngine::quit);
        const QString folder = desk.library.boardFolder("desk");
        desk.add(note(100, 100, "Not on disk"));
        // A folder that cannot be written to, as a full disk would be.
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        desk.key(Qt::Key_Q);
        QCOMPARE(quit.count(), 0);
        QCOMPARE(desk.panel(), QString("quit"));
        QCOMPARE(desk.backend.saveState(), QString("failed"));
        QVERIFY(!desk.backend.problem().isEmpty());
        // Esc goes back to the board, which is saved as soon as it can be.
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        desk.add(note(300, 100));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(desk.onDisk().value("items").toArray().size(), 2);

        // Enter quits anyway.
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        desk.add(note(500, 100));
        desk.key(Qt::Key_Q);
        QCOMPARE(desk.panel(), QString("quit"));
        desk.key(Qt::Key_Return);
        QCOMPARE(quit.count(), 1);
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void theWindowFollowsTheTheme() {
        Desk desk("desk", [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/colors.toml", "background = \"#1a1b26\"\naccent = \"#7aa2f7\"\n");
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.window->color(), QColor("#1a1b26"));
        // A theme switch replaces the file.
        QFile::remove(desk.home.filePath("theme/colors.toml"));
        write(desk.home.filePath("theme/colors.toml"),
              "mode = \"light\"\nbackground = \"#fafafa\"\nforeground = \"#222222\"\naccent = \"#aa3355\"\n");
        QTRY_COMPARE(desk.window->color(), QColor("#fafafa"));
        QCOMPARE(desk.theme.accent(), QColor("#aa3355"));
        QCOMPARE(desk.warnings, QStringList());
    }

    // --- Working on the canvas ----------------------------------------------

    void handlesHoldOnWhateverKeysArePressed() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124));
        desk.click(QPointF(300, 200));
        // A corner taken by its handle, and arrows pressed while it is held:
        // they move nothing, and the corner is still in hand.
        desk.press(QPointF(379, 279));
        desk.moveTo(QPointF(389, 289));
        desk.moveTo(QPointF(399, 299));
        QVERIFY(desk.canvas->busy());
        desk.key(Qt::Key_Right);
        desk.key(Qt::Key_Down, Qt::ShiftModifier);
        desk.moveTo(QPointF(409, 309));
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 182, 182));
        desk.release(QPointF(409, 309));
        // One step back, as any resize is.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect, QRectF(224, 124, 152, 152));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());

        // The same of a connector's end, with the arrow pressed before the
        // mouse has moved at all.
        const quint32 a = desk.add(note(100, 400)), b = desk.add(note(500, 400));
        const quint32 line = desk.add(connector(a, b));
        desk.canvas->select({line});
        const QPointF end = desk.board().item(line)->p2;
        desk.press(end);
        desk.key(Qt::Key_Up);
        desk.moveTo(end + QPointF(30, 100));
        desk.moveTo(end + QPointF(60, 150));
        QCOMPARE(desk.board().item(line)->p2, end + QPointF(60, 150));
        desk.release(end + QPointF(60, 150));
        QCOMPARE(desk.board().item(line)->to, quint32(0));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(line)->to, b);
        QCOMPARE(desk.board().item(line)->p2, end);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!desk.board().item(line));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aMoveIsOneStepWhateverKeysArePressed() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Move me"));
        // Pressed on, and Space before the mouse has moved: no note.
        desk.press(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        QVERIFY(!desk.canvas->editing());
        // And while it moves, the arrows, Space and Ctrl+D do nothing either.
        desk.moveTo(QPointF(320, 220));
        desk.moveTo(QPointF(340, 240));
        QVERIFY(desk.canvas->busy());
        desk.key(Qt::Key_Right);
        desk.key(Qt::Key_Up, Qt::ShiftModifier);
        desk.key(Qt::Key_Space);
        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 1);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(264, 164));
        // It follows the mouse all the way to where it is let go...
        desk.moveTo(QPointF(400, 300));
        desk.moveTo(QPointF(500, 400));
        desk.release(QPointF(500, 400));
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(424, 324));
        QCOMPARE(desk.board().count(), 1);
        // ...and all of it is one step back.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect.topLeft(), QPointF(224, 124));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());
        QCOMPARE(desk.warnings, QStringList());
    }

    void nothingElseChangesTheBoardWhileTheMouseIsDown() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100, "a"));
        desk.add(note(400, 100, "b"));
        desk.click(QPointF(150, 150));
        desk.key(Qt::Key_C, Qt::ControlModifier);
        const QJsonObject before = desk.board().toJson();
        // The mouse down on what is selected, not yet moved: whatever else
        // asks the canvas for a change meanwhile is refused...
        desk.press(QPointF(150, 150));
        QVERIFY(desk.canvas->pressed());
        QVERIFY(!desk.canvas->busy());
        desk.canvas->nudge(10, 10);
        desk.canvas->newNote();
        desk.canvas->insertText("words");
        QVERIFY(desk.canvas->paste());
        desk.canvas->duplicateSelection();
        desk.canvas->setSelectionColor("blue");
        desk.canvas->bringToFront();
        desk.canvas->editSelection();
        desk.canvas->undo();
        desk.canvas->deleteSelection();
        desk.canvas->setTool("note");
        QCOMPARE(desk.board().toJson(), before);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.canvas->tool(), QString("select"));
        // ...so the drag goes on as it began, and is one step.
        desk.moveTo(QPointF(170, 170));
        desk.moveTo(QPointF(190, 190));
        desk.canvas->undo();
        desk.canvas->redo();
        desk.release(QPointF(190, 190));
        QVERIFY(!desk.canvas->pressed());
        QCOMPARE(desk.board().item(a)->rect.topLeft(), QPointF(140, 140));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().toJson(), before);

        // A stroke of the pen too: asked for a note halfway, it goes on, and
        // lands whole.
        desk.key(Qt::Key_P);
        desk.press(QPointF(100, 500));
        desk.moveTo(QPointF(200, 520));
        desk.canvas->newNote();
        desk.canvas->undo();
        desk.moveTo(QPointF(300, 500));
        desk.release(QPointF(300, 500));
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->kind, Item::Kind::Stroke);
        QCOMPARE(desk.last()->rect.left(), 100.0);
        QCOMPARE(desk.last()->rect.right(), 300.0);

        // And keys that would only move the view, or take up a tool, wait
        // for the mouse to let go.
        desk.key(Qt::Key_Escape);
        desk.press(QPointF(700, 600));
        desk.key(Qt::Key_Right);
        desk.key(Qt::Key_H);
        QCOMPARE(desk.canvas->origin(), QPointF(0, 0));
        QCOMPARE(desk.canvas->tool(), QString("select"));
        desk.release(QPointF(700, 600));
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.canvas->origin(), QPointF(48, 0));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardThatCouldNotBeReadIsNotChanged() {
        Desk desk("desk", [](const QString &home) {
            QDir(home).mkpath("boards/precious");
            write(home + "/boards/precious/board.json", R"({"sprawl": 1, "items": [{"type": "sticker", "x": 0, "y": 0}]})");
        });
        QVERIFY(desk.ready());
        const QString photo = picture(desk.home.filePath("photo.png"), QSize(80, 60));

        // The pen in hand as a board that could not be read is opened: it is
        // put down, and the tools that make things are not to be had.
        desk.key(Qt::Key_P);
        QVERIFY(desk.backend.openBoard("precious"));
        QVERIFY(desk.canvas->readOnly());
        QCOMPARE(desk.canvas->tool(), QString("select"));
        for (const char *tool : {"note", "text", "shape", "pen", "connector", "image", "frame"})
            QVERIFY2(!desk.item(QString("tool-") + tool)->isEnabled(), tool);
        QVERIFY(desk.item("tool-select")->isEnabled() && desk.item("tool-pan")->isEnabled());

        // What it shows (put there by hand here: such a board is shown
        // empty) stays as it is, whatever is done to it.
        const quint32 a = desk.add(note(100, 100, "a"));
        const quint32 b = desk.add(shape(Item::Shape::Rect, 400, 100));
        desk.add(connector(a, b));
        Item words;
        words.kind = Item::Kind::Text;
        words.fontSize = Defaults::textSize;
        words.text = "words";
        words.rect = QRectF(100, 400, 2, 16);
        TextMetrics::fit(words);
        desk.add(words);
        desk.window->grabWindow();
        QTRY_COMPARE(desk.canvas->pendingItems(), 0);
        const QJsonObject before = desk.board().toJson();
        auto unchanged = [&desk, &before] { return desk.board().toJson() == before; };

        // No tool is taken up, by its key or on the rail, so a click makes
        // nothing; and no picture is asked for.
        desk.picker.images = {QUrl::fromLocalFile(photo)};
        for (Qt::Key key : {Qt::Key_N, Qt::Key_T, Qt::Key_S, Qt::Key_P, Qt::Key_C, Qt::Key_F, Qt::Key_I}) {
            desk.key(key);
            QCOMPARE(desk.canvas->tool(), QString("select"));
        }
        QVERIFY(desk.clickOn("tool-note"));
        QVERIFY(desk.clickOn("tool-image"));
        desk.canvas->setTool("pen");
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QCOMPARE(desk.picker.opened, 0);
        desk.click(QPointF(700, 500));
        QVERIFY(unchanged());

        // Nothing is written: not by Space, Enter, or a double click on
        // something or on the empty board.
        desk.hover(QPointF(300, 500));
        desk.key(Qt::Key_Space);
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        desk.key(Qt::Key_Return);
        desk.doubleClick(QPointF(150, 150));
        desk.doubleClick(QPointF(700, 500));
        QVERIFY(!desk.canvas->editing());
        QVERIFY(unchanged());

        // Nothing is moved or resized by the mouse: what is pressed on is
        // picked, and stays where it is; and there are no handles.
        desk.drag(QPointF(150, 150), QPointF(300, 350));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        desk.click(QPointF(450, 130));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{b});
        // Nor offered a bar of changes by the selection.
        desk.window->grabWindow();
        QVERIFY(!desk.shows("selected-shape"));
        desk.drag(QPointF(503, 163), QPointF(560, 220));
        QVERIFY(unchanged());

        // What is selected is not deleted, cut, duplicated, grouped,
        // restacked, made bold or bigger, or moved by the arrows; and no
        // step is undone.
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), 4);
        const QList<QPair<Qt::Key, Qt::KeyboardModifiers>> keys{
            {Qt::Key_Delete, {}},
            {Qt::Key_Backspace, {}},
            {Qt::Key_X, Qt::ControlModifier},
            {Qt::Key_D, Qt::ControlModifier},
            {Qt::Key_G, Qt::ControlModifier},
            {Qt::Key_BracketRight, {}},
            {Qt::Key_BracketLeft, {}},
            {Qt::Key_B, Qt::ControlModifier},
            {Qt::Key_Plus, Qt::ControlModifier},
            {Qt::Key_Right, {}},
            {Qt::Key_Down, Qt::ShiftModifier},
            {Qt::Key_Z, Qt::ControlModifier},
            {Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier}};
        for (const auto &[key, modifiers] : keys) {
            desk.key(key, modifiers);
            QVERIFY2(unchanged(), qPrintable(QKeySequence(QKeyCombination(modifiers, key)).toString()));
        }
        QCOMPARE(desk.canvas->selectionCount(), 4);

        // Nothing is pasted: neither what was copied from it (copying is
        // only looking), nor text or a picture from anywhere else.
        desk.key(Qt::Key_C, Qt::ControlModifier);
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasFormat("application/x-sprawl-items"));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QGuiApplication::clipboard()->setText("from a terminal");
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QGuiApplication::clipboard()->setImage(QImage(photo));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        // Nor is anything dropped on it: the window does not take it.
        QVERIFY(!desk.drop(QPointF(600, 400), photo));
        QVERIFY(unchanged());
        QVERIFY(QDir(desk.backend.assetFolder()).entryList(QDir::Files).isEmpty());

        // And the canvas refuses all of it when asked directly, too.
        desk.canvas->newNote();
        desk.canvas->insertText("words");
        emit desk.backend.imageAdded("photo.png", QSize(80, 60), 0);
        emit desk.backend.textPasted("words");
        QVERIFY(desk.canvas->paste());
        desk.canvas->nudge(10, 10);
        desk.canvas->duplicateSelection();
        desk.canvas->groupSelection();
        desk.canvas->sendToBack();
        desk.canvas->setSelectionColor("blue");
        desk.canvas->setSelectionShape("star");
        desk.canvas->setSelectionLine("route", "curved");
        desk.canvas->toggleBold();
        desk.canvas->resizeText(1);
        desk.canvas->undo();
        desk.canvas->cut();
        desk.canvas->deleteSelection();
        desk.canvas->select({a});
        desk.canvas->editSelection();
        QVERIFY(!desk.canvas->editing());
        QVERIFY(unchanged());

        // A board that can be read is changed as ever, by the same drop and
        // the same keys.
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(!desk.canvas->readOnly());
        QVERIFY(desk.drop(QPointF(600, 400), photo));
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->kind, Item::Kind::Image);
        desk.key(Qt::Key_N);
        QCOMPARE(desk.canvas->tool(), QString("note"));
        QVERIFY(desk.item("tool-note")->isEnabled());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardThatCouldNotBeReadIsStillLookedAt() {
        Desk desk("precious", [](const QString &home) {
            QDir(home).mkpath("boards/precious");
            write(home + "/boards/precious/board.json", R"({"sprawl": 1, "items": [{"type": "sticker", "x": 0, "y": 0}]})");
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.canvas->readOnly());
        const quint32 a = desk.add(note(100, 100, "a")), b = desk.add(note(400, 100, "b"));
        const QJsonObject before = desk.board().toJson();

        // Things are picked by a click, with Shift, by a box, and all at once.
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        desk.click(QPointF(450, 150), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->selection(), (QVector<quint32>{a, b}));
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        // The box drawn up from below, clear of what the window may show over
        // the top of the board.
        desk.drag(QPointF(350, 290), QPointF(600, 60));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{b});
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), 2);
        // And copied.
        desk.key(Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("a\n\nb"));

        // The view is moved about: by the arrows, even with something
        // selected; by the pan tool; and zoomed by the wheel and the keys.
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.canvas->origin(), QPointF(48, 0));
        desk.key(Qt::Key_H);
        QCOMPARE(desk.canvas->tool(), QString("pan"));
        const QPoint from = desk.at(QPointF(500, 400));
        QTest::mousePress(desk.window, Qt::LeftButton, {}, from);
        QTest::mouseMove(desk.window, from - QPoint(50, 50));
        QTest::mouseMove(desk.window, from - QPoint(100, 100));
        QTest::mouseRelease(desk.window, Qt::LeftButton, {}, from - QPoint(100, 100));
        QCOMPARE(desk.canvas->origin(), QPointF(148, 100));
        desk.key(Qt::Key_V);
        QCOMPARE(desk.canvas->tool(), QString("select"));
        desk.wheel(QPointF(400, 300), QPoint(0, 120), Qt::ControlModifier);
        QVERIFY(qAbs(desk.canvas->zoom() - 1.2) < 1e-6);
        desk.key(Qt::Key_0);
        QTRY_VERIFY(qAbs(desk.canvas->zoom() - 1) < 1e-6);

        // And it is exported.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("looked.png"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QVERIFY(!QImage(desk.home.filePath("looked.png")).isNull());
        QCOMPARE(desk.board().toJson(), before);
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingIsOneStepWhateverElseHappens() {
        Desk desk;
        QVERIFY(desk.ready());
        const QString photo = picture(desk.home.filePath("photo.png"), QSize(80, 60));
        desk.hover(QPointF(300, 200));
        desk.key(Qt::Key_Space);
        desk.type("Pack");
        // A color, or ungrouping, asked for meanwhile is not done: the
        // writing goes on.
        desk.canvas->setSelectionColor("blue");
        desk.canvas->ungroupSelection();
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.last()->color, QString("yellow"));
        desk.type(" up");
        // A picture dropped on the board meanwhile, as a drop hands it over:
        // the writing is finished, and the picture is where it was dropped.
        desk.canvas->pointTo(desk.canvas->toItem(QPointF(700, 400)));
        desk.backend.addFiles({QUrl::fromLocalFile(photo)});
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->kind, Item::Kind::Image);
        QCOMPARE(desk.last()->rect.center(), QPointF(700, 400));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{desk.last()->id});
        // One step back takes the picture, and the next the note with all
        // that was written on it.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->text, QString("Pack up"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());
        QCOMPARE(desk.warnings, QStringList());
    }

    void theSelectionIsLeftAloneWhileTheMouseIsDown() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 a = desk.add(note(100, 100, "a")), b = desk.add(note(400, 100, "b"));
        desk.click(QPointF(150, 150));
        // Pressed on what is selected, not yet moved: everything is selected
        // meanwhile, then nothing, and still it carries just what it was
        // pressed on; and the same once it moves.
        desk.press(QPointF(150, 150));
        desk.canvas->selectAll();
        desk.canvas->clearSelection();
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        desk.moveTo(QPointF(170, 170));
        desk.moveTo(QPointF(190, 190));
        desk.canvas->selectAll();
        desk.canvas->clearSelection();
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});
        desk.release(QPointF(190, 190));
        QCOMPARE(desk.board().item(a)->rect.topLeft(), QPointF(140, 140));
        QCOMPARE(desk.board().item(b)->rect.topLeft(), QPointF(400, 100));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{a});

        // Everything selected mid-stroke would take up the select tool, and
        // put down the stroke: the pen stays in hand, and the stroke lands
        // whole. A frame being drawn, the same.
        desk.key(Qt::Key_P);
        desk.press(QPointF(100, 500));
        desk.moveTo(QPointF(200, 520));
        desk.canvas->selectAll();
        QCOMPARE(desk.canvas->tool(), QString("pen"));
        desk.moveTo(QPointF(300, 500));
        desk.release(QPointF(300, 500));
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->kind, Item::Kind::Stroke);
        QCOMPARE(desk.last()->rect.left(), 100.0);
        QCOMPARE(desk.last()->rect.right(), 300.0);
        desk.key(Qt::Key_F);
        desk.press(QPointF(600, 300));
        desk.moveTo(QPointF(650, 340));
        desk.canvas->selectAll();
        QCOMPARE(desk.canvas->tool(), QString("frame"));
        desk.moveTo(QPointF(800, 450));
        desk.release(QPointF(800, 450));
        QCOMPARE(desk.board().count(), 4);
        QCOMPARE(desk.last()->kind, Item::Kind::Frame);
        QCOMPARE(desk.last()->rect, QRectF(600, 300, 200, 150));

        // With the mouse up, and the frame's title left unwritten,
        // everything is selected as ever.
        desk.key(Qt::Key_Escape);
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aReloadWhileTheMouseIsDownLetsGoOfThePress() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(224, 124, "Mine"));
        desk.click(QPointF(300, 200));
        // A corner in hand, and the whole board replaced meanwhile, as a
        // board read again from its file is: what the press had hold of
        // went with the board it was on, so the press is let go of...
        desk.press(QPointF(379, 279));
        desk.moveTo(QPointF(399, 299));
        QVERIFY(desk.canvas->busy());
        Board outside;
        outside.add(note(500, 100, "From outside"));
        QCOMPARE(desk.board().fromJson(outside.toJson()), QStringList());
        QVERIFY(!desk.canvas->pressed());
        QVERIFY(!desk.canvas->busy());
        // ...and the keys are the board's again at once, the mouse still
        // down; the rest of the press does nothing.
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.canvas->origin(), QPointF(48, 0));
        desk.moveTo(QPointF(419, 319));
        desk.release(QPointF(419, 319));
        QCOMPARE(desk.board().toJson(), outside.toJson());
        desk.key(Qt::Key_N);
        QCOMPARE(desk.canvas->tool(), QString("note"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aDoubleClickWritesNothingWhileTheMiddleButtonPans() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(224, 124, "Call"));
        const QJsonObject before = desk.board().toJson();
        // The view held by the middle button, by a note and then by the empty
        // board, and a double click meanwhile on what it holds (still under
        // the pointer, as a pan keeps it): nothing is written, and the view
        // is still the middle button's.
        for (const QPointF point : {QPointF(300, 200), QPointF(700, 500)}) {
            const QPoint from = desk.at(point);
            QTest::mouseMove(desk.window, from);
            QTest::mousePress(desk.window, Qt::MiddleButton, {}, from);
            QTest::mouseMove(desk.window, from - QPoint(20, 10));
            const QPointF origin = desk.canvas->origin();
            QTest::mouseDClick(desk.window, Qt::LeftButton, {}, desk.at(point));
            QVERIFY(!desk.canvas->editing());
            QCOMPARE(desk.board().toJson(), before);
            QVERIFY(desk.canvas->pressed());
            QTest::mouseMove(desk.window, from - QPoint(40, 20));
            QCOMPARE(desk.canvas->origin(), origin + QPointF(20, 10));
            QTest::mouseRelease(desk.window, Qt::MiddleButton, {}, from - QPoint(40, 20));
            QVERIFY(!desk.canvas->pressed());
        }
        // With the middle button up, the same double click writes, as ever.
        desk.doubleClick(QPointF(300, 200));
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.canvas->editorText(), QString("Call"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aThinPictureComesInAsAThinBoxNotALine() {
        Desk desk;
        QVERIFY(desk.ready());
        const qreal least = Defaults::minimumSize;
        // A rule 2000 pixels long and 2 high. Brought down to a size to
        // arrange it would be 420 by 0.42: a line, saved as 0 high, and a
        // board that would not open again.
        const QString rule = picture(desk.home.filePath("rule.png"), QSize(2000, 2));
        // It is as long, and as thin as anything can be made, however it
        // comes in: picked, pasted or dropped.
        desk.picker.images = {QUrl::fromLocalFile(rule)};
        desk.hover(QPointF(400, 100));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->rect, QRectF(190, 92, 420, least));
        QGuiApplication::clipboard()->setImage(QImage(rule));
        desk.hover(QPointF(400, 250));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.last()->rect, QRectF(190, 242, 420, least));
        QVERIFY(desk.drop(QPointF(400, 400), rule));
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->rect, QRectF(190, 392, 420, least));
        // Stood on end, it is as narrow, and as tall as a picture starts.
        desk.picker.images = {QUrl::fromLocalFile(picture(desk.home.filePath("post.png"), QSize(2, 2000)))};
        desk.hover(QPointF(800, 300));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.last()->rect, QRectF(792, 140, least, 320));
        // A picture less than the least across and down grows to it, in
        // proportion.
        desk.picker.images = {QUrl::fromLocalFile(picture(desk.home.filePath("dot.png"), QSize(6, 3)))};
        desk.hover(QPointF(400, 550));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.last()->rect, QRectF(384, 542, 32, least));
        QCOMPARE(desk.all(Item::Kind::Image).size(), 5);

        // What is on disk reads back, and the board opens again with all of
        // it, to be changed as ever.
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QVERIFY(desk.backend.newBoard("other"));
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(!desk.canvas->readOnly());
        QCOMPARE(desk.backend.problem(), QString());
        const auto pictures = desk.all(Item::Kind::Image);
        QCOMPARE(pictures.size(), 5);
        for (const Item *image : pictures)
            QVERIFY(image->rect.width() >= least && image->rect.height() >= least);
        QCOMPARE(desk.warnings, QStringList());
    }

    void nothingIsResizedOrDrawnThinnerThanTheLeast() {
        Desk desk;
        QVERIFY(desk.ready());
        const qreal least = Defaults::minimumSize;
        // A frame around a row of things, 800 by 40, taken by its right side
        // with Shift and dragged past its left. It keeps its proportions as
        // far as they go: it stops as low as anything can be made, about
        // its middle, rather than going on to 16 by 0.8.
        Item row;
        row.kind = Item::Kind::Frame;
        row.rect = QRectF(60, 100, 800, 40);
        const quint32 frame = desk.add(row);
        desk.canvas->select({frame});
        desk.drag(QPointF(863, 120), QPointF(0, 120), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(frame)->rect, QRectF(60, 112, 320, least));
        // Off whole units, as a file may put it, it stops at the least just
        // the same: not a hair under it for where its middle fell.
        Item off = row;
        off.rect = QRectF(60, 228.4, 800, 40);
        const quint32 offFrame = desk.add(off);
        desk.canvas->select({offFrame});
        desk.drag(QPointF(863, 248), QPointF(0, 248), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(offFrame)->rect, QRectF(60, 240.4, 320, least));
        QVERIFY(desk.board().item(offFrame)->rect.height() >= least);
        // A column, taken by its bottom, stops as narrow.
        Item column;
        column.kind = Item::Kind::Frame;
        column.rect = QRectF(880, 160, 40, 400);
        const quint32 tall = desk.add(column);
        desk.canvas->select({tall});
        desk.drag(QPointF(900, 563), QPointF(900, 0), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(tall)->rect, QRectF(892, 160, least, 160));
        // A shape the same, by its left side.
        const quint32 bar = desk.add(shape(Item::Shape::Rounded, 60, 600, 760, 30));
        desk.canvas->select({bar});
        desk.drag(QPointF(57, 615), QPointF(900, 615), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(bar)->rect, QRectF(820 - least * 760 / 30, 607, least * 760 / 30, least));
        // And a note, though no lower than a line written on it would be.
        const quint32 pad = desk.add(note(600, 160));
        desk.canvas->select({pad});
        desk.drag(QPointF(597, 236), QPointF(900, 236), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(pad)->rect.topLeft(), QPointF(736, 228));
        QCOMPARE(desk.board().item(pad)->rect.width(), least);
        QVERIFY(desk.board().item(pad)->rect.height() >= least);

        // A banner keeps its proportions without Shift, as pictures do, and
        // stops the same way by either side.
        desk.picker.images = {QUrl::fromLocalFile(picture(desk.home.filePath("banner.png"), QSize(1200, 100)))};
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_I);
        const quint32 banner = desk.last()->id;
        QCOMPARE(desk.board().item(banner)->rect, QRectF(190, 283, 420, 35));
        desk.drag(QPointF(613, 300), QPointF(0, 300));
        QCOMPARE(desk.board().item(banner)->rect, QRectF(190, 292.5, 192, least));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(banner)->rect, QRectF(190, 283, 420, 35));
        desk.drag(QPointF(187, 300), QPointF(900, 300), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(banner)->rect, QRectF(418, 292.5, 192, least));

        // Type's sides only set where it wraps, Shift or not: its height is
        // what its lines come to, so they go as narrow as they did.
        Item words;
        words.kind = Item::Kind::Text;
        words.fontSize = Defaults::textSize;
        words.text = "A line of words that runs on";
        words.rect = QRectF(100, 420, 2, 16);
        TextMetrics::fit(words);
        const quint32 text = desk.add(words);
        desk.canvas->select({text});
        const QRectF line = desk.board().item(text)->rect;
        desk.drag(QPointF(line.right() + 3, line.center().y()), QPointF(160, line.center().y()), Qt::ShiftModifier);
        QVERIFY(desk.board().item(text)->wrap < line.width() / 2);
        QVERIFY(desk.board().item(text)->rect.height() > 2 * line.height());

        // A frame or a shape drawn as a sliver is as thin as anything can be
        // made, and no thinner.
        desk.key(Qt::Key_F);
        desk.drag(QPointF(100, 520), QPointF(500, 523));
        QCOMPARE(desk.last()->rect, QRectF(100, 520, 400, least));
        desk.key(Qt::Key_Escape);
        desk.key(Qt::Key_S);
        desk.drag(QPointF(700, 380), QPointF(702, 600));
        QCOMPARE(desk.last()->rect, QRectF(700, 380, least, 220));
        desk.key(Qt::Key_Escape);

        // All of it reads back from disk, and the board opens again.
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QVERIFY(desk.backend.newBoard("other"));
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(!desk.canvas->readOnly());
        QCOMPARE(desk.board().count(), 9);
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsAlreadyThinnerThanTheLeastDoesNotLeap() {
        // A board from before there was a least, or from the command line: a
        // picture 160 by 8, a frame as thin, one made by hand in the file,
        // 900 million units long, and a stroke drawn close up, shorter than
        // a unit.
        Desk desk("old", [](const QString &home) {
            QDir(home).mkpath("boards/old/assets");
            picture(home + "/boards/old/assets/rule.png", QSize(2000, 100));
            write(home + "/boards/old/board.json", R"({"sprawl": 1, "items": [
                {"id": 1, "type": "image", "x": 100, "y": 100, "w": 160, "h": 8, "asset": "rule.png"},
                {"id": 2, "type": "frame", "x": 100, "y": 300, "w": 160, "h": 8},
                {"id": 3, "type": "frame", "x": 0, "y": 500, "w": 900000000, "h": 8},
                {"id": 4, "type": "stroke", "x": 100, "y": 700, "points": [0, 0, 0.5, 0.3]}]})");
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.problem(), QString());
        // Close enough to take each by its sides.
        desk.canvas->setView(QPointF(90, 80), 4);
        desk.canvas->select({1});
        // The picture by its right side, pulled in as far as it goes: it
        // stays as it was, rather than leap to twice as long to be the least
        // high, or go on to 16 by 0.8...
        desk.drag(QPointF(260.75, 104), QPointF(100, 104));
        QCOMPARE(desk.board().item(1)->rect, QRectF(100, 100, 160, 8));
        // ...and pulled out, it grows in proportion from there.
        desk.drag(QPointF(260.75, 104), QPointF(280.75, 104));
        QCOMPARE(desk.board().item(1)->rect, QRectF(100, 99.5, 180, 9));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        // By its corner, the same.
        desk.drag(QPointF(260.75, 108.75), QPointF(200, 108.75));
        QCOMPARE(desk.board().item(1)->rect, QRectF(100, 100, 160, 8));

        // The frame with Shift the same; without it, its side in hand is
        // made the least at once, and the other left as it is.
        desk.canvas->setView(QPointF(90, 280), 4);
        desk.canvas->select({2});
        desk.drag(QPointF(260.75, 304), QPointF(100, 304), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(2)->rect, QRectF(100, 300, 160, 8));
        desk.drag(QPointF(180, 308.75), QPointF(180, 309.75));
        QCOMPARE(desk.board().item(2)->rect, QRectF(100, 300, 160, Defaults::minimumSize));

        // Nor does the long one leap past the most a file holds, nor grow
        // past it: a board with it would not open.
        desk.canvas->setView(QPointF(900000000 - 100, 480), 4);
        desk.canvas->select({3});
        desk.drag(QPointF(900000000.75, 504), QPointF(900000000 - 99.25, 504), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(3)->rect, QRectF(0, 500, 900000000, 8));
        desk.drag(QPointF(900000000.75, 508.75), QPointF(900000000.75, 510.75), Qt::ShiftModifier);
        const QRectF longest = desk.board().item(3)->rect;
        QCOMPARE(longest.topLeft(), QPointF(0, 500));
        QVERIFY(longest.width() <= 1e9 && longest.width() > 999999999);
        QVERIFY(qAbs(longest.width() / longest.height() - 900000000 / 8.0) < 1);

        // The stroke with Shift by its corner, pulled in: it stays as it was,
        // rather than leap to a unit long; and pulled out, it grows in
        // proportion from there.
        desk.canvas->setView(QPointF(60, 660), 8);
        desk.canvas->select({4});
        desk.drag(QPointF(101.875, 701.625), QPointF(101.125, 701), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(4)->rect, QRectF(100, 700, 0.5, 0.3));
        desk.drag(QPointF(101.875, 701.625), QPointF(102.875, 702.125), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(4)->rect, QRectF(100, 700, 1.5, 0.9));

        // All of it reads back, and the board opens again.
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QVERIFY(desk.backend.newBoard("other"));
        QVERIFY(desk.backend.openBoard("old"));
        QVERIFY(!desk.canvas->readOnly());
        QCOMPARE(desk.board().count(), 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNearlyLevelStrokeKeepsItsSlope() {
        Desk desk;
        QVERIFY(desk.ready());
        // A line drawn close up, a quarter of a unit off level over 80, and
        // off whole units, as the pen leaves one.
        Item line = stroke({QPointF(0, 0), QPointF(40, 0.125), QPointF(80, 0.25)}, 10, 20.125);
        line.width = 4;
        const quint32 id = desk.add(line);
        desk.canvas->setView(QPointF(0, 0), 8);
        desk.canvas->select({id});
        // Its right side pulled in: shorter, and as far off level as it was,
        // not flattened to whole units.
        desk.drag(QPointF(92.375, 20.25), QPointF(87.375, 20.25));
        QCOMPARE(desk.board().item(id)->rect, QRectF(10, 20.125, 75, 0.25));
        QCOMPARE(desk.board().item(id)->points.last().y(), 0.25);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->rect, line.rect);
        // With Shift, it is shorter and lower in proportion, about its
        // middle: not made a unit high, and four times as long.
        desk.drag(QPointF(92.375, 20.25), QPointF(87.375, 20.25), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(id)->rect, QRectF(10, 20.1328125, 75, 0.234375));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        // And by its corner, pulled out, it grows from where it was.
        desk.drag(QPointF(92.375, 22.75), QPointF(97.375, 22.75), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(id)->rect, QRectF(10, 20.125, 85, 0.265625));
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QCOMPARE(desk.warnings, QStringList());
    }

    void nothingIsResizedFartherOutThanAFileHolds() {
        // Things out at the edge of the board, as far as a file has them, a
        // billion units from its middle: as a board made by hand or by the
        // command line may put them.
        Desk desk("edge", [](const QString &home) {
            QDir(home).mkpath("boards/edge");
            write(home + "/boards/edge/board.json", R"({"sprawl": 1, "items": [
                {"id": 1, "type": "frame", "x": -500000000, "y": 0, "w": 400000000, "h": 16},
                {"id": 2, "type": "frame", "x": 999999900, "y": 300, "w": 1000, "h": 100},
                {"id": 3, "type": "frame", "x": -999999950, "y": 600, "w": 100, "h": 100},
                {"id": 4, "type": "frame", "x": 999999990, "y": 900, "w": 100, "h": 100},
                {"id": 5, "type": "text", "x": -1000000000, "y": 1200, "wrap": 10, "text": "Far"},
                {"id": 6, "type": "text", "x": 0, "y": 1500, "size": 4, "wrap": 900000000, "text": "Small print"},
                {"id": 7, "type": "frame", "x": 0, "y": -999999950, "w": 100, "h": 100},
                {"id": 8, "type": "frame", "x": 0, "y": 999999990, "w": 100, "h": 100}]})");
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.problem(), QString());

        // A long thin frame, its top left corner pulled up with Shift: it
        // grows in proportion until its left edge is as far out as a file
        // has anything, its other corner where it was, rather than on past
        // it to (-1.1e9, -24) and a board that would not open.
        desk.canvas->setView(QPointF(-500000100, -100), 1);
        desk.canvas->select({1});
        desk.drag(QPointF(-500000003, -3), QPointF(-500000003, -60), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(1)->rect, QRectF(-1e9, -20, 9e8, 36));

        // One that reaches past the far edge, pulled in by its left side:
        // that side stops at the edge.
        desk.canvas->setView(QPointF(999999800, 250), 1);
        desk.canvas->select({2});
        desk.drag(QPointF(999999897, 350), QPointF(1000000397, 350));
        QCOMPARE(desk.board().item(2)->rect, QRectF(1e9, 300, 900, 100));

        // In proportion by a side, the other side grows about its middle, and
        // stops where its edge comes to the edge of the board...
        desk.canvas->setView(QPointF(-1000000000, 500), 1);
        desk.canvas->select({3});
        desk.drag(QPointF(-999999900, 703), QPointF(-999999900, 1003), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(3)->rect, QRectF(-1e9, 600, 200, 200));
        // ...and shrinks about it only as far as keeps its edge in.
        desk.canvas->setView(QPointF(999999900, 800), 1);
        desk.canvas->select({4});
        desk.drag(QPointF(1000000040, 1003), QPointF(1000000040, 943), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(4)->rect, QRectF(1e9, 900, 80, 80));
        // The same up and down: a top pulled up stops at the edge...
        desk.canvas->setView(QPointF(-100, -1000000100), 1);
        desk.canvas->select({7});
        desk.drag(QPointF(50, -999999953), QPointF(50, -1000000053));
        QCOMPARE(desk.board().item(7)->rect, QRectF(0, -1e9, 100, 150));
        // ...and with Shift by a side, the height shrinks about its middle
        // only as far as keeps its top in.
        desk.canvas->setView(QPointF(-100, 999999900), 1);
        desk.canvas->select({8});
        desk.drag(QPointF(103, 1000000040), QPointF(43, 1000000040), Qt::ShiftModifier);
        QCOMPARE(desk.board().item(8)->rect, QRectF(0, 1e9, 80, 80));

        // Type at the very edge, wrapping narrower than its side can make it,
        // pulled out by its left side: it wraps at the least, and is pushed
        // back in rather than out.
        desk.canvas->setView(QPointF(-1000000010, 1190), 2);
        desk.canvas->select({5});
        const qreal middle = desk.board().item(5)->rect.center().y();
        desk.drag(QPointF(-1000000001.5, middle), QPointF(-1000000009.5, middle));
        QCOMPARE(desk.board().item(5)->wrap, 24.0);
        QCOMPARE(desk.board().item(5)->rect.left(), -1e9);
        QCOMPARE(desk.board().item(5)->rect.top(), 1200.0);
        // And small type wrapping almost as wide as a file holds, touched by
        // its corner: brought up to the least type can be made, it wraps at
        // no more than a file holds, not half as much again.
        desk.canvas->setView(QPointF(-100, 1400), 1);
        desk.canvas->select({6});
        desk.drag(QPointF(-3, 1497), QPointF(-5, 1495));
        QCOMPARE(desk.board().item(6)->fontSize, 6.0);
        QCOMPARE(desk.board().item(6)->wrap, 1e9);

        // Every one of them stays where a file can have it.
        for (const quint32 id : desk.board().order()) {
            const QRectF rect = desk.board().item(id)->rect;
            QVERIFY2(qAbs(rect.x()) <= 1e9 && qAbs(rect.y()) <= 1e9, qPrintable(QString::number(id)));
            QVERIFY2(rect.width() <= 1e9 && rect.height() <= 1e9, qPrintable(QString::number(id)));
        }
        // What it is kept to is just what a file holds: at the edge it reads
        // back, and a unit past it, not.
        Item read;
        const qreal farthest = Item::farthest;
        QCOMPARE(Board::itemFromJson({{"type", "frame"}, {"x", -farthest}, {"y", farthest}, {"w", farthest}}, read),
                 QString());
        QVERIFY(!Board::itemFromJson({{"type", "frame"}, {"x", farthest + 1}, {"y", 0}}, read).isEmpty());
        QVERIFY(!Board::itemFromJson({{"type", "frame"}, {"x", 0}, {"y", 0}, {"w", farthest + 1}}, read).isEmpty());
        // All of it reads back, and the board opens again.
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QVERIFY(desk.backend.newBoard("other"));
        QVERIFY(desk.backend.openBoard("edge"));
        QVERIFY(!desk.canvas->readOnly());
        QCOMPARE(desk.board().count(), 8);
        QCOMPARE(desk.warnings, QStringList());
    }

    void ctrlYRedoesAsCtrlShiftZDoes() {
        Desk desk;
        QVERIFY(desk.ready());
        // Some desktops count Ctrl+Y among their own keys for redo, and some
        // do not (the tests' does). Either way, each redoes what was undone.
        const quint32 id = desk.add(note(100, 100));
        desk.click(QPointF(150, 150));
        const QList<QPair<Qt::Key, Qt::KeyboardModifiers>> redoes{
            {Qt::Key_Y, Qt::ControlModifier}, {Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier}};
        for (const auto &[key, modifiers] : redoes) {
            desk.key(Qt::Key_Right);
            const QRectF moved = desk.board().item(id)->rect;
            desk.key(Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(desk.board().item(id)->rect, moved.translated(-1, 0));
            desk.key(key, modifiers);
            QVERIFY2(desk.board().item(id)->rect == moved,
                     qPrintable(QKeySequence(QKeyCombination(modifiers, key)).toString()));
        }
        // Where the desktop's own keys for redo have no Ctrl+Y, as GTK's have
        // not, it is named as well; where they have it, it is not named again.
        const QKeySequence ctrlY(Qt::CTRL | Qt::Key_Y), ctrlShiftZ(Qt::CTRL | Qt::SHIFT | Qt::Key_Z);
        const QKeySequence redoKey(Qt::Key_Redo);
        QCOMPARE(CanvasView::redoKeysWith({ctrlShiftZ, redoKey}),
                 (QVariantList{int(QKeySequence::Redo), QStringLiteral("Ctrl+Y")}));
        QCOMPARE(CanvasView::redoKeysWith({ctrlY, ctrlShiftZ, redoKey}), QVariantList{int(QKeySequence::Redo)});
        QCOMPARE(desk.canvas->redoKeys(), CanvasView::redoKeysWith(QKeySequence::keyBindings(QKeySequence::Redo)));
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsDoneToABigSelectionIsToldOnce() {
        // Two thousand notes, joined in pairs, all selected.
        const int notes = 2000, columns = 50;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < notes; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", (i % columns) * 170.0},
                                         {"y", (i / columns) * 170.0}});
            for (int i = 1; i < notes; i += 2)
                items.append(QJsonObject{{"type", "connector"}, {"from", i}, {"to", i + 1}});
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        const int all = notes + notes / 2;
        QCOMPARE(desk.board().count(), all);
        // How many times the window was told the selection changed, and the
        // box around it, since last asked.
        QSignalSpy changed(desk.canvas, &CanvasView::selectionChanged);
        QSignalSpy boxed(desk.canvas, &CanvasView::selectionBoxChanged);
        auto told = [&] {
            const QPair<int, int> times(int(changed.count()), int(boxed.count()));
            changed.clear();
            boxed.clear();
            return times;
        };
        // And how many times all of the selection was gone through to keep up
        // with it: a time or two a step, and not once for each thing in it.
        int passes = desk.canvas->selectionPasses();
        auto keptUp = [&] {
            const int since = desk.canvas->selectionPasses() - passes;
            passes += since;
            return since <= 2;
        };
        QElapsedTimer clock;
        clock.start();
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), all);
        QCOMPARE(told(), qMakePair(1, 1));
        const QRectF box = desk.canvas->selectionBox();
        QVERIFY(keptUp());

        // An arrow moves the notes, and the connectors follow them: the box
        // around them is told once, and not once for each thing it moved,
        // and the bar by them, which shows nothing a move changes, not at all.
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.board().item(1)->rect.topLeft(), QPointF(1, 0));
        QCOMPARE(told(), qMakePair(0, 1));
        QCOMPARE(desk.canvas->selectionBox(), box.translated(1, 0));
        QVERIFY(keptUp());
        // A color for all of them, which the bar by the selection is told of
        // once more; undone, and the arrow undone.
        desk.canvas->setSelectionColor("blue");
        QCOMPARE(desk.board().item(1)->color, QString("blue"));
        QCOMPARE(desk.canvas->selectionColor(), QString("blue"));
        QCOMPARE(told(), qMakePair(2, 0));
        QVERIFY(keptUp());
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionColor(), QString());
        QCOMPARE(told(), qMakePair(1, 0));
        QVERIFY(keptUp());
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(told(), qMakePair(1, 1));
        QCOMPARE(desk.canvas->selectionBox(), box);
        QVERIFY(keptUp());

        // Asked in the middle of a step, the selection is already what the
        // step has made of it; the window is told when the step is over.
        QVector<quint32> column;
        for (int i = 0; i < notes; i += columns)
            column.append(i + 1);
        desk.board().begin();
        desk.board().remove(column);
        QCOMPARE(desk.canvas->selectionCount(), all - 2 * int(column.size()));
        QCOMPARE(desk.canvas->selection().size(), all - 2 * column.size());
        QVERIFY(!desk.canvas->selection().contains(quint32(1)));
        QCOMPARE(desk.canvas->selectionBox(), box.adjusted(170, 0, 0, 0));
        QCOMPARE(told(), qMakePair(0, 0));
        desk.board().commit();
        QCOMPARE(told(), qMakePair(1, 1));
        QVERIFY(keptUp());
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), all);
        QCOMPARE(told(), qMakePair(0, 0));
        QVERIFY(keptUp());

        // A step left open, as a drag leaves one: what it has done so far is
        // told once the events of the moment have been handled, without
        // waiting for it to end, or for a frame to be drawn.
        const QVector<quint32> picked = desk.canvas->selection();
        desk.board().begin();
        for (quint32 id : picked) {
            Item item = *desk.board().item(id);
            item.color = "green";
            desk.board().set(item);
        }
        QCoreApplication::sendPostedEvents(desk.canvas, QEvent::MetaCall);
        QCOMPARE(told(), qMakePair(1, 0));
        QCOMPARE(desk.canvas->selectionColor(), QString("green"));
        QVERIFY(keptUp());
        desk.board().commit();
        QCOMPARE(told(), qMakePair(0, 0));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(told(), qMakePair(1, 0));
        QVERIFY(keptUp());

        // Delete takes away all that is selected in one go, however it was
        // picked: here from the top of the stack down. Whoever hears of the
        // first of it going finds all of it gone. And undo brings it all back
        // as it was stacked, nothing of it selected.
        QVector<quint32> downward = desk.board().order();
        std::reverse(downward.begin(), downward.end());
        desk.canvas->select(downward);
        told();
        QVERIFY(keptUp());
        const QJsonObject before = desk.board().toJson();
        const QVector<quint32> stacked = desk.board().order();
        int left = -1;
        const QMetaObject::Connection heard =
            connect(&desk.board(), &Board::itemChanged, this, [&](quint32, Board::Change change) {
                if (change == Board::Change::Removed && left < 0)
                    left = desk.board().count();
            });
        desk.key(Qt::Key_Delete);
        disconnect(heard);
        QCOMPARE(left, 0);
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.canvas->selectionCount(), 0);
        QCOMPARE(desk.canvas->selection(), QVector<quint32>());
        QCOMPARE(desk.canvas->selectionBox(), QRectF());
        QCOMPARE(told(), qMakePair(1, 1));
        QVERIFY(keptUp());
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().toJson(), before);
        QCOMPARE(desk.board().order(), stacked);
        QCOMPARE(desk.canvas->selectionCount(), 0);
        QCOMPARE(told(), qMakePair(0, 0));
        QVERIFY(keptUp());
        // All of it in a moment. It was close to twenty seconds, here, when
        // the selection was worked out again for each thing changed.
        QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(desk.warnings, QStringList());
    }

    void draggingABigSelectionWorksItOutOnceAMove() {
        // Two thousand notes, joined in pairs, all selected, and taken by one
        // of them.
        const int notes = 2000;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < notes; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", (i % 50) * 170.0},
                                         {"y", (i / 50) * 170.0}});
            for (int i = 1; i < notes; i += 2)
                items.append(QJsonObject{{"type", "connector"}, {"from", i}, {"to", i + 1}});
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        desk.key(Qt::Key_A, Qt::ControlModifier);
        const int all = desk.canvas->selectionCount();
        QCOMPARE(all, notes + notes / 2);
        const QRectF box = desk.canvas->selectionBox();
        QSignalSpy changed(desk.canvas, &CanvasView::selectionChanged);
        QSignalSpy boxed(desk.canvas, &CanvasView::selectionBoxChanged);
        desk.press(QPointF(80, 80));
        // Each move carries all of it, connectors following: the box around
        // it is worked out, and told, once a move, not once for each thing
        // moved; and the bar by it, which shows nothing a move changes, is
        // not told at all.
        for (int step = 1; step <= 5; ++step) {
            const int passes = desk.canvas->selectionPasses();
            desk.moveTo(QPointF(80 + 10 * step, 80 + 6 * step));
            QCOMPARE(desk.board().item(1)->rect.topLeft(), QPointF(10 * step, 6 * step));
            QCOMPARE(desk.canvas->selectionPasses() - passes, 1);
            QCOMPARE(changed.count(), 0);
            QCOMPARE(boxed.count(), step);
            QCOMPARE(desk.canvas->selectionBox(), box.translated(10 * step, 6 * step));
        }
        desk.release(QPointF(130, 110));
        QCOMPARE(desk.canvas->selectionCount(), all);
        QCOMPARE(changed.count(), 0);
        // One step back, the box with it.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(1)->rect.topLeft(), QPointF(0, 0));
        QCOMPARE(desk.canvas->selectionBox(), box);

        // A drag put back with Esc puts the box back with it, told once.
        desk.press(QPointF(80, 80));
        desk.moveTo(QPointF(120, 100));
        desk.moveTo(QPointF(160, 120));
        QCOMPARE(desk.canvas->selectionBox(), box.translated(80, 40));
        changed.clear();
        boxed.clear();
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().item(1)->rect.topLeft(), QPointF(0, 0));
        QCOMPARE(desk.canvas->selectionBox(), box);
        QCOMPARE(boxed.count(), 1);
        QVERIFY(changed.count() <= 1);
        desk.release(QPointF(160, 120));
        QCOMPARE(desk.canvas->selectionCount(), all);
        QCOMPARE(desk.warnings, QStringList());
    }

    void removingManyTogetherIsRemovingEachInTurn() {
        // Two boards alike. From one, things are removed one at a time; from
        // the other, all together: both come to the same board, say the same
        // of it in the same order, and undo puts back the same.
        auto fill = [](Board &board) {
            const quint32 a = board.add(note(0, 0)), b = board.add(note(300, 0)), c = board.add(note(600, 0));
            board.add(connector(a, b));
            board.add(connector(b, c));
            board.add(connector(a, c));
            board.add(note(0, 300));
            board.add(shape(Item::Shape::Rect, 300, 300));
        };
        Board each, together;
        fill(each);
        fill(together);
        const QJsonValue before = together.toJson().value("items");
        auto said = [](const QSignalSpy &spy) {
            QVector<QPair<quint32, Board::Change>> changes;
            for (const auto &change : spy)
                changes.append({change[0].toUInt(), change[1].value<Board::Change>()});
            return changes;
        };
        // In a step that has just added a note, and a connector to it: a
        // connector, a note and its two, one that is not there, the new note
        // and its own, another note, and two that have gone already.
        for (Board *board : {&each, &together}) {
            board->begin();
            board->add(connector(board->add(note(900, 0)), 8));
        }
        QSignalSpy eachSaid(&each, &Board::itemChanged), togetherSaid(&together, &Board::itemChanged);
        const QVector<quint32> ids{6, 2, 99, 9, 7, 2, 4};
        for (quint32 id : ids)
            each.remove(id);
        together.remove(ids);
        each.commit();
        together.commit();
        QCOMPARE(together.toJson(), each.toJson());
        QCOMPARE(together.order(), (QVector<quint32>{1, 3, 8}));
        QCOMPARE(said(togetherSaid), said(eachSaid));
        QCOMPARE(said(togetherSaid).size(), 7);
        QCOMPARE(said(togetherSaid).last(), qMakePair(quint32(7), Board::Change::Removed));

        // One step, which undo takes back to what there was, in its order.
        together.undo();
        QCOMPARE(together.toJson().value("items"), before);
        QCOMPARE(together.order(), (QVector<quint32>{1, 2, 3, 4, 5, 6, 7, 8}));
        QCOMPARE(together.connectorsOf(2).size(), 2);
        together.redo();
        QCOMPARE(together.toJson(), each.toJson());
        // And what is not there is no step at all.
        QSignalSpy history(&together, &Board::historyChanged);
        together.remove(QVector<quint32>{2, 99});
        together.remove(QVector<quint32>());
        QCOMPARE(history.count(), 0);
        QCOMPARE(together.toJson(), each.toJson());
    }

    void copiesOfABigSelectionArePutDownInAMoment() {
        // Four thousand notes, all selected.
        const int notes = 4000, columns = 50;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < notes; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", (i % columns) * 170.0},
                                         {"y", (i / columns) * 170.0}});
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        const QJsonValue before = desk.board().toJson().value("items");
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.window->grabWindow();
        const int layerings = desk.canvas->layerings(), asked = desk.canvas->depthsAsked();
        QSignalSpy changed(desk.canvas, &CanvasView::selectionChanged);
        QSignalSpy boxed(desk.canvas, &CanvasView::selectionBoxChanged);
        // Duplicated, and the copies copied and pasted, again and again: each
        // time all that was put down is what is selected, and the window is
        // told so once.
        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 2 * notes);
        QCOMPARE(desk.canvas->selectionCount(), notes);
        QCOMPARE(desk.board().item(desk.canvas->selection().first())->rect.topLeft(), QPointF(16, 16));
        desk.key(Qt::Key_C, Qt::ControlModifier);
        for (int pasted = 1; pasted <= 6; ++pasted) {
            desk.key(Qt::Key_V, Qt::ControlModifier);
            QCOMPARE(desk.board().count(), (2 + pasted) * notes);
            QCOMPARE(desk.canvas->selectionCount(), notes);
        }
        QCOMPARE(changed.count(), 7);
        QCOMPARE(boxed.count(), 7);
        // And drawn. Each copy's layer was worked out as it went down, on top
        // of everything: no other on the board has its own worked out again.
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->layerings(), layerings);
        // Nor was the board asked how deep any copy is. It took seconds, here,
        // when each copy had the board count out its whole stack again as the
        // copy went on top of it.
        QCOMPARE(desk.canvas->depthsAsked(), asked);
        // A step each, which undo takes back.
        for (int step = 1; step <= 7; ++step)
            desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().toJson().value("items"), before);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aMarqueeOverManyGroupsLooksThroughTheBoardOnceAMove() {
        // Eight thousand small shapes, a hundred to a row, in groups of two.
        const int shapes = 8000, columns = 100;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < shapes; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "shape"}, {"shape", "rect"},
                                         {"x", (i % columns) * 24.0}, {"y", (i / columns) * 24.0},
                                         {"w", 16}, {"h", 16}, {"group", i / 2 + 1}});
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        desk.canvas->setView(QPointF(-40, -40), 0.25);
        // A marquee across all of them. Each move finds the rest of the groups
        // it touches in one look through the board, and the box around them
        // in one through the selection: not a look through the board for
        // each group, four thousand of them by the end.
        desk.press(QPointF(-20, -20));
        for (int step = 1; step <= 4; ++step) {
            const int passes = desk.canvas->selectionPasses();
            desk.moveTo(QPointF(-20, -20) + QPointF(2440, 1960) * step / 4.0);
            QVERIFY2(desk.canvas->selectionPasses() - passes <= 2,
                     qPrintable(QString::number(desk.canvas->selectionPasses() - passes)));
        }
        desk.release(QPointF(2420, 1940));
        QCOMPARE(desk.canvas->selectionCount(), shapes);
        // Touching one of a group takes the rest of it along: the first two
        // here, and then the second of one and the first of the next.
        desk.drag(QPointF(-10, -10), QPointF(4, 4));
        QCOMPARE(desk.canvas->selection(), (QVector<quint32>{1, 2}));
        desk.drag(QPointF(20, 20), QPointF(50, 34));
        QCOMPARE(desk.canvas->selectionCount(), 4);
        for (quint32 id : {101, 102, 103, 104})
            QVERIFY(desk.canvas->selection().contains(id));
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatAFrameMeasuresOfTheSelectionIsShownInThatFrame() {
        Desk desk;
        QVERIFY(desk.ready());
        // Text as a file may have it, its box not measured yet, selected
        // before it is drawn.
        Item text;
        text.kind = Item::Kind::Text;
        text.fontSize = Defaults::textSize;
        text.text = "Measured as it is drawn";
        text.rect = QRectF(100, 100, 2, 16);
        const quint32 id = desk.add(text);
        desk.canvas->select({id});
        QCOMPARE(desk.canvas->selectionSize(), QStringLiteral("2 × 16"));
        QSignalSpy boxed(desk.canvas, &CanvasView::selectionBoxChanged);
        // The frame that measures it shows by it the size it measured, told
        // before that frame is drawn and not after it.
        desk.window->grabWindow();
        QVERIFY(desk.board().item(id)->rect.width() > 2);
        QCOMPARE(boxed.count(), 1);
        const QString size = desk.canvas->selectionSize();
        QVector<QQuickItem *> shown;
        Desk::texts(desk.window->contentItem(), shown);
        QVERIFY(std::any_of(shown.cbegin(), shown.cend(), [&](QQuickItem *label) {
            return label->isVisible() && label->property("text").toString() == size;
        }));
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsDeletedOrCutIsGoneFromTheFileToo() {
        // Three notes, the first two joined, as the file has them.
        Desk desk("desk", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 3; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", i * 300.0}, {"y", 100}});
            items.append(QJsonObject{{"id", 4}, {"type", "connector"}, {"from", 1}, {"to", 2}});
            QDir(home).mkpath("boards/desk");
            write(home + "/boards/desk/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        auto onDisk = [&desk] { return desk.onDisk().value("items").toArray().size(); };
        QCOMPARE(onDisk(), 4);
        // The two joined deleted together, the connector between them with
        // them, all in one go: saved by itself, as anything done is.
        desk.canvas->select({1, 2});
        desk.key(Qt::Key_Delete);
        QCOMPARE(desk.board().count(), 1);
        QTRY_COMPARE(onDisk(), 1);
        // The last one cut, and the window closed at once: what closing saves
        // has it gone too.
        desk.click(QPointF(680, 180));
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{3});
        desk.key(Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(desk.backend.flush());
        QCOMPARE(onDisk(), 0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void shiftTakesAWholeGroupOutOfABigSelectionAndPutsItBack() {
        // Four thousand notes, the second half of them one group, all selected.
        const int notes = 4000, columns = 50;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < notes; ++i) {
                QJsonObject note{{"id", i + 1}, {"type", "note"}, {"x", (i % columns) * 170.0}, {"y", (i / columns) * 170.0}};
                if (i >= notes / 2)
                    note.insert("group", 1);
                items.append(note);
            }
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QVector<quint32> loose, group;
        for (int i = 0; i < notes; ++i)
            (i < notes / 2 ? loose : group).append(i + 1);
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selection(), loose + group);
        QSignalSpy changed(desk.canvas, &CanvasView::selectionChanged);
        QSignalSpy boxed(desk.canvas, &CanvasView::selectionBoxChanged);
        auto told = [&] {
            const QPair<int, int> times(int(changed.count()), int(boxed.count()));
            changed.clear();
            boxed.clear();
            return times;
        };
        // Shift and a click on the last of the group: all of the group leaves
        // the selection, and the rest stays as it was picked. The window is
        // told once, the group found in one look through the board and the
        // box around what is left in one through the selection.
        const int last = notes - 1;
        const QPointF corner((last % columns) * 170.0, (last / columns) * 170.0);
        desk.canvas->setView(corner - QPointF(100, 100), 1);
        told();
        int passes = desk.canvas->selectionPasses();
        desk.click(corner + QPointF(70, 70), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->selection(), loose);
        QCOMPARE(told(), qMakePair(1, 1));
        QVERIFY(desk.canvas->selectionPasses() - passes <= 2);
        // And again: all of it comes back, after what was there.
        passes = desk.canvas->selectionPasses();
        desk.click(corner + QPointF(70, 70), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->selection(), loose + group);
        QCOMPARE(told(), qMakePair(1, 1));
        QVERIFY(desk.canvas->selectionPasses() - passes <= 2);
        // With only some of the group selected, a click on one of it that is
        // not brings the rest along, after the others, and none of it twice;
        // a click on one that is takes all of it away.
        desk.canvas->select(QVector<quint32>{group[5], 1, group[2]});
        desk.click(corner + QPointF(70, 70), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{1} + group);
        desk.canvas->select(QVector<quint32>{group[5], 1, quint32(notes)});
        desk.click(corner + QPointF(70, 70), Qt::ShiftModifier);
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{1});
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatAnOlderSprawlCopiedIsPasted() {
        Desk desk;
        QVERIFY(desk.ready());
        // Already on the board: a shape with the id that the thing left
        // behind had where it was copied. An id on the clipboard says nothing
        // of this board.
        desk.add(note(600, 100, "Here already"));
        const quint32 here = desk.add(shape(Item::Shape::Rect, 800, 100));
        QCOMPARE(here, quint32(2));
        // What Sprawl 0.1.0 put on the clipboard for a note and the connector
        // from it to a shape that was not copied: the connector is still on
        // the shape, which is not there. And a note with more in it than an
        // item holds now.
        const QJsonObject idea{{"id", 1}, {"type", "note"}, {"x", 100}, {"y", 100}, {"w", 152}, {"h", 152},
                               {"color", "yellow"}, {"text", "Idea"}};
        const QJsonObject line{{"id", 3}, {"type", "connector"}, {"from", 1}, {"to", 2}, {"x1", 252}, {"y1", 176},
                               {"x2", 400}, {"y2", 176}, {"width", 1.5}};
        const QJsonObject pages{{"id", 4}, {"type", "note"}, {"x", 100}, {"y", 300}, {"w", 152}, {"h", 152},
                                {"color", "blue"}, {"text", QString(25'000, 'a')}};
        auto *data = new QMimeData;
        data->setData("application/x-sprawl-items",
                      QJsonDocument(QJsonObject{{"sprawl", 1},
                                                {"items", QJsonArray{idea, line, pages}},
                                                {"assets", desk.home.filePath("boards/plans/assets")}})
                          .toJson(QJsonDocument::Compact));
        data->setText("Idea");
        QGuiApplication::clipboard()->setMimeData(data);
        desk.hover(QPointF(500, 400));
        desk.key(Qt::Key_V, Qt::ControlModifier);

        // All three, and nothing else.
        QCOMPARE(desk.board().count(), 5);
        const QVector<quint32> pasted = desk.canvas->selection();
        QCOMPARE(pasted.size(), 3);
        const Item *copy = desk.board().item(pasted[0]);
        const Item *connector = desk.board().item(pasted[1]);
        QCOMPARE(copy->text, QString("Idea"));
        QCOMPARE(connector->kind, Item::Kind::Connector);
        // The connector is on the note it came with, and its other end is
        // where it was beside it, on nothing: not on the shape here.
        QCOMPARE(connector->from, copy->id);
        QCOMPARE(connector->to, quint32(0));
        QCOMPARE(connector->p2 - copy->rect.topLeft(), QPointF(300, 76));
        QVERIFY(copy->rect.adjusted(-1, -1, 1, 1).contains(connector->p1));
        QVERIFY(desk.board().connectorsOf(here).isEmpty());
        // The note holds as much as an item holds, and the rest is said to be
        // left out.
        QCOMPARE(desk.board().item(pasted[2])->text, QString(Item::longestText, 'a'));
        QVERIFY2(desk.backend.notice().contains("characters"), qPrintable(desk.backend.notice()));
        // One step, as any paste is.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBrokenItemOnTheClipboardIsLeftOutAndTheRestPasted() {
        Desk desk;
        QVERIFY(desk.ready());
        auto item = [](int id, const QString &type, qreal x, const QString &text) {
            return QJsonObject{{"id", id}, {"type", type}, {"x", x}, {"y", 0}, {"text", text}};
        };
        auto line = [](int id, int from, int to) {
            return QJsonObject{{"id", id}, {"type", "connector"}, {"from", from}, {"to", to}};
        };
        auto clipboard = [](const QJsonObject &root, const QString &text = "not this") {
            auto *data = new QMimeData;
            data->setData("application/x-sprawl-items", QJsonDocument(root).toJson());
            if (!text.isEmpty())
                data->setText(text);
            QGuiApplication::clipboard()->setMimeData(data);
        };
        QJsonObject far = item(4, "note", 0, "Four");
        far["x"] = "far away";
        // Connectors: from the note to the thing of no kind there is, saying
        // where that end is; from the shape to the note that is not right,
        // not saying; from a stroke, saying where it starts; and from the
        // note to the stroke, not saying where it ends.
        QJsonObject loose = line(8, 1, 2);
        loose["x2"] = 250;
        loose["y2"] = 30;
        QJsonObject fromStroke = line(11, 10, 1);
        fromStroke["x1"] = 30;
        fromStroke["y1"] = 220;
        const QJsonObject pen{
            {"id", 10}, {"type", "stroke"}, {"x", 0}, {"y", 200}, {"points", QJsonArray{0, 0, 60, 40}}};
        // And the note has more in it than an item holds.
        const QString pages = "One" + QString(25'000, '.');
        clipboard(QJsonObject{{"sprawl", 1},
                              {"items", QJsonArray{item(1, "note", 0, pages), item(2, "sticker", 200, "Two"),
                                                   item(3, "shape", 400, "Three"), far, 42,
                                                   item(3, "note", 600, "Three again"), line(7, 1, 3), loose,
                                                   line(9, 3, 4), pen, fromStroke, line(12, 1, 10)}}});
        desk.hover(QPointF(400, 300));
        // How much of it is on the board when the window says what it says
        // of the paste.
        int downWhenSaid = -1;
        const QMetaObject::Connection said = QObject::connect(
            &desk.backend, &Backend::noticeChanged, [&desk, &downWhenSaid] { downWhenSaid = desk.board().count(); });
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QObject::disconnect(said);

        // A thing of no kind there is, a number that is not one, what is not
        // a thing at all, and an id given twice are each left out, and so is
        // a connector with an end that has nothing to be on and does not say
        // where it is. The rest is pasted, back to front as it was.
        const QVector<quint32> order = desk.board().order();
        QCOMPARE(order.size(), 6);
        QCOMPARE(desk.canvas->selection(), order);
        const Item *one = desk.board().item(order[0]), *three = desk.board().item(order[1]);
        QCOMPARE(one->text, pages.left(Item::longestText));
        QCOMPARE(three->kind, Item::Kind::Shape);
        QCOMPARE(three->text, QString("Three"));
        QCOMPARE(desk.board().item(order[4])->kind, Item::Kind::Stroke);
        const QPointF offset = one->rect.topLeft();
        // Joined to both of what it joined, it joins their copies.
        const Item *joined = desk.board().item(order[2]);
        QCOMPARE(joined->from, one->id);
        QCOMPARE(joined->to, three->id);
        // An end on what was left out, or on a stroke, is free where it says.
        const Item *freedEnd = desk.board().item(order[3]);
        QCOMPARE(freedEnd->kind, Item::Kind::Connector);
        QCOMPARE(freedEnd->from, one->id);
        QCOMPARE(freedEnd->to, quint32(0));
        QCOMPARE(freedEnd->p2, QPointF(250, 30) + offset);
        const Item *freedStart = desk.board().item(order[5]);
        QCOMPARE(freedStart->kind, Item::Kind::Connector);
        QCOMPARE(freedStart->from, quint32(0));
        QCOMPARE(freedStart->to, one->id);
        QCOMPARE(freedStart->p1, QPointF(30, 220) + offset);
        // And the window says that not all of it was pasted: that, and not
        // that the note was cut short, which is some of what was left out.
        // It says so once all the rest is down, so that after a paste that
        // takes a while it is up as long as after any other.
        const QString notAll("Not all of what was copied could be pasted.");
        QCOMPARE(desk.backend.notice(), notAll);
        QCOMPARE(downWhenSaid, 6);

        // A clipboard with nothing on it that can be pasted is pasted as
        // anything else would paste it, as the text beside it, and the window
        // says why; so is one from a Sprawl that writes boards this one
        // cannot read. One with no list of items, and no text beside it,
        // pastes nothing, and says so too.
        clipboard(QJsonObject{{"sprawl", 1}, {"items", QJsonArray{item(1, "sticker", 0, "One"), far}}});
        desk.backend.notify({});
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 7);
        QCOMPARE(desk.last()->kind, Item::Kind::Text);
        QCOMPARE(desk.last()->text, QString("not this"));
        QCOMPARE(desk.backend.notice(), notAll);
        clipboard(QJsonObject{{"sprawl", 2}, {"items", QJsonArray{item(1, "note", 0, "One")}}});
        desk.backend.notify({});
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 8);
        QCOMPARE(desk.last()->text, QString("not this"));
        QCOMPARE(desk.backend.notice(), notAll);
        // Text beside it with more in it than an item holds is cut short, as
        // any pasted is, and why it was pasted is not hidden by that.
        clipboard(QJsonObject{{"sprawl", 2}, {"items", QJsonArray{item(1, "note", 0, "One")}}}, pages);
        desk.backend.notify({});
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 9);
        QCOMPARE(desk.last()->text, pages.left(Item::longestText));
        QCOMPARE(desk.backend.notice(), notAll);
        clipboard(QJsonObject{{"sprawl", 1}, {"items", "One"}}, {});
        desk.backend.notify({});
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 9);
        QCOMPARE(desk.backend.notice(), notAll);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aCopiedConnectorIsJoinedAsItGoesDown() {
        Desk desk;
        QVERIFY(desk.ready());
        // Two notes, a connector between them over them, as one is drawn, and
        // one sent under them.
        const quint32 a = desk.add(note(100, 100, "a")), b = desk.add(note(500, 100, "b"));
        const quint32 over = desk.add(connector(a, b)), under = desk.add(connector(b, a));
        desk.board().restack({under}, false);
        desk.canvas->select({a, b, over, under});
        desk.key(Qt::Key_C, Qt::ControlModifier);
        auto said = [](const QSignalSpy &spy) {
            QVector<QPair<quint32, Board::Change>> changes;
            for (const auto &change : spy)
                changes.append({change[0].toUInt(), change[1].value<Board::Change>()});
            return changes;
        };
        // Pasted, and then duplicated, each copy goes down as it is to be,
        // and the window is told of it once: the connector over the notes
        // goes down joined to their copies, there already. The one under them
        // is joined once they are down, and the window is told of it again
        // then. Each time is a look through all that is in the connector's
        // box, for what it is drawn over: for a long one, much of the board.
        for (const bool pasted : {true, false}) {
            QSignalSpy told(&desk.board(), &Board::itemChanged);
            if (pasted)
                QVERIFY(desk.canvas->paste());
            else
                desk.canvas->duplicateSelection();
            const QVector<quint32> copies = desk.canvas->selection();
            QCOMPARE(copies.size(), 4);
            const Item *underCopy = desk.board().item(copies[0]), *aCopy = desk.board().item(copies[1]);
            const Item *bCopy = desk.board().item(copies[2]), *overCopy = desk.board().item(copies[3]);
            QCOMPARE(overCopy->from, aCopy->id);
            QCOMPARE(overCopy->to, bCopy->id);
            QCOMPARE(underCopy->from, bCopy->id);
            QCOMPARE(underCopy->to, aCopy->id);
            const QVector<QPair<quint32, Board::Change>> once{
                {underCopy->id, Board::Change::Added}, {aCopy->id, Board::Change::Added},
                {bCopy->id, Board::Change::Added},     {overCopy->id, Board::Change::Added},
                {underCopy->id, Board::Change::Edited}};
            QCOMPARE(said(told), once);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardIsPutWholeInTheWindowAtTheSizeItIsGiven() {
        // A board not looked at before, much wider than it is high.
        Desk desk("desk", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 6; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", i * 300.0}, {"y", 0}});
            QDir(home).mkpath("boards/wide");
            write(home + "/boards/wide/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        // The desktop gives the window a size, and it is laid out in it.
        auto place = [&desk](QSize size) {
            desk.window->resize(size);
            return QTest::qWaitFor([&desk, size] { return desk.window->contentItem()->size() == QSizeF(size); });
        };
        QVERIFY(desk.backend.openBoard("wide"));
        const QRectF all = desk.board().contentBounds();
        // All of it, in the middle of the canvas, with 48 of air at each end.
        auto whole = [&desk, all] {
            const QPointF middle = desk.canvas->toItem(all.center());
            return qAbs(middle.x() - desk.canvas->width() / 2) < 0.01 &&
                   qAbs(middle.y() - desk.canvas->height() / 2) < 0.01 &&
                   qAbs(all.width() * desk.canvas->zoom() - (desk.canvas->width() - 96)) < 0.01;
        };
        QVERIFY(whole());
        // A tiling desktop gives a window its real size a moment after it
        // opens. The board is put in that as it would have been had the
        // window opened at that size: narrower and taller...
        QVERIFY(place(QSize(600, 900)));
        QVERIFY(whole());
        // ...or only lower.
        QVERIFY(place(QSize(600, 600)));
        QVERIFY(whole());
        // Nobody chose that view, so it is not remembered: away and back, the
        // board is put whole in the window as it is by then.
        QVERIFY(desk.backend.rememberedView().isEmpty());
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(place(QSize(800, 600)));
        QVERIFY(desk.backend.openBoard("wide"));
        QVERIFY(whole());

        // Once the view is moved, it is left where it was taken, whatever
        // size the window comes to, and remembered...
        desk.key(Qt::Key_Right);
        const QPointF origin = desk.canvas->origin();
        const qreal zoom = desk.canvas->zoom();
        QVERIFY(!whole());
        QCOMPARE(desk.backend.rememberedView().value("x").toDouble(), origin.x());
        QVERIFY(place(QSize(1000, 700)));
        QCOMPARE(desk.canvas->origin(), origin);
        QCOMPARE(desk.canvas->zoom(), zoom);
        // ...so that is where the board opens again, at any size.
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(desk.backend.openBoard("wide"));
        QVERIFY(place(QSize(700, 900)));
        QCOMPARE(desk.canvas->origin(), origin);
        QCOMPARE(desk.canvas->zoom(), zoom);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aWindowJustOpenedPutsTheBoardInTheSizeItIsGiven() {
        Desk desk("desk", {}, QSize(1200, 760));
        QVERIFY(desk.ready());
        // A board much wider than it is high that nobody has looked around,
        // and another left at a view of its own.
        QVERIFY(desk.backend.newBoard("wide"));
        for (int i = 0; i < 6; ++i)
            desk.add(note(i * 300, 0));
        QVERIFY(desk.backend.newBoard("left"));
        desk.add(note(0, 0));
        desk.canvas->setView(QPointF(100, 50), 0.5);
        // The app started again on a board: its window as main() opens one,
        // at the size it starts at and with nothing done to its view. The
        // desk's own, which puts the view at the board's corner for every
        // test, makes way for it.
        auto start = [&desk](const QString &name) {
            delete desk.window;
            desk.window = nullptr;
            desk.canvas = nullptr;
            if (!desk.backend.openBoard(name))
                return false;
            desk.engine.load(QUrl("qrc:/Main.qml"));
            if (desk.engine.rootObjects().isEmpty())
                return false;
            desk.window = qobject_cast<QQuickWindow *>(desk.engine.rootObjects().last());
            desk.canvas = desk.window ? desk.window->findChild<CanvasView *>() : nullptr;
            return desk.canvas && QTest::qWaitForWindowExposed(desk.window);
        };
        // The desktop gives the window a size, and it is laid out in it.
        auto place = [&desk](QSize size) {
            desk.window->resize(size);
            return QTest::qWaitFor([&desk, size] { return desk.window->contentItem()->size() == QSizeF(size); });
        };
        // All of the board, in the middle of the canvas, with 48 of air at
        // each end.
        auto whole = [&desk] {
            const QRectF all = desk.board().contentBounds();
            const QPointF middle = desk.canvas->toItem(all.center());
            return qAbs(middle.x() - desk.canvas->width() / 2) < 0.01 &&
                   qAbs(middle.y() - desk.canvas->height() / 2) < 0.01 &&
                   qAbs(all.width() * desk.canvas->zoom() - (desk.canvas->width() - 96)) < 0.01;
        };

        // All of the board is in the window as it opens, and in each size it
        // is given after: its real size a moment later, from a tiling
        // desktop, and others as windows come and go beside it.
        QVERIFY(start("wide"));
        QVERIFY(whole());
        for (const QSize size : {QSize(850, 1060), QSize(1700, 1060), QSize(850, 520)}) {
            QVERIFY(place(size));
            QVERIFY(whole());
        }
        QVERIFY(desk.backend.rememberedView().isEmpty());
        // A board opens where it was left, at any size.
        QVERIFY(start("left"));
        for (const QSize size : {QSize(850, 1060), QSize(1700, 1060)}) {
            QVERIFY(place(size));
            QCOMPARE(desk.canvas->origin(), QPointF(100, 50));
            QCOMPARE(desk.canvas->zoom(), 0.5);
        }
        // A view moved before the real size comes, as --zoom moves it, stays
        // where it was moved to.
        QVERIFY(start("wide"));
        desk.canvas->zoomBy(0.25 / desk.canvas->zoom(), QPointF(600, 380));
        const QPointF origin = desk.canvas->origin();
        QVERIFY(place(QSize(850, 1060)));
        QCOMPARE(desk.canvas->origin(), origin);
        QCOMPARE(desk.canvas->zoom(), 0.25);
        QCOMPARE(desk.backend.rememberedView().value("zoom").toDouble(), 0.25);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theViewStaysAsItIsOnceTheBoardIsWorkedOn() {
        Desk desk;
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.newBoard("wide"));
        for (int i = 0; i < 6; ++i)
            desk.add(note(i * 300, 0));
        // Opened again, the board is put all in the window, by nobody.
        auto reopen = [&desk] { return desk.backend.openBoard("desk") && desk.backend.openBoard("wide"); };
        auto place = [&desk](QSize size) {
            desk.window->resize(size);
            return QTest::qWaitFor([&desk, size] { return desk.window->contentItem()->size() == QSizeF(size); });
        };
        // The window is given another size, and the view is as it was.
        auto stays = [&desk, &place](QSize size) {
            const QPointF origin = desk.canvas->origin();
            const qreal zoom = desk.canvas->zoom();
            return place(size) && desk.canvas->origin() == origin && desk.canvas->zoom() == zoom;
        };
        // Until it is worked on, it is put all in each size the window is
        // given...
        QVERIFY(reopen());
        QVERIFY(!stays(QSize(700, 900)));
        // ...but not once it is clicked...
        QVERIFY(reopen());
        desk.click(desk.canvas->toCanvas(QPointF(desk.canvas->width() / 2, desk.canvas->height() - 20)));
        QVERIFY(stays(QSize(900, 600)));
        // ...written on, while that is being written and after...
        QVERIFY(reopen());
        desk.key(Qt::Key_Space);
        QVERIFY(desk.canvas->editing());
        desk.type("Mine");
        QVERIFY(stays(QSize(700, 900)));
        desk.key(Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(stays(QSize(900, 600)));
        // ...changed with the keys alone...
        QVERIFY(reopen());
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.key(Qt::Key_Right);
        QVERIFY(stays(QSize(700, 900)));
        // ...or changed from outside, by an agent's command: nothing on it
        // moves on screen.
        QVERIFY(reopen());
        const QString file = desk.library.boardFile("wide");
        QJsonObject root = QJsonDocument::fromJson(read(file)).object();
        QJsonArray items = root.value("items").toArray();
        items.append(QJsonObject{{"id", 99}, {"type", "note"}, {"x", 9000.0}, {"y", 3000.0}});
        root.insert("items", items);
        write(file, QJsonDocument(root).toJson());
        QVERIFY(QTest::qWaitFor([&desk] { return desk.board().count() == 8; }));
        QVERIFY(stays(QSize(900, 600)));
        // None of that moved the view, so none of it is remembered.
        QVERIFY(desk.backend.rememberedView().isEmpty());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aZoomJustBeforeTheWindowIsGivenItsSizeIsKept() {
        Desk desk("desk", {}, QSize(1200, 760));
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.newBoard("wide"));
        for (int i = 0; i < 6; ++i)
            desk.add(note(i * 300, 0));
        // Opened again, the board is put all in the window, by nobody, at a
        // zoom between the steps of 50% and 67%.
        QVERIFY(desk.backend.openBoard("desk"));
        QVERIFY(desk.backend.openBoard("wide"));
        QVERIFY(desk.canvas->zoom() > 0.5 && desk.canvas->zoom() < 0.67);
        // A step in, and the desktop gives the window another size before
        // the zoom has taken its first step: the person has moved the view
        // all the same.
        desk.key(Qt::Key_Equal);
        desk.window->resize(QSize(700, 900));
        QVERIFY(QTest::qWaitFor([&desk] { return desk.window->contentItem()->size() == QSizeF(700, 900); }));
        // It goes on to 67%, and stays there, remembered.
        QTRY_COMPARE(desk.canvas->zoom(), 0.67);
        QCOMPARE(desk.backend.rememberedView().value("zoom").toDouble(), 0.67);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardLeftAtAViewThatMakesNoSenseOpensWhole() {
        // Where five boards were left, as a state file edited by hand may
        // have it: not numbers, no zoom, no place down, and a zoom of nothing
        // or less.
        const QList<QJsonObject> views{QJsonObject{{"x", "abc"}, {"y", QJsonArray{1}}, {"zoom", "abc"}},
                                       QJsonObject{{"x", 10.0}, {"y", 10.0}, {"zoom", QJsonValue::Null}},
                                       QJsonObject{{"x", 10.0}, {"zoom", 2.0}},
                                       QJsonObject{{"x", 10.0}, {"y", 10.0}, {"zoom", 0.0}},
                                       QJsonObject{{"x", 10.0}, {"y", 10.0}, {"zoom", -2.0}}};
        Desk desk("desk", [&views](const QString &home) {
            QJsonObject left;
            for (int i = 0; i < views.size(); ++i) {
                const QString folder = home + QString("/boards/left%1").arg(i);
                QDir().mkpath(folder);
                const QJsonArray items{QJsonObject{{"id", 1}, {"type", "note"}, {"x", 500.0}, {"y", 300.0}}};
                write(folder + "/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
                left.insert(folder + "/board.json", views[i]);
            }
            write(home + "/state.json", QJsonDocument(QJsonObject{{"views", left}}).toJson());
        });
        QVERIFY(desk.ready());
        for (int i = 0; i < views.size(); ++i) {
            QVERIFY(desk.backend.openBoard(QString("left%1").arg(i)));
            // As a board nobody has looked around opens: all of it, in the
            // middle, at actual size.
            const QRectF all = desk.board().contentBounds();
            QCOMPARE(desk.canvas->zoom(), 1.0);
            QCOMPARE(desk.canvas->toItem(all.center()), QPointF(desk.canvas->width() / 2, desk.canvas->height() / 2));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingWaitsWhileAnotherWindowHasTheKeys() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Call"));
        QQuickItem *editor = desk.item("editor");
        QVERIFY(editor);
        // Another window takes the keys a while (a terminal, a notification,
        // a launcher), and gives them back.
        QWindow elsewhere;
        elsewhere.resize(320, 200);
        auto away = [&desk, &elsewhere] {
            elsewhere.show();
            elsewhere.requestActivate();
            return QTest::qWaitForWindowActive(&elsewhere) && !desk.window->isActive();
        };
        auto back = [&desk] {
            desk.window->requestActivate();
            return QTest::qWaitForWindowActive(desk.window);
        };

        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        QCOMPARE(desk.window->activeFocusItem(), editor);
        // Away from the window, what is being written is not finished...
        QVERIFY(away());
        QVERIFY(desk.canvas->editing());
        // ...and back, the keys go on into it: N is a letter, and the space
        // no new note.
        QVERIFY(back());
        QCOMPARE(desk.window->activeFocusItem(), editor);
        desk.type(" now");
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.canvas->tool(), QString("select"));
        QCOMPARE(desk.board().count(), 1);
        // Ctrl+Enter finishes it, as ever, and all of it is one step.
        desk.key(Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam now"));
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->text, QString("Call"));

        // The same while none of the app's windows has the keys, as when the
        // desktop shows another workspace.
        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QVERIFY(!QGuiApplication::focusWindow());
        QVERIFY(desk.canvas->editing());
        QVERIFY(back());
        QCOMPARE(desk.window->activeFocusItem(), editor);
        desk.type(" now");
        desk.key(Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam now"));
        QCOMPARE(desk.board().count(), 1);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().item(id)->text, QString("Call"));

        // So do Esc and a click elsewhere in the window, after a while away.
        desk.doubleClick(QPointF(300, 200));
        QVERIFY(away());
        QVERIFY(back());
        desk.type("!");
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call!"));
        desk.doubleClick(QPointF(300, 200));
        QVERIFY(away());
        QVERIFY(back());
        desk.type("?");
        desk.click(QPointF(700, 500));
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call!?"));
        QCOMPARE(desk.window->activeFocusItem(), desk.canvas);
        QCOMPARE(desk.warnings, QStringList());
    }

    void boardsAreListedOnlyToPickOne() {
        Desk desk;
        QVERIFY(desk.ready());
        QCOMPARE(desk.library.create("garden"), QString());
        QCOMPARE(desk.library.create("trip"), QString());
        auto askingForAName = [&desk] {
            QVector<QQuickItem *> shown;
            Desk::texts(desk.window->contentItem(), shown);
            return std::any_of(shown.cbegin(), shown.cend(), [](QQuickItem *text) {
                return text->isVisible() && text->property("text").toString() == "Name the new board";
            });
        };
        desk.key(Qt::Key_O, Qt::ControlModifier);
        QVERIFY(desk.shows("board-garden"));
        // Ctrl+N with nothing typed asks for a name, and the boards go, as
        // when Ctrl+N opens the panel: left listed, a click on one would
        // make a new board rather than open it.
        desk.key(Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(desk.panel(), QString("boards"));
        QVERIFY(askingForAName());
        QVERIFY(!desk.clickOn("board-garden"));
        desk.type("Plans");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("plans"));
        QCOMPARE(desk.library.boards().size(), 4);

        // Spaces make no name either. They go as the boards do: left in the
        // field, they would hide the question.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.type("  ");
        QVERIFY(desk.shows("board-garden"));
        desk.key(Qt::Key_N, Qt::ControlModifier);
        QVERIFY(askingForAName());
        QVERIFY(!desk.shows("board-garden"));
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.library.boards().size(), 4);

        // Renaming and the trash list none either: not the board being
        // renamed, nor one whose name is typed (garden. names garden). Back
        // from them the boards are listed again, there to be clicked.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.key(Qt::Key_F2);
        QVERIFY(!desk.shows("board-plans"));
        desk.type("garden.");
        QVERIFY(!desk.shows("board-garden"));
        desk.key(Qt::Key_Escape);
        QVERIFY(desk.shows("board-garden"));
        desk.key(Qt::Key_Delete, Qt::ControlModifier);
        QVERIFY(!desk.shows("board-garden"));
        desk.key(Qt::Key_Escape);
        QVERIFY(desk.clickOn("board-garden"));
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("garden"));
        QCOMPARE(desk.library.boards().size(), 4);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theSelectionGoesNoFurtherThanTheBoardsShown() {
        Desk desk;
        QVERIFY(desk.ready());
        // More boards than the switcher has rows for.
        for (int week = 1; week <= 12; ++week)
            QCOMPARE(desk.library.create(QString("week-%1").arg(week)), QString());
        desk.key(Qt::Key_O, Qt::ControlModifier);
        const QVariantList boards = desk.backend.findBoards(QString());
        QCOMPARE(boards.size(), 13);
        auto name = [](const QVariantList &found, int row) { return found.at(row).toMap().value("name").toString(); };
        auto selected = [&desk, &name](const QVariantList &found, int row) {
            const QQuickItem *shown = desk.item("board-" + name(found, row));
            return shown && shown->isVisible() && shown->property("selected").toBool();
        };
        QVERIFY(desk.shows("board-" + name(boards, 8)));
        QVERIFY(!desk.shows("board-" + name(boards, 9)));
        QVERIFY(selected(boards, 0));
        // Down goes as far as the last row shown, and no further: Up is then
        // the row above it. Tab goes no further either...
        for (int i = 0; i < 12; ++i)
            desk.key(Qt::Key_Down);
        QVERIFY(selected(boards, 8));
        desk.key(Qt::Key_Up);
        QVERIFY(selected(boards, 7));
        for (int i = 0; i < 12; ++i)
            desk.key(Qt::Key_Tab);
        QVERIFY(selected(boards, 8));
        // ...so Enter opens the board selected, not one further down that is
        // not shown.
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), name(boards, 8));

        // What is typed finds the boards again, and the selection starts
        // again at the first of them. Left on the ninth row with fewer
        // found, it would be on none, and Enter would open nothing.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        const QVariantList again = desk.backend.findBoards(QString());
        for (int i = 0; i < 8; ++i)
            desk.key(Qt::Key_Down);
        QVERIFY(selected(again, 8));
        desk.type("1");
        const QVariantList ones = desk.backend.findBoards("1");
        QCOMPARE(ones.size(), 4);
        QVERIFY(selected(ones, 0));
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), name(ones, 0));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardNamedInFullComesFirst() {
        Desk desk;
        QVERIFY(desk.ready());
        // A board a week, each changed a week after the one before. "Week 1"
        // fits week-10 to week-19 just as well as week-1, and those were
        // changed later: by the letters alone, week-1 is the last of eleven,
        // two past the rows there are.
        const QDateTime now = QDateTime::currentDateTime();
        for (int week = 1; week <= 19; ++week) {
            const QString name = QString("week-%1").arg(week);
            QCOMPARE(desk.library.create(name), QString());
            QFile file(desk.library.boardFile(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.setFileTime(now.addDays(7 * (week - 20)), QFileDevice::FileModificationTime));
        }
        const QVariantList fit = desk.backend.findBoards("Week 1");
        QCOMPARE(fit.size(), 11);
        QCOMPARE(fit.last().toMap().value("name").toString(), QString("week-1"));
        // Its whole name, as it might be typed, puts it first: shown,
        // selected, and what Enter opens.
        desk.key(Qt::Key_O, Qt::ControlModifier);
        desk.type("Week 1");
        QVERIFY(desk.shows("board-week-1"));
        QVERIFY(desk.item("board-week-1")->property("selected").toBool());
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.backend.boardName(), QString("week-1"));

        // So does a whole name typed with a dot for a dash, or a dash too
        // many. By their letters these find no board at all, and Enter would
        // make another beside the one named: week-12-2, week-3-2.
        const QList<QPair<QString, QString>> spellings{{"week.12", "week-12"}, {"Week--3", "week-3"}};
        for (const auto &[text, board] : spellings) {
            QVERIFY(desk.backend.findBoards(text).isEmpty());
            desk.key(Qt::Key_O, Qt::ControlModifier);
            desk.type(text);
            QVERIFY2(desk.shows("board-" + board), qPrintable(text));
            QVERIFY(desk.item("board-" + board)->property("selected").toBool());
            desk.key(Qt::Key_Return);
            QCOMPARE(desk.panel(), QString());
            QCOMPARE(desk.backend.boardName(), board);
        }
        QCOMPARE(desk.library.boards().size(), 20);
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingLeftOpenWhileAwayGoesOnThroughACommandFromOutside() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Call"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QQuickItem *editor = desk.item("editor");
        QVERIFY(editor);
        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        // The desktop shows another workspace, with a terminal on it, where
        // an agent adds a note to the board by command.
        QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QVERIFY(desk.canvas->editing());
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "add", "desk", "note", "--text", "From outside", "--x", "600", "--y", "100"});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        QTRY_COMPARE(desk.board().count(), 2);
        // Nothing typed is lost to it, in the window or in the file...
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        auto written = [&desk] {
            QStringList texts;
            for (const QJsonValue &value : desk.onDisk().value("items").toArray())
                texts << value.toObject().value("text").toString();
            return texts;
        };
        QCOMPARE(written(), (QStringList{"Call Sam", "From outside"}));
        // ...and back in the window, the writing is still open with what was
        // typed in it, and the keys go on into it.
        desk.window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.window));
        QCOMPARE(desk.window->activeFocusItem(), editor);
        desk.type(" now");
        QCOMPARE(desk.board().count(), 2);
        desk.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), (QStringList{"Call Sam now", "From outside"}));
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingLeftOpenWhileAwayIsKeptWhenTheWindowIsClosed() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(224, 124, "Call"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QVERIFY(desk.canvas->editing());
        // The window is closed from outside while another workspace is shown:
        // a script, the session ending.
        QVERIFY(desk.window->close());
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray items = desk.onDisk().value("items").toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value("text").toString(), QString("Call Sam"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingLeftOpenWhileAwayIsSavedShouldTheAppBeKilled() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Call"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QVERIFY(desk.canvas->editing());
        // Saved while it is still being written, as the window says it is:
        // the app killed now, or crashing, nothing let go of and nothing
        // written on the way out, the board opens with what was typed.
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.canvas->editing());
        QJsonObject root;
        QCOMPARE(Library::read(desk.library.boardFile("desk"), root), QString());
        Board next;
        QCOMPARE(next.fromJson(root), QStringList());
        QCOMPARE(next.count(), 1);
        QCOMPARE(next.item(id)->text, QString("Call Sam"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNoteLeftOpenWhileAwayIsKeptFromACommandThatTakesItAway() {
        Desk desk;
        QVERIFY(desk.ready());
        // As main() has it, so what is counted is said as English says it.
        QCoreApplication::installTranslator(&desk.translator);
        const quint32 id = desk.add(note(224, 124, "Call"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        desk.doubleClick(QPointF(300, 200));
        desk.type(" Sam");
        QWindowSystemInterface::handleFocusWindowChanged(nullptr, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // Away in a terminal, an agent removes the note being written...
        qputenv("SPRAWL_HOME", desk.home.path().toUtf8());
        const int code = runCli({"sprawl", "remove", "desk", QString::number(id)});
        qunsetenv("SPRAWL_HOME");
        QCOMPARE(code, 0);
        // ...and it is kept, as it is while the window has the keys (see
        // aNoteOpenToBeWrittenInIsNotTakenAwayFromUnderIt): the window says
        // so, the file is given it back, and the writing goes on.
        QTRY_VERIFY(!desk.backend.notice().isEmpty());
        QCOMPARE(desk.backend.notice(), QString("Updated desk from outside, but kept 1 item as it is here"));
        QVERIFY(desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text, QString("Call Sam"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        const QJsonArray written = desk.onDisk().value("items").toArray();
        QCOMPARE(written.size(), 1);
        QCOMPARE(written.first().toObject().value("text").toString(), QString("Call Sam"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aSelectionGivenNewIdsIsToldOfOnce() {
        // A thousand notes, as the file has them.
        const int notes = 1000;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < notes; ++i)
                items.append(QJsonObject{{"id", i + 1}, {"type", "note"}, {"x", (i % 40) * 170.0},
                                         {"y", (i / 40) * 170.0}, {"text", "Mine"}});
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        const QVector<Item> base = desk.board().items();
        // All of them copied and pasted: the copies are what is selected, and
        // the file does not have them yet...
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.key(Qt::Key_C, Qt::ControlModifier);
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), notes);
        // ...when an agent writes as many notes of its own to it, which the
        // file numbers as the copies are numbered here.
        QVector<Item> theirs = base;
        for (int i = 0; i < notes; ++i) {
            Item added = note(i * 170.0, -400, "Theirs");
            added.id = quint32(notes + 1 + i);
            theirs.append(added);
        }
        QSignalSpy changed(desk.canvas, &CanvasView::selectionChanged);
        desk.board().merge(base, theirs, 2 * notes);
        QCoreApplication::processEvents();
        // The copies are given new ids, and are still what is selected: the
        // window is told so once, and not once for each.
        QCOMPARE(changed.count(), 1);
        QCOMPARE(desk.canvas->selectionCount(), notes);
        for (quint32 id : desk.canvas->selection())
            QVERIFY(id > quint32(2 * notes) && desk.board().item(id)->text == "Mine");
        QCOMPARE(desk.warnings, QStringList());
    }

    void nothingIsMovedFartherOutThanAFileHolds() {
        // Frames at the edge of the board, as far out as a file has them: as
        // a board made by hand or by the command line may put them.
        Desk desk("edge", [](const QString &home) {
            QDir(home).mkpath("boards/edge");
            write(home + "/boards/edge/board.json", R"({"sprawl": 1, "items": [
                {"id": 1, "type": "frame", "x": -1000000000, "y": 0, "w": 100, "h": 40},
                {"id": 2, "type": "frame", "x": 1000000000, "y": 300, "w": 100, "h": 40}]})");
        });
        QVERIFY(desk.ready());
        // Nudged on out, ten units at a time, and duplicated out past the
        // other edge: each stops at the edge.
        desk.canvas->select({1});
        for (int i = 0; i < 11; ++i)
            desk.key(Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(desk.board().item(1)->rect, QRectF(-1e9, 0, 100, 40));
        desk.canvas->select({2});
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.board().item(2)->rect, QRectF(1e9, 300, 100, 40));
        desk.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 3);
        QCOMPARE(desk.last()->rect, QRectF(1e9, 316, 100, 40));
        // The first, pulled in by its side at the edge, is as much narrower
        // as it was pulled: not a box of no width, or less.
        desk.canvas->setView(QPointF(-1000000100, -100), 1);
        desk.canvas->select({1});
        desk.drag(QPointF(-1000000003, 20), QPointF(-999999983, 20));
        QCOMPARE(desk.board().item(1)->rect, QRectF(-999999980, 0, 80, 40));
        // All of it reads back, and the board opens again.
        QVERIFY(desk.backend.flush());
        Board again;
        QCOMPARE(again.fromJson(desk.onDisk()), QStringList());
        QCOMPARE(again.count(), 3);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPictureFarLongerThanItIsWideIsRead() {
        // Thirty thousand pixels by one, and the same standing up: brought
        // down to the longest side a picture is kept at, each is still a
        // pixel across, and drawn.
        QTemporaryDir folder;
        const QImage lying = ImageStore::read(picture(folder.filePath("lying.png"), QSize(30000, 1)));
        QCOMPARE(lying.size(), QSize(ImageStore::longestSide, 1));
        const QImage standing = ImageStore::read(picture(folder.filePath("standing.png"), QSize(1, 30000)));
        QCOMPARE(standing.size(), QSize(1, ImageStore::longestSide));
    }

    // --- What is asked of the GPU ------------------------------------------------

    void aBigBoardIsDrawnAPartAtATime() {
        // Three thousand notes side by side, a scribble on every fifth.
        const int columns = 60, count = 3000;
        Desk desk("big", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < count; ++i) {
                const double x = (i % columns) * 170.0, y = (i / columns) * 170.0;
                items.append(QJsonObject{{"type", "note"}, {"x", x}, {"y", y},
                                         {"text", QString("Note %1\nwith a line under it").arg(i)}});
            }
            for (int i = 0; i < count; i += 5) {
                QJsonArray points;
                for (int p = 0; p < 120; ++p) {
                    points.append(p * 1.2);
                    points.append(20 + 12 * std::sin(p * 0.35 + i));
                }
                items.append(QJsonObject{{"type", "stroke"}, {"x", (i % columns) * 170.0 + 4},
                                         {"y", (i / columns) * 170.0 + 90}, {"points", points}});
            }
            QDir(home).mkpath("boards/big");
            write(home + "/boards/big/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.board().count(), count + count / 5);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const qint64 budget = desk.canvas->loadBudget();
        auto at = [&desk](QPointF origin, qreal zoom) {
            desk.canvas->setView(origin, zoom);
            // A frame, start to finish.
            desk.window->grabWindow();
            return desk.canvas->load();
        };

        // At 100% only the part of the board near the window is drawn at
        // all, and all of that in full.
        const SceneLoad close = at(QPointF(0, 0), 1);
        QVERIFY2(close.buckets >= 40, qPrintable(QString::number(close.buckets)));
        QVERIFY2(close.shown <= 4, qPrintable(QString::number(close.shown)));
        // (Sixty-odd things of 3,600: what a window of this size shows is
        // twenty-four notes.)
        QVERIFY2(close.items > 20 && close.items < 90, qPrintable(QString::number(close.items)));
        QCOMPARE(close.strokeDetail, 0);
        QVERIFY(close.smallestText <= 12);
        QVERIFY(!close.reduced && !close.strokesHidden);
        QVERIFY(close.bytes > 0 && close.bytes < budget);

        // Somewhere else on the board it is another part, and no more of it.
        const SceneLoad elsewhere = at(QPointF(6000, 5000), 1);
        QVERIFY(elsewhere.shown <= 8);
        QVERIFY2(elsewhere.items > 20 && elsewhere.items < 120, qPrintable(QString::number(elsewhere.items)));

        // Halfway out, strokes lose the detail that no longer shows; type
        // is still read, and still drawn.
        const SceneLoad half = at(QPointF(0, 0), 0.5);
        QCOMPARE(half.strokeDetail, 1);
        QVERIFY(half.smallestText <= 12);
        QVERIFY(!half.reduced);

        // All the way out the whole board is in view. Type that small is
        // not drawn, strokes are at their plainest, and the frame is a
        // fraction of what the board would be in full.
        const SceneLoad far = at(QPointF(-200, -200), 0.1);
        QVERIFY(far.shown > 30);
        QVERIFY(far.items > 2500);
        QCOMPARE(far.strokeDetail, 2);
        QVERIFY(far.smallestText > 13);
        QVERIFY(!far.reduced);
        QVERIFY2(far.bytes < budget, qPrintable(QString::number(far.bytes)));
        QVERIFY2(far.triangles < 60000, qPrintable(QString::number(far.triangles)));

        // What is in view never comes to more than a frame is allowed. With
        // less allowed, type is the first to go...
        QSignalSpy changed(desk.canvas, &CanvasView::loadChanged);
        desk.canvas->setLoadBudget(half.bytes - 1);
        const SceneLoad tight = at(QPointF(0, 0), 0.5);
        QVERIFY(tight.reduced);
        QVERIFY(tight.bytes <= half.bytes - 1);
        QVERIFY(tight.smallestText > 12);
        QCOMPARE(tight.strokeDetail, 1);
        QVERIFY(!tight.strokesHidden);
        // The window is told, so that it can say so.
        QVERIFY(desk.canvas->simplified());
        QTRY_VERIFY(changed.count() >= 1);
        // ...and with next to nothing allowed, the strokes go too, and what
        // is left is the paper.
        desk.canvas->setLoadBudget(1000);
        const SceneLoad least = at(QPointF(0, 0), 0.5);
        QVERIFY(least.reduced && least.strokesHidden);
        QVERIFY(least.smallestText > 13);
        QVERIFY(least.bytes < tight.bytes);
        // Allowed again, it all comes back.
        desk.canvas->setLoadBudget(budget);
        const SceneLoad again = at(QPointF(0, 0), 0.5);
        QVERIFY(!again.reduced && !again.strokesHidden);
        QVERIFY(again.smallestText <= 12);
        QCOMPARE(again.strokeDetail, 1);
        // (At least what it was: parts of the board that were in view a
        // moment ago are kept a little longer than new ones are let in.)
        QVERIFY(again.bytes >= half.bytes && again.bytes < budget);
        QTRY_VERIFY(!desk.canvas->simplified());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPileIsNeverMoreThanAFrame() {
        // Six thousand notes, all in one place and its surroundings.
        Desk desk("pile", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 6000; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 40) * 30.0}, {"y", (i / 40 % 30) * 30.0}});
            QDir(home).mkpath("boards/pile");
            write(home + "/boards/pile/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // With all of it in view.
        desk.canvas->setView(QPointF(-100, -100), 0.5);
        desk.window->grabWindow();
        const SceneLoad whole = desk.canvas->load();
        QCOMPARE(whole.items, 6000);
        QVERIFY(!whole.reduced);
        // Allowed a third of what that comes to, a third or so is drawn:
        // the middle of the view, and never nothing.
        desk.canvas->setLoadBudget(whole.bytes / 3);
        desk.window->grabWindow();
        const SceneLoad some = desk.canvas->load();
        QVERIFY(some.reduced);
        QVERIFY2(some.bytes <= whole.bytes / 3, qPrintable(QString::number(some.bytes)));
        QVERIFY2(some.items > 500 && some.items < 2500, qPrintable(QString::number(some.items)));
        desk.canvas->setLoadBudget(1);
        desk.window->grabWindow();
        QVERIFY(desk.canvas->load().items > 0);
        QVERIFY(desk.canvas->load().items < 1500);
    }

    void thingsStayDrawnWhereverTheyAreMoved() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(100, 100, "Far"));
        desk.add(note(400, 100, "Near"));
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 2);
        // Carried to another part of the board entirely, out of view...
        Item moved = *desk.board().item(id);
        moved.rect.moveTo(30000, 20000);
        desk.board().set(moved);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().buckets, 2);
        QCOMPARE(desk.canvas->load().items, 1);
        // ...it is there when the view gets to it,
        desk.canvas->setView(QPointF(29800, 19800), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        QCOMPARE(desk.canvas->load().shown, 1);
        // and undo brings it back to where the other one is.
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        desk.canvas->setView(QPointF(0, 0), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().buckets, 1);
        QCOMPARE(desk.canvas->load().items, 2);
        // A thing too big for one part of the board is drawn from wherever
        // any of it shows.
        Item frame;
        frame.kind = Item::Kind::Frame;
        frame.rect = QRectF(-5000, -5000, 12000, 9000);
        desk.add(frame);
        desk.canvas->setView(QPointF(6500, 3500), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
    }

    void aStrokeThatNeverEndsIsSeveral() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        desk.press(QPointF(100, 100));
        // Back and forth across the board, a pixel at a time.
        int x = 100, step = 1, y = 100;
        for (int i = 0; i < 4500; ++i) {
            x += step;
            if (x >= 900 || x <= 100) {
                step = -step;
                y += 3;
            }
            desk.moveTo(QPointF(x, y));
        }
        desk.release(QPointF(x, y));
        const auto strokes = desk.all(Item::Kind::Stroke);
        QCOMPARE(strokes.size(), 2);
        // The second carries on from where the first was put down.
        const QPointF end = strokes[0]->points.last() + strokes[0]->rect.topLeft();
        const QPointF start = strokes[1]->points.first() + strokes[1]->rect.topLeft();
        QCOMPARE(start, end);
        QCOMPARE(desk.warnings, QStringList());
    }

    // --- What a frame may hold ----------------------------------------------

    void aStrokeIsCutAPointAtATimeAsItWouldBeWhole() {
        // A scribble with loops, sharp turns and a point drawn twice.
        QVector<QPointF> points;
        for (int i = 0; i < 160; ++i)
            points.append(QPointF(i * 6 + 30 * std::cos(i * 0.7), 200 + 90 * std::sin(i * 0.31) + (i % 9) * 4));
        points[80] = points[79];
        auto bytes = [](const Geo::StrokeMesh &mesh) {
            return QByteArray(reinterpret_cast<const char *>(mesh.vertices.constData()),
                              mesh.vertices.size() * qsizetype(sizeof(Geo::StrokeVertex)));
        };
        for (const qreal scale : {4.0, 1.0, 0.1}) {
            Geo::Pen pen(3, 0xff00ff00, scale);
            for (int count = 1; count <= points.size(); ++count) {
                pen.add(points[count - 1]);
                // After every point, the same triangles as the whole line so
                // far cut at once: the end is cut again, and the rest kept.
                Geo::StrokeMesh whole;
                Geo::strokePolyline(whole, Geo::smooth(points.first(count), pen.tolerance()), {}, 3, 0xff00ff00,
                                    Geo::Cap::Round, Geo::Join::Round, scale);
                QCOMPARE(pen.mesh().indices, whole.indices);
                QCOMPARE(bytes(pen.mesh()), bytes(whole));
            }
        }
    }

    void aStrokeThePenThinksAgainAboutIsCutAsItWouldBeWhole() {
        // The scribble of aStrokeIsCutAPointAtATimeAsItWouldBeWhole(), and
        // after most of its points the pen thinks again about the last one or
        // two, as it does finding a corner it went by (see
        // Geo::cornerBefore()): takes them back, and goes on through a corner
        // short of them, given twice, and them again.
        QVector<QPointF> points;
        for (int i = 0; i < 160; ++i)
            points.append(QPointF(i * 6 + 30 * std::cos(i * 0.7), 200 + 90 * std::sin(i * 0.31) + (i % 9) * 4));
        auto bytes = [](const Geo::StrokeMesh &mesh) {
            return QByteArray(reinterpret_cast<const char *>(mesh.vertices.constData()),
                              mesh.vertices.size() * qsizetype(sizeof(Geo::StrokeVertex)));
        };
        for (const qreal scale : {4.0, 1.0, 0.1}) {
            Geo::Pen pen(3, 0xff00ff00, scale);
            QVector<QPointF> line;
            for (qsizetype i = 0; i < points.size(); ++i) {
                pen.add(points[i]);
                line.append(points[i]);
                if (const int back = int(i % 3); back > 0 && line.size() > back + 1) {
                    const QPointF corner = line[line.size() - back - 1] + QPointF(2, -3);
                    const QVector<QPointF> again = QVector<QPointF>{corner, corner} + line.mid(line.size() - back);
                    pen.redraw(back, again);
                    line.resize(line.size() - back);
                    line += again;
                }
                // After every point, the same triangles as the whole line so
                // far cut at once: what was taken back is cut again, and the
                // rest kept.
                QCOMPARE(pen.points(), line);
                Geo::StrokeMesh whole;
                Geo::strokePolyline(whole, Geo::smooth(line, pen.tolerance()), {}, 3, 0xff00ff00, Geo::Cap::Round,
                                    Geo::Join::Round, scale);
                QCOMPARE(pen.mesh().indices, whole.indices);
                QCOMPARE(bytes(pen.mesh()), bytes(whole));
            }
        }
    }

    void aStrokeIsSimplifiedOnlyAsFarAsItShows() {
        // A line drawn slowly, a point every pixel or two on whole pixels, as
        // the mouse gives them: waving, then turning a corner and back to
        // waving, then a corner again.
        QVector<QPointF> points;
        for (int i = 0; i <= 300; ++i)
            points.append(QPointF(std::round(100 + i * 1.3), std::round(200 + 12 * std::sin(i * 0.05))));
        const QPointF first = points.last();
        points.append(first);
        for (int i = 1; i <= 200; ++i)
            points.append(QPointF(std::round(first.x() + 8 * std::sin(i * 0.07)), std::round(first.y() + i * 1.5)));
        const QPointF second = points.last();
        points.append(second);
        for (int i = 1; i <= 100; ++i)
            points.append(QPointF(second.x() - i * 2, second.y()));
        const qreal tolerance = 0.35;
        const QVector<QPointF> kept = Geo::simplifyCurve(points, tolerance);
        // It keeps fewer, each of them a point it was given, in order...
        QVERIFY2(kept.size() < points.size(), qPrintable(QString::number(kept.size())));
        qsizetype at = 0;
        for (const QPointF &point : kept) {
            while (at < points.size() && points[at] != point)
                ++at;
            QVERIFY2(at < points.size(), qPrintable(QString("(%1, %2)").arg(point.x()).arg(point.y())));
            ++at;
        }
        // ...every one the path needs (see Geo::simplify())...
        for (const QPointF &point : Geo::simplify(points, tolerance))
            QVERIFY(kept.contains(point));
        // ...and both corners, each given twice, to come to a point at.
        for (const QPointF &corner : {first, second})
            QCOMPARE(kept.count(corner), 2);
        QCOMPARE(kept.indexOf(first) + 1, kept.lastIndexOf(first));
        QCOMPARE(kept.indexOf(second) + 1, kept.lastIndexOf(second));
        // The curve through them is nowhere further than `tolerance` from the
        // curve through them all, measured cut a twentieth of it finely:
        // which is what the stroke being drawn showed.
        const QVector<QPointF> drawn = Geo::smooth(points, tolerance / 20);
        const QVector<QPointF> simplified = Geo::smooth(kept, tolerance / 20);
        const qreal apart = qMax(farthest(drawn, simplified), farthest(simplified, drawn));
        QVERIFY2(apart < tolerance * 1.1, qPrintable(QString::number(apart)));
    }

    void aLineThatWavesLessThanShowsIsKeptStraight() {
        // Lines three and four thousand points long, as long as a stroke
        // gets: a point every 0.9 of a pixel waving a fifth of a pixel either
        // side, a wave every seven points or so, and a point every 0.8
        // wandering up to 0.15 of a pixel off at random, as a hand on a
        // tablet does. The curve through either strays less than a third of
        // a pixel from the line between its ends, which is all it needs: kept
        // point by point instead, wave by wave, it would be hundreds of
        // points, and a long while finding them.
        const qreal tolerance = 0.35;
        QRandomGenerator random(5);
        for (const int count : {3000, 4096}) {
            QVector<QPointF> wave, wander;
            for (int i = 0; i < count; ++i) {
                wave.append(QPointF(100 + i * 0.9, 300 + 0.2 * std::sin(i * 0.9)));
                wander.append(QPointF(100 + i * 0.8 + 0.15 * (random.generateDouble() * 2 - 1),
                                      300 + 0.15 * (random.generateDouble() * 2 - 1)));
            }
            for (const auto &[name, line] : {std::pair{"waving", wave}, std::pair{"wandering", wander}}) {
                const QVector<QPointF> kept = Geo::simplifyCurve(line, tolerance);
                QVERIFY2(kept.size() <= 4,
                         qPrintable(QString("%1 points %2 keeps %3").arg(count).arg(name).arg(kept.size())));
                const QVector<QPointF> drawn = Geo::smooth(line, tolerance / 20);
                const QVector<QPointF> simplified = Geo::smooth(kept, tolerance / 20);
                const qreal apart = qMax(farthest(drawn, simplified), farthest(simplified, drawn));
                QVERIFY2(apart < tolerance, qPrintable(QString("%1 points %2: %3").arg(count).arg(name).arg(apart)));
            }
        }
    }

    void aStrokeDrawnFarOutIsNoMoreTrianglesThanCloseUp() {
        // One scribble on screen, a zigzag forty pixels a step that turns
        // every third, drawn at 100%, 25% and 10%: the same pixels, ten
        // times as many units across at the last.
        QVector<QPointF> screen;
        for (int i = 0; i < 1500; ++i)
            screen.append(QPointF((i / 3 % 2 ? 3 - i % 3 : i % 3) * 40 + (i * 7) % 5, i * 0.6 + (i * 13) % 7));
        auto cut = [&screen](qreal scale) {
            Geo::Pen pen(1, 0xffffffff, scale);
            for (const QPointF &point : std::as_const(screen))
                pen.add(point / scale);
            return pen.triangles();
        };
        const qsizetype close = cut(1), quarter = cut(0.25), far = cut(0.1);
        QVERIFY2(quarter <= close && far <= close, qPrintable(QString("%1 %2 %3").arg(close).arg(quarter).arg(far)));
        // Cut a tenth of a unit fine whatever the zoom, as a stroke on the
        // board is, it would be more than twice that far out.
        QVector<QPointF> units;
        for (const QPointF &point : std::as_const(screen))
            units.append(point / 0.1);
        Geo::StrokeMesh fine;
        Geo::strokePolyline(fine, Geo::smooth(units, Geo::strokeTolerance), {}, 1, 0xffffffff);
        QVERIFY2(fine.indices.size() / 3 > 2 * far, qPrintable(QString::number(fine.indices.size() / 3)));

        // Round ends and corners are cut by how big they are on screen: a
        // dot a pixel or two across is five or six triangles, and the dot of
        // the widest pen up close as many as on the board.
        auto dot = [](qreal halfWidth, qreal scale) {
            Geo::Pen pen(halfWidth, 0xffffffff, scale);
            pen.add(QPointF(0, 0));
            return pen.triangles();
        };
        Geo::StrokeMesh board;
        Geo::strokePolyline(board, {QPointF(0, 0)}, {}, 32, 0xffffffff);
        QCOMPARE(dot(1, 0.1), 5);
        QCOMPARE(dot(1, 1), 6);
        QCOMPARE(dot(32, 8), board.indices.size() / 3);
    }

    void theStrokeBeingDrawnIsPartOfTheFrame() {
        // A page of handwriting, seven hundred short strokes, which is most
        // of what a frame may hold however far out it is seen from.
        Desk desk("page", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 700; ++i)
                items.append(QJsonObject{{"type", "stroke"}, {"x", (i % 35) * 28.0}, {"y", (i / 35) * 34.0},
                                         {"points", QJsonArray{0, 0, 4, 8, 8, 0, 12, 8}}});
            QDir(home).mkpath("boards/page");
            write(home + "/boards/page/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        // A fast scribble, as a hand scribbling something out makes one and
        // never lifts: round and round, a seventh of a loop a hundred and
        // sixty pixels across at each step, wandering down the window and up
        // again. Curved all the way round, it is many triangles to a point;
        // a hand shading back and forth comes to a point at every turn.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        QMap<qreal, qsizetype> pieces;
        for (const qreal zoom : {1.0, 0.25, 0.1}) {
            desk.canvas->setView(QPointF(0, 0), zoom);
            const qsizetype before = desk.all(Item::Kind::Stroke).size();
            frames.all.clear();
            const int steps = 3000;
            desk.press(scribble(0));
            for (int i = 1; i <= steps; ++i) {
                desk.moveTo(scribble(i));
                if (i % 20 == 0)
                    desk.window->grabWindow();
            }
            desk.release(scribble(steps));
            desk.window->grabWindow();

            // In every frame the stroke being drawn was no more than its
            // share, and with it the frame was no more than it may be: the
            // board gave way to it. And that is what the GPU was handed.
            QVERIFY(frames.all.size() > steps / 20);
            const QString wrong = frames.wrong(budget);
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
            qint64 most = 0;
            bool gaveWay = false;
            for (const Frames::Frame &frame : std::as_const(frames.all)) {
                QVERIFY2(frame.load.draft <= budget / 4, qPrintable(QString::number(frame.load.draft)));
                most = qMax(most, frame.load.draft);
                gaveWay |= frame.load.reduced;
            }
            QVERIFY2(most > budget / 8, qPrintable(QString::number(most)));
            QVERIFY(gaveWay);
            QCOMPARE(desk.canvas->load().draft, qint64(0));

            // Fewer points than a stroke is given, but more triangles: the
            // scribble went on the board as several strokes, each carrying
            // on from where the last one stopped.
            const auto strokes = desk.all(Item::Kind::Stroke).mid(before);
            pieces[zoom] = strokes.size();
            QVERIFY2(strokes.size() >= 2, qPrintable(QString::number(strokes.size())));
            QCOMPARE(strokes.first()->points.first() + strokes.first()->rect.topLeft(), scribble(0));
            for (qsizetype i = 1; i < strokes.size(); ++i)
                QCOMPARE(strokes[i]->points.first() + strokes[i]->rect.topLeft(),
                         strokes[i - 1]->points.last() + strokes[i - 1]->rect.topLeft());
            QCOMPARE(strokes.last()->points.last() + strokes.last()->rect.topLeft(), scribble(steps));
        }
        // The same scribble on screen is no more triangles drawn far out.
        QVERIFY(pieces[0.25] <= pieces[1.0]);
        QVERIFY(pieces[0.1] <= pieces[1.0]);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theBoardHoldsStillWhileAStrokeGoesDownInPieces() {
        // The page of handwriting of theStrokeBeingDrawnIsPartOfTheFrame(),
        // and its scribble, which goes on the board in pieces and which the
        // board gives way to as it grows.
        Desk desk("page", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 700; ++i)
                items.append(QJsonObject{{"type", "stroke"}, {"x", (i % 35) * 28.0}, {"y", (i / 35) * 34.0},
                                         {"points", QJsonArray{0, 0, 4, 8, 8, 0, 12, 8}}});
            QDir(home).mkpath("boards/page");
            write(home + "/boards/page/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.key(Qt::Key_P);
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        // Drawn in a color of its own at each zoom, so that the frames tell
        // it apart from the rest of the board.
        const QVector<std::pair<qreal, QString>> rounds{{1.0, "red"}, {0.25, "green"}};
        for (const auto &[zoom, color] : rounds) {
            desk.canvas->setView(QPointF(0, 0), zoom);
            desk.canvas->setPenColor(color);
            desk.window->grabWindow();
            const int before = desk.board().count();
            Frames frames(desk);
            frames.marked = desk.theme.swatch(color);
            const int steps = 3000;
            desk.press(scribble(0));
            for (int i = 1; i <= steps; ++i) {
                desk.moveTo(scribble(i));
                if (i % 20 == 0)
                    desk.window->grabWindow();
            }
            const QVector<Frames::Frame> drawing = frames.all;
            desk.release(scribble(steps));
            QVERIFY(desk.board().count() >= before + 3);
            // Each piece put down leaves the stroke being drawn next to
            // nothing, until the next has grown as big. From the first one
            // until the pen is lifted, the rest of the board keeps to what
            // the biggest leaves it, and to what the pieces leave it (see
            // theWholeStrokeIsDrawnWhileItGoesDownInPieces()): what it left
            // out stays out, rather than coming back with every piece only to
            // go again as the next one grows.
            auto rest = [](const Frames::Frame &frame) { return frame.load.bytes - (frame.inMarked - frame.load.draft); };
            qsizetype first = 0;
            while (first < drawing.size() && drawing[first].items == before)
                ++first;
            QVERIFY(first > 0 && first + 20 < drawing.size());
            QVERIFY(drawing[first - 1].load.reduced);
            for (qsizetype i = first; i < drawing.size(); ++i) {
                const Frames::Frame &was = drawing[i - 1], &is = drawing[i];
                QVERIFY2(is.load.reduced && rest(is) <= rest(was) &&
                             is.load.items - was.load.items <= is.items - was.items,
                         qPrintable(QString("at %1%, frame %2 drew %3 bytes of %4 items besides the stroke, after %5 "
                                            "of %6")
                                        .arg(zoom * 100)
                                        .arg(i)
                                        .arg(rest(is))
                                        .arg(is.load.items)
                                        .arg(rest(was))
                                        .arg(was.load.items)));
            }
            const QString wrong = frames.wrong(desk.canvas->loadBudget());
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokeBeingDrawnFollowsTheZoom() {
        Desk desk;
        QVERIFY(desk.ready());
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        desk.canvas->setView(QPointF(0, 0), 0.1);
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 560 + qRound(70 * std::cos(round)), y = 270 + qRound(70 * std::sin(round)) + i / 4;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        // A scribble far out, with the pen still down...
        desk.press(scribble(0));
        int step = 1;
        for (; step <= 600; ++step)
            desk.moveTo(scribble(step));
        desk.window->grabWindow();
        const qint64 far = desk.canvas->load().draft;
        QVERIFY(far > 0 && far <= budget / 4);
        QCOMPARE(desk.board().count(), 0);
        // ...is cut finer when it is zoomed in on (as a wheel or a pinch
        // zooms, around the pen), as it then shows...
        const QPointF pen = desk.canvas->toItem(scribble(step - 1));
        desk.canvas->zoomBy(1.2, pen);
        desk.window->grabWindow();
        QVERIFY2(desk.canvas->load().draft > far, qPrintable(QString::number(desk.canvas->load().draft)));
        // ...and once that is more than it may be, what there is of it goes
        // on the board and drawing carries on from there.
        for (int notch = 0; notch < 20 && desk.canvas->zoom() < 1; ++notch) {
            desk.canvas->zoomBy(1.2, pen);
            desk.window->grabWindow();
        }
        QVERIFY(desk.canvas->zoom() >= 1);
        QVERIFY(desk.board().count() >= 1);
        for (int more = 0; more < 100; ++more, ++step)
            desk.moveTo(scribble(step));
        desk.release(scribble(step - 1));
        desk.window->grabWindow();
        for (const Frames::Frame &frame : std::as_const(frames.all))
            QVERIFY2(frame.load.draft <= budget / 4, qPrintable(QString::number(frame.load.draft)));
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        const auto strokes = desk.all(Item::Kind::Stroke);
        QVERIFY(strokes.size() >= 2);
        for (qsizetype i = 1; i < strokes.size(); ++i)
            QCOMPARE(strokes[i]->points.first() + strokes[i]->rect.topLeft(),
                     strokes[i - 1]->points.last() + strokes[i - 1]->rect.topLeft());
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokeBeingDrawnIsCutForTheViewThatIsDrawn() {
        Desk desk;
        QVERIFY(desk.ready());
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        desk.canvas->setView(QPointF(0, 0), 0.1);
        // The scribble of theStrokeBeingDrawnFollowsTheZoom(), far out, with
        // the pen still down: within what it may be there, and more than
        // that close up.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 560 + qRound(70 * std::cos(round)), y = 270 + qRound(70 * std::sin(round)) + i / 4;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        desk.press(scribble(0));
        for (int step = 1; step <= 600; ++step)
            desk.moveTo(scribble(step));
        desk.window->grabWindow();
        const qint64 far = desk.canvas->load().draft;
        QVERIFY(far > 0);
        QCOMPARE(desk.board().count(), 0);
        // Zoomed in close and back out before the next frame (a pinch that
        // turns back, say), it is cut for the view that frame shows, which is
        // the one it was cut for: not for the one in between, which no frame
        // showed, and where it would have been more than it may be.
        const QPointF pen = desk.canvas->toItem(scribble(600));
        desk.canvas->zoomBy(10, pen);
        desk.canvas->zoomBy(0.1, pen);
        desk.window->grabWindow();
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.canvas->load().draft, far);
        // Once that view is drawn, what there is of it goes on the board.
        desk.canvas->zoomBy(10, pen);
        desk.window->grabWindow();
        QCOMPARE(desk.board().count(), 1);
        // Let go of there, before the pen went any further, it leaves no dot
        // of its own where the next piece would have started.
        desk.release(scribble(600));
        QCOMPARE(desk.board().count(), 1);
        QCOMPARE(desk.last()->points.last() + desk.last()->rect.topLeft(), scribble(600));
        for (const Frames::Frame &frame : std::as_const(frames.all))
            QVERIFY2(frame.load.draft <= budget / 4, qPrintable(QString::number(frame.load.draft)));
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aStrokeInSeveralPiecesActsAsOne() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // The fast scribble of theStrokeBeingDrawnIsPartOfTheFrame(), which
        // goes on the board in pieces while the pen is still down.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 3000;
        auto draw = [&] {
            desk.press(scribble(0));
            for (int i = 1; i <= steps; ++i)
                desk.moveTo(scribble(i));
        };
        // Escape while drawing backs out of all of it, the pieces already put
        // down too, and leaves nothing to undo.
        draw();
        QVERIFY(desk.board().count() >= 1);
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());
        desk.release(scribble(steps));
        QCOMPARE(desk.board().count(), 0);
        QCOMPARE(desk.canvas->tool(), QString("pen"));

        // Let go of, it is one thing to pick up, and one step of undo,
        // however many strokes it is.
        draw();
        desk.release(scribble(steps));
        const int pieces = desk.board().count();
        QVERIFY(pieces >= 2);
        desk.key(Qt::Key_V);
        desk.click(scribble(0));
        QCOMPARE(desk.canvas->selectionCount(), pieces);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 0);
        QVERIFY(!desk.canvas->canUndo());
        desk.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk.board().count(), pieces);

        // Taken from it mid-stroke, as when something else takes the mouse,
        // it keeps what it drew, and that is one step of undo all the same.
        desk.key(Qt::Key_P);
        draw();
        desk.canvas->ungrabMouse();
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().count(), 2 * pieces);
        desk.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), pieces);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokeBeingDrawnFitsWhatTheBoardLeaves() {
        Desk desk;
        QVERIFY(desk.ready());
        // A hundred short strokes crowded into a corner of the board, each
        // too light to be kept in levels of detail and so drawn whatever
        // happens; and the part of the board nearest the middle of the view
        // is never left out. It can give way by none of them.
        for (int i = 0; i < 100; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 40 + (i % 10) * 24, 40 + (i / 10) * 30));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad crowded = desk.canvas->load();
        QCOMPARE(crowded.floor, crowded.bytes);
        // A frame with room for those and a sixth of itself besides, which is
        // less than the quarter the stroke being drawn is given.
        const qint64 budget = crowded.bytes * 6 / 5, room = budget - crowded.bytes;
        desk.canvas->setLoadBudget(budget);
        Frames frames(desk);
        desk.key(Qt::Key_P);
        // A fast scribble over them.
        auto scribble = [&desk](int i) {
            const int x = 120 + (i / 3 % 2 ? 3 - i % 3 : i % 3) * 40 + (i * 7) % 5;
            const int y = 60 + qRound(300 * std::abs(std::sin(i * 0.004))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 1500;
        desk.press(scribble(0));
        for (int i = 1; i <= steps; ++i) {
            desk.moveTo(scribble(i));
            if (i % 20 == 0)
                desk.window->grabWindow();
        }
        desk.release(scribble(steps));
        desk.window->grabWindow();
        // The stroke being drawn was given what the board left, and went on
        // to fill it, and no frame came to more than it may.
        QVERIFY(frames.all.size() > steps / 20);
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        qint64 most = 0;
        for (const Frames::Frame &frame : std::as_const(frames.all)) {
            QVERIFY2(frame.load.draft <= room, qPrintable(QString::number(frame.load.draft)));
            most = qMax(most, frame.load.draft);
        }
        QVERIFY2(most > room / 2, qPrintable(QString::number(most)));
        QVERIFY(desk.all(Item::Kind::Stroke).size() >= 100 + 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokeBeingDrawnMakesRoomForWhatComesIntoView() {
        Desk desk;
        QVERIFY(desk.ready());
        // The crowded corner of theStrokeBeingDrawnFitsWhatTheBoardLeaves(),
        // a little way off to the right, and a frame with room for it and a
        // sixth of itself besides.
        for (int i = 0; i < 100; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 2088 + (i % 10) * 24, 40 + (i / 10) * 30));
        desk.canvas->setView(QPointF(2048, 0), 1);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad crowded = desk.canvas->load();
        QCOMPARE(crowded.floor, crowded.bytes);
        const qint64 budget = crowded.bytes * 6 / 5, room = budget - crowded.bytes;
        desk.canvas->setLoadBudget(budget);
        // Away from it, a scribble comes to more than that would leave it.
        desk.canvas->setView(QPointF(0, 0), 1);
        desk.key(Qt::Key_P);
        Frames frames(desk);
        auto scribble = [&desk](int i) {
            const int x = 420 + (i / 3 % 2 ? 3 - i % 3 : i % 3) * 40, y = 120 + i / 2;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        desk.press(scribble(0));
        int step = 1;
        for (; step <= 900 && desk.canvas->load().draft <= room; ++step) {
            desk.moveTo(scribble(step));
            if (step % 20 == 0)
                desk.window->grabWindow();
        }
        QVERIFY(desk.canvas->load().draft > room);
        const int down = desk.board().count();
        // With the pen held still, the view goes over the crowd, which the
        // board cannot give way by. No frame comes to more than it may, the
        // first to show the crowd included: that one is the first to know
        // what the crowd holds, so it goes without the stroke being drawn,
        // which is then put down as far as it had got, to leave the frames
        // after it their room, and drawing carries on from there.
        const qsizetype away = frames.all.size();
        desk.canvas->panBy(2048, 0);
        desk.window->grabWindow();
        QTRY_COMPARE(desk.board().count(), down + 1);
        desk.window->grabWindow();
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        qsizetype first = away;
        while (first < frames.all.size() && frames.all[first].load.floor < crowded.floor)
            ++first;
        QVERIFY(first < frames.all.size());
        QCOMPARE(frames.all[first].load.draft, qint64(0));
        QCOMPARE(frames.all[first].items, down);
        const qint64 drawn = desk.canvas->load().draft;
        QVERIFY2(drawn > 0 && drawn <= room, qPrintable(QString::number(drawn)));
        desk.release(scribble(step - 1));
        QCOMPARE(desk.board().count(), down + 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theSelectionIsPartOfTheFrame() {
        Desk desk;
        QVERIFY(desk.ready());
        // Two hundred notes, and the view far enough out to see all of them.
        for (int i = 0; i < 200; ++i)
            desk.add(note((i % 20) * 170, (i / 20) * 170));
        desk.canvas->setView(QPointF(-100, -100), 0.25);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const qint64 notes = desk.canvas->load().bytes;
        QCOMPARE(desk.canvas->load().items, 200);
        QCOMPARE(desk.canvas->load().chrome, qint64(0));
        // A frame with room for the notes, and not for an outline around each
        // as well.
        const qint64 budget = notes * 3 / 2;
        desk.canvas->setLoadBudget(budget);
        Frames frames(desk);
        desk.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(desk.canvas->selectionCount(), 200);
        desk.window->grabWindow();
        // The outlines (as much again as the notes' paper) are drawn whole,
        // so the board gives way to them, as it does to what is being drawn.
        const SceneLoad load = desk.canvas->load();
        QCOMPARE(load.chrome, notes);
        QVERIFY(load.reduced);
        QVERIFY(!frames.all.isEmpty());
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QCOMPARE(desk.warnings, QStringList());
    }

    void textIsLeftOutOnlyWhenItHasToBe() {
        // Twelve notes, a title and a line under it on each, and below them
        // five long scribbles that in full come to more than a frame may hold.
        Desk desk("notes", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 12; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 4) * 170.0}, {"y", (i / 4) * 170.0},
                                         {"text", QString("Note %1\nwith a line under it").arg(i)}});
            for (int i = 0; i < 5; ++i) {
                QJsonArray points;
                for (int p = 0; p < 2500; ++p) {
                    points.append(p * 0.25 + 20 * std::sin(p * 0.21));
                    points.append(60 * std::sin(p * 0.35) + 30 * std::cos(p * 0.11));
                }
                items.append(QJsonObject{{"type", "stroke"}, {"x", 40 + i * 60.0}, {"y", 560}, {"points", points}});
            }
            QDir(home).mkpath("boards/notes");
            write(home + "/boards/notes/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.canvas->setView(QPointF(-20, -20), 1);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // The pieces of writing that held, in the scene graph: a note's
        // title, or the line under it.
        auto writing = [&frames] { return frames.all.last().texts; };

        // All in view at 100%, the scribbles give up a level of their curves.
        // What the notes say went first, to make way for them, and that level
        // leaves more than enough room for it: it comes back.
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 17);
        QVERIFY(full.reduced);
        QCOMPARE(full.strokeDetail, 1);
        QVERIFY(!full.strokesHidden);
        QVERIFY2(full.smallestText <= 12, qPrintable(QString::number(full.smallestText)));
        QCOMPARE(writing(), 24);
        QVERIFY(full.bytes <= budget);

        // With just too little room left for all of it, the largest comes
        // back, the titles, and the lines under them do not...
        const SceneLoad titles = frame(full.bytes - 1);
        QCOMPARE(titles.strokeDetail, 1);
        QVERIFY2(titles.smallestText > 12 && titles.smallestText <= 13,
                 qPrintable(QString::number(titles.smallestText)));
        QCOMPARE(writing(), 12);
        QVERIFY(titles.bytes <= full.bytes - 1);
        // ...and with too little for the titles, none of it does.
        const SceneLoad none = frame(titles.bytes - 1);
        QCOMPARE(none.strokeDetail, 1);
        QVERIFY(none.smallestText > 13);
        QCOMPARE(writing(), 0);
        QVERIFY(none.bytes <= titles.bytes - 1);
        // What all the writing comes to.
        const qint64 text = full.bytes - none.bytes;

        // Allowed less than the scribbles come to at that level, they give
        // up another, and all the writing is back...
        const SceneLoad coarser = frame(none.bytes - 1);
        QCOMPARE(coarser.strokeDetail, 2);
        QVERIFY(!coarser.strokesHidden);
        QVERIFY(coarser.smallestText <= 12);
        QCOMPARE(writing(), 24);
        QVERIFY(coarser.bytes <= none.bytes - 1);
        // ...and allowed less than they come to at their plainest, they go,
        // and it stays.
        const SceneLoad plain = frame(coarser.bytes - text - 1);
        QVERIFY(plain.strokesHidden);
        QVERIFY(plain.smallestText <= 12);
        QCOMPARE(writing(), 24);
        QCOMPARE(plain.items, 17);
        QVERIFY(plain.bytes <= coarser.bytes - text - 1);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(writing(), 24);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theWritingStaysWhileAStrokeGoesDownInPieces() {
        // Twenty-four notes with writing on them, which fill the window.
        Desk desk("notes", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 24; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 6) * 170.0}, {"y", (i / 6) * 170.0},
                                         {"text", QString("Note %1\nwith a line under it").arg(i)}});
            QDir(home).mkpath("boards/notes");
            write(home + "/boards/notes/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        // The fast scribble of theStrokeBeingDrawnIsPartOfTheFrame() over
        // them, which goes on the board in pieces that in full come to more
        // than a frame may hold.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 3000;
        desk.press(scribble(0));
        for (int i = 1; i <= steps; ++i) {
            desk.moveTo(scribble(i));
            if (i % 20 == 0)
                desk.window->grabWindow();
        }
        desk.release(scribble(steps));
        desk.window->grabWindow();
        QVERIFY(desk.board().count() >= 24 + 2);
        QVERIFY(frames.all.size() > steps / 20);
        QVERIFY(frames.all.last().load.reduced);
        // The pieces gave way, a level of their curves, and that left room
        // for all the writing: in every frame as the scribble went down, and
        // once it was done.
        for (qsizetype i = 0; i < frames.all.size(); ++i) {
            const Frames::Frame &frame = frames.all[i];
            QVERIFY2(frame.total() <= budget && frame.texts == 48 && frame.load.smallestText <= 12,
                     qPrintable(QString("frame %1 came to %2 and held %3 pieces of writing")
                                    .arg(i)
                                    .arg(frame.total())
                                    .arg(frame.texts)));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void theWritingComesBackOnlyWhenTheStrokesGiveWay() {
        // Twenty-four notes with twenty lines on each, which fill the window:
        // what is written on them comes to a sixth of what a frame may hold.
        Desk desk("notes", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 24; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 6) * 160.0}, {"y", (i / 6) * 160.0},
                                         {"text", QString("Note %1").arg(i) + QString("\nwritten all over").repeated(20)}});
            QDir(home).mkpath("boards/notes");
            write(home + "/boards/notes/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        // The fast scribble of theStrokeBeingDrawnIsPartOfTheFrame() over
        // them, for longer: it goes on the board in ten pieces or so, which
        // soon come to more than there is room for in full.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 10000;
        desk.press(scribble(0));
        for (int i = 1; i <= steps; ++i) {
            desk.moveTo(scribble(i));
            if (i % 20 == 0)
                desk.window->grabWindow();
        }
        const QVector<Frames::Frame> drawing = frames.all;
        desk.release(scribble(steps));
        desk.window->grabWindow();
        QVERIFY(desk.board().count() >= 24 + 6);
        // A frame for every one grabbed while the pen was down, at least.
        QVERIFY(drawing.size() >= steps / 20);
        // While the pen is down the pieces only pile up, and the strokes on
        // the board only ever give way more. The writing goes to make room
        // for them (from the first piece on, a quarter of the frame is kept
        // for the stroke until the pen is lifted), and comes back only in a
        // frame where they give way: once a level at most, never with every
        // piece.
        int back = 0;
        for (qsizetype i = 1; i < drawing.size(); ++i) {
            const SceneLoad &was = drawing[i - 1].load, &is = drawing[i].load;
            QVERIFY2(is.strokeDetail >= was.strokeDetail && (is.strokesHidden || !was.strokesHidden),
                     qPrintable(QString("frame %1 drew the strokes in more detail than the one before").arg(i)));
            if (is.smallestText < was.smallestText) {
                const bool gaveWay = is.strokeDetail > was.strokeDetail || (is.strokesHidden && !was.strokesHidden);
                QVERIFY2(gaveWay, qPrintable(QString("frame %1 brought back type down to %2 with the strokes as "
                                                     "they were, at %3")
                                                 .arg(i)
                                                 .arg(is.smallestText)
                                                 .arg(is.strokeDetail)));
                ++back;
            }
        }
        QVERIFY(back >= 1);
        // No frame came to more than it may; and with the pen lifted, all the
        // writing is back.
        for (qsizetype i = 0; i < frames.all.size(); ++i)
            QVERIFY2(frames.all[i].total() <= budget,
                     qPrintable(QString("frame %1 came to %2").arg(i).arg(frames.all[i].total())));
        QCOMPARE(frames.all.last().texts, 48);
        QVERIFY(frames.all.last().load.smallestText <= 12);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theWritingComesBackIntoTheRoomFarPartsLeave() {
        Desk desk;
        QVERIFY(desk.ready());
        // Twelve notes, a title and a line under it on each, towards the
        // middle of the window...
        for (int i = 0; i < 12; ++i)
            desk.add(note((i % 4) * 170, (i / 4) * 170, QString("Note %1\nwith a line under it").arg(i)));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // The pieces of writing that held, in the scene graph.
        auto writing = [&frames] { return frames.all.last().texts; };
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad notes = frame(budget);
        QCOMPARE(notes.items, 12);
        QVERIFY(!notes.reduced);
        QCOMPARE(writing(), 24);
        // ...and in its far corner a page of handwriting: short strokes, which
        // like the notes' paper are drawn whatever happens for as long as the
        // part of the board they are in is, and which come to far more than
        // the notes and all their writing. Beside it, a note written on like
        // the others.
        for (int i = 0; i < 40; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 720 + (i % 10) * 20, 540 + (i / 10) * 18));
        desk.add(note(560, 540, "Far note\nwith a line under it"));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 53);
        QVERIFY(!full.reduced);
        QCOMPARE(writing(), 26);
        QVERIFY(full.bytes - notes.bytes > notes.bytes);

        // Allowed only what the twelve notes come to, the writing goes, and
        // that is not enough: the part of the board furthest from the middle
        // of the view goes as well, the handwriting and the note beside it,
        // and that leaves room for all of the twelve notes' writing again,
        // as what was written on the note that went goes with it.
        const SceneLoad some = frame(notes.bytes);
        QVERIFY(some.reduced);
        QCOMPARE(some.items, 12);
        QVERIFY2(some.smallestText <= 12, qPrintable(QString::number(some.smallestText)));
        QCOMPARE(writing(), 24);
        QCOMPARE(some.bytes, notes.bytes);
        // A byte less, and the largest comes back, the titles, and the lines
        // under them do not...
        const SceneLoad titles = frame(notes.bytes - 1);
        QCOMPARE(titles.items, 12);
        QVERIFY2(titles.smallestText > 12 && titles.smallestText <= 13,
                 qPrintable(QString::number(titles.smallestText)));
        QCOMPARE(writing(), 12);
        QVERIFY(titles.bytes <= notes.bytes - 1);
        // ...and with too little left for the titles, none of it does.
        const SceneLoad none = frame(titles.bytes - 1);
        QCOMPARE(none.items, 12);
        QVERIFY(none.smallestText > 13);
        QCOMPARE(writing(), 0);
        QVERIFY(none.bytes <= titles.bytes - 1);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(writing(), 26);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokesComeBackIntoTheRoomFarPartsLeave() {
        Desk desk;
        QVERIFY(desk.ready());
        // The twelve notes of theWritingComesBackIntoTheRoomFarPartsLeave(),
        // and a long scribble over them, towards the middle of the window;
        // and beside them a cylinder, whose curves give way with the
        // scribble's.
        for (int i = 0; i < 12; ++i)
            desk.add(note((i % 4) * 170, (i / 4) * 170, QString("Note %1\nwith a line under it").arg(i)));
        QVector<QPointF> points;
        for (int p = 0; p < 1500; ++p)
            points.append(QPointF(p * 0.25 + 20 * std::sin(p * 0.21), 60 * std::sin(p * 0.35) + 30 * std::cos(p * 0.11)));
        desk.add(stroke(points, 120, 160));
        const quint32 cylinder = desk.add(shape(Item::Shape::Cylinder, 680, 20, 104, 120));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // The pieces of writing that held, in the scene graph, and what the
        // cylinder is drawn with there, in triangles.
        auto writing = [&frames] { return frames.all.last().texts; };
        auto curves = [&desk, cylinder] { return strokedWithin(desk.canvas, desk.board().bounds(cylinder)); };

        // On their own they give way as textIsLeftOutOnlyWhenItHasToBe()
        // has it: allowed a byte less than they come to in full, the lines
        // under the titles go; allowed less than the scribble, the cylinder
        // and the paper come to, the scribble gives up a level of its curves,
        // and that leaves room for the cylinder in full and all the writing
        // again.
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad near = frame(budget);
        QCOMPARE(near.items, 14);
        QVERIFY(!near.reduced);
        QCOMPARE(near.strokeDetail, 0);
        QCOMPARE(writing(), 24);
        const qint64 inFull = curves();
        const SceneLoad titles = frame(near.bytes - 1);
        QCOMPARE(titles.strokeDetail, 0);
        QCOMPARE(writing(), 12);
        const SceneLoad none = frame(titles.bytes - 1);
        QCOMPARE(none.strokeDetail, 0);
        QCOMPARE(writing(), 0);
        const SceneLoad coarser = frame(none.bytes - 1);
        QCOMPARE(coarser.strokeDetail, 1);
        QVERIFY(!coarser.strokesHidden);
        QCOMPARE(curves(), inFull);
        QCOMPARE(writing(), 24);

        // In the window's far corner, a page of handwriting that comes to more
        // than all of that: short strokes, drawn whatever happens for as long
        // as the part of the board they are in is. Beside it, in the same part
        // of the board, a scribble of its own.
        for (int i = 0; i < 200; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 600 + (i % 20) * 20, 520 + (i / 20) * 18));
        QVector<QPointF> beside;
        for (int p = 0; p < 400; ++p)
            beside.append(QPointF(25 * std::sin(p * 0.35) + 10 * std::cos(p * 0.11), p * 0.4));
        desk.add(stroke(beside, 551, 525));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 215);
        QVERIFY(!full.reduced);
        QVERIFY(full.bytes - near.bytes > near.bytes);

        // Allowed only what the scribble, the cylinder and the notes come to,
        // the writing goes, then the curves of the scribbles and the
        // cylinder, then the scribbles, and that is not enough: the part of
        // the board furthest from the middle of the view goes as well, the
        // page and the scribble beside it. What is left is then drawn as it
        // was on its own, as what went goes uncounted: the scribble in full,
        // and all the writing...
        const SceneLoad some = frame(near.bytes);
        QVERIFY(some.reduced);
        QCOMPARE(some.items, 14);
        QCOMPARE(some.strokeDetail, 0);
        QVERIFY(!some.strokesHidden);
        QCOMPARE(writing(), 24);
        QCOMPARE(some.bytes, near.bytes);
        // ...allowed a byte less, the scribble still in full and the titles
        // only, as type goes before the curves of strokes do...
        const SceneLoad fewer = frame(near.bytes - 1);
        QCOMPARE(fewer.items, 14);
        QCOMPARE(fewer.strokeDetail, 0);
        QVERIFY(!fewer.strokesHidden);
        QCOMPARE(writing(), 12);
        QCOMPARE(fewer.bytes, titles.bytes);
        // ...and allowed less than the scribble in full, the cylinder and the
        // paper come to, the scribble a level coarser, the cylinder in full,
        // and all the writing.
        const SceneLoad less = frame(none.bytes - 1);
        QCOMPARE(less.items, 14);
        QCOMPARE(less.strokeDetail, 1);
        QVERIFY(!less.strokesHidden);
        QCOMPARE(curves(), inFull);
        QCOMPARE(writing(), 24);
        QCOMPARE(less.bytes, coarser.bytes);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(desk.warnings, QStringList());
    }

    void shapesAndLinesStayWhenThePensStrokesGo() {
        Desk desk;
        QVERIFY(desk.ready());
        // A cylinder, a speech bubble, and a dashed line between two boxes:
        // shapes with curves and a line of dashes by the hundred, which like
        // the pen's long strokes are kept in levels of detail.
        const quint32 cylinder = desk.add(shape(Item::Shape::Cylinder, 40, 40, 104, 120));
        const quint32 bubble = desk.add(shape(Item::Shape::Bubble, 200, 40, 152, 104));
        const quint32 from = desk.add(shape(Item::Shape::Rect, 40, 220));
        const quint32 to = desk.add(shape(Item::Shape::Rect, 760, 220));
        Item dashed = connector(from, to);
        dashed.dash = Item::Dash::Dashed;
        const quint32 line = desk.add(dashed);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // What the cylinder, the bubble and the dashed line are drawn with in
        // the scene graph, in triangles.
        auto drawn = [&desk, cylinder, bubble, line] {
            QVector<qint64> triangles;
            for (quint32 id : {cylinder, bubble, line})
                triangles.append(strokedWithin(desk.canvas, desk.board().bounds(id)));
            return triangles;
        };
        // On their own at 100%, they are drawn in full.
        const qint64 budget = desk.canvas->loadBudget();
        QVERIFY(!frame(budget).reduced);
        const QVector<qint64> inFull = drawn();

        // Under them, five long scribbles, which in full come to more than a
        // frame may hold. They give up a level of their curves, and that
        // leaves room for the three in full.
        QRectF scribbles;
        for (int i = 0; i < 5; ++i) {
            QVector<QPointF> points;
            for (int p = 0; p < 2500; ++p)
                points.append(QPointF(p * 0.25 + 20 * std::sin(p * 0.21),
                                      60 * std::sin(p * 0.35) + 30 * std::cos(p * 0.11)));
            scribbles |= desk.board().bounds(desk.add(stroke(points, 40 + i * 40, 480)));
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 10);
        QVERIFY(full.reduced);
        QCOMPARE(full.strokeDetail, 1);
        QCOMPARE(drawn(), inFull);

        // A frame allowed a byte less than the last came to, and what was
        // wrong with it, if anything: the three are drawn in it, nothing else
        // went, the scribbles are drawn unless it says they were left out, it
        // is no more than it was allowed, and that is what the scene graph
        // was handed.
        SceneLoad load = full;
        auto lower = [&] {
            const qint64 allowed = load.bytes - 1;
            frames.all.clear();
            load = frame(allowed);
            const QVector<qint64> now = drawn();
            if (load.items != 10 || now.contains(0))
                return QString("%1 items, and the three drawn with %2, %3 and %4 triangles")
                    .arg(load.items)
                    .arg(now[0])
                    .arg(now[1])
                    .arg(now[2]);
            if (load.strokesHidden == (strokedWithin(desk.canvas, scribbles) > 0))
                return QString(load.strokesHidden ? "the scribbles were said to be left out, and drawn"
                                                  : "the scribbles were left out, and not said to be");
            return frames.wrong(allowed);
        };
        // Lowered like that, the scribbles give up their curves, then the
        // three theirs, and then the scribbles go.
        QVector<qint64> plainest;
        for (int step = 0; step < 10 && !load.strokesHidden; ++step) {
            plainest = drawn();
            const QString wrong = lower();
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        }
        QVERIFY(load.strokesHidden);
        // The last frame with the scribbles in it drew the three more plainly
        // than in full: the shapes with fewer edges, the dashed line as the
        // one line it looks like from far off...
        for (int i = 0; i < 3; ++i)
            QVERIFY2(plainest[i] < inFull[i],
                     qPrintable(QString("%1 triangles, and %2 in full").arg(plainest[i]).arg(inFull[i])));
        // ...and the room the scribbles leave brings them back in full.
        QCOMPARE(drawn(), inFull);
        // A byte less than that, and they give way again, the scribbles still
        // left out: drawn more plainly, and drawn.
        const QString wrong = lower();
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QVERIFY(load.strokesHidden);
        QVERIFY(drawn() != inFull);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(drawn(), inFull);
        QCOMPARE(desk.warnings, QStringList());
    }

    void shapesAndLinesComeBackInFullBeforeTheWriting() {
        Desk desk;
        QVERIFY(desk.ready());
        // The cylinder, the bubble and the dashed line between two boxes of
        // shapesAndLinesStayWhenThePensStrokesGo(), with a title written on
        // each shape and a line under it.
        auto written = [](Item item, const QString &title) {
            item.text = title + "\nand a line";
            return item;
        };
        const quint32 cylinder = desk.add(written(shape(Item::Shape::Cylinder, 40, 40, 104, 120), "Orders"));
        const quint32 bubble = desk.add(written(shape(Item::Shape::Bubble, 200, 40, 152, 104), "Hello"));
        const quint32 from = desk.add(written(shape(Item::Shape::Rect, 40, 220), "From"));
        const quint32 to = desk.add(written(shape(Item::Shape::Rect, 760, 220), "To"));
        Item dashed = connector(from, to);
        dashed.dash = Item::Dash::Dashed;
        const quint32 line = desk.add(dashed);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // What the cylinder, the bubble and the dashed line are drawn with in
        // the scene graph, in triangles, and the pieces of writing that held:
        // a title, or the line under it.
        auto drawn = [&desk, cylinder, bubble, line] {
            QVector<qint64> triangles;
            for (quint32 id : {cylinder, bubble, line})
                triangles.append(strokedWithin(desk.canvas, desk.board().bounds(id)));
            return triangles;
        };
        auto writing = [&frames] { return frames.all.last().texts; };
        const qint64 budget = desk.canvas->loadBudget();
        QVERIFY(!frame(budget).reduced);
        const QVector<qint64> inFull = drawn();
        QCOMPARE(writing(), 8);

        // Under them, the five long scribbles, which at 100% give up a level
        // of their curves.
        for (int i = 0; i < 5; ++i) {
            QVector<QPointF> points;
            for (int p = 0; p < 2500; ++p)
                points.append(QPointF(p * 0.25 + 20 * std::sin(p * 0.21),
                                      60 * std::sin(p * 0.35) + 30 * std::cos(p * 0.11)));
            desk.add(stroke(points, 40 + i * 40, 480));
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 10);
        QCOMPARE(full.strokeDetail, 1);

        // Allowed a byte less than each frame came to, the writing goes, then
        // the curves of the scribbles and of the three. The first frame with
        // the scribbles' next level given up has room left over: what went
        // last comes back first, the three in full, and then the writing...
        SceneLoad load = full;
        for (int step = 0; step < 20 && load.strokeDetail < 2; ++step) {
            const qint64 allowed = load.bytes - 1;
            load = frame(allowed);
            QVERIFY(load.bytes <= allowed);
        }
        QCOMPARE(load.strokeDetail, 2);
        QVERIFY(!load.strokesHidden);
        QCOMPARE(drawn(), inFull);
        QCOMPARE(writing(), 8);
        // ...so that a byte less than that, the three are still drawn in full,
        // and the lines under the titles are what go.
        const SceneLoad titles = frame(load.bytes - 1);
        QCOMPARE(titles.strokeDetail, 2);
        QVERIFY(titles.bytes <= load.bytes - 1);
        QCOMPARE(drawn(), inFull);
        QCOMPARE(writing(), 4);

        // The same when the scribbles go: the room they leave brings back the
        // three in full, and then all the writing...
        load = titles;
        for (int step = 0; step < 20 && !load.strokesHidden; ++step) {
            const qint64 allowed = load.bytes - 1;
            load = frame(allowed);
            QVERIFY(load.bytes <= allowed);
        }
        QVERIFY(load.strokesHidden);
        QCOMPARE(load.items, 10);
        QCOMPARE(drawn(), inFull);
        QCOMPARE(writing(), 8);
        // ...and a byte less than that, the three in full, and the titles.
        const SceneLoad hidden = frame(load.bytes - 1);
        QVERIFY(hidden.strokesHidden);
        QVERIFY(hidden.bytes <= load.bytes - 1);
        QCOMPARE(drawn(), inFull);
        QCOMPARE(writing(), 4);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(writing(), 8);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theWritingComesBackOnlyWhenTheStrokesOrTheShapesGiveWay() {
        // Twenty-four notes with a title and twenty lines on each, which fill
        // the window; and piled on the left of them, a hundred and twenty
        // cylinders, whose curves in full come to half of what a frame may
        // hold.
        Desk desk("notes", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 24; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 6) * 160.0}, {"y", (i / 6) * 160.0},
                                         {"text", QString("Note %1").arg(i) + QString("\nwritten all over").repeated(20)}});
            QDir(home).mkpath("boards/notes");
            write(home + "/boards/notes/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        for (int i = 0; i < 120; ++i)
            desk.add(shape(Item::Shape::Cylinder, 16 + (i % 4) * 72 + (i / 20) * 4,
                           16 + (i / 4 % 5) * 120 + (i / 20) * 4, 104, 120));
        const QRectF pile(0, 0, 400, 660);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().diagramDetail, 0);
        const qint64 inFull = strokedWithin(desk.canvas, pile);
        const qint64 budget = desk.canvas->loadBudget();
        Frames frames(desk);
        desk.key(Qt::Key_P);
        // The scribble of theWritingComesBackOnlyWhenTheStrokesGiveWay() over
        // them, in loops sixty pixels across, which a level plainer come to
        // less than larger ones. With the first piece down, the strokes give
        // up a level of their curves, and the cylinders theirs with them; the
        // room that leaves brings the cylinders back in full first. As more
        // pile up there is soon no room for that, and the cylinders give up a
        // level of their curves on their own, which brings back writing that
        // went. The pen stays down until they have, and writing has come
        // back, however many pieces that takes: how many triangles a
        // scribble comes to says that, and this is not a test of it.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(30 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(30 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        // Whether the strokes gave way from one frame to the next, and whether
        // writing came back with them as they were.
        auto strokesGaveWay = [](const SceneLoad &was, const SceneLoad &is) {
            return is.strokeDetail > was.strokeDetail || (is.strokesHidden && !was.strokesHidden);
        };
        auto shapesMadeRoomIn = [&](const Frames::Frame &before, const Frames::Frame &frame) {
            return frame.load.smallestText < before.load.smallestText && !strokesGaveWay(before.load, frame.load);
        };
        const int most = 30000;
        int steps = 0;
        bool madeRoom = false;
        desk.press(scribble(0));
        for (qsizetype seen = 1; steps < most && !(madeRoom && frames.all.last().load.diagramDetail == 1);) {
            for (const int next = steps + 20; steps < next;)
                desk.moveTo(scribble(++steps));
            desk.window->grabWindow();
            for (; seen < frames.all.size(); ++seen)
                madeRoom = madeRoom || shapesMadeRoomIn(frames.all[seen - 1], frames.all[seen]);
        }
        const QVector<Frames::Frame> drawing = frames.all;
        const qint64 plainer = strokedWithin(desk.canvas, pile);
        desk.release(scribble(steps));
        desk.window->grabWindow();
        QVERIFY2(steps < most, "the cylinders never gave way on their own and brought writing back");
        // It went down in pieces.
        QVERIFY(desk.board().count() >= 144 + 2);
        QVERIFY(drawing.size() >= steps / 20);
        // While the pen is down the strokes on the board only ever give way
        // more, and the cylinders, never drawn more plainly than the strokes,
        // come back in more detail only in a frame where the strokes give
        // way. A level of the cylinders' curves is a lot of room too, which
        // brings back writing that went: it comes back only in a frame where
        // the strokes or the cylinders give way, once a level at most, never
        // with every piece.
        for (qsizetype i = 1; i < drawing.size(); ++i) {
            const SceneLoad &was = drawing[i - 1].load, &is = drawing[i].load;
            const bool shapesGaveWay = is.diagramDetail > was.diagramDetail;
            QVERIFY2(is.strokeDetail >= was.strokeDetail && (is.strokesHidden || !was.strokesHidden),
                     qPrintable(QString("frame %1 drew the strokes in more detail than the one before").arg(i)));
            QVERIFY2(is.diagramDetail <= is.strokeDetail &&
                         (is.diagramDetail >= was.diagramDetail || strokesGaveWay(was, is)),
                     qPrintable(QString("frame %1 drew the cylinders at %2, after %3, with the strokes at %4 after %5")
                                    .arg(i)
                                    .arg(is.diagramDetail)
                                    .arg(was.diagramDetail)
                                    .arg(is.strokeDetail)
                                    .arg(was.strokeDetail)));
            if (is.smallestText < was.smallestText) {
                QVERIFY2(strokesGaveWay(was, is) || shapesGaveWay,
                         qPrintable(QString("frame %1 brought back type down to %2 with the strokes and the cylinders "
                                            "as they were, at %3 and %4")
                                        .arg(i)
                                        .arg(is.smallestText)
                                        .arg(is.strokeDetail)
                                        .arg(is.diagramDetail)));
            }
        }
        // By the last frame with the pen down the cylinders were a level
        // plainer, the frames said: the scene graph holds them so.
        QVERIFY2(plainer < inFull, qPrintable(QString("%1 triangles, and %2 in full").arg(plainer).arg(inFull)));
        // No frame came to more than it may.
        for (qsizetype i = 0; i < frames.all.size(); ++i)
            QVERIFY2(frames.all[i].total() <= budget,
                     qPrintable(QString("frame %1 came to %2").arg(i).arg(frames.all[i].total())));
        QCOMPARE(desk.warnings, QStringList());
    }

    void theStrokeBeingDrawnFitsWhatShapesLeave() {
        Desk desk;
        QVERIFY(desk.ready());
        // Sixteen cylinders side by side in a corner of the board, all in one
        // part of it: shapes with curves, which give up their curves when a
        // frame is full, and nothing more. The part of the board nearest the
        // middle of the view is never left out, and this one could be that.
        for (int i = 0; i < 16; ++i)
            desk.add(shape(Item::Shape::Cylinder, 8 + (i % 4) * 120, 8 + (i / 4) * 124, 104, 120));
        const QRectF crowd(0, 0, 480, 504);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Allowed next to nothing, they are drawn at their plainest: the
        // least the board can come to, and the least it says it can.
        desk.canvas->setLoadBudget(1);
        desk.window->grabWindow();
        const SceneLoad plainest = desk.canvas->load();
        const qint64 cylinders = strokedWithin(desk.canvas, crowd);
        QCOMPARE(plainest.items, 16);
        QCOMPARE(plainest.strokeDetail, 2);
        QVERIFY(cylinders > 0);
        QCOMPARE(plainest.floor, plainest.bytes);
        // A frame with room for that and a sixth of it besides, which is less
        // than the quarter the stroke being drawn is given.
        const qint64 budget = plainest.bytes * 6 / 5, room = budget - plainest.bytes;
        desk.canvas->setLoadBudget(budget);
        desk.key(Qt::Key_P);
        desk.window->grabWindow();
        // A fast scribble beside them.
        auto scribble = [&desk](int i) {
            const int x = 600 + (i / 3 % 2 ? 3 - i % 3 : i % 3) * 40 + (i * 7) % 5;
            const int y = 60 + qRound(300 * std::abs(std::sin(i * 0.004))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 1500;
        desk.press(scribble(0));
        // The frames from the press on, as one the window draws of its own
        // accord just before has no stroke in it.
        Frames frames(desk);
        for (int i = 1; i <= steps; ++i) {
            desk.moveTo(scribble(i));
            if (i % 20 == 0) {
                desk.window->grabWindow();
                // The cylinders stay, as plainly drawn as they were.
                const qint64 drawn = strokedWithin(desk.canvas, crowd);
                QVERIFY2(drawn == cylinders, qPrintable(QString("at step %1 the cylinders were drawn with %2 triangles")
                                                            .arg(i)
                                                            .arg(drawn)));
            }
        }
        const QVector<Frames::Frame> drawing = frames.all;
        desk.release(scribble(steps));
        desk.window->grabWindow();
        // The stroke being drawn was given what the cylinders leave at their
        // plainest, and went on to fill it. It was in every frame while the
        // pen was down (one for every one grabbed, at least), as the board was
        // never asked to give way by more than it can, and no frame came to
        // more than it may.
        QVERIFY(drawing.size() >= steps / 20);
        qint64 most = 0;
        for (qsizetype i = 0; i < drawing.size(); ++i) {
            const SceneLoad &load = drawing[i].load;
            QVERIFY2(load.draft > 0 && load.draft <= room && load.bytes >= plainest.bytes,
                     qPrintable(QString("frame %1 drew %2 of the stroke, with room for %3, and %4 of the board")
                                    .arg(i)
                                    .arg(load.draft)
                                    .arg(room)
                                    .arg(load.bytes)));
            most = qMax(most, load.draft);
        }
        QVERIFY2(most > room / 2, qPrintable(QString::number(most)));
        const QString wrong = frames.wrong(budget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QVERIFY(desk.all(Item::Kind::Stroke).size() >= 2);
        QCOMPARE(strokedWithin(desk.canvas, crowd), cylinders);
        QCOMPARE(desk.warnings, QStringList());
    }

    void farPartsTakeTheirShapesWithThem() {
        Desk desk;
        QVERIFY(desk.ready());
        // The twelve notes of theWritingComesBackIntoTheRoomFarPartsLeave(),
        // towards the middle of the window...
        for (int i = 0; i < 12; ++i)
            desk.add(note((i % 4) * 170, (i / 4) * 170, QString("Note %1\nwith a line under it").arg(i)));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        // A frame allowed so much, and what it drew.
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // The pieces of writing that held, in the scene graph.
        auto writing = [&frames] { return frames.all.last().texts; };
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad notes = frame(budget);
        QCOMPARE(notes.items, 12);
        QCOMPARE(writing(), 24);
        // ...and sixteen cylinders side by side in its far corner, all in one
        // part of the board, which even at their plainest come to more than
        // all that is written on the notes.
        for (int i = 0; i < 16; ++i)
            desk.add(shape(Item::Shape::Cylinder, 560 + (i % 4) * 120, 520 + (i / 4) * 124, 104, 120));
        const QRectF corner(560, 520, 480, 496);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 28);
        QVERIFY(!full.reduced);
        QVERIFY(strokedWithin(desk.canvas, corner) > 0);

        // Allowed only what the notes come to, the writing goes, then the
        // cylinders' curves, and that is not enough: the part of the board
        // furthest from the middle of the view goes as well, and the
        // cylinders with it. What went is no longer counted, which leaves
        // room for all of the writing again.
        const SceneLoad some = frame(notes.bytes);
        QVERIFY(some.reduced);
        QCOMPARE(some.items, 12);
        QCOMPARE(strokedWithin(desk.canvas, corner), qint64(0));
        QCOMPARE(writing(), 24);
        QCOMPARE(some.bytes, notes.bytes);

        // Allowed in full again, it is all as it was.
        QCOMPARE(frame(budget).bytes, full.bytes);
        QCOMPARE(writing(), 24);
        QCOMPARE(desk.warnings, QStringList());
    }

    void cornersDrawnWithThePenStayCorners() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // An L with legs two hundred pixels long, a V, and a zigzag...
        const QVector<std::pair<QString, QVector<QPointF>>> shapes{
            {"an L", {{100, 100}, {300, 100}, {300, 300}}},
            {"a V", {{380, 100}, {440, 280}, {500, 100}}},
            {"a zigzag", {{560, 100}, {620, 280}, {680, 100}, {740, 280}, {800, 100}}},
        };
        // ...each drawn at 100%, 25% and 400%: slowly, a point every two
        // pixels of its lines; a little faster, every four; fast, every
        // twenty-five; and as fast as can be, a point at each corner and none
        // between.
        for (const qreal zoom : {1.0, 0.25, 4.0}) {
            desk.canvas->setView(QPointF(0, 0), zoom);
            for (const qreal spacing : {2.0, 4.0, 25.0, 1000.0}) {
                for (const auto &[name, onScreen] : shapes) {
                    QVector<QPointF> corners;
                    for (const QPointF &corner : onScreen)
                        corners.append(desk.canvas->toCanvas(corner));
                    QVector<QPointF> path{corners.first()};
                    for (qsizetype c = 1; c < corners.size(); ++c) {
                        const QPointF from = onScreen[c - 1], to = onScreen[c];
                        const int steps = qMax(1, qRound(std::hypot(to.x() - from.x(), to.y() - from.y()) / spacing));
                        for (int s = 1; s < steps; ++s)
                            path.append(desk.canvas->toCanvas(from + (to - from) * s / steps));
                        path.append(corners[c]);
                    }
                    const QVector<QPointF> drawing = drawWithThePen(desk, path);
                    const QVector<QPointF> drawn = strokeMiddles(desk.canvas);
                    QCOMPARE(desk.board().count(), 1);
                    const QVector<QPointF> curve = curveOf(*desk.last());
                    const QString how = QString("%1 drawn a point every %2 pixels at %3%")
                                            .arg(name)
                                            .arg(spacing)
                                            .arg(zoom * 100);
                    // On the board it never strays from the lines the pen went
                    // along by as much as half a pixel, and it goes through
                    // each of their corners...
                    const qreal off = farthest(curve, wentAlong(desk, path)) * zoom;
                    QVERIFY2(off < 0.5, qPrintable(QString("%1 strays %2 pixels from its lines").arg(how).arg(off)));
                    for (qsizetype c = 1; c + 1 < corners.size(); ++c) {
                        const qreal miss = Geo::distanceToPolyline(curve, corners[c]) * zoom;
                        QVERIFY2(miss < 0.01,
                                 qPrintable(QString("%1 misses corner %2 by %3 pixels").arg(how).arg(c).arg(miss)));
                        // ...where drawn fast it comes to a point, as the
                        // stroke being drawn did. Drawn slowly, a pixel or two
                        // at a time, it turns there within less than a pixel,
                        // which is all a pen moving over pixels can tell of a
                        // corner.
                        if (spacing > 2) {
                            const qreal turn = turnAt(curve, corners[c]);
                            QVERIFY2(turn > 75, qPrintable(QString("%1 turns by %2 degrees at corner %3")
                                                               .arg(how)
                                                               .arg(turn)
                                                               .arg(c)));
                        }
                    }
                    // And letting go of it changed nothing that shows.
                    const QString jump = jumped(drawing, drawn, curve, zoom);
                    QVERIFY2(jump.isEmpty(), qPrintable(QString("%1: %2").arg(how, jump)));
                    desk.board().undo();
                    QCOMPARE(desk.board().count(), 0);
                }
            }
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void cornersThePenWentByStayCorners() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // An L, a V and a zigzag with legs two hundred pixels long, and an
        // arrowhead as sharp as a V gets, drawn by a hand going too fast for
        // any point to land on a corner: a point every ten to fifty pixels
        // from half that on, each corner halfway between two. The pen turns
        // at both, half the corner at each, and either side of them it ran
        // straight. Where the lines either side meet nearly head on, as the
        // arrowhead's do, the pixels the pen moves over put where they meet
        // a pixel or two off.
        struct Shape {
            QString name;
            QVector<QPointF> onScreen;
            qreal near;
        };
        const QVector<Shape> shapes{
            {"an L", {{100, 100}, {300, 100}, {300, 300}}, 1},
            {"a V", {{380, 100}, {500, 260}, {620, 100}}, 1},
            {"a zigzag", {{100, 360}, {220, 520}, {340, 360}, {460, 520}, {580, 360}}, 1},
            {"an arrowhead", {{660, 100}, {716, 292}, {772, 100}}, 2.5},
        };
        // Draws through `onScreen` a point every `spacing` screen pixels.
        auto draw = [&desk](const QString &how, const QVector<QPointF> &onScreen, qreal spacing, qreal near) {
            const qreal zoom = desk.canvas->zoom();
            QVector<QPointF> corners;
            for (const QPointF &corner : onScreen)
                corners.append(desk.canvas->toCanvas(corner));
            const QVector<QPointF> path = pacedAlong(corners, spacing / zoom);
            const QVector<QPointF> drawing = drawWithThePen(desk, path);
            const QVector<QPointF> drawn = strokeMiddles(desk.canvas);
            QCOMPARE(desk.board().count(), 1);
            const Item *stroke = desk.last();
            const QVector<QPointF> curve = curveOf(*stroke);
            // It comes to a point once at each corner, near where the lines
            // either side meet...
            const QVector<QPointF> points = cornersOf(*stroke);
            QVERIFY2(points.size() == corners.size() - 2,
                     qPrintable(QString("%1 comes to a point %2 times").arg(how).arg(points.size())));
            for (qsizetype c = 1; c + 1 < corners.size(); ++c) {
                const qreal miss = nearest(points, corners[c]) * zoom;
                QVERIFY2(miss < near,
                         qPrintable(QString("%1 comes to a point %2 pixels from corner %3").arg(how).arg(miss).arg(c)));
            }
            // ...and keeps as near those lines all the way.
            const qreal off = farthest(curve, corners) * zoom;
            QVERIFY2(off < near, qPrintable(QString("%1 strays %2 pixels from its lines").arg(how).arg(off)));
            const QString jump = jumped(drawing, drawn, curve, zoom);
            QVERIFY2(jump.isEmpty(), qPrintable(QString("%1: %2").arg(how, jump)));
            desk.board().undo();
            QCOMPARE(desk.board().count(), 0);
        };
        for (const qreal zoom : {1.0, 0.25, 4.0}) {
            desk.canvas->setView(QPointF(0, 0), zoom);
            for (const qreal spacing : {10.0, 20.0, 25.0, 50.0})
                for (const auto &[name, onScreen, near] : shapes) {
                    draw(QString("%1 drawn a point every %2 pixels at %3%").arg(name).arg(spacing).arg(zoom * 100),
                         onScreen, spacing, near);
                    if (QTest::currentTestFailed())
                        return;
                }
        }
        // Those shapes go along rows and columns of pixels, or a step of 3
        // across and 4 down, and every point of them is on a pixel. An L and
        // a V with legs as long, heading off at 17 and 30 degrees and moved by
        // a part of a pixel, have each point up to half a pixel off its line
        // either way, and how far each time is new: where the lines either
        // side of a corner meet is known only as near as that, a pixel and a
        // half. A point every thirty pixels from fifteen on, the last before
        // a corner is five pixels short of it, and the pen takes it for the
        // corner until the next shows the corner was further on. A point
        // every ten pixels, the stretches either side are too short to tell a
        // turned corner from one a few pixels round, which the pen leaves
        // round (see curvesDrawnWithThePenStaySmooth()), so these go a point
        // every twenty pixels and more.
        desk.canvas->setView(QPointF(0, 0), 1);
        for (const auto &[name, turn, corner] : {std::tuple{"an L", 90.0, QPointF(450, 250)},
                                                 std::tuple{"a V", 106.26, QPointF(450, 400)}})
            for (const qreal heading : {17.0, 30.0})
                for (const QPointF moved : {QPointF(0, 0), QPointF(0.3, 0.55)})
                    for (const qreal spacing : {20.0, 25.0, 30.0, 50.0}) {
                        const QPointF at = corner + moved;
                        const QVector<QPointF> turned{at - turnedBy(QPointF(200, 0), heading), at,
                                                      at + turnedBy(QPointF(200, 0), heading + turn)};
                        draw(QString("%1 heading %2 degrees, moved by (%3, %4), drawn a point every %5 pixels")
                                 .arg(name)
                                 .arg(heading)
                                 .arg(moved.x())
                                 .arg(moved.y())
                                 .arg(spacing),
                             turned, spacing, 1.5);
                        if (QTest::currentTestFailed())
                            return;
                    }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aTickDrawnFastComesToAPointAtItsCorner() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // Ticks, a short stroke down into the corner and a long one up out of
        // it, drawn as fast as a tick is: a point every thirty or fifty
        // pixels from half that on, so that one point or two go down into
        // the corner and none lands on it. The line into the corner is no
        // more than the stretches from where the stroke starts, the first of
        // them as short as the hand happened to make it. Each is moved by
        // parts of a pixel, so the window's pixels put its points off
        // differently each time.
        const QVector<QVector<QPointF>> ticks{
            {{100, 380}, {160, 440}, {320, 260}},
            {{420, 360}, {470, 430}, {650, 300}},
            {{720, 420}, {790, 470}, {900, 280}},
            {{150, 120}, {200, 180}, {380, 60}},
            {{600, 120}, {640, 165}, {800, 40}},
        };
        for (const QPointF moved : {QPointF(0, 0), QPointF(0.3, 0.55), QPointF(0.85, 0.15)})
            for (const qreal spacing : {30.0, 50.0})
                for (const QVector<QPointF> &onScreen : ticks) {
                    QVector<QPointF> tick;
                    for (const QPointF &corner : onScreen)
                        tick.append(desk.canvas->toCanvas(corner + moved));
                    const QVector<QPointF> drawing = drawWithThePen(desk, pacedAlong(tick, spacing));
                    const QVector<QPointF> drawn = strokeMiddles(desk.canvas);
                    QCOMPARE(desk.board().count(), 1);
                    const Item *stroke = desk.last();
                    const QVector<QPointF> curve = curveOf(*stroke);
                    const QString how = QString("the tick into (%1, %2) moved by (%3, %4) drawn every %5 pixels")
                                            .arg(onScreen[1].x())
                                            .arg(onScreen[1].y())
                                            .arg(moved.x())
                                            .arg(moved.y())
                                            .arg(spacing);
                    // It comes to a point once, where the lines either side
                    // meet, as near as the pixels tell, and keeps as near those
                    // lines all the way: rounded off, it bulged eight pixels
                    // and more out of its corner.
                    const QVector<QPointF> points = cornersOf(*stroke);
                    QVERIFY2(points.size() == 1,
                             qPrintable(QString("%1 comes to a point %2 times").arg(how).arg(points.size())));
                    const qreal miss = nearest(points, tick[1]);
                    QVERIFY2(miss < 2,
                             qPrintable(QString("%1 comes to a point %2 pixels from its corner").arg(how).arg(miss)));
                    const qreal off = farthest(curve, tick);
                    QVERIFY2(off < 2, qPrintable(QString("%1 strays %2 pixels from its lines").arg(how).arg(off)));
                    const QString jump = jumped(drawing, drawn, curve);
                    QVERIFY2(jump.isEmpty(), qPrintable(QString("%1: %2").arg(how, jump)));
                    desk.board().undo();
                    QCOMPARE(desk.board().count(), 0);
                }
        QCOMPARE(desk.warnings, QStringList());
    }

    void gentleCornersDrawnWithThePenStayCorners() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // A pentagon and a hexagon three hundred pixels across, their corners
        // turns of 72 and 60 degrees, drawn fast: a point every fifteen or
        // twenty-five pixels, landing on the corners or going by them as a
        // hand going at one pace would. Each points up, a side of it along a
        // row or a column of pixels, and turned so that none is. And each is
        // drawn with the view where every test starts and moved from there by
        // parts of a pixel, as a pan leaves it: the window's pixels put the
        // points up to half a pixel off, and differently each time.
        const QVector<std::pair<int, qreal>> polygons{{5, 0}, {6, 0}, {5, 30}, {6, 12}};
        for (const QPointF origin : {QPointF(0, 0), QPointF(0.3, 0.55), QPointF(0.85, 0.15)}) {
            desk.canvas->setView(origin, 1);
            for (const auto &[sides, turned] : polygons) {
                QVector<QPointF> corners;
                for (int i = 0; i <= sides; ++i) {
                    const qreal angle = i * 2 * M_PI / sides - M_PI / 2 + qDegreesToRadians(turned);
                    corners.append(QPointF(480 + 150 * std::cos(angle), 330 + 150 * std::sin(angle)));
                }
                for (const qreal spacing : {15.0, 25.0}) {
                    QVector<QPointF> onCorners{corners.first()};
                    for (qsizetype c = 1; c < corners.size(); ++c) {
                        const QLineF side(corners[c - 1], corners[c]);
                        const int steps = qRound(side.length() / spacing);
                        for (int s = 1; s <= steps; ++s)
                            onCorners.append(side.pointAt(qreal(s) / steps));
                    }
                    // A point landing on a corner is as far off as the pixels
                    // put it. Going by a corner, it is where the sides either
                    // side of it meet, as near as the points along them tell,
                    // which is a little less near: they meet at 60 and 72
                    // degrees, and a side a little off puts where they meet
                    // further along the other.
                    struct Way {
                        const char *name;
                        QVector<QPointF> path;
                        qreal near, off;
                    };
                    for (const Way &way : {Way{"landing on its corners", onCorners, 1, 1},
                                           Way{"going by its corners", pacedAlong(corners, spacing), 1.5, 1.25}}) {
                        const QVector<QPointF> drawing = drawWithThePen(desk, way.path);
                        const QVector<QPointF> drawn = strokeMiddles(desk.canvas);
                        QCOMPARE(desk.board().count(), 1);
                        const Item *stroke = desk.last();
                        const QVector<QPointF> curve = curveOf(*stroke);
                        const QString how = QString("%1 sides turned %2 degrees drawn a point every %3 pixels, %4, "
                                                    "the view at (%5, %6)")
                                                .arg(sides)
                                                .arg(turned)
                                                .arg(spacing)
                                                .arg(way.name)
                                                .arg(origin.x())
                                                .arg(origin.y());
                        // It comes to a point at each of its corners...
                        const QVector<QPointF> points = cornersOf(*stroke);
                        QVERIFY2(points.size() == sides - 1,
                                 qPrintable(QString("%1 comes to a point %2 times").arg(how).arg(points.size())));
                        for (qsizetype c = 1; c + 1 < corners.size(); ++c) {
                            const qreal miss = nearest(points, corners[c]);
                            QVERIFY2(miss < way.near, qPrintable(QString("%1 comes to a point %2 pixels from corner %3")
                                                                     .arg(how)
                                                                     .arg(miss)
                                                                     .arg(c)));
                        }
                        // ...and keeps as near its sides, where rounded off it
                        // bulged out past its corners by more.
                        const qreal off = farthest(curve, corners);
                        QVERIFY2(off < way.off,
                                 qPrintable(QString("%1 strays %2 pixels from its sides").arg(how).arg(off)));
                        const QString jump = jumped(drawing, drawn, curve);
                        QVERIFY2(jump.isEmpty(), qPrintable(QString("%1: %2").arg(how, jump)));
                        desk.board().undo();
                        QCOMPARE(desk.board().count(), 0);
                    }
                }
            }
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void curvesDrawnWithThePenStaySmooth() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // A circle three hundred across, drawn slowly, nine hundred points
        // round: a pixel or so apart on the window's whole pixels, they step
        // across them, a right angle at a time. Drawn fast, twenty-four.
        // Smaller ones, sixty and thirty across, drawn as fast as a hand
        // scribbles, five to eight points round, which turn by as much as
        // the corners of a polygon do. Handwriting, rows of loops drawn fast
        // and leaning either way, which turn sharply at the foot of each
        // loop. And a U and a box with round corners, between straight
        // stretches, drawn a point every ten or twenty-five pixels: going by
        // a corner, a hand that keeps its pace goes as far as it does either
        // side, and going round a curve less far.
        struct Curve {
            QString name;
            QVector<QPointF> path;
            // How far from the curve meant it may be, if it is a circle.
            qreal round = 0;
        };
        QVector<Curve> curves;
        auto circle = [](qreal radius, int points) {
            QVector<QPointF> path;
            for (int i = 0; i <= points; ++i)
                path.append(QPointF(480 + radius * std::cos(i * 2 * M_PI / points + 0.3),
                                    330 + radius * std::sin(i * 2 * M_PI / points + 0.3)));
            return path;
        };
        // The pixels put each point of a circle up to 0.7 of a pixel off it,
        // and the curve between twenty-four of them a little further.
        curves.append({"a circle drawn 900 points round", circle(150, 900), 1});
        curves.append({"a circle drawn 24 points round", circle(150, 24), 1.3});
        // Drawn slowly by a hand that shakes, a point every three pixels and
        // each up to a pixel and a half in or out: it turns sharply at many
        // points, one way and back, and runs straight into none of them.
        for (const qreal radius : {60.0, 100.0}) {
            QVector<QPointF> path;
            for (int i = 0; i <= int(2.5 * M_PI * radius / 3); ++i) {
                const qreal out = radius + 0.75 * ((i * 7) % 5 - 2);
                path.append(QPointF(480, 330) + out * QPointF(std::cos(i * 3 / radius), std::sin(i * 3 / radius)));
            }
            curves.append({QString("a circle %1 across drawn by a shaking hand").arg(radius * 2), path});
        }
        for (const qreal radius : {15.0, 30.0})
            for (const int points : {5, 6, 8})
                curves.append({QString("a circle %1 across drawn %2 points round").arg(radius * 2).arg(points),
                                circle(radius, points)});
        for (const qreal radius : {6.0, 10.0, 15.0})
            for (const int points : {6, 8, 10})
                for (const qreal lean : {0.0, 12.0, -20.0}) {
                    QVector<QPointF> path;
                    for (int i = 0; i <= points * 6; ++i) {
                        const qreal t = i * 2 * M_PI / points;
                        const QPointF along(0.6 * radius * t + radius * std::sin(t), -radius * std::cos(t));
                        path.append(QPointF(200, 330) + turnedBy(along, lean));
                    }
                    curves.append({QString("loops %1 high drawn %2 points a loop, leaning %3 degrees")
                                       .arg(radius * 2)
                                       .arg(points)
                                       .arg(lean),
                                   path});
                }
        auto arc = [](QVector<QPointF> &line, QPointF middle, qreal radius, qreal from, qreal to) {
            for (int i = 1; i <= 64; ++i)
                line.append(middle + radius * QPointF(std::cos(from + (to - from) * i / 64),
                                                      std::sin(from + (to - from) * i / 64)));
        };
        for (const qreal radius : {20.0, 40.0})
            for (const qreal spacing : {10.0, 25.0}) {
                QVector<QPointF> u{{300, 100}, {300, 300}};
                arc(u, {300 + radius, 300}, radius, M_PI, 0);
                u.append(QPointF(300 + 2 * radius, 100));
                curves.append({QString("a U %1 across drawn a point every %2 pixels").arg(radius * 2).arg(spacing),
                               pacedAlong(u, spacing)});
                QVector<QPointF> box{{300 + radius, 100}, {600 - radius, 100}};
                arc(box, {600 - radius, 100 + radius}, radius, -M_PI / 2, 0);
                box.append(QPointF(600, 400 - radius));
                arc(box, {600 - radius, 400 - radius}, radius, 0, M_PI / 2);
                box.append(QPointF(300 + radius, 400));
                arc(box, {300 + radius, 400 - radius}, radius, M_PI / 2, M_PI);
                box.append(QPointF(300, 100 + radius));
                arc(box, {300 + radius, 100 + radius}, radius, M_PI, 1.5 * M_PI);
                curves.append(
                    {QString("a box with corners %1 round drawn a point every %2 pixels").arg(radius).arg(spacing),
                     pacedAlong(box, spacing)});
            }
        // Each is drawn with the view where every test starts and moved from
        // there by parts of a pixel, as a pan leaves it: the window's pixels
        // put the points up to half a pixel off, and differently each time,
        // which can make a curve seem to turn sharply at one point and run
        // straight into it.
        for (const QPointF origin : {QPointF(0, 0), QPointF(0.3, 0.55), QPointF(0.55, 0.55)})
            for (const Curve &drawnAs : std::as_const(curves)) {
                desk.canvas->setView(origin, 1);
                const QVector<QPointF> drawing = drawWithThePen(desk, drawnAs.path);
                const QVector<QPointF> drawn = strokeMiddles(desk.canvas);
                QCOMPARE(desk.board().count(), 1);
                const Item *stroke = desk.last();
                const QVector<QPointF> curve = curveOf(*stroke);
                const QString how = drawnAs.name + QString(", the view at (%1, %2)").arg(origin.x()).arg(origin.y());
                // No point of it is given twice, to come to a point at: where
                // it goes through one of its points, cut finely, it turns by
                // little, where a corner would turn by all of its angle.
                for (qsizetype i = 1; i < stroke->points.size(); ++i)
                    QVERIFY2(stroke->points[i] != stroke->points[i - 1],
                             qPrintable(QString("%1 comes to a point at point %2").arg(how).arg(i)));
                for (qsizetype i = 1; i + 1 < stroke->points.size(); ++i) {
                    const qreal turn = turnAt(curve, stroke->points[i] + stroke->rect.topLeft());
                    QVERIFY2(turn < 30,
                             qPrintable(QString("%1 turns by %2 degrees at point %3").arg(how).arg(turn).arg(i)));
                }
                // A circle drawn with points enough is round, nowhere further
                // from the circle drawn than the pixels put it...
                if (drawnAs.round > 0) {
                    qreal off = 0;
                    for (const QPointF &point : curve)
                        off = qMax(off, std::abs(std::hypot(point.x() - 480, point.y() - 330) - 150));
                    QVERIFY2(off < drawnAs.round, qPrintable(QString("%1 is %2 from the circle").arg(how).arg(off)));
                }
                // ...and letting go of it changed nothing that shows.
                const QString jump = jumped(drawing, drawn, curve);
                QVERIFY2(jump.isEmpty(), qPrintable(QString("%1: %2").arg(how, jump)));
                desk.board().undo();
                QCOMPARE(desk.board().count(), 0);
            }
        QCOMPARE(desk.warnings, QStringList());
    }

    void cornersStayCornersOnTheBoardAndInItsFile() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.key(Qt::Key_P);
        // An L drawn fast, a point landing on its corner, and a V with none
        // landing on it.
        QVector<QPointF> ell{{100, 100}};
        for (int s = 1; s <= 8; ++s)
            ell.append(QPointF(100 + 25 * s, 100));
        for (int s = 1; s <= 8; ++s)
            ell.append(QPointF(300, 100 + 25 * s));
        drawWithThePen(desk, ell);
        drawWithThePen(desk, pacedAlong({{380, 100}, {500, 260}, {620, 100}}, 25));
        QCOMPARE(desk.board().count(), 2);
        QVector<Item> strokes;
        for (const quint32 id : desk.board().order()) {
            strokes.append(*desk.board().item(id));
            QCOMPARE(cornersOf(strokes.last()).size(), 1);
        }
        // Each comes to a point where it is saved and read back as where it
        // was drawn: the point it has twice is written twice, and read so.
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        Board read;
        QCOMPARE(read.fromJson(desk.onDisk()), QStringList());
        QCOMPARE(read.count(), 2);
        for (qsizetype i = 0; i < strokes.size(); ++i) {
            const Item *again = read.item(read.order()[i]);
            QCOMPARE(again->points.size(), strokes[i].points.size());
            const QVector<QPointF> corners = cornersOf(*again);
            QCOMPARE(corners.size(), 1);
            QVERIFY(nearest(corners, cornersOf(strokes[i]).first()) < 0.01);
            QVERIFY(farthest(curveOf(*again), curveOf(strokes[i])) < 0.01);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void noFrameIsAllowedMoreThanIsKnownToBeSafe() {
        Desk desk;
        QVERIFY(desk.ready());
        // The whole board drawn whether in view or not, and four times as
        // much asked for as any frame is allowed, as `sprawl open --probe 16
        // --probe-with unseen` used to ask: a frame is allowed that much, and
        // no more.
        desk.canvas->setDrawingEverything(true);
        desk.canvas->setLoadBudget(CanvasView::largestLoadBudget * 4);
        QCOMPARE(desk.canvas->loadBudget(), CanvasView::largestLoadBudget);
        // Then scribbles as that probe draws them (see scribble() in
        // main.cpp), where it puts them: off to the right of the window and a
        // window away, forty at a time, until they come to more than that.
        Frames frames(desk);
        for (int i = 0; i < 400; ++i) {
            QVector<QPointF> points;
            qreal x = 0, y = 0;
            for (int p = 0; p < 120; ++p) {
                x += 1.2;
                y += std::sin(p * 0.35 + i) * 4;
                points.append(QPointF(x, y));
            }
            desk.add(stroke(points, 2010 + (i % 5) * 170.0, 10 + (i / 5) * 70.0));
            if (i % 40 == 39)
                desk.window->grabWindow();
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        // No frame came to more than that, and that is all the GPU was
        // handed: the strokes gave way to keep within it.
        const QString wrong = frames.wrong(CanvasView::largestLoadBudget);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        const SceneLoad load = desk.canvas->load();
        QCOMPARE(load.items, 400);
        QVERIFY(load.reduced);
        QVERIFY(load.strokeDetail > 0 || load.strokesHidden);
        // None of it is in view, as the window shows the board.
        desk.canvas->setDrawingEverything(false);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 0);
        // Less than the most is allowed as it is asked for, and the least is
        // nothing.
        desk.canvas->setLoadBudget(CanvasView::largestLoadBudget / 2);
        QCOMPARE(desk.canvas->loadBudget(), CanvasView::largestLoadBudget / 2);
        desk.canvas->setLoadBudget(-1);
        QCOMPARE(desk.canvas->loadBudget(), qint64(0));
        QCOMPARE(desk.warnings, QStringList());
    }

    void theProbeIsGivenNoMoreThanIsKnownToBeSafe() {
        // The megabytes `sprawl open --probe` is given are what it draws up
        // to...
        QStringList said;
        QCOMPARE(Measuring::probeLimit("8", said), qint64(8'000'000));
        QCOMPARE(Measuring::probeLimit("0.5", said), qint64(500'000));
        QCOMPARE(Measuring::probeLimit("16", said), CanvasView::largestLoadBudget);
        QCOMPARE(said, QStringList());
        // ...as far as what any frame is allowed, and however much more it is
        // given, that is what it draws up to, and it says so.
        for (const QString &more : QStringList{"16.5", "40", "1e9", "inf"}) {
            said.clear();
            QCOMPARE(Measuring::probeLimit(more, said), CanvasView::largestLoadBudget);
            QCOMPARE(said.size(), 1);
            QVERIFY2(said.first().contains("16 MB"), qPrintable(said.first()));
        }
        // What is not a number of megabytes is refused, saying what is.
        for (const QString &wrong : QStringList{"0", "-8", "nan", "eight", "8MB", ""}) {
            said.clear();
            QCOMPARE(Measuring::probeLimit(wrong, said), qint64(0));
            QCOMPARE(said.size(), 1);
            QVERIFY2(said.first().contains("like 8"), qPrintable(said.first()));
        }
        // So is something to draw that it does not know how to.
        said.clear();
        QCOMPARE(Measuring::probeKind("", said), QString("strokes"));
        for (const QString &kind : Measuring::probeKinds())
            QCOMPARE(Measuring::probeKind(kind, said), kind);
        QCOMPARE(said, QStringList());
        QCOMPARE(Measuring::probeKind("unsen", said), QString());
        QCOMPARE(said.size(), 1);
        QVERIFY2(said.first().contains("unseen"), qPrintable(said.first()));
    }

    void theBenchMakesUpNoBiggerABoardThanIsKnownToBeSafe() {
        // The items `sprawl open --bench` is given are what its board has...
        QStringList said;
        QCOMPARE(Measuring::benchItems("10000", said), 10000);
        QCOMPARE(Measuring::benchItems("0", said), 0);
        QCOMPARE(Measuring::benchItems("100000", said), Measuring::mostItems);
        QCOMPARE(said, QStringList());
        // ...up to so many, and however many more it is given, that is how
        // many it has, and it says so.
        for (const QString &more : QStringList{"100001", "1000000", "1e12", "inf"}) {
            said.clear();
            QCOMPARE(Measuring::benchItems(more, said), Measuring::mostItems);
            QCOMPARE(said.size(), 1);
            QVERIFY2(said.first().contains("100000 items"), qPrintable(said.first()));
        }
        // What is not a number of items is refused, saying what is.
        for (const QString &wrong : QStringList{"-5", "2.5", "nan", "many", ""}) {
            said.clear();
            QVERIFY(Measuring::benchItems(wrong, said) < 0);
            QCOMPARE(said.size(), 1);
            QVERIFY2(said.first().contains("like 10000"), qPrintable(said.first()));
        }
    }

    void theWholeStrokeIsDrawnWhileItGoesDownInPieces() {
        // The page of handwriting of theStrokeBeingDrawnIsPartOfTheFrame(),
        // which with a stroke being drawn is more than a frame may hold, and
        // its scribble. The pieces that goes on the board in are long
        // strokes, which the board leaves out first when it has no room for
        // them: but not those of the stroke being drawn. Whoever is drawing
        // it sees all of it, and the board makes room for it otherwise.
        Desk desk("page", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 700; ++i)
                items.append(QJsonObject{{"type", "stroke"}, {"x", (i % 35) * 28.0}, {"y", (i / 35) * 34.0},
                                         {"points", QJsonArray{0, 0, 4, 8, 8, 0, 12, 8}}});
            QDir(home).mkpath("boards/page");
            write(home + "/boards/page/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.key(Qt::Key_P);
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        // Drawn in a color of its own at each zoom, so that the frames tell
        // it apart from what was drawn before.
        const QVector<std::pair<qreal, QString>> rounds{{1.0, "red"}, {0.25, "green"}, {0.1, "blue"}};
        for (const auto &[zoom, color] : rounds) {
            desk.canvas->setView(QPointF(0, 0), zoom);
            desk.canvas->setPenColor(color);
            desk.window->grabWindow();
            const int before = desk.board().count();
            Frames frames(desk);
            frames.marked = desk.theme.swatch(color);
            const int steps = 3000;
            desk.press(scribble(0));
            for (int i = 1; i <= steps; ++i) {
                desk.moveTo(scribble(i));
                if (i % 20 == 0)
                    desk.window->grabWindow();
            }
            const QVector<Frames::Frame> drawing = frames.all;
            desk.release(scribble(steps));
            desk.window->grabWindow();
            QVERIFY(drawing.size() >= steps / 20);
            QVERIFY2(desk.board().count() >= before + 3, qPrintable(QString::number(desk.board().count() - before)));
            // In every frame, every piece of it on the board so far was drawn,
            // and what was being drawn after them when that was.
            for (qsizetype i = 0; i < drawing.size(); ++i) {
                const Frames::Frame &frame = drawing[i];
                const int pieces = frame.items - before, after = frame.load.draft > 0 ? 1 : 0;
                QVERIFY2(frame.nodesInMarked == pieces + after,
                         qPrintable(QString("at %1%, frame %2 drew %3 strokes of %4 pieces and %5 being drawn")
                                        .arg(zoom * 100)
                                        .arg(i)
                                        .arg(frame.nodesInMarked)
                                        .arg(pieces)
                                        .arg(after)));
            }
            // However much of it there was, no frame came to more than it may.
            const QString wrong = frames.wrong(desk.canvas->loadBudget());
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aStrokeInPiecesTakenBackWithEscIsSavedAsItWas() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(100, 100, "Here"));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        // Something added a moment before is still to be written as the
        // stroke begins.
        desk.add(note(600, 100, "There"));
        desk.key(Qt::Key_P);
        // The fast scribble of theStrokeBeingDrawnIsPartOfTheFrame(), which
        // goes on the board in pieces while the pen is still down.
        auto scribble = [&desk](int i) {
            const qreal round = i * 2 * M_PI / 7;
            const int x = 480 + qRound(80 * std::cos(round)) + (i * 7) % 5;
            const int y = 200 + qRound(80 * std::sin(round) + 360 * std::abs(std::sin(i * 0.0015))) + (i * 13) % 7;
            return desk.canvas->toCanvas(QPointF(x, y));
        };
        const int steps = 3000;
        desk.press(scribble(0));
        for (int i = 1; i <= steps; ++i)
            desk.moveTo(scribble(i));
        QVERIFY(desk.canvas->busy());
        const int drawing = desk.board().count();
        QVERIFY(drawing >= 2 + 2);
        auto written = [&desk] { return int(desk.onDisk().value("items").toArray().size()); };
        // Held there for a while, what has gone down of it is written.
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), drawing);
        // Esc takes all of it back, and the file goes back with it.
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->busy());
        QCOMPARE(desk.board().count(), 2);
        QVERIFY(desk.backend.saveState() != "saved");
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QCOMPARE(written(), 2);
        desk.release(scribble(steps));
        QCOMPARE(desk.board().count(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsLeftWhenFarPartsGoIsDrawnOnlyAsFinelyAsItShows() {
        Desk desk;
        QVERIFY(desk.ready());
        // The scribble of theStrokesComeBackIntoTheRoomFarPartsLeave(), on its
        // own: what it comes to in full.
        QVector<QPointF> points;
        for (int p = 0; p < 1500; ++p)
            points.append(QPointF(p * 0.25 + 20 * std::sin(p * 0.21), 60 * std::sin(p * 0.35) + 30 * std::cos(p * 0.11)));
        desk.add(stroke(points, 120, 160));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad inFull = frame(budget);
        QCOMPARE(inFull.strokeDetail, 0);
        // Seen at half size, with the middle of the window where it was, it
        // is drawn a level plainer: as finely as shows.
        desk.canvas->setView(QPointF(-500, -350), 0.5);
        const SceneLoad half = frame(budget);
        QVERIFY(!half.reduced);
        QCOMPARE(half.strokeDetail, 1);
        // And its page of handwriting in the window's far corner, which comes
        // to more than the scribble in full.
        for (int i = 0; i < 200; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 600 + (i % 20) * 20, 520 + (i / 20) * 18));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad both = frame(budget);
        QCOMPARE(both.items, 201);
        QVERIFY(!both.reduced);
        QCOMPARE(both.strokeDetail, 1);
        QVERIFY(both.bytes - half.bytes > inFull.bytes);
        // Allowed what the scribble comes to in full, the page has to go.
        // What is left is drawn as it would be on its own: the scribble as
        // finely as shows, and no finer, though there is room for that.
        const SceneLoad left = frame(inFull.bytes);
        QVERIFY(left.reduced);
        QCOMPARE(left.items, 1);
        QCOMPARE(left.strokeDetail, 1);
        QVERIFY(!left.strokesHidden);
        QVERIFY(left.bytes < inFull.bytes);
        QCOMPARE(desk.warnings, QStringList());
    }

    void writingOnItsOwnFarOffIsLeftOutOnlyWhenItHasToBe() {
        Desk desk;
        QVERIFY(desk.ready());
        // The twelve notes of theWritingComesBackIntoTheRoomFarPartsLeave(),
        // and in the window's far corner four labels: writing on nothing.
        for (int i = 0; i < 12; ++i)
            desk.add(note((i % 4) * 170, (i / 4) * 170, QString("Note %1\nwith a line under it").arg(i)));
        for (int i = 0; i < 4; ++i) {
            Item label;
            label.kind = Item::Kind::Text;
            label.fontSize = Defaults::textSize;
            label.rect = QRectF(i * 110, 640, 100, 30);
            label.text = QString("Label %1").arg(i);
            desk.add(label);
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        Frames frames(desk);
        auto frame = [&desk](qint64 allowed) {
            desk.canvas->setLoadBudget(allowed);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        auto writing = [&frames] { return frames.all.last().texts; };
        const qint64 budget = desk.canvas->loadBudget();
        const SceneLoad all = frame(budget);
        QCOMPARE(all.items, 16);
        QCOMPARE(writing(), 28);
        // Nearer the middle of the window than the labels, a page of
        // handwriting that comes to more than all of that.
        for (int i = 0; i < 200; ++i)
            desk.add(stroke({{0, 0}, {4, 8}, {8, 0}, {12, 8}}, 600 + (i % 20) * 20, 520 + (i / 20) * 3));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const SceneLoad full = frame(budget);
        QCOMPARE(full.items, 216);
        QVERIFY(full.bytes - all.bytes > all.bytes);
        // Allowed what the notes and the labels come to, the page has to go,
        // and that is all the room they need. The labels' part of the board
        // is further off, but would make no room by going, as what is written
        // goes before any part does: it stays, and so does what is written.
        const SceneLoad some = frame(all.bytes);
        QVERIFY(some.reduced);
        QCOMPARE(some.items, 16);
        QCOMPARE(writing(), 28);
        QCOMPARE(some.bytes, all.bytes);
        QCOMPARE(desk.warnings, QStringList());
    }

    // --- Library ---------------------------------------------------------------

    void boardNamesAreTidy() {
        QCOMPARE(Library::slug("Desk setup!"), QString("desk-setup"));
        QCOMPARE(Library::slug("  Weekend   Trip  "), QString("weekend-trip"));
        QCOMPARE(Library::slug("Été 2026"), QString("été-2026"));
        QCOMPARE(Library::slug("v0.1 scope"), QString("v0-1-scope"));
        // Nothing a name is made of can step outside the boards folder or be
        // taken for an option on a command line.
        QCOMPARE(Library::slug("../../etc/passwd"), QString("etc-passwd"));
        QCOMPARE(Library::slug("--help"), QString("help"));
        QCOMPARE(Library::slug("..."), QString());
        QCOMPARE(Library::slug(""), QString());
        const QString longest = Library::slug(QString(40, 'a') + " " + QString(40, 'b'));
        QVERIFY(longest.size() <= 64);
        QVERIFY(!longest.endsWith('-'));
    }

    void theLibraryKeepsEachBoardInAFolder() {
        QTemporaryDir dir;
        Library library(dir.path());
        QCOMPARE(library.boards().size(), 0);
        QCOMPARE(library.create("plan"), QString());
        QVERIFY(QFile::exists(dir.filePath("boards/plan/board.json")));
        QVERIFY(library.create("plan").contains("already"));
        QVERIFY(!library.create("Not Tidy").isEmpty());
        QCOMPARE(library.freeName("Plan"), QString("plan-2"));
        QCOMPARE(library.freeName(""), QString("untitled"));

        QCOMPARE(library.rename("plan", "trip"), QString());
        QVERIFY(library.exists("trip"));
        QVERIFY(!library.exists("plan"));
        QVERIFY(!library.rename("gone", "somewhere").isEmpty());
        QCOMPARE(library.create("plan"), QString());
        QVERIFY(library.rename("plan", "trip").contains("already"));

        // A board is found by its name, its folder, or its file.
        QCOMPARE(library.locate("trip").file, dir.filePath("boards/trip/board.json"));
        QCOMPARE(library.locate(dir.filePath("boards/trip")).name, QString("trip"));
        QCOMPARE(library.locate(dir.filePath("boards/trip/board.json")).assets, dir.filePath("boards/trip/assets"));
        QVERIFY(!library.locate("nowhere").isValid());
        // Kept somewhere of its own, a library keeps its state there too.
        QCOMPARE(library.stateFile(), dir.filePath("state.json"));

        QCOMPARE(library.remove("trip"), QString());
        QVERIFY(!library.exists("trip"));
        QVERIFY(!library.remove("trip").isEmpty());
        QCOMPARE(library.boards().size(), 1);
    }

    void theSwitcherFindsBoardsByAFewLetters() {
        QVERIFY(Library::score("weekend-trip", "wt") > 0);
        QVERIFY(Library::score("weekend-trip", "WEEK") > 0);
        QVERIFY(Library::score("weekend-trip", "xyz") < 0);
        QVERIFY(Library::score("weekend-trip", "tw") < 0);
        QCOMPARE(Library::score("anything", ""), 0);
        // Letters together beat letters apart, and the start of a word beats its middle.
        QVERIFY(Library::score("plan", "plan") > Library::score("p-l-a-n", "plan"));
        QVERIFY(Library::score("desk-setup", "set") > Library::score("closet-up", "set"));
        // Of two that match alike, the shorter name is the closer.
        QVERIFY(Library::score("desk", "desk") > Library::score("desk-setup-for-the-office", "desk"));
    }

    void picturesAreKeptOncePerBoard() {
        QTemporaryDir dir;
        const QString assets = dir.filePath("board/assets");
        const QString one = picture(dir.filePath("one.png"), QSize(20, 10));
        QFile::copy(one, dir.filePath("same-picture.PNG"));
        QString problem;
        const QString first = Library::importFile(assets, one, &problem);
        QCOMPARE(problem, QString());
        QVERIFY(first.endsWith(".png"));
        QCOMPARE(Library::importFile(assets, dir.filePath("same-picture.PNG"), &problem), first);
        QCOMPARE(QDir(assets).entryList(QDir::Files).size(), 1);
        QVERIFY(QFile::exists(one));
        // A name is a name of a file in the folder, nothing more.
        QVERIFY(!first.contains('/'));

        QCOMPARE(Library::importFile(assets, dir.filePath("missing.png"), &problem), QString());
        QVERIFY(problem.contains("missing.png"));
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        const QString pasted = Library::importImage(assets, image, &problem);
        QVERIFY(pasted.endsWith(".png"));
        QCOMPARE(Library::importImage(assets, image, &problem), pasted);
        QCOMPARE(QDir(assets).entryList(QDir::Files).size(), 2);
        QCOMPARE(Library::importImage(assets, QImage(), &problem), QString());
    }

    // --- What is drawn where ----------------------------------------------

    void wrappedTextIsDrawnAsFarDownAsItGoes() {
        // A dozen paragraphs of prose pasted onto a board and wrapped. A file
        // says where text starts and where it wraps, and nothing of how tall
        // that makes it.
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        Desk desk("prose", [&prose](const QString &home) {
            QDir(home).mkpath("boards/prose");
            const QJsonObject text{{"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 480}, {"text", prose.trimmed()}};
            write(home + "/boards/prose/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", QJsonArray{text}}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Measured, it is as tall as its type makes it: thousands of units.
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QCOMPARE(page.width(), 480.0);
        QVERIFY2(page.height() > 1500, qPrintable(QString::number(page.height())));
        auto drawn = [&desk](qreal y) {
            desk.canvas->setView(QPointF(0, y), 1);
            desk.window->grabWindow();
            return desk.canvas->load();
        };
        // At 100% it is drawn wherever the window is on it, down to its last
        // line, and every letter of it...
        for (qreal y = 0; y < page.bottom(); y += 400) {
            const SceneLoad load = drawn(y);
            const QString where = QString("with the view at y = %1").arg(y);
            QVERIFY2(load.items == 1, qPrintable(where));
            QVERIFY2(!load.reduced && load.smallestText <= Defaults::textSize, qPrintable(where));
        }
        // ...and not once the window is past it.
        QCOMPARE(drawn(page.bottom() + 1000).items, 0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsLeaveTextFromWhereItsTypeEnds() {
        // The same prose, joined to a note below it and off to the right, and
        // the board opened numbered every way there is: however they are
        // numbered, text read from a file is measured before a connector to
        // it is made ready to draw (see CanvasView::prepareVisible()).
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        QVector<int> ids{1, 2, 3};
        do {
            const int text = ids[0], note = ids[1], line = ids[2];
            const QString numbered = QString("text %1, note %2, connector %3").arg(text).arg(note).arg(line);
            Desk desk("joined", [&](const QString &home) {
                QDir(home).mkpath("boards/joined");
                const QJsonArray items{QJsonObject{{"id", text}, {"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 480},
                                                   {"text", prose.trimmed()}},
                                       QJsonObject{{"id", note}, {"type", "note"}, {"x", 2400}, {"y", 6000}},
                                       QJsonObject{{"id", line}, {"type", "connector"}, {"from", text}, {"to", note}}};
                write(home + "/boards/joined/board.json",
                      QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
            });
            QVERIFY(desk.ready());
            QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
            const QRectF page = desk.all(Item::Kind::Text).first()->rect;
            const Item connector = *desk.all(Item::Kind::Connector).first();
            // It leaves the text from the edge of the text as measured...
            const bool onEdge = page.adjusted(-0.5, -0.5, 0.5, 0.5).contains(connector.p1) &&
                                !page.adjusted(0.5, 0.5, -0.5, -0.5).contains(connector.p1);
            QVERIFY2(onEdge, qPrintable(numbered));
            auto drawn = [&desk](QPointF origin) {
                desk.canvas->setView(origin, 1);
                desk.window->grabWindow();
                return desk.canvas->load().items;
            };
            // ...and that is where it is drawn from: on its way to the note it
            // is there,
            const QPointF middle = (connector.p1 + connector.p2) / 2;
            const QPointF half = QPointF(desk.canvas->width(), desk.canvas->height()) / 2;
            QVERIFY2(drawn(middle - half) == 1, qPrintable(numbered));
            // and beside the text, a window and more above where it leaves it,
            // nothing is: no line from the box the text had before it was
            // measured.
            const QPointF beside(page.right() + 220, 200);
            QVERIFY(connector.p1.y() > beside.y() + desk.canvas->height() + 300);
            QVERIFY2(drawn(beside) == 0, qPrintable(numbered));
            QCOMPARE(desk.warnings, QStringList());
        } while (std::next_permutation(ids.begin(), ids.end()));
    }

    void textIsMeasuredEvenWhenTheSceneStartsOver() {
        // A thousand notes, all in view and with a fair amount written on
        // each, and far off, a line of text.
        Board board;
        QJsonArray items;
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        for (int i = 0; i < 1000; ++i)
            items.append(
                QJsonObject{{"type", "note"}, {"x", (i % 40) * 90.0}, {"y", (i / 40) * 90.0}, {"text", words}});
        items.append(QJsonObject{{"type", "text"}, {"x", 50000}, {"y", 50000}, {"wrap", 480}, {"text", "A line"}});
        QCOMPARE(board.fromJson(QJsonObject{{"sprawl", 1}, {"items", items}}), QStringList());
        const quint32 text = board.order().last();
        QQuickWindow first, second;
        first.resize(1000, 700);
        second.resize(1000, 700);
        CanvasView canvas;
        canvas.setSize(QSizeF(1000, 700));
        canvas.setBoard(&board);
        canvas.setView(QPointF(-100, -100), 0.25);
        canvas.setParentItem(first.contentItem());
        first.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        QTRY_COMPARE_WITH_TIMEOUT(canvas.pendingItems(), 0, 30000);

        // All of it changes at once: every note's color, and what the text
        // says, which is longer than its box until it is measured again.
        board.begin();
        for (quint32 id : board.order()) {
            Item item = *board.item(id);
            if (id == text)
                item.text = QString("much longer now ").repeated(60);
            else
                item.color = "blue";
            board.set(item);
        }
        board.commit();
        // And the scene starts over, as it does whenever the scene graph is
        // torn down: here, by the canvas being taken to another window. What
        // is on screen takes all of the first frame there, so the text, off
        // screen, is still waiting when the scene graph is built...
        canvas.setParentItem(second.contentItem());
        second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        QTRY_COMPARE_WITH_TIMEOUT(canvas.pendingItems(), 0, 30000);
        // ...and is measured all the same, and drawn by what it measures.
        Item fitted = *board.item(text);
        TextMetrics::fit(fitted);
        QCOMPARE(board.item(text)->rect, fitted.rect);
        canvas.setView(QPointF(50000, fitted.rect.bottom() - 100), 1);
        second.grabWindow();
        QCOMPARE(canvas.load().items, 1);
    }

    void textBeingReadStaysDrawnWhenItsFileIsReadAgain() {
        // The prose, and well away from it thousands of notes with something
        // written on each: far more than one frame has time to make ready.
        // Which of them is made ready first goes by how they are numbered, so
        // the text is numbered differently every time the file is read.
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(6);
        auto file = [&](int text) {
            QJsonArray items{QJsonObject{
                {"id", text}, {"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 480}, {"text", prose.trimmed()}}};
            for (int i = 0; i < 4000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 20000 + (i % 80) * 200.0}, {"y", (i / 80) * 200.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("busy", [&file](const QString &home) {
            QDir(home).mkpath("boards/busy");
            write(home + "/boards/busy/board.json", QJsonDocument(file(1)).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QVERIFY2(page.height() > 1500, qPrintable(QString::number(page.height())));
        // Someone is reading it, from well down to near its end...
        int numbered = 1;
        for (qreal y = 500; y < page.bottom() - 300; y += 300) {
            const QString where = QString("with the view at y = %1").arg(y);
            desk.canvas->setView(QPointF(0, y), 1);
            desk.window->grabWindow();
            QVERIFY2(desk.canvas->load().items == 1, qPrintable(where));
            // ...when the file is read again, as it is whenever something
            // else has changed it. The text is back to a guess at its size,
            // a line to each paragraph, and all of the board is to be made
            // ready again...
            QCOMPARE(desk.board().fromJson(file(numbered += 1000)), QStringList());
            QVERIFY(desk.all(Item::Kind::Text).first()->rect.height() < 400);
            QCOMPARE(desk.canvas->pendingItems(), desk.board().count());
            // ...and the very next frame has it where it is being read.
            desk.window->grabWindow();
            const SceneLoad load = desk.canvas->load();
            QVERIFY2(load.items == 1, qPrintable(where));
            QVERIFY2(!load.reduced && load.smallestText <= Defaults::textSize, qPrintable(where));
            QCOMPARE(desk.all(Item::Kind::Text).first()->rect, page);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void longTextsSideBySideDoNotCrowdEachOtherOut() {
        // The prose ten times over, in columns side by side: each column far
        // taller than a window, and all ten more type than a frame may hold.
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const int columns = 10;
        Desk desk("columns", [&](const QString &home) {
            QDir(home).mkpath("boards/columns");
            QJsonArray items;
            for (int i = 0; i < columns; ++i)
                items.append(QJsonObject{
                    {"type", "text"}, {"x", i * 400}, {"y", 0}, {"wrap", 320}, {"text", prose.trimmed()}});
            write(home + "/boards/columns/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QVERIFY2(page.height() > 2500, qPrintable(QString::number(page.height())));
        // Reading them at 100%, each from top to bottom and then on to the
        // next: every letter in the window is drawn, because the columns away
        // from it, the one just read included, are not what the frame is
        // spent on.
        for (int column = 0; column < columns; ++column)
            for (qreal y = 0; y < page.bottom(); y += 500) {
                desk.canvas->setView(QPointF(column * 400, y), 1);
                desk.window->grabWindow();
                const SceneLoad load = desk.canvas->load();
                const QString where = QString("with the view at column %1, y = %2").arg(column + 1).arg(y);
                QVERIFY2(!load.reduced, qPrintable(where));
                QVERIFY2(load.smallestText <= Defaults::textSize, qPrintable(where));
                QVERIFY2(load.items > 0 && load.items < columns / 2, qPrintable(where));
            }
        QCOMPARE(desk.warnings, QStringList());
    }

    void textIsNeverMeasuredPastItsReach() {
        // How far text could reach, worked out from what is written in it,
        // is what decides which text is measured before a frame is drawn:
        // whatever could be on screen. So it holds for type of every kind
        // (but a handful of signs far wider than a letter, which the next
        // test is about): prose, code indented with tabs, lines broken inside
        // a paragraph, the widest and the narrowest of ASCII and words of
        // every length, which the font in use measures; letters, signs and
        // spaces of other scripts, measured in whatever fonts they come from,
        // digits among them, and icons; and scripts whose lines are tall or
        // whose signs are wide, which no font may have; at sizes and wraps
        // from the smallest to large.
        QString prose, code, wide;
        for (int word = 0; word < 100; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 25 == 24 ? '\n' : ' ');
        for (int i = 0; i < 8; ++i)
            code += QString("\tif (value%1 > limit) {\n\t\treturn compute(value%1);\n\t}\n").arg(i);
        for (char32_t c : {0x1242B, 0x12219, 0x12030})
            wide += QString::fromUcs4(&c, 1);
        const QStringList texts{
            prose.trimmed(),
            code,
            QString("\t").repeated(20),
            QString("a\t").repeated(20),
            (QString("line of text") + QChar(QChar::LineSeparator)).repeated(20),
            QString("line of text\r\n").repeated(20),
            QString("x").repeated(300),
            QString("%&@#WM_").repeated(50),
            QString("i").repeated(300),
            QString("a bb ccc dddd eeeee ffffff ggggggg hhhhhhhh iiiiiiiii ").repeated(8),
            QString("0123456789").repeated(30),
            QString("plain words and then \u00e9\n").repeated(15),
            QString("Über die Brücke gehen wir schön langsam weiter, Ærø, Øresund, ŁÓDŹ. ").repeated(6),
            QString("Мы идём по мосту очень медленно, ЖЩЫЮ жщыю. ").repeated(8),
            QString("Περπατάμε πάνω στη γέφυρα πολύ αργά. ").repeated(8),
            QString("Բարեւ աշխարհ, გამარჯობა მსოფლიო. ").repeated(8),
            QString("한국어로 쓴 긴 문장입니다. ").repeated(12),
            QString("中文的句子，很长。「引用」（括号）！？ＡＢＣ１２３ ").repeated(8),
            QString("第1章、2024年10月3日。").repeated(12),
            QString("→ ★ ☺ € — … ∞ ─│┌┐ \u00a0\u3000\u2003\uf120\ue0a0\uf0e7").repeated(12),
            QString("日本語の文章をここに書きます。").repeated(8),
            QString("🙂🚀👩‍👩‍👧‍👦 ").repeated(12),
            QString("هذا نص عربي طويل للاختبار ").repeated(6),
            QString("བསྒྲུབས་སྐྱེད\n").repeated(20),
            QString("မြန်မာနိုင်ငံ၏ ကြောင်း\n").repeated(20),
            QString("ꦲꦏ꧀ꦱꦫꦗꦮ ᬅᬓ᭄ᬲᬭᬩᬮᬶ ").repeated(12),
            QString("a̐̑̒̓̔ ").repeated(30),
            QString("⸻").repeated(40),
            wide.repeated(12),
        };
        for (const QString &text : texts)
            for (qreal size : {4.0, 12.0, 48.0})
                for (qreal wrap : {0.0, 40.0, 480.0})
                    for (bool bold : {false, true}) {
                        Item item;
                        item.kind = Item::Kind::Text;
                        item.text = text;
                        item.fontSize = size;
                        item.wrap = wrap;
                        item.bold = bold;
                        const QRectF reach = TextMetrics::reach(item);
                        TextMetrics::fit(item);
                        const QString what = QString("%1... at size %2, wrap %3%4: measured %5 x %6, reach %7 x %8")
                                                 .arg(text.left(12)).arg(size).arg(wrap).arg(bold ? ", bold" : "")
                                                 .arg(item.rect.width()).arg(item.rect.height())
                                                 .arg(reach.width()).arg(reach.height());
                        QVERIFY2(reach.contains(item.rect), qPrintable(what));
                    }
    }

    void textWiderThanItsReachIsDrawnAsSoonAsItIsRead() {
        // A line of the widest sign there is. In type that has it, U+FDFD is
        // twelve times as wide as the type is tall, which is past what text
        // could reach by what is written in it, and read near its far end it
        // is not known to be on screen until it is measured. Well away, a
        // board's worth of notes with something written on each.
        const QString signs = QString(QChar(0xFDFD)).repeated(60);
        // Where no type has it, it is drawn as a box no wider than its reach
        // allows for, and there is nothing to find out here.
        Item measured;
        QCOMPARE(Board::itemFromJson(QJsonObject{{"type", "text"}, {"x", 0}, {"y", 0}, {"text", signs}}, measured),
                 QString());
        const qreal reach = TextMetrics::reach(measured).width();
        TextMetrics::fit(measured);
        if (measured.rect.width() <= reach)
            QSKIP("No type here has U+FDFD, so a line of it is no wider than its reach allows for");
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(6);
        auto file = [&](int text) {
            QJsonArray items{QJsonObject{{"id", text}, {"type", "text"}, {"x", 0}, {"y", 0}, {"text", signs}}};
            for (int i = 0; i < 3000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 40000 + (i % 80) * 250.0}, {"y", (i / 80) * 250.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("signs", [&file](const QString &home) {
            QDir(home).mkpath("boards/signs");
            write(home + "/boards/signs/board.json", QJsonDocument(file(1)).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF line = desk.all(Item::Kind::Text).first()->rect;
        desk.canvas->setView(QPointF(qMax(0.0, line.right() - 600), 0), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        // Each time the file is read again, the text is measured before the
        // rest of the board is made ready, and is drawn within a frame or two
        // (one, unless a frame is cut short), not once all of it is.
        int numbered = 1;
        for (int reload = 0; reload < 3; ++reload) {
            QCOMPARE(desk.board().fromJson(file(numbered += 1000)), QStringList());
            int frames = 0;
            do {
                desk.window->grabWindow();
                ++frames;
            } while (desk.canvas->load().items != 1 && frames < 3);
            const QString when = QString("after reading it again %1 times").arg(reload + 1);
            QVERIFY2(desk.canvas->load().items == 1, qPrintable(when));
            QVERIFY(desk.canvas->pendingItems() > 1000);
            QCOMPARE(desk.all(Item::Kind::Text).first()->rect, line);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsOnScreenAreDrawnFromWhereFarTextEnds() {
        // A crowd of notes with plenty written on each, all in view: more than
        // one frame has time to make ready. Far off, up and to the left, the
        // prose, joined to a note in the crowd by a dashed line, which is as
        // many dashes as it is long.
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        auto file = [&](int first) {
            const int text = first, note = first + 1, line = first + 2;
            QJsonArray items{QJsonObject{{"id", text}, {"type", "text"}, {"x", -15000}, {"y", -15000}, {"wrap", 480},
                                         {"text", prose.trimmed()}},
                             QJsonObject{{"id", note}, {"type", "note"}, {"x", 1800}, {"y", 1200}},
                             QJsonObject{{"id", line}, {"type", "connector"}, {"from", text}, {"to", note},
                                         {"dash", "dashed"}}};
            for (int i = 0; i < 1000; ++i)
                items.append(
                    QJsonObject{{"type", "note"}, {"x", (i % 40) * 90.0}, {"y", (i / 40) * 90.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("crowd", [&file](const QString &home) {
            QDir(home).mkpath("boards/crowd");
            write(home + "/boards/crowd/board.json", QJsonDocument(file(1)).toJson());
        });
        QVERIFY(desk.ready());
        desk.canvas->setView(QPointF(-100, -100), 0.25);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        // The notes are two triangles each, and the line thousands more.
        const qint64 drawn = desk.canvas->load().triangles;
        QVERIFY2(drawn > 2 * 2 * 1001, qPrintable(QString::number(drawn)));
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QVERIFY2(page.height() > 1500, qPrintable(QString::number(page.height())));
        QVERIFY(!desk.canvas->visibleArea().intersects(page));
        // Each time the file is read again the text is back to a guess at its
        // size, and the line, on screen, leaves it from where that ends. The
        // very next frame has the line drawn from where the text really ends,
        // dash for dash as it is once all of the board is ready again.
        int numbered = 1;
        for (int reload = 0; reload < 3; ++reload) {
            const QString when = QString("after reading it again %1 times").arg(reload + 1);
            QCOMPARE(desk.board().fromJson(file(numbered += 2000)), QStringList());
            QVERIFY(desk.all(Item::Kind::Text).first()->rect.height() < 400);
            desk.window->grabWindow();
            QCOMPARE(desk.all(Item::Kind::Text).first()->rect, page);
            const Item connector = *desk.all(Item::Kind::Connector).first();
            const bool onEdge = page.adjusted(-0.5, -0.5, 0.5, 0.5).contains(connector.p1) &&
                                !page.adjusted(0.5, 0.5, -0.5, -0.5).contains(connector.p1);
            QVERIFY2(onEdge, qPrintable(when));
            QVERIFY2(desk.canvas->load().triangles == drawn,
                     qPrintable(QString("%1: %2 triangles, against %3 once ready")
                                    .arg(when).arg(desk.canvas->load().triangles).arg(drawn)));
            QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void textBesideACrowdIsDrawnAsSoonAsItIsReadAgain() {
        // The crowd of notes again, all in view, and beside it prose in large
        // type that starts well above the window and runs on down through it.
        QString prose;
        for (int word = 0; word < 300; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        auto file = [&](int text) {
            QJsonArray items{QJsonObject{{"id", text}, {"type", "text"}, {"x", 0}, {"y", -4000}, {"size", 48},
                                         {"wrap", 480}, {"text", prose.trimmed()}}};
            for (int i = 0; i < 1000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 600 + (i % 34) * 90.0}, {"y", (i / 34) * 90.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("beside", [&file](const QString &home) {
            QDir(home).mkpath("boards/beside");
            write(home + "/boards/beside/board.json", QJsonDocument(file(1)).toJson());
        });
        QVERIFY(desk.ready());
        desk.canvas->setView(QPointF(-100, -100), 0.25);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad settled = desk.canvas->load();
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QVERIFY(page.top() < desk.canvas->visibleArea().top() && page.bottom() > desk.canvas->visibleArea().bottom());
        // Each time the file is read again the text is back to a guess at its
        // size, a line to each paragraph, all of it well above the window. The
        // very next frame has it drawn where it runs through the window, every
        // letter, as it is once all of the board is ready again.
        int numbered = 1;
        for (int reload = 0; reload < 3; ++reload) {
            const QString when = QString("after reading it again %1 times").arg(reload + 1);
            QCOMPARE(desk.board().fromJson(file(numbered += 2000)), QStringList());
            QVERIFY(desk.all(Item::Kind::Text).first()->rect.bottom() < -2000);
            desk.window->grabWindow();
            const SceneLoad load = desk.canvas->load();
            QVERIFY2(load.items == settled.items, qPrintable(when));
            QVERIFY2(!load.reduced && load.smallestText <= 48, qPrintable(when));
            QVERIFY2(load.triangles == settled.triangles, qPrintable(when));
            QCOMPARE(desk.all(Item::Kind::Text).first()->rect, page);
            QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesFromAFileHoldWhatIsWrittenOnThem() {
        // A note as an agent writes one into a board's file, with an id as
        // the skill shows them: a title and thirty lines under it, and no
        // size, so it is given the usual one. Typed in, it would have grown to
        // hold all of that: at least a line as tall as its type for each line
        // written, in whatever font.
        QStringList lines{"Packing"};
        for (int i = 0; i < 30; ++i)
            lines << QString("line %1 of what the note says").arg(i + 1);
        const QJsonObject written{{"id", 1}, {"type", "note"}, {"x", 0}, {"y", 0}, {"text", lines.join('\n')}};
        const QJsonObject file{{"sprawl", 1}, {"items", QJsonArray{written}}};
        Item typed;
        QCOMPARE(Board::itemFromJson(written, typed), QString());
        QCOMPARE(typed.rect, QRectF(0, 0, Defaults::noteSize, Defaults::noteSize));
        QVERIFY(TextMetrics::fit(typed));
        QVERIFY2(typed.rect.height() > lines.size() * TextMetrics::body + 2 * TextMetrics::notePadding,
                 qPrintable(QString::number(typed.rect.height())));
        Desk desk("packing", [&file](const QString &home) {
            QDir(home).mkpath("boards/packing");
            write(home + "/boards/packing/board.json", QJsonDocument(file).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);

        // In the window it is as tall as that, and drawn down to its last line.
        QCOMPARE(desk.all(Item::Kind::Note).first()->rect, typed.rect);
        desk.canvas->setView(QPointF(0, typed.rect.bottom() - 200), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        // Which is not something done to the board: there is no step of it
        // to undo, and the file is left as it was...
        QVERIFY(!desk.canvas->canUndo());
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.backend.flush());
        QCOMPARE(desk.onDisk(), file);

        // ...where `sprawl export` reads it, as the agent wrote it, and makes
        // it as tall: the picture holds all of the note, its paper down to the
        // bottom and its last line written on it.
        const QString picture = desk.home.filePath("packing.png");
        QCOMPARE(runCli({"sprawl", "export", desk.library.boardFile("packing"), "-o", picture, "--scale", "1",
                         "--transparent"}),
                 0);
        const QImage exported(picture);
        const int margin = 32, padding = int(TextMetrics::notePadding), bottom = margin + int(typed.rect.height());
        QCOMPARE(exported.size(), QSize(int(Defaults::noteSize) + 2 * margin, bottom + margin));
        const QColor paper = exported.pixelColor(margin + 76, bottom - padding / 2);
        QCOMPARE(paper.alpha(), 255);
        QCOMPARE(exported.pixelColor(margin + 76, bottom + padding / 2).alpha(), 0);
        bool inked = false;
        for (int y = bottom - padding - 16; y < bottom - padding; ++y)
            for (int x = margin + padding; x < margin + int(Defaults::noteSize) - padding; ++x)
                inked = inked || exported.pixelColor(x, y) != paper;
        QVERIFY(inked);

        // With the next save, whatever it is for, the note goes into the file
        // as tall as it is.
        desk.add(note(400, 0, "Beside it"));
        QVERIFY(desk.backend.flush());
        QCOMPARE(desk.onDisk().value("items").toArray().first().toObject().value("h").toDouble(),
                 typed.rect.height());
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesAreNeverMadeSmallerThanTheirFileHasThem() {
        // A roomy note with little written on it, and one given a size too
        // small for what it says.
        const QJsonObject roomy{{"type", "note"}, {"x", 0},   {"y", 0},
                                {"w", 300},       {"h", 400}, {"text", "Plenty of room\nfor a line or two"}};
        const QJsonObject cramped{{"type", "note"}, {"x", 400}, {"y", 0},
                                  {"w", 200},       {"h", 40},  {"text", "Cramped\none\ntwo\nthree"}};
        const QJsonObject file{{"sprawl", 1}, {"items", QJsonArray{roomy, cramped}}};
        Item grown;
        QCOMPARE(Board::itemFromJson(cramped, grown), QString());
        QVERIFY(TextMetrics::fit(grown));
        QCOMPARE(grown.rect.topLeft(), QPointF(400, 0));
        QCOMPARE(grown.rect.width(), 200.0);
        QVERIFY(grown.rect.height() > 40);
        Desk desk("sizes", [&file](const QString &home) {
            QDir(home).mkpath("boards/sizes");
            write(home + "/boards/sizes/board.json", QJsonDocument(file).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // The roomy one keeps all its room, and the other grows to hold what
        // it says.
        QCOMPARE(desk.all(Item::Kind::Note)[0]->rect, QRectF(0, 0, 300, 400));
        QCOMPARE(desk.all(Item::Kind::Note)[1]->rect, grown.rect);

        // The commands have them the same, as they write them back.
        QTemporaryDir dir;
        write(dir.filePath("sizes.json"), QJsonDocument(file).toJson());
        QCOMPARE(runCli({"sprawl", "add", dir.filePath("sizes.json"), "text", "--text", "Added", "--x", "0", "--y",
                         "600"}),
                 0);
        Board board;
        QJsonObject root;
        QCOMPARE(Library::read(dir.filePath("sizes.json"), root), QString());
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.count(), 3);
        QCOMPARE(board.item(board.order()[0])->rect, QRectF(0, 0, 300, 400));
        QCOMPARE(board.item(board.order()[1])->rect, grown.rect);
        QCOMPARE(desk.warnings, QStringList());
    }

    void shapesFromAFileGrowAsTheyDoWhenTypedIn() {
        // A shape of every outline dropped in the window and written on, line
        // by line.
        const QString text = "Ship it?\nthe tests pass\nthe guide is written\nthe changes are listed\nit is tagged";
        Desk desk;
        QVERIFY(desk.ready());
        const QStringList shapes = Board::shapes();
        QVector<QRectF> dropped, typed;
        for (int i = 0; i < shapes.size(); ++i) {
            const QPointF corner((i % 5) * 1000.0, (i / 5) * 1000.0);
            desk.canvas->setView(corner, 1);
            desk.canvas->setTool("shape");
            desk.canvas->setShape(shapes[i]);
            desk.click(corner + QPointF(300, 300));
            QVERIFY(desk.canvas->editing());
            dropped.append(desk.last()->rect);
            desk.type(text);
            desk.key(Qt::Key_Escape);
            QCOMPARE(desk.last()->text, text);
            typed.append(desk.last()->rect);
            QVERIFY2(typed.last().height() > dropped.last().height(), qPrintable(shapes[i]));
        }

        // The same shapes written into the board's file, where they were
        // dropped and at the size they were dropped at, and read, as when an
        // agent has written it.
        QJsonArray items;
        for (int i = 0; i < shapes.size(); ++i)
            items.append(QJsonObject{{"type", "shape"},
                                     {"shape", shapes[i]},
                                     {"x", dropped[i].x()},
                                     {"y", dropped[i].y()},
                                     {"text", text}});
        const QJsonObject file{{"sprawl", 1}, {"items", items}};
        QCOMPARE(desk.board().fromJson(file), QStringList());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Each grows just as much as it did when it was typed in, a letter at
        // a time, and about its middle; as fit() grows it.
        for (int i = 0; i < shapes.size(); ++i) {
            const QRectF fromFile = desk.board().item(desk.board().order()[i])->rect;
            const QString what = QString("%1 dropped at %2 tall: typed in to %3, read to %4")
                                     .arg(shapes[i]).arg(dropped[i].height())
                                     .arg(typed[i].height()).arg(fromFile.height());
            QVERIFY2(fromFile.size() == typed[i].size(), qPrintable(what));
            QVERIFY2(qAbs(fromFile.center().y() - dropped[i].center().y()) <= 0.5, qPrintable(what));
            Item fitted;
            QCOMPARE(Board::itemFromJson(items[i].toObject(), fitted), QString());
            QVERIFY(TextMetrics::fit(fitted));
            QCOMPARE(fromFile, fitted.rect);
        }

        // The commands have them the same as they write them back, to the
        // hundredth of a unit a file keeps; and read back from that file,
        // none of them has to grow again.
        QTemporaryDir dir;
        write(dir.filePath("shapes.json"), QJsonDocument(file).toJson());
        QCOMPARE(runCli({"sprawl", "add", dir.filePath("shapes.json"), "note", "--x", "0", "--y", "-400"}), 0);
        QJsonObject root;
        QCOMPARE(Library::read(dir.filePath("shapes.json"), root), QString());
        const QJsonArray saved = root.value("items").toArray();
        QCOMPARE(saved.size(), items.size() + 1);
        for (int i = 0; i < items.size(); ++i) {
            const QJsonObject written = saved[i].toObject();
            const QRectF inWindow = desk.board().item(desk.board().order()[i])->rect;
            QCOMPARE(QPointF(written.value("x").toDouble(), written.value("y").toDouble()), inWindow.topLeft());
            QCOMPARE(written.value("w").toDouble(), inWindow.width());
            QCOMPARE(written.value("h").toDouble(), inWindow.height());
            Item reread;
            QCOMPARE(Board::itemFromJson(written, reread), QString());
            QVERIFY2(!TextMetrics::fit(reread), qPrintable(shapes[i]));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesAndShapesAreNeverFittedPastTheirReach() {
        // How far a note or a shape could grow, worked out from what is
        // written on it, decides which are made big enough for it before a
        // frame is drawn, as it does for text: whatever could be on screen.
        // So it holds for writing of every kind (but the handful of signs far
        // wider than a letter), under a title in any of it, on notes and on
        // shapes of every outline, from tiny to large, wherever they are.
        QString prose, code, wide;
        for (int word = 0; word < 100; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 25 == 24 ? '\n' : ' ');
        for (int i = 0; i < 8; ++i)
            code += QString("\tif (value%1 > limit) {\n\t\treturn compute(value%1);\n\t}\n").arg(i);
        for (char32_t c : {0x1242B, 0x12219, 0x12030})
            wide += QString::fromUcs4(&c, 1);
        const QStringList texts{
            prose.trimmed(),
            code,
            QString("\t").repeated(20),
            (QString("line of text") + QChar(QChar::LineSeparator)).repeated(20),
            QString("x").repeated(300),
            QString("%&@#WM_").repeated(50),
            QString("i").repeated(300),
            QString("a bb ccc dddd eeeee ffffff ggggggg hhhhhhhh iiiiiiiii ").repeated(8),
            QString("0123456789").repeated(30),
            QString("plain words and then \u00e9\n").repeated(15),
            QString("Über die Brücke gehen wir schön langsam weiter, Ærø, Øresund, ŁÓDŹ. ").repeated(6),
            QString("Мы идём по мосту очень медленно, ЖЩЫЮ жщыю. ").repeated(8),
            QString("Περπατάμε πάνω στη γέφυρα πολύ αργά. ").repeated(8),
            QString("Բարեւ աշխարհ, გამარჯობა მსოფლიო. ").repeated(8),
            QString("한국어로 쓴 긴 문장입니다. ").repeated(12),
            QString("中文的句子，很长。「引用」（括号）！？ＡＢＣ１２３ ").repeated(8),
            QString("第1章、2024年10月3日。").repeated(12),
            QString("→ ★ ☺ € — … ∞ ─│┌┐ \u00a0\u3000\u2003\uf120\ue0a0\uf0e7").repeated(12),
            QString("日本語の文章をここに書きます。").repeated(8),
            QString("🙂🚀👩‍👩‍👧‍👦 ").repeated(12),
            QString("هذا نص عربي طويل للاختبار ").repeated(6),
            QString("བསྒྲུབས་སྐྱེད\n").repeated(20),
            QString("မြန်မာနိုင်ငံ၏ ကြောင်း\n").repeated(20),
            QString("a̐̑̒̓̔ ").repeated(30),
            QString("⸻").repeated(40),
            wide.repeated(12),
        };
        // Under titles in any of it too, which are in bold: a font can have
        // more letters in bold than it has in its regular weight.
        const QStringList titles{"A title", "Größe Ærø", "Заголовок", "見出し", "제목 1", "★ \uf120 → …"};
        const QVector<QSizeF> sizes{QSizeF(10, 10), QSizeF(40, 20), QSizeF(152, 152), QSizeF(400, 60)};
        for (const QString &body : texts) {
            QVector<Item> items;
            for (const QSizeF &size : sizes) {
                Item paper = note(7.5, -3.25);
                paper.rect.setSize(size);
                items.append(paper);
                for (int outline = 0; outline < Item::shapeCount; ++outline)
                    items.append(shape(Item::Shape(outline), 7.5, -3.25, size.width(), size.height()));
            }
            for (int i = 0; i < items.size(); ++i) {
                Item &item = items[i];
                const QString &title = titles[i % titles.size()];
                item.text = title + "\n" + body;
                const QRectF before = item.rect;
                const QRectF reach = TextMetrics::reach(item).translated(before.topLeft());
                TextMetrics::fit(item);
                const QString what =
                    QString("%1 of %2 x %3 with %4... on it: fitted from %5 to %6, could reach from %7 to %8")
                        .arg(item.kind == Item::Kind::Shape ? Board::shapeName(item.shape) : QStringLiteral("note"))
                        .arg(before.width()).arg(before.height())
                        .arg(title + " / " + body.left(12))
                        .arg(item.rect.top()).arg(item.rect.bottom())
                        .arg(reach.top()).arg(reach.bottom());
                QVERIFY2(reach.contains(item.rect), qPrintable(what));
            }
        }
        // A shape's reach stands on two things about the part of it that holds
        // what is written: that part is no wider for the shape being taller,
        // and never less than a third of the shape's height.
        for (int outline = 0; outline < Item::shapeCount; ++outline)
            for (qreal width : {1.0, 10.0, 40.0, 152.0, 1000.0}) {
                qreal wider = qInf();
                for (qreal height = 1; height < 1e7; height *= 1.5) {
                    const QRectF part = Geo::textBox(Item::Shape(outline), QSizeF(width, height));
                    const QString what = QString("%1 of %2 x %3: its text in %4 x %5")
                                             .arg(Board::shapeName(Item::Shape(outline)))
                                             .arg(width).arg(height).arg(part.width()).arg(part.height());
                    QVERIFY2(part.width() <= wider + 1e-9, qPrintable(what));
                    QVERIFY2(part.height() >= height / 3 - 1e-9, qPrintable(what));
                    wider = part.width();
                }
            }
    }

    void notesAndShapesAreDrawnWhereTheyGrowToAsSoonAsTheyAreRead() {
        // A list on a note, a long label on a shape further down, which grows
        // up as well as down, and a dashed line from the list to a note far
        // off. On their own, or with thousands of notes well away from them,
        // something written on each: far more than one frame has time to make
        // ready. Which are made ready first goes by how they are numbered, so
        // they are numbered differently every time the file is read.
        QStringList list{"To do"}, label{"Steps"};
        for (int i = 0; i < 150; ++i)
            list << QString("item %1").arg(i + 1);
        for (int i = 0; i < 100; ++i)
            label << QString("step %1").arg(i + 1);
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(6);
        auto file = [&](int first, int crowd) {
            QJsonArray items{
                QJsonObject{{"id", first}, {"type", "note"}, {"x", 0}, {"y", 0}, {"text", list.join('\n')}},
                QJsonObject{{"id", first + 1}, {"type", "shape"}, {"x", 400}, {"y", 4000}, {"text", label.join('\n')}},
                QJsonObject{{"id", first + 2}, {"type", "note"}, {"x", 3000}, {"y", 6000}, {"text", "Done"}},
                QJsonObject{{"id", first + 3}, {"type", "connector"}, {"from", first}, {"to", first + 2},
                            {"dash", "dashed"}}};
            for (int i = 0; i < crowd; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 20000 + (i % 80) * 200.0}, {"y", (i / 80) * 200.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("list", [&file](const QString &home) {
            QDir(home).mkpath("boards/list");
            write(home + "/boards/list/board.json", QJsonDocument(file(1, 0)).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF listed = desk.board().item(1)->rect, labelled = desk.board().item(2)->rect;
        const Item line = *desk.board().item(4);
        QVERIFY2(listed.height() > 2000, qPrintable(QString::number(listed.height())));
        QVERIFY(labelled.top() < 3500 && labelled.bottom() > 4500);
        // Read down the list and the label, and then along the line between
        // the list and the note it leads to, where neither is in view: what is
        // drawn there, once all of it is ready.
        QVector<QPointF> views;
        for (qreal y = 500; y < labelled.bottom(); y += 500)
            views.append(QPointF(0, y));
        const QPointF half = QPointF(desk.canvas->width(), desk.canvas->height()) / 2;
        views.append((line.p1 + line.p2) / 2 - half);
        QVector<SceneLoad> ready;
        for (const QPointF &view : views) {
            desk.canvas->setView(view, 1);
            QCOMPARE(desk.board().fromJson(file(1, 0)), QStringList());
            QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
            desk.window->grabWindow();
            ready.append(desk.canvas->load());
            QVERIFY(ready.last().items > 0);
        }
        int numbered = 1;
        for (int i = 0; i < views.size(); ++i) {
            const QString where = QString("with the view at (%1, %2)").arg(views[i].x()).arg(views[i].y());
            desk.canvas->setView(views[i], 1);
            // The file is read again with the crowd in it, as it is whenever
            // something else has changed it. The note and the shape are back
            // to the size the file gives them, and all of the board is to be
            // made ready again...
            QCOMPARE(desk.board().fromJson(file(numbered += 1000, 4000)), QStringList());
            QCOMPARE(desk.board().item(numbered)->rect.height(), Defaults::noteSize);
            QCOMPARE(desk.canvas->pendingItems(), desk.board().count());
            // ...and the very next frame has what is on screen as big as it
            // is, and the line leaving the list from where the list ends,
            // while the rest of the board waits for the frames after...
            desk.window->grabWindow();
            const QRectF area = desk.canvas->visibleArea();
            if (area.intersects(listed))
                QCOMPARE(desk.board().item(numbered)->rect, listed);
            if (area.intersects(labelled))
                QCOMPARE(desk.board().item(numbered + 1)->rect, labelled);
            if (area.intersects(QRectF(line.p1, line.p2).normalized()))
                QCOMPARE(desk.board().item(numbered + 3)->p1, line.p1);
            QVERIFY2(desk.canvas->pendingItems() > 1000, qPrintable(where));
            // ...and draws them where they are, dash for dash as they are once
            // they are ready.
            const SceneLoad load = desk.canvas->load();
            QVERIFY2(load.items == ready[i].items, qPrintable(where));
            QVERIFY2(load.triangles == ready[i].triangles, qPrintable(where));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesAndShapesFromAFileAreDrawnAsBigAsTheyGrow() {
        // A note with a title and thirty lines on it and no size, and shapes
        // with a list on each: a diamond, which the shape shader draws from
        // its box, and a triangle and a cylinder, drawn as their outlines.
        QStringList lines{"Packing"};
        for (int i = 0; i < 30; ++i)
            lines << QString("line %1 of what the note says").arg(i + 1);
        const QString list = "Ship it?\nthe tests pass\nthe guide is written\nthe changes are listed\nit is tagged";
        QJsonArray items{QJsonObject{{"type", "note"}, {"x", 0}, {"y", 0}, {"text", lines.join('\n')}}};
        const QStringList outlines{"diamond", "triangle", "cylinder"};
        for (int i = 0; i < outlines.size(); ++i)
            items.append(QJsonObject{
                {"type", "shape"}, {"shape", outlines[i]}, {"x", 300 + i * 300}, {"y", 200}, {"text", list}});
        const QJsonObject file{{"sprawl", 1}, {"items", items}};
        // Each as it grows to hold what is written on it.
        QVector<QRectF> grown;
        for (const QJsonValue &value : std::as_const(items)) {
            Item item;
            QCOMPARE(Board::itemFromJson(value.toObject(), item), QString());
            QVERIFY(TextMetrics::fit(item));
            grown.append(item.rect);
        }
        Board board;
        QCOMPARE(board.fromJson(file), QStringList());
        // Drawn by a canvas that keeps what it drew, to see how big they are
        // drawn (see WatchedCanvas).
        QQuickWindow window;
        window.resize(1200, 800);
        WatchedCanvas canvas;
        canvas.setSize(QSizeF(1200, 800));
        canvas.setBoard(&board);
        canvas.setView(QPointF(-50, -50), 1);
        canvas.setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE_WITH_TIMEOUT(canvas.pendingItems(), 0, 30000);
        // Drawn as big as that, to within a unit (an outline is drawn just
        // inside its box): as the board is first drawn, and in the very next
        // frame after it is read again, as when an agent has changed the file.
        for (int read = 1; read <= 2; ++read) {
            if (read > 1)
                QCOMPARE(board.fromJson(file), QStringList());
            window.grabWindow();
            QStringList boxes;
            for (const QRectF &box : std::as_const(canvas.drawn))
                boxes << QString("%1,%2 %3x%4").arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height());
            for (int i = 0; i < grown.size(); ++i) {
                const QRectF &rect = grown[i];
                const QString what = QString("read %1 times, %2 grown to %3,%4 %5x%6, drawn as %7")
                                         .arg(read)
                                         .arg(i == 0 ? QString("the note") : outlines[i - 1])
                                         .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
                                         .arg(boxes.join("; "));
                QCOMPARE(board.item(board.order()[i])->rect, rect);
                QVERIFY2(canvas.drawnAt(rect) >= 0, qPrintable(what));
            }
        }
    }

    void notesAndShapesWithNothingWrittenOnThemKeepTheirSize() {
        // Things kept small on purpose, with nothing written on them: a star
        // and a cross as small as a mark, a note made a strip. With nothing
        // to hold they are as big as their file has them, however small, in
        // the window and in the commands.
        const QJsonArray items{
            QJsonObject{{"type", "shape"}, {"shape", "star"}, {"x", 0}, {"y", 0}, {"w", 24}, {"h", 24}},
            QJsonObject{{"type", "shape"}, {"shape", "cross"}, {"x", 100}, {"y", 0}, {"w", 24}, {"h", 24}},
            QJsonObject{{"type", "note"}, {"x", 200}, {"y", 0}, {"w", 152}, {"h", 16}},
        };
        const QJsonObject file{{"sprawl", 1}, {"items", items}};
        QVector<Item> made;
        for (const QJsonValue &value : items) {
            Item item;
            QCOMPARE(Board::itemFromJson(value.toObject(), item), QString());
            QVERIFY(!TextMetrics::fit(item));
            QCOMPARE(TextMetrics::reach(item), QRectF(QPointF(0, 0), item.rect.size()));
            made.append(item);
        }
        Desk desk("marks", [&file](const QString &home) {
            QDir(home).mkpath("boards/marks");
            write(home + "/boards/marks/board.json", QJsonDocument(file).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        for (int i = 0; i < made.size(); ++i)
            QCOMPARE(desk.board().item(desk.board().order()[i])->rect, made[i].rect);

        QTemporaryDir dir;
        write(dir.filePath("marks.json"), QJsonDocument(file).toJson());
        QCOMPARE(runCli({"sprawl", "add", dir.filePath("marks.json"), "text", "--text", "Added", "--x", "0", "--y",
                         "100"}),
                 0);
        QJsonObject root;
        QCOMPARE(Library::read(dir.filePath("marks.json"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        for (int i = 0; i < made.size(); ++i)
            QCOMPARE(board.item(board.order()[i])->rect, made[i].rect);

        // Something written on one, and it grows to hold it, as ever.
        for (Item item : std::as_const(made)) {
            item.text = "Go";
            QVERIFY(TextMetrics::fit(item));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardExportedAsSoonAsItIsOpenedHoldsAllThatIsWrittenOnIt() {
        // The note with a title and thirty lines on it, an id and no size, in
        // a board's file, and the board exported from the window the moment
        // it is opened: before a frame of it is drawn, and so before the
        // canvas has made the note big enough for what is written on it.
        QStringList lines{"Packing"};
        for (int i = 0; i < 30; ++i)
            lines << QString("line %1 of what the note says").arg(i + 1);
        const QJsonObject written{{"id", 1}, {"type", "note"}, {"x", 0}, {"y", 0}, {"text", lines.join('\n')}};
        const QJsonObject file{{"sprawl", 1}, {"items", QJsonArray{written}}};
        Item grown;
        QCOMPARE(Board::itemFromJson(written, grown), QString());
        QVERIFY(TextMetrics::fit(grown));
        Desk desk("desk", [&file](const QString &home) {
            QDir(home).mkpath("boards/packing");
            write(home + "/boards/packing/board.json", QJsonDocument(file).toJson());
        });
        QVERIFY(desk.ready());
        QVERIFY(desk.backend.openBoard("packing"));
        QCOMPARE(desk.canvas->pendingItems(), 1);
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("packing.png"));
        desk.backend.exportBoard("png", {});
        // The picture has all of the note, at twice its size, with a margin
        // all round...
        const QImage exported(desk.home.filePath("packing.png"));
        const int margin = 32;
        QCOMPARE(exported.size(), QSize(2 * (int(grown.rect.width()) + 2 * margin),
                                        2 * (int(grown.rect.height()) + 2 * margin)));
        // ...and the note is as the window then draws it, which is not
        // something done to the board: nothing to undo, nothing to save.
        QCOMPARE(desk.board().item(desk.board().order().first())->rect, grown.rect);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        QCOMPARE(desk.board().item(desk.board().order().first())->rect, grown.rect);
        QVERIFY(!desk.canvas->canUndo());
        QCOMPARE(desk.backend.saveState(), QString("saved"));
        QVERIFY(desk.backend.flush());
        QCOMPARE(desk.onDisk(), file);
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesWellAboveTheWindowWaitTheirTurnWhenTheBoardIsReadAgain() {
        // A note in the window. Well above it, rows of notes with a long
        // paragraph on each, which grow to a few times the size their file
        // gives them, and could still never reach it: in English, and in
        // languages written in other letters, which are as wide and as tall
        // as the fonts they come from make them. And far off, thousands of
        // notes with something written on each: far more than one frame has
        // time to make ready.
        const QStringList paragraphs{
            QString("lorem ipsum dolor sit amet ").repeated(22),
            QString("Über die Brücke gehen wir schön langsam weiter ").repeated(13),
            QString("Мы идём по мосту очень медленно дальше ").repeated(16),
            QString("Περπατάμε πάνω στη γέφυρα πολύ αργά ").repeated(16),
            QString("日本語の文章をここに書きます。").repeated(30),
            QString("한국어로 쓴 긴 문장입니다. ").repeated(30),
        };
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(6);
        const int above = 120;
        auto file = [&](int first, const QString &paragraph) {
            QJsonArray items{
                QJsonObject{{"id", first}, {"type", "note"}, {"x", 100}, {"y", 100}, {"text", "In the window"}}};
            for (int i = 0; i < above; ++i)
                items.append(QJsonObject{{"id", first + 1 + i},
                                         {"type", "note"},
                                         {"x", (i % 6) * 172.0},
                                         {"y", -2400 - (i / 6) * 172.0},
                                         {"text", QString("Above %1\n").arg(i + 1) + paragraph}});
            for (int i = 0; i < 3000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 20000 + (i % 80) * 200.0}, {"y", (i / 80) * 200.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("rows", [&](const QString &home) {
            QDir(home).mkpath("boards/rows");
            write(home + "/boards/rows/board.json", QJsonDocument(file(1, paragraphs.first())).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF window = desk.canvas->visibleArea();
        int numbered = 1;
        for (const QString &paragraph : paragraphs) {
            const QJsonArray items = file(1, paragraph).value("items").toArray();
            for (int i = 1; i <= above; ++i) {
                Item grown;
                QCOMPARE(Board::itemFromJson(items[i].toObject(), grown), QString());
                TextMetrics::fit(grown);
                QVERIFY2(grown.rect.height() > 2 * Defaults::noteSize &&
                             grown.rect.bottom() < window.top() - window.height(),
                         qPrintable(paragraph.left(12)));
            }
            // Each time the file is read again, the very next frame has the
            // note in the window ready, and is not spent on those above: of
            // them, only any that time left over in the frame gets to are
            // made ready.
            for (int reload = 0; reload < 2; ++reload) {
                const QString when =
                    QString("after reading it again with %1... on the notes above").arg(paragraph.left(12));
                QCOMPARE(desk.board().fromJson(file(numbered += 1000, paragraph)), QStringList());
                desk.window->grabWindow();
                QVERIFY2(desk.canvas->load().items == 1, qPrintable(when));
                int early = 0;
                for (int i = 0; i < above; ++i)
                    if (desk.board().item(numbered + 1 + i)->rect.height() > Defaults::noteSize)
                        ++early;
                QVERIFY2(early < above / 2, qPrintable(QString("%1: %2 made ready").arg(when).arg(early)));
                QVERIFY2(desk.canvas->pendingItems() > 1000, qPrintable(when));
            }
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void notesWithRoomToSpareAreNotLaidOutWhenABoardIsRead() {
        // A board of notes with plenty of room on each for what is written on
        // it, as most notes have. The commands make every board they read big
        // enough for what is written on it, as an export from the window
        // does, and there is nothing of this one that could grow: so they
        // take a fraction of the time laying out all that is written on it
        // would. Each is timed at the fastest of a few goes, so that how busy
        // the machine is does not decide it.
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(8);
        QJsonArray items;
        for (int i = 0; i < 1000; ++i)
            items.append(QJsonObject{{"type", "note"},
                                     {"x", (i % 40) * 172.0},
                                     {"y", (i / 40) * 620.0},
                                     {"h", 600},
                                     {"text", QString("Note %1\n").arg(i + 1) + words}});
        Board board;
        QCOMPARE(board.fromJson(QJsonObject{{"sprawl", 1}, {"items", items}}), QStringList());
        auto fastest = [](const std::function<void()> &work) {
            qint64 best = std::numeric_limits<qint64>::max();
            for (int go = 0; go < 5; ++go) {
                QElapsedTimer timer;
                timer.start();
                work();
                best = qMin(best, timer.nsecsElapsed());
            }
            return best;
        };
        const Palette palette = Palette::fallback();
        const qint64 layingOut = fastest([&board, &palette] {
            for (quint32 id : board.order())
                TextBlock(board.item(id)->text, TextMetrics::styleFor(*board.item(id), palette));
        });
        const qint64 fitting = fastest([&board] { TextMetrics::fitAll(board); });
        QVERIFY2(4 * fitting < layingOut,
                 qPrintable(QString("read in %1 ms, laid out in %2 ms").arg(fitting / 1e6).arg(layingOut / 1e6)));
        // And each is just as big as its file has it.
        for (quint32 id : board.order())
            QCOMPARE(board.item(id)->rect.height(), 600.0);
    }

    void signsWiderThanAllowedForAreStillGivenRoomWhenABoardIsRead() {
        // The widest sign there is, U+FDFD, which type that has it sets
        // twelve times as wide as it is tall: past what any sign is allowed.
        // On notes and shapes of every outline, each given all the room it
        // could need by that allowance, which is not enough. The commands,
        // which leave alone a note or a shape that could never grow, make each
        // of these as big as fit() does: how far it could reach was allowed
        // for, not measured. Where no font has the sign, it is no wider than
        // allowed and there is nothing to grow.
        Board board;
        QVector<Item> made;
        for (int count = 1; count <= 6; ++count)
            for (int kind = 0; kind <= Item::shapeCount; ++kind) {
                Item paper = kind == Item::shapeCount ? note(0, 0) : shape(Item::Shape(kind), 0, 0, 300, 10);
                paper.rect.setWidth(300);
                paper.text = QString(QChar(0xFDFD)).repeated(count);
                bool measured = true;
                paper.rect = TextMetrics::reach(paper, &measured).translated(paper.rect.topLeft());
                QVERIFY(!measured);
                made.append(paper);
                board.add(paper);
            }
        TextMetrics::fitAll(board);
        for (int i = 0; i < made.size(); ++i) {
            Item fitted = made[i];
            TextMetrics::fit(fitted);
            QCOMPARE(board.item(board.order()[i])->rect, fitted.rect);
        }
    }

    void bigFramesAndShapesFarApartComeIntoViewOneByOne() {
        // A plan laid out in rows far apart: a row of frames, two of shapes
        // with something written on each, and a note, text, a stroke, a line
        // and a picture. Every one of them is bigger than a square of the
        // board (see scene.cpp), and a window at 100% only ever has one of
        // them near it.
        const QStringList outlines{"rect",     "rounded", "ellipse",  "diamond", "pill",
                                   "triangle", "star",    "cylinder", "bubble",  "hexagon"};
        const int columns = 5, count = columns * 4;
        auto corner = [&](int i) { return QPointF((i % columns) * 3400.0, (i / columns) * 1700.0); };
        Desk desk("plan", [&](const QString &home) {
            QDir(home).mkpath("boards/plan/assets");
            picture(home + "/boards/plan/assets/wide.png", QSize(240, 100));
            auto box = [&](const QString &type, int i) {
                const QPointF at = corner(i);
                return QJsonObject{{"type", type}, {"x", at.x()}, {"y", at.y()}, {"w", 2400}, {"h", 1000}};
            };
            QJsonArray items;
            for (int i = 0; i < columns; ++i) {
                QJsonObject frame = box("frame", i);
                frame["text"] = QString("Stage %1").arg(i + 1);
                items.append(frame);
            }
            for (int i = 0; i < outlines.size(); ++i) {
                QJsonObject shape = box("shape", columns + i);
                shape["shape"] = outlines[i];
                shape["text"] = "Milestone\nwhat is done by then\nand who does it";
                items.append(shape);
            }
            const int last = columns * 3;
            QJsonObject paper = box("note", last);
            paper["text"] = "Risks\nwhat could slow it down";
            items.append(paper);
            items.append(QJsonObject{{"type", "text"}, {"x", corner(last + 1).x()}, {"y", corner(last + 1).y()},
                                     {"wrap", 2400}, {"text", QString("notes on the plan ").repeated(40)}});
            QJsonArray points;
            for (int p = 0; p <= 24; ++p) {
                points.append(p * 100.0);
                points.append(p % 2 ? 600.0 : 0.0);
            }
            items.append(QJsonObject{
                {"type", "stroke"}, {"x", corner(last + 2).x()}, {"y", corner(last + 2).y()}, {"points", points}});
            const QPointF from = corner(last + 3);
            items.append(QJsonObject{{"type", "connector"}, {"x1", from.x()}, {"y1", from.y()},
                                     {"x2", from.x() + 2400}, {"y2", from.y() + 600}});
            QJsonObject image = box("image", last + 4);
            image["asset"] = "wide.png";
            items.append(image);
            write(home + "/boards/plan/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        }, QSize(1800, 1000));
        QVERIFY(desk.ready());
        QCOMPARE(desk.board().count(), count);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QVector<quint32> order = desk.board().order();
        auto what = [&desk, &order](int i) { return Board::kindName(desk.board().item(order[i])->kind); };
        for (int i = 0; i < count; ++i) {
            const QRectF bounds = desk.board().bounds(order[i]);
            QVERIFY2(bounds.width() > 2048 || bounds.height() > 2048, qPrintable(what(i)));
        }
        auto view = [&desk](QPointF origin, qreal zoom) {
            desk.canvas->setView(origin, zoom);
            desk.window->grabWindow();
            return desk.canvas->load();
        };

        // Looked at one by one at 100%, each is all that is drawn, and all
        // that is counted against the frame.
        for (int i = 0; i < count; ++i) {
            const SceneLoad load = view(corner(i) - QPointF(300, 200), 1);
            const QString where = QString("at the %1 (%2 of %3): %4 drawn").arg(what(i)).arg(i + 1).arg(count);
            QVERIFY2(load.items == 1, qPrintable(where.arg(load.items)));
            QVERIFY2(!load.reduced, qPrintable(where.arg(load.items)));
        }

        // And seen whole, all of them are drawn.
        const SceneLoad whole = view(QPointF(-500, -500), 0.1);
        QCOMPARE(whole.items, count);
        QVERIFY(!whole.reduced);
        QCOMPARE(desk.warnings, QStringList());
    }

    void bigThingsNearEachOtherAreDrawnTogetherAndInTheirOrder() {
        // Swimlanes: a big rectangle, five lanes across it each marked by a
        // line as long, two notes on every line, and a frame around all of
        // it, put there last. The rectangle, the lines and the frame are each
        // bigger than a square of the board (see scene.cpp), and all of it is
        // in the same square.
        const int lanes = 5;
        QJsonArray items{QJsonObject{{"type", "shape"}, {"x", 0}, {"y", 0}, {"w", 4000}, {"h", 1800}}};
        for (int lane = 1; lane <= lanes; ++lane)
            items.append(QJsonObject{{"type", "connector"}, {"x1", 100}, {"y1", lane * 300}, {"x2", 3900},
                                     {"y2", lane * 300}, {"end", "none"}});
        for (int lane = 1; lane <= lanes; ++lane)
            for (const qreal x : {700.0, 1700.0})
                items.append(QJsonObject{{"type", "note"}, {"x", x - 76}, {"y", lane * 300 - 76}});
        items.append(QJsonObject{{"type", "frame"}, {"x", -100}, {"y", -100}, {"w", 4200}, {"h", 2000}});
        Board board;
        QCOMPARE(board.fromJson(QJsonObject{{"sprawl", 1}, {"items", items}}), QStringList());
        const QVector<quint32> order = board.order();
        const quint32 rectangle = order.first(), frame = order.last();
        const QVector<quint32> lines = order.mid(1, lanes), notes = order.mid(1 + lanes, 2 * lanes);
        // Drawn by a canvas that keeps what it drew, in the order it drew it
        // (see WatchedCanvas).
        QQuickWindow window;
        window.resize(1200, 800);
        WatchedCanvas canvas;
        canvas.setSize(QSizeF(1200, 800));
        canvas.setBoard(&board);
        canvas.setView(QPointF(-300, -300), 0.25);
        canvas.setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE_WITH_TIMEOUT(canvas.pendingItems(), 0, 30000);
        window.grabWindow();

        // Seen whole, all of it is drawn, from a group for each of the four
        // things stacked: the rectangle, the lines, the notes and the frame.
        // However many lines there are, they are drawn together, not each
        // from a group of its own, which would be a call or two apiece.
        const SceneLoad whole = canvas.load();
        QCOMPARE(whole.items, board.count());
        QCOMPARE(whole.shown, 4);
        QCOMPARE(whole.buckets, 4);
        // And in the order they are stacked: the rectangle under the lines,
        // the lines under their notes, and the frame over all of it.
        auto at = [&](quint32 id) { return canvas.drawnAt(board.item(id)->rect); };
        QVERIFY(at(rectangle) >= 0);
        for (quint32 line : lines) {
            QVERIFY(at(line) > at(rectangle));
            for (quint32 note : notes)
                QVERIFY(at(note) > at(line));
        }
        for (quint32 note : notes)
            QVERIFY(at(frame) > at(note));

        // Up close, at 400%, each line is drawn only where it is: at the left
        // end of one there is the rectangle under it and the frame around
        // it, and nothing else near.
        for (int lane = 1; lane <= lanes; ++lane) {
            canvas.setView(QPointF(50, lane * 300 - 100), 4);
            window.grabWindow();
            const QString where = QString("at lane %1: %2 drawn").arg(lane).arg(canvas.load().items);
            QVERIFY2(canvas.load().items == 3, qPrintable(where));
            QVERIFY2(at(lines[lane - 1]) >= 0 && at(rectangle) >= 0 && at(frame) >= 0, qPrintable(where));
        }

        // The frame sent to the back and the rectangle brought to the front,
        // each is drawn from a group where it now belongs: the frame under all
        // of the rest, and the rectangle over all of it.
        board.restack({frame}, false);
        board.restack({rectangle}, true);
        canvas.setView(QPointF(-300, -300), 0.25);
        window.grabWindow();
        QCOMPARE(canvas.load().items, board.count());
        QVERIFY(at(frame) >= 0);
        for (quint32 line : lines) {
            QVERIFY(at(line) > at(frame));
            for (quint32 note : notes)
                QVERIFY(at(note) > at(line));
        }
        for (quint32 note : notes)
            QVERIFY(at(rectangle) > at(note));
    }

    void longLinesCloseTogetherAreDrawnOneByOneAsTheyChange() {
        // A page ruled with lines longer than a square of the board, more of
        // them in one square than a bucket has parts (see scene.cpp). Looked
        // at up close, each is drawn only where it is, as lines are taken
        // away and put back, carried off and back, and made short and long.
        Desk desk;
        QVERIFY(desk.ready());
        auto line = [](qreal y, qreal length = 2200) {
            Item item;
            Board::itemFromJson(QJsonObject{{"type", "connector"}, {"x1", 100}, {"y1", y}, {"x2", 100 + length},
                                            {"y2", y}, {"end", "none"}},
                                item);
            return item;
        };
        // Each line on the page, and where it is.
        QMap<quint32, qreal> lines;
        for (int i = 0; i < 17; ++i)
            lines.insert(desk.add(line(60 + i * 120)), 60 + i * 120);
        for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
            QCOMPARE(desk.board().item(it.key())->kind, Item::Kind::Connector);
            QVERIFY(desk.board().bounds(it.key()).width() > 2048);
        }
        auto drawn = [&desk](qreal y) {
            desk.canvas->setView(QPointF(60, y - 40), 8);
            desk.window->grabWindow();
            return desk.canvas->load().items;
        };
        // What is drawn wrong where a line is, if anything.
        auto wrong = [&]() {
            for (auto it = lines.cbegin(); it != lines.cend(); ++it)
                if (const int items = drawn(it.value()); items != 1)
                    return QString("with the view at y = %1, %2 drawn").arg(it.value()).arg(items);
            return QString();
        };
        QString problem = wrong();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        // Drawn from two groups: as many as a group has room for in one.
        QCOMPARE(desk.canvas->load().buckets, 2);

        // Some are taken away, and where they were nothing is drawn; others
        // are put on the page in their place, and drawn there alone.
        const QList<quint32> ids = lines.keys();
        for (int i : {2, 9, 16}) {
            const qreal y = lines.take(ids[i]);
            desk.board().remove(ids[i]);
            QCOMPARE(drawn(y), 0);
        }
        problem = wrong();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        for (int i : {9, 2})
            lines.insert(desk.add(line(60 + i * 120)), 60 + i * 120);
        problem = wrong();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        // One is carried off the page, and back again by undo.
        const quint32 carried = ids[5];
        const qreal home = lines.value(carried);
        Item away = line(30000);
        away.id = carried;
        desk.board().set(away);
        lines.insert(carried, 30000);
        problem = wrong();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(drawn(home), 0);
        desk.board().undo();
        lines.insert(carried, home);
        problem = wrong();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        // And one is made short, smaller than a square, and long again.
        for (const qreal length : {1000.0, 2200.0}) {
            Item made = line(lines.value(ids[7]), length);
            made.id = ids[7];
            desk.board().set(made);
            problem = wrong();
            QVERIFY2(problem.isEmpty(), qPrintable(QString("%1 long, %2").arg(length).arg(problem)));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPictureIsUploadedOnceHoweverOftenItIsDrawnAgain() {
        // A photo on a board, and a note beside it. However often the photo
        // is drawn again, it is drawn from the picture the GPU was handed the
        // first time: in a new theme, carried far off and back, beside a note
        // that changes, resized, grouped, and shown twice. Handed over again,
        // a photo can be tens of megabytes, and its mipmaps are made again.
        Desk desk("desk", [](const QString &home) {
            QDir(home).mkpath("theme");
            write(home + "/theme/colors.toml", "background = \"#1a1b26\"\n");
        });
        QVERIFY(desk.ready());
        QDir().mkpath(desk.backend.assetFolder());
        picture(desk.backend.assetFolder() + "/photo.png", QSize(640, 480));
        Item photo;
        photo.kind = Item::Kind::Image;
        photo.asset = "photo.png";
        photo.rect = QRectF(100, 100, 320, 240);
        const quint32 id = desk.add(photo);
        const quint32 beside = desk.add(note(500, 100, "Beside it"));
        QTRY_VERIFY(!desk.canvas->loadingImages());
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 2);
        QCOMPARE(desk.canvas->picturesUploaded(), 1);
        QCOMPARE(desk.canvas->picturesHeld(), 1);

        // A new theme, in which everything is drawn again. (A theme switch
        // replaces the file.)
        QFile::remove(desk.home.filePath("theme/colors.toml"));
        write(desk.home.filePath("theme/colors.toml"), "mode = \"light\"\nbackground = \"#fafafa\"\n");
        QTRY_COMPARE(desk.window->color(), QColor("#fafafa"));
        QTRY_COMPARE(desk.canvas->pendingItems(), 0);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 1);

        // Carried to another part of the board entirely, out of view, and
        // looked at there; then back again. The GPU keeps it all the while.
        Item away = *desk.board().item(id);
        away.rect.moveTo(30000, 20000);
        desk.board().set(away);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        desk.canvas->setView(QPointF(29800, 19800), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        desk.board().undo();
        desk.canvas->setView(QPointF(0, 0), 1);
        desk.window->grabWindow();
        QCOMPARE(desk.board().item(id)->rect, photo.rect);
        QCOMPARE(desk.canvas->load().items, 2);
        QCOMPARE(desk.canvas->picturesUploaded(), 1);

        // The note beside it written on, and colored.
        Item written = *desk.board().item(beside);
        written.text = "Beside it, and more";
        written.color = "blue";
        desk.board().set(written);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 1);

        // The photo resized by its handle as a hand does it, with a frame
        // drawn at every step of the drag...
        desk.click(photo.rect.center());
        QCOMPARE(desk.canvas->selection(), QVector<quint32>{id});
        const QPointF handle = photo.rect.bottomRight() + QPointF(3, 3);
        desk.press(handle);
        for (int step = 1; step <= 10; ++step) {
            desk.moveTo(handle + QPointF(step * 8, step * 6));
            desk.window->grabWindow();
        }
        desk.release(handle + QPointF(80, 60));
        desk.window->grabWindow();
        QVERIFY(desk.board().item(id)->rect.width() > photo.rect.width());
        QCOMPARE(desk.canvas->picturesUploaded(), 1);

        // ...grouped with the note...
        desk.canvas->select({id, beside});
        desk.canvas->groupSelection();
        QVERIFY(desk.board().item(id)->group != 0);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 1);

        // ...and shown again, below itself.
        Item copy = photo;
        copy.rect.moveTo(100, 420);
        desk.add(copy);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 3);
        QCOMPARE(desk.canvas->picturesUploaded(), 1);
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPictureIsLetGoOfWithTheLastThingShowingIt() {
        // A photo shown twice, in view and far off, and a map beside it.
        Desk desk;
        QVERIFY(desk.ready());
        QDir().mkpath(desk.backend.assetFolder());
        picture(desk.backend.assetFolder() + "/photo.png", QSize(640, 480));
        picture(desk.backend.assetFolder() + "/map.png", QSize(400, 400), QColor("#9ece6a"));
        Item photo;
        photo.kind = Item::Kind::Image;
        photo.asset = "photo.png";
        photo.rect = QRectF(100, 100, 320, 240);
        const quint32 inView = desk.add(photo);
        photo.rect.moveTo(30000, 20000);
        const quint32 farOff = desk.add(photo);
        Item map = photo;
        map.asset = "map.png";
        map.rect = QRectF(500, 100, 200, 200);
        desk.add(map);
        QTRY_VERIFY(!desk.canvas->loadingImages());
        QTRY_COMPARE(desk.canvas->pendingItems(), 0);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 2);
        QCOMPARE(desk.canvas->picturesHeld(), 2);

        // As long as anything shows the photo, in view or not, the GPU keeps
        // it...
        desk.board().remove(inView);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 2);
        // ...and once nothing does, it is let go of.
        desk.board().remove(farOff);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        // Brought back, it is handed over again.
        desk.board().undo();
        QTRY_COMPARE(desk.canvas->pendingItems(), 0);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 2);
        QCOMPARE(desk.canvas->picturesUploaded(), 3);
        // And with another board open, one with no pictures on it, the GPU
        // holds none.
        QVERIFY(desk.backend.newBoard("bare"));
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 0);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPictureReadAgainIsDrawnAsItIsNow() {
        // A photo shown twice, which something outside then changes on disk,
        // and which is read again before a frame is drawn with it: here,
        // while the window is hidden, by the canvas being given the pictures
        // of another folder and then these again, which has it forget what
        // it read. Both are drawn from the picture as it is now, handed to
        // the GPU afresh, and the one before is let go of. Neither is drawn
        // from that one because the other still showed it.
        QTemporaryDir dir;
        const QString assets = dir.filePath("assets");
        QDir().mkpath(assets);
        const QColor before("#f7768e"), after("#9ece6a");
        picture(assets + "/photo.png", QSize(640, 480), before);
        Board board;
        Item photo;
        photo.kind = Item::Kind::Image;
        photo.asset = "photo.png";
        photo.rect = QRectF(100, 100, 320, 240);
        board.add(photo);
        photo.rect.moveTo(500, 100);
        board.add(photo);
        QQuickWindow window;
        window.resize(1000, 700);
        CanvasView canvas;
        canvas.setSize(QSizeF(1000, 700));
        canvas.setBoard(&board);
        canvas.setAssetFolder(assets);
        canvas.setView(QPointF(0, 0), 1);
        canvas.setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(!canvas.loadingImages());
        QTRY_COMPARE(canvas.pendingItems(), 0);
        // The middle of each, as drawn.
        auto drawn = [&window] {
            const QImage frame = window.grabWindow();
            const qreal pixels = frame.devicePixelRatio();
            return QList<QColor>{frame.pixelColor((QPointF(260, 220) * pixels).toPoint()),
                                 frame.pixelColor((QPointF(660, 220) * pixels).toPoint())};
        };
        QCOMPARE(drawn(), (QList<QColor>{before, before}));
        QCOMPARE(canvas.picturesUploaded(), 1);

        picture(assets + "/photo.png", QSize(640, 480), after);
        window.hide();
        canvas.setAssetFolder(dir.path());
        canvas.setAssetFolder(assets);
        QTRY_VERIFY(!canvas.loadingImages());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(drawn(), (QList<QColor>{after, after}));
        QCOMPARE(canvas.picturesUploaded(), 2);
        QCOMPARE(canvas.picturesHeld(), 1);
    }

    void aPictureIsKeptWhileItsBoardIsReadAgain() {
        // A photo in view, and far off the photo again, a map and a plan,
        // beyond a thousand texts with something written in each: more than
        // one frame has time to make ready. Off screen, text is made ready
        // before anything else (until it is measured it could be on screen),
        // so the pictures far off are made ready only once all of it is.
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(6);
        auto file = [&words](const QStringList &pictures) {
            QJsonArray items;
            auto image = [&items](const QString &name, QPointF at) {
                items.append(QJsonObject{{"type", "image"}, {"asset", name + ".png"}, {"x", at.x()}, {"y", at.y()},
                                         {"w", 320}, {"h", 240}});
            };
            if (pictures.contains("photo")) {
                image("photo", QPointF(100, 100));
                image("photo", QPointF(30000, 20000));
            }
            if (pictures.contains("map"))
                image("map", QPointF(30000, 30000));
            if (pictures.contains("plan"))
                image("plan", QPointF(30000, 40000));
            for (int i = 0; i < 1000; ++i)
                items.append(QJsonObject{{"type", "text"}, {"x", 20000 + (i % 40) * 300.0}, {"y", (i / 40) * 200.0},
                                         {"wrap", 240}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        const QStringList all{"photo", "map", "plan"};
        Desk desk("pictures", [&](const QString &home) {
            QDir(home).mkpath("boards/pictures/assets");
            write(home + "/boards/pictures/board.json", QJsonDocument(file(all)).toJson());
            picture(home + "/boards/pictures/assets/photo.png", QSize(640, 480));
            picture(home + "/boards/pictures/assets/map.png", QSize(400, 400), QColor("#9ece6a"));
            picture(home + "/boards/pictures/assets/plan.png", QSize(400, 400), QColor("#e0af68"));
        });
        QVERIFY(desk.ready());
        QTRY_VERIFY(!desk.canvas->loadingImages());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 3);
        QCOMPARE(desk.canvas->picturesHeld(), 3);

        // The file is read again, as it is whenever something else has
        // changed it, and all of the board is to be made ready again. The
        // very next frame draws the photo in view from the picture the GPU
        // has, and makes some of the text far off ready but not all of it:
        // more is waiting than the three pictures far off, which wait behind
        // the text...
        QCOMPARE(desk.board().fromJson(file(all)), QStringList());
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        QCOMPARE(desk.canvas->picturesUploaded(), 3);
        QVERIFY2(desk.canvas->pendingItems() > 3, qPrintable(QString::number(desk.canvas->pendingItems())));
        // ...while the GPU keeps their pictures for when they are ready: none
        // is handed to it again.
        QCOMPARE(desk.canvas->picturesHeld(), 3);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesUploaded(), 3);
        QCOMPARE(desk.canvas->picturesHeld(), 3);

        // Read again, and the map taken off the board before it is ready: the
        // GPU lets go of its picture with the next frame, not once all of the
        // board is ready, and keeps the others for what is to show them.
        QCOMPARE(desk.board().fromJson(file(all)), QStringList());
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 3);
        quint32 map = 0;
        for (const Item *item : desk.all(Item::Kind::Image))
            if (item->asset == "map.png")
                map = item->id;
        desk.board().remove(map);
        desk.window->grabWindow();
        QVERIFY2(desk.canvas->pendingItems() > 2, qPrintable(QString::number(desk.canvas->pendingItems())));
        QCOMPARE(desk.canvas->picturesHeld(), 2);
        // Before that is ready either, something else changes the file again,
        // and it is read with only the photo on it: the plan is let go of
        // with the first frame, and the photo kept for the one far off...
        QCOMPARE(desk.board().fromJson(file({"photo"})), QStringList());
        desk.window->grabWindow();
        QVERIFY2(desk.canvas->pendingItems() > 1, qPrintable(QString::number(desk.canvas->pendingItems())));
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        // ...which shows it once it is ready, from the picture handed over at
        // first.
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        QCOMPARE(desk.canvas->picturesUploaded(), 3);
        // Then both photos are changed to show the plan instead: it is handed
        // over again, and the photo, which nothing shows now, is let go of.
        QVector<quint32> photos;
        for (const Item *item : desk.all(Item::Kind::Image))
            photos.append(item->id);
        for (quint32 id : std::as_const(photos)) {
            Item changed = *desk.board().item(id);
            changed.asset = "plan.png";
            desk.board().set(changed);
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        QCOMPARE(desk.canvas->picturesUploaded(), 4);

        // Another board is opened, with the same text and the plan, which it
        // keeps in a folder of its own, and looked at from the same corner
        // (shown whole, as a board is when it is first opened, all of it
        // would be made ready in its first frame). Its pictures are read anew
        // from there, so the GPU lets go of this board's plan in that first
        // frame, not once all of the board is ready, and is handed that
        // board's once it has been read.
        QDir().mkpath(desk.library.assetsFolder("elsewhere"));
        write(desk.library.boardFile("elsewhere"), QJsonDocument(file({"plan"})).toJson());
        picture(desk.library.assetsFolder("elsewhere") + "/plan.png", QSize(400, 400), QColor("#e0af68"));
        QVERIFY(desk.backend.openBoard("elsewhere"));
        desk.canvas->setView(QPointF(0, 0), 1);
        desk.window->grabWindow();
        QVERIFY2(desk.canvas->pendingItems() > 0, qPrintable(QString::number(desk.canvas->pendingItems())));
        QCOMPARE(desk.canvas->picturesHeld(), 0);
        QTRY_VERIFY(!desk.canvas->loadingImages());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->picturesHeld(), 1);
        QCOMPARE(desk.canvas->picturesUploaded(), 5);
        QCOMPARE(desk.warnings, QStringList());
    }

    void theFirstFrameDoesNotWaitForTextOutOfView() {
        // A big board of labels, a few words each, wrapped so that they run
        // to several lines: read from its file, each is a line tall until it
        // is measured.
        const int columns = 120, rows = 60;
        Desk desk("desk", [&](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < columns * rows; ++i)
                items.append(QJsonObject{{"type", "text"},
                                         {"x", (i % columns) * 200.0},
                                         {"y", (i / columns) * 200.0},
                                         {"wrap", 64},
                                         {"text", QString("Label %1 with a few words").arg(i + 1)}});
            QDir(home).mkpath("boards/labels");
            write(home + "/boards/labels/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        // Whether a label is measured, and where it is once it is.
        auto measured = [&desk](quint32 id, QRectF &box) {
            Item fitted = *desk.board().item(id);
            const bool done = !TextMetrics::fit(fitted);
            box = fitted.rect;
            return done;
        };
        QVERIFY(desk.backend.openBoard("labels"));
        QCOMPARE(desk.canvas->pendingItems(), columns * rows);
        // Opened for the first time it is shown whole, as far out as the
        // canvas goes: thousands of labels in the window, and thousands more
        // around it.
        const QRectF window = desk.canvas->visibleArea();
        QVERIFY2(window.width() > 5000, qPrintable(QString::number(window.width())));
        desk.window->grabWindow();
        // The first frame has measured all that is in the window, and drawn
        // it...
        const qreal clear = qMax(window.width(), window.height()) / 10;
        const QRectF around = window.adjusted(-clear, -clear, clear, clear);
        int inView = 0, away = 0, early = 0;
        for (quint32 id : desk.board().order()) {
            QRectF box;
            const bool done = measured(id, box);
            if (box.intersects(window)) {
                ++inView;
                QVERIFY2(done, qPrintable(desk.board().item(id)->text));
            } else if (!box.intersects(around)) {
                ++away;
                early += done ? 1 : 0;
            }
        }
        QVERIFY2(inView > 1000 && away > 1000, qPrintable(QString("%1 in view, %2 away").arg(inView).arg(away)));
        QVERIFY2(desk.canvas->load().items >= inView, qPrintable(QString::number(desk.canvas->load().items)));
        // ...and of the labels away from it, only any that time left over in
        // the frame got to.
        QVERIFY2(early < away / 10, qPrintable(QString("%1 of %2 away measured").arg(early).arg(away)));

        // The rest is measured in the frames after. Meanwhile, some of the
        // labels far off are written on again: some measured already, and some
        // not yet.
        QVector<quint32> ready, waiting;
        for (int frame = 0; frame < 1000 && (ready.size() < 40 || waiting.size() < 40); ++frame) {
            // The frame after: a frame asks for the next while anything is
            // still to be made ready.
            QCoreApplication::processEvents();
            desk.window->grabWindow();
            ready.clear();
            waiting.clear();
            for (quint32 id : desk.board().order()) {
                QRectF box;
                const bool done = measured(id, box);
                if (!box.intersects(around))
                    (done ? ready : waiting).append(id);
            }
        }
        QVERIFY2(ready.size() >= 40 && waiting.size() >= 40,
                 qPrintable(QString("%1 measured, %2 not").arg(ready.size()).arg(waiting.size())));
        for (const QVector<quint32> &some : {ready.mid(0, 40), waiting.mid(0, 40)})
            for (quint32 id : some) {
                Item label = *desk.board().item(id);
                label.text += ", written on again and now rather longer";
                desk.board().set(label);
            }
        // In the end every label is measured, as it reads now.
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        for (quint32 id : desk.board().order()) {
            QRectF box;
            QVERIFY2(measured(id, box), qPrintable(desk.board().item(id)->text));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void textMovedBeforeItIsMeasuredIsDrawnWhereItReaches() {
        // A crowd of notes with plenty written on each, all in view: more than
        // one frame has time to make ready. Far off, prose in large type.
        QString prose;
        for (int word = 0; word < 300; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        auto file = [&](int text) {
            QJsonArray items{QJsonObject{{"id", text}, {"type", "text"}, {"x", 40000}, {"y", 0}, {"size", 48},
                                         {"wrap", 480}, {"text", prose.trimmed()}}};
            for (int i = 0; i < 1000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 600 + (i % 34) * 90.0}, {"y", (i / 34) * 90.0}, {"text", words}});
            return QJsonObject{{"sprawl", 1}, {"items", items}};
        };
        Desk desk("moved", [&file](const QString &home) {
            QDir(home).mkpath("boards/moved");
            write(home + "/boards/moved/board.json", QJsonDocument(file(1)).toJson());
        });
        QVERIFY(desk.ready());
        desk.canvas->setView(QPointF(-100, -100), 0.25);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Measured, the prose is thousands of units tall. Moved beside the
        // crowd, it starts well above the window and runs on down through it.
        const QRectF window = desk.canvas->visibleArea();
        QRectF page = desk.all(Item::Kind::Text).first()->rect;
        QVERIFY2(page.height() > 2 * window.height(), qPrintable(QString::number(page.height())));
        page.moveTopLeft(QPointF(0, window.top() - page.height() / 2));
        auto move = [&desk](const QPointF &corner) {
            Item text = *desk.all(Item::Kind::Text).first();
            text.rect.moveTopLeft(corner);
            desk.board().set(text);
        };
        move(page.topLeft());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad settled = desk.canvas->load();
        QCOMPARE(desk.all(Item::Kind::Text).first()->rect, page);
        // Each time the file is read again, the prose is back far off at a
        // guess of its size, a line to each paragraph, and is moved beside
        // the crowd before a frame is drawn: what it is guessed to cover is
        // well above the window, and what it could reach runs down through
        // it. The very next frame has it measured and drawn, every letter in
        // the window, as it is once all of the board is ready again.
        int numbered = 1;
        for (int reload = 0; reload < 3; ++reload) {
            const QString when = QString("after reading it again %1 times").arg(reload + 1);
            QCOMPARE(desk.board().fromJson(file(numbered += 2000)), QStringList());
            move(page.topLeft());
            QVERIFY(desk.all(Item::Kind::Text).first()->rect.bottom() < window.top() - window.height() / 10);
            desk.window->grabWindow();
            QCOMPARE(desk.all(Item::Kind::Text).first()->rect, page);
            const SceneLoad load = desk.canvas->load();
            QVERIFY2(load.items == settled.items, qPrintable(when));
            QVERIFY2(!load.reduced && load.triangles == settled.triangles, qPrintable(when));
            QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void theDotGridSitsOnTheBoardWhereverTheCanvasIsInTheWindow() {
        // In the window the canvas is beside the rail, 44 pixels in from the
        // left (in a right-to-left language the rail is on the right, and the
        // canvas starts at the window's edge). Its dot grid is one quad over
        // all of it, and the shader works out which point of the board each
        // pixel shows from where the pixel is in the quad (grid.vert): the
        // middle of a dot is where that is a multiple of the step. The
        // software renderer the tests run with draws no grid, so this follows
        // what it is drawn from (see WatchedCanvas) as the scene graph does.
        // To batch a quad, it hands the shader its corners already moved to
        // where they are in the window (nothing above the canvas clips it),
        // unless its material asks for the whole matrix or for no batching at
        // all. Batched so, every dot would be drawn as far left of its place,
        // and up, as the canvas is from the window's corner: beside the rail
        // at 100%, four pixels right of the board's multiples of 24. The board
        // has a note with something written on it, and a photo.
        QTemporaryDir assets;
        picture(assets.filePath("photo.png"), QSize(64, 48));
        Board board;
        board.add(note(0, 0, "On the corner"));
        Item photo;
        photo.kind = Item::Kind::Image;
        photo.asset = "photo.png";
        photo.rect = QRectF(192, 0, 64, 48);
        board.add(photo);
        const QList<QPair<QPointF, qreal>> views{
            {{0, 0}, 1}, {{-100.3, -50.7}, 1.37}, {{5000.5, 3000.25}, 0.5}, {{-60, -40}, 2.5}};
        for (const QPointF place : {QPointF(0, 0), QPointF(44, 0), QPointF(97, 31)}) {
            QQuickWindow window;
            window.resize(1000, 700);
            WatchedCanvas canvas;
            canvas.setPosition(place);
            canvas.setSize(QSizeF(1000 - place.x(), 700 - place.y()));
            canvas.setAssetFolder(assets.path());
            canvas.setBoard(&board);
            canvas.setParentItem(window.contentItem());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTRY_VERIFY(!canvas.loadingImages());
            for (const auto &[origin, zoom] : views) {
                canvas.setView(origin, zoom);
                window.grabWindow();
                const WatchedCanvas::Grid &grid = canvas.grid;
                QCOMPARE(grid.corners, QRectF(QPointF(0, 0), canvas.size()));
                const bool batched = !grid.flags.testFlag(QSGMaterial::RequiresFullMatrix) &&
                                     !grid.flags.testFlag(QSGMaterial::NoBatching);
                // Each point of the board in view that is a multiple of the
                // step is drawn in the middle of a dot.
                const qreal step = grid.spacing;
                const QRectF area = canvas.visibleArea();
                for (qreal x = std::ceil(area.left() / step) * step; x < area.right(); x += step)
                    for (qreal y = std::ceil(area.top() / step) * step; y < area.bottom(); y += step) {
                        const QPointF point(x, y), at = canvas.view.map(point);
                        const QPointF dot = dotFor(point, batched ? canvas.mapToScene(at) : at, grid.origin,
                                                   grid.zoom, grid.spacing);
                        if (qAbs(dot.x()) > 0.01 || qAbs(dot.y()) > 0.01)
                            QFAIL(qPrintable(QString("With the canvas at %1,%2 in the window and the view at %3,%4 "
                                                     "and %5%, the dot for board %6,%7 is drawn %8,%9 pixels from it")
                                                 .arg(place.x()).arg(place.y()).arg(origin.x()).arg(origin.y())
                                                 .arg(zoom * 100).arg(x).arg(y).arg(dot.x()).arg(dot.y())));
                    }
            }
        }
    }

    void theDotGridSitsOnTheBoardAtEveryZoomAndPixelDensity() {
        // What the grid is handed for a view (GridMaterial::follow()), on a
        // display of one device pixel to the pixel and on one of two: at any
        // zoom, panned however far, a point of the board that is a multiple
        // of the step is drawn in the middle of a dot, as above. The dots are
        // 16 to 32 pixels apart (the coarser grid's twice that), and a pixel
        // in radius on either display.
        for (const qreal pixels : {1.0, 2.0})
            for (const qreal zoom : {0.1, 0.3, 0.5, 2.0 / 3, 1.0, 1.37, 2.5, 8.0})
                for (const QPointF origin :
                     {QPointF(0, 0), QPointF(-100.3, -50.7), QPointF(5000.5, 3000.25), QPointF(-123456.75, 76543.5)}) {
                    // Panned as a canvas pans: by whole device pixels.
                    QMatrix4x4 view;
                    view.translate(float(std::round(-origin.x() * zoom * pixels) / pixels),
                                   float(std::round(-origin.y() * zoom * pixels) / pixels));
                    view.scale(float(zoom));
                    GridMaterial grid;
                    grid.follow(view, zoom, pixels);
                    const QString where = QString("at %1%, panned to %2,%3, with %4 device pixels to the pixel")
                                              .arg(zoom * 100).arg(origin.x()).arg(origin.y()).arg(pixels);
                    QVERIFY2(grid.spacing * zoom >= 16 && grid.spacing * zoom < 32, qPrintable(where));
                    QCOMPARE(grid.dpr, float(pixels));
                    QCOMPARE(grid.radius, float(pixels));
                    // A window's worth of the board, which the grid's shader is
                    // handed where the canvas draws it.
                    const qreal step = grid.spacing;
                    const QRectF area(origin, QSizeF(1000, 700) / zoom);
                    for (qreal x = std::ceil(area.left() / step) * step; x < area.right(); x += step)
                        for (qreal y = std::ceil(area.top() / step) * step; y < area.bottom(); y += step) {
                            const QPointF point(x, y);
                            const QPointF dot = dotFor(point, view.map(point), grid.origin, grid.zoom, grid.spacing);
                            if (qAbs(dot.x()) > 0.01 || qAbs(dot.y()) > 0.01)
                                QFAIL(qPrintable(QString("%1, the dot for board %2,%3 is drawn %4,%5 pixels from it")
                                                     .arg(where).arg(x).arg(y).arg(dot.x()).arg(dot.y())));
                        }
                }
    }

    void connectorsOfPastedTextLeaveItWhereItsTypeEnds() {
        // The prose joined to a note below it, copied, and pasted far off.
        // What is copied says where text starts and where it wraps, and
        // nothing of how tall that makes it: the copy is put down at a guess
        // of its size, a line to each paragraph, and the copy of the
        // connector leaves it from where that ends. The connector is in the
        // window the copies are pasted in, and the text is not.
        QString prose;
        for (int word = 0; word < 1200; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        Desk desk("joined", [&prose](const QString &home) {
            QDir(home).mkpath("boards/joined");
            const QJsonArray items{QJsonObject{{"id", 1}, {"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 480},
                                               {"text", prose.trimmed()}},
                                   QJsonObject{{"id", 2}, {"type", "note"}, {"x", 2400}, {"y", 6000}},
                                   QJsonObject{{"id", 3}, {"type", "connector"}, {"from", 1}, {"to", 2}}};
            write(home + "/boards/joined/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.canvas->selectAll();
        desk.key(Qt::Key_C, Qt::ControlModifier);
        desk.canvas->setView(QPointF(20000, 0), 1);
        desk.hover(QPointF(20500, 350));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.board().count(), 6);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Once all is ready, the copy is measured, and its connector leaves it
        // from the edge of that...
        const QRectF page = desk.all(Item::Kind::Text).last()->rect;
        QCOMPARE(page.size(), desk.all(Item::Kind::Text).first()->rect.size());
        const Item connector = *desk.all(Item::Kind::Connector).last();
        QVERIFY(page.adjusted(-0.5, -0.5, 0.5, 0.5).contains(connector.p1) &&
                !page.adjusted(0.5, 0.5, -0.5, -0.5).contains(connector.p1));
        // ...which is where it is drawn from: a little way down the copy, well
        // above where the connector leaves it, only the text is drawn, and no
        // line from the box it was put down in.
        desk.canvas->setView(QPointF(page.left(), page.top() + 400), 1);
        QVERIFY(connector.p1.y() > desk.canvas->visibleArea().bottom() + 300);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void connectorsFollowANoteWrittenOnFromOutsideAsItGrows() {
        // A note joined to another well below it, both ready and drawn.
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 top = desk.add(note(100, 100, "Plans"));
        const quint32 line = desk.add(connector(top, desk.add(note(100, 4000))));
        QTRY_COMPARE(desk.backend.saveState(), QString("saved"));
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        // Something else writes a great deal more on the first, in the board's
        // file, and nothing of how big that makes it. It comes in at the size
        // it was, so the connector, on the same edge of it, is where it was.
        QString plans = "Plans";
        for (int step = 1; step <= 120; ++step)
            plans += QString("\nStep %1").arg(step);
        QJsonArray items = desk.onDisk().value("items").toArray();
        for (qsizetype i = 0; i < items.size(); ++i)
            if (items[i].toObject().value("id").toInteger() == top) {
                QJsonObject written = items[i].toObject();
                written["text"] = plans;
                items[i] = written;
            }
        write(desk.library.boardFile("desk"), QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        QTRY_COMPARE(desk.board().item(top)->text, plans);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        // Measured, the note is as tall as all of that, and the connector
        // leaves it from its new bottom edge...
        const QRectF grown = desk.board().item(top)->rect;
        QVERIFY2(grown.height() > 1500, qPrintable(QString::number(grown.height())));
        const Item connector = *desk.board().item(line);
        QCOMPARE(connector.p1.y(), grown.bottom());
        // ...which is where it is drawn from: over the note, well above that,
        // only the note is drawn, and no line from where its edge was.
        desk.canvas->setView(QPointF(grown.left() - 200, grown.top() + 400), 1);
        QVERIFY(connector.p1.y() > desk.canvas->visibleArea().bottom() + 300);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().items, 1);
        QCOMPARE(desk.warnings, QStringList());
    }

    void pastedTextIsDrawnWhereItReachesAsSoonAsItIsDown() {
        // A crowd of notes with plenty written on each, and beside it prose in
        // large type that starts well above them and runs on down past them,
        // all of it copied and pasted far off. The copies are seen in a
        // window with all of the crowd in it: more than one frame has time to
        // make ready. What is copied says where text starts and where it
        // wraps, and nothing of how tall that makes it, so the copy of the
        // prose is put down at a guess of its size, a line to each paragraph,
        // well above that window, and could reach down through it.
        QString prose;
        for (int word = 0; word < 300; ++word)
            prose += QString("word%1").arg(word) + QChar(word % 100 == 99 ? '\n' : ' ');
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        Desk desk("pasted", [&](const QString &home) {
            QDir(home).mkpath("boards/pasted");
            QJsonArray items{QJsonObject{{"type", "text"}, {"x", 0}, {"y", -4000}, {"size", 48}, {"wrap", 480},
                                         {"text", prose.trimmed()}}};
            for (int i = 0; i < 1000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 600 + (i % 34) * 90.0}, {"y", (i / 34) * 90.0}, {"text", words}});
            write(home + "/boards/pasted/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF page = desk.all(Item::Kind::Text).first()->rect;
        desk.canvas->selectAll();
        desk.canvas->copy();
        desk.canvas->setView(QPointF(40000, 0), 0.25);
        desk.hover(QPointF(42000, 1400));
        // The window is put on the copies before a frame is drawn.
        QVERIFY(desk.canvas->paste());
        const QRectF guess = desk.all(Item::Kind::Text).last()->rect;
        const QPointF offset = guess.topLeft() - page.topLeft();
        desk.canvas->setView(QPointF(-100, -100) + offset, 0.25);
        const QRectF window = desk.canvas->visibleArea();
        QVERIFY(guess.bottom() < window.top() - window.height() / 10);
        QVERIFY(page.translated(offset).intersects(window));
        // The very next frame has it measured and drawn, every letter in the
        // window, as it is once all of the copies are ready.
        desk.window->grabWindow();
        QCOMPARE(desk.all(Item::Kind::Text).last()->rect, page.translated(offset));
        const SceneLoad first = desk.canvas->load();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad settled = desk.canvas->load();
        QCOMPARE(first.items, settled.items);
        QVERIFY(!first.reduced && first.triangles == settled.triangles);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aNoteWrittenOnFromOutsideIsDrawnAsFarAsItGrowsAtOnce() {
        // The crowd of notes all in view, and above the window a note with a
        // word on it.
        const QString words = QString("lorem ipsum dolor sit amet ").repeated(24);
        Desk desk("written", [&](const QString &home) {
            QDir(home).mkpath("boards/written");
            QJsonArray items{QJsonObject{{"id", 1}, {"type", "note"}, {"x", 0}, {"y", -1200}, {"text", "Plans"}}};
            for (int i = 0; i < 1000; ++i)
                items.append(QJsonObject{
                    {"type", "note"}, {"x", 600 + (i % 34) * 90.0}, {"y", (i / 34) * 90.0}, {"text", words}});
            write(home + "/boards/written/board.json",
                  QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        desk.canvas->setView(QPointF(-100, -100), 0.25);
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        const QRectF window = desk.canvas->visibleArea();
        QVERIFY(desk.board().item(1)->rect.bottom() < window.top() - window.height() / 10);
        // All of it changes at once, as a change from outside comes in: every
        // other note's color, and a great deal more written on the first,
        // with nothing said of how big that makes it.
        QString plans = "Plans";
        for (int step = 1; step <= 200; ++step)
            plans += QString("\nStep %1").arg(step);
        desk.board().begin();
        for (quint32 id : desk.board().order()) {
            Item item = *desk.board().item(id);
            if (id == 1)
                item.text = plans;
            else
                item.color = "blue";
            desk.board().set(item);
        }
        desk.board().commit();
        // Made big enough for that, it runs on down through the window. The
        // very next frame has it measured and drawn there, as it is once all
        // of the board is ready again.
        Item fitted = *desk.board().item(1);
        TextMetrics::fit(fitted);
        QVERIFY(fitted.rect.intersects(window));
        desk.window->grabWindow();
        QCOMPARE(desk.board().item(1)->rect, fitted.rect);
        const SceneLoad first = desk.canvas->load();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.window->grabWindow();
        const SceneLoad settled = desk.canvas->load();
        QCOMPARE(first.items, settled.items);
        QVERIFY(!first.reduced && first.triangles == settled.triangles);
        QCOMPARE(desk.warnings, QStringList());
    }

    // --- Text --------------------------------------------------------------------

    void thingsAreAsBigAsWhatIsWrittenOnThem() {
        Item small = note(0, 0, "short");
        QVERIFY(!TextMetrics::fit(small));
        Item full = note(0, 0, QString("line\n").repeated(20));
        QVERIFY(TextMetrics::fit(full));
        QVERIFY(full.rect.height() > 152);
        QCOMPARE(full.rect.width(), 152.0);
        QCOMPARE(full.rect.topLeft(), QPointF(0, 0));

        // A shape grows about its middle, so its connectors stay level.
        Item box = shape(Item::Shape::Rect, 0, 100, 100, 20);
        box.text = "one\ntwo\nthree\nfour";
        QVERIFY(TextMetrics::fit(box));
        QVERIFY(qAbs(box.rect.center().y() - 110) <= 1);

        // Where a shape has room for writing in only part of it, it is that
        // part that grows to hold what is written.
        for (Item::Shape outline : {Item::Shape::Triangle, Item::Shape::Arrow, Item::Shape::Bubble,
                                    Item::Shape::Cylinder, Item::Shape::Star}) {
            Item figure = shape(outline, 0, 0, 120, 60);
            figure.text = "one\ntwo\nthree\nfour\nfive";
            QVERIFY(TextMetrics::fit(figure));
            const TextStyle style = TextMetrics::styleFor(figure, Palette::fallback());
            QVERIFY(style.box.height() + 0.5 >= TextBlock(figure.text, style).size().height() + 12);
            QVERIFY(qAbs(figure.rect.center().y() - 30) <= 1);
            // And once it fits, it is left alone.
            QVERIFY(!TextMetrics::fit(figure));
        }

        Item text;
        text.kind = Item::Kind::Text;
        text.text = "a line of text that is fairly long";
        QVERIFY(TextMetrics::fit(text));
        const QSizeF unwrapped = text.rect.size();
        QVERIFY(unwrapped.width() > 100);
        // Wrapped, it is as wide as it was told and as tall as that takes.
        text.wrap = 80;
        QVERIFY(TextMetrics::fit(text));
        QCOMPARE(text.rect.width(), 80.0);
        QVERIFY(text.rect.height() > unwrapped.height() * 2);
        // Bigger type is bigger.
        text.wrap = 0;
        text.fontSize = 28;
        QVERIFY(TextMetrics::fit(text));
        QVERIFY(text.rect.height() > unwrapped.height() * 1.8);
    }

    // --- Exporter ----------------------------------------------------------------

    void exportsCoverTheBoardAndNothingElse() {
        QTemporaryDir dir;
        Board board;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        QVERIFY(Exporter::write(dir.filePath("empty.png"), board, {}, palette, {}, options).contains("nothing"));
        QVERIFY(!QFile::exists(dir.filePath("empty.png")));

        const quint32 first = board.add(note(10, 20, "Title\nbody"));
        const quint32 second = board.add(shape(Item::Shape::Diamond, 400, 300, 120, 80));
        board.add(connector(first, second));
        board.add(stroke({{0, 0}, {30, 10}, {60, 0}}, 100, 400));
        QCOMPARE(Exporter::area(board, {first}, 32), QRectF(-22, -12, 216, 216));

        QCOMPARE(Exporter::write(dir.filePath("board.png"), board, {}, palette, {}, options), QString());
        const QImage whole(dir.filePath("board.png"));
        const QRectF area = Exporter::area(board, {}, options.margin);
        QCOMPARE(whole.size(), (area.size() * 2).toSize());
        QCOMPARE(whole.pixel(1, 1), palette.background.rgb());
        // The note is where it is on the board, in its paper.
        const QPointF inNote = (QPointF(10 + 140, 20 + 140) - area.topLeft()) * 2;
        QCOMPARE(whole.pixel(inNote.toPoint()), palette.note("yellow").rgb());

        // Without the canvas behind it, and at another scale.
        options.background = false;
        options.scale = 1;
        QCOMPARE(Exporter::write(dir.filePath("clear.png"), board, {first}, palette, {}, options), QString());
        const QImage clear(dir.filePath("clear.png"));
        QCOMPARE(clear.size(), QSize(216, 216));
        QCOMPARE(clear.pixelColor(1, 1).alpha(), 0);
        QCOMPARE(clear.pixel(32 + 140, 32 + 140), palette.note("yellow").rgb());

        // However big the board, no picture is too big to open.
        board.add(note(60000, 0));
        options.scale = 2;
        QCOMPARE(Exporter::write(dir.filePath("wide.png"), board, {}, palette, {}, options), QString());
        QVERIFY(QImage(dir.filePath("wide.png")).width() <= 16384);

        options.format = "svg";
        QCOMPARE(Exporter::write(dir.filePath("board.svg"), board, {first}, palette, {}, options), QString());
        QVERIFY(read(dir.filePath("board.svg")).contains("<svg"));
        options.format = "pdf";
        QCOMPARE(Exporter::write(dir.filePath("board.pdf"), board, {first}, palette, {}, options), QString());
        QVERIFY(read(dir.filePath("board.pdf")).startsWith("%PDF"));
        options.format = "gif";
        QVERIFY(Exporter::write(dir.filePath("board.gif"), board, {}, palette, {}, options).contains("png, svg, pdf"));
        // Somewhere that cannot be written is said, not ignored.
        options.format = "png";
        QVERIFY(!Exporter::write("/nonexistent-folder/board.png", board, {}, palette, {}, options).isEmpty());
    }

    // --- Exports, names and starting up ----------------------------------

    void exportsHoldEveryLabelWhole() {
        QTemporaryDir dir;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        options.background = false;
        options.scale = 1;
        // A line from one place to another, its ends attached to nothing.
        auto line = [](QPointF from, QPointF to, const QString &text) {
            Item item;
            item.kind = Item::Kind::Connector;
            item.p1 = from;
            item.p2 = to;
            item.width = Defaults::connectorWidth;
            item.text = text;
            return item;
        };
        Item frame;
        frame.kind = Item::Kind::Frame;
        frame.rect = QRectF(0, 0, 120, 80);
        frame.text = "Everything to pack for the trip";
        // Upright, with its label to its right; slanting, with its label up
        // and to the right; level, with a label of four lines above it; and
        // a frame with a title longer than its top. Each label reaches past
        // its line or its frame.
        const QString label = "when the form has been filled in";
        const QVector<Item> items{line({0, 0}, {0, 200}, label), line({0, 0}, {200, 150}, label),
                                  line({0, 0}, {300, 0}, "from here\nto there\nand back\nonce more"), frame};
        // What is wrong with a picture of an export, if anything: there are
        // letters in `letters`, and around everything drawn there is the
        // margin, give or take the room a line's ends are given. Nothing is
        // cut off, and the picture is no bigger than it needs to be.
        auto wrong = [](const QImage &picture, const QRectF &letters) {
            const QRect part = letters.toAlignedRect();
            int found = 0;
            QRect drawn;
            for (int y = 0; y < picture.height(); ++y)
                for (int x = 0; x < picture.width(); ++x)
                    if (qAlpha(picture.pixel(x, y)) > 0) {
                        drawn |= QRect(x, y, 1, 1);
                        found += part.contains(x, y);
                    }
            if (found == 0)
                return QString("the letters are not there");
            for (const int blank : {drawn.left(), drawn.top(), picture.width() - 1 - drawn.right(),
                                    picture.height() - 1 - drawn.bottom()})
                if (blank < 16 || blank > 48)
                    return QString("%1 units blank at an edge").arg(blank);
            return QString();
        };
        for (const Item &item : items) {
            Board board;
            const quint32 id = board.add(item);
            // And something far off, so that the item on its own, as what is
            // selected is exported, is not the whole board.
            board.add(note(2000, 2000, "elsewhere"));
            const Item &added = *board.item(id);
            // The text as the window lays it out, where the window puts it.
            const TextBlock text(added.text, TextMetrics::styleFor(added, palette));
            const QRectF box(added.kind == Item::Kind::Connector ? TextMetrics::connectorLabelCorner(added, text.size())
                                                                 : added.rect.topLeft() + text.origin(),
                             text.size());
            QVERIFY2(Exporter::area(board, {}, options.margin).contains(box), qPrintable(added.text));
            const QRectF area = Exporter::area(board, {id}, options.margin);
            QVERIFY2(area.marginsRemoved(QMarginsF(1, 1, 1, 1) * options.margin).contains(box), qPrintable(added.text));
            const QRectF inPicture = box.translated(-area.topLeft());

            // As a picture, a pixel to the unit, with the label's last letters
            // in it.
            options.format = "png";
            QCOMPARE(Exporter::write(dir.filePath("label.png"), board, {id}, palette, {}, options), QString());
            const QImage png(dir.filePath("label.png"));
            QCOMPARE(png.size(), area.size().toSize());
            const QRectF end(inPicture.right() - 12, inPicture.top(), 12, inPicture.height());
            QCOMPARE(wrong(png, end), QString());
            // As a drawing, a CSS pixel to the unit, looked at that size. What
            // shows a drawing sets its writing in type of its own, a little
            // narrower or wider, so the label is only somewhere in its box.
            options.format = "svg";
            QCOMPARE(Exporter::write(dir.filePath("label.svg"), board, {id}, palette, {}, options), QString());
            QSvgRenderer svg(dir.filePath("label.svg"));
            QCOMPARE(svg.viewBoxF(), QRectF(QPointF(0, 0), area.size()));
            QImage drawing(area.size().toSize(), QImage::Format_ARGB32_Premultiplied);
            drawing.fill(Qt::transparent);
            QPainter painter(&drawing);
            svg.render(&painter);
            painter.end();
            QCOMPARE(wrong(drawing, inPicture), QString());
            // As a document, its page the area at three quarters of a point
            // to the unit.
            options.format = "pdf";
            QCOMPARE(Exporter::write(dir.filePath("label.pdf"), board, {id}, palette, {}, options), QString());
            const QString page = QString("/MediaBox [0 0 %1 %2").arg(area.width() * 0.75).arg(area.height() * 0.75);
            QVERIFY2(read(dir.filePath("label.pdf")).contains(page.toLatin1()), qPrintable(page));
        }
    }

    void theWindowAndTheCommandBothExportAWholeLabel() {
        // Two shapes one above the other and a line between them, its label
        // to its right and reaching further than they do.
        const QString label = "when the form has been filled in";
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 top = desk.add(shape(Item::Shape::Rect, 0, 0, 152, 56));
        const quint32 bottom = desk.add(shape(Item::Shape::Rect, 0, 200, 152, 56));
        Item line = connector(top, bottom);
        line.text = label;
        const Item &added = *desk.board().item(desk.add(line));
        const TextBlock text(added.text, TextMetrics::styleFor(added, desk.theme.palette()));
        const QRectF box(TextMetrics::connectorLabelCorner(added, text.size()), text.size());
        QVERIFY(box.right() > 152);
        // A picture of `area` of the board, `scale` pixels to the unit, with
        // the label's last letters in it and nothing near its edges. What
        // differs from its corner is something drawn.
        auto whole = [&box](const QImage &picture, const QRectF &area, qreal scale) {
            if (picture.size() != (area.size() * scale).toSize())
                return false;
            const QRgb blank = picture.pixel(0, 0);
            auto ink = [&picture, blank](const QRectF &part) {
                int found = 0;
                const QRect inside = part.toAlignedRect() & picture.rect();
                for (int y = inside.top(); y <= inside.bottom(); ++y)
                    for (int x = inside.left(); x <= inside.right(); ++x)
                        found += picture.pixel(x, y) != blank;
                return found;
            };
            const QRectF drawn((box.topLeft() - area.topLeft()) * scale, box.size() * scale);
            const QRectF clear = QRectF(picture.rect()).marginsRemoved(QMarginsF(16, 16, 16, 16) * scale);
            return ink(QRectF(drawn.right() - 12 * scale, drawn.top(), 12 * scale, drawn.height())) > 0 &&
                   ink(clear) == ink(picture.rect());
        };

        // The window's, two pixels to the unit on the canvas color: of the
        // whole board, and of the line alone, as what is selected is.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("flow.png"));
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.backend.problem(), QString());
        QVERIFY(whole(QImage(desk.home.filePath("flow.png")), Exporter::area(desk.board(), {}, 32), 2));
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("line.png"));
        desk.backend.exportBoard("png", {QVariant(added.id)});
        QCOMPARE(desk.backend.problem(), QString());
        QVERIFY(whole(QImage(desk.home.filePath("line.png")), Exporter::area(desk.board(), {added.id}, 32), 2));

        // `sprawl export` of the same board, a pixel to the unit with nothing
        // behind it.
        QTemporaryDir dir;
        qputenv("SPRAWL_HOME", dir.path().toUtf8());
        // Unset again however the test ends, as main() leaves it, so that no
        // test after this one is pointed at a folder that has gone.
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        auto run = [](QStringList arguments) {
            arguments.prepend("sprawl");
            return runCli(arguments);
        };
        QCOMPARE(run({"new", "flow"}), 0);
        QCOMPARE(run({"add", "flow", "shape", "--x", "0", "--y", "0", "--width", "152", "--height", "56"}), 0);
        QCOMPARE(run({"add", "flow", "shape", "--x", "0", "--y", "200", "--width", "152", "--height", "56"}), 0);
        QCOMPARE(run({"add", "flow", "connector", "--from", "1", "--to", "2", "--text", label}), 0);
        QCOMPARE(run({"export", "flow", "-o", dir.filePath("flow.png"), "--transparent", "--scale", "1"}), 0);
        QVERIFY(whole(QImage(dir.filePath("flow.png")), Exporter::area(desk.board(), {}, 32), 1));
        QCOMPARE(desk.warnings, QStringList());
    }

    void aConnectorsBoundsHoldItsLabelWhicheverWayItRuns() {
        const Palette palette = Palette::fallback();
        Board board;
        const quint32 from = board.add(shape(Item::Shape::Rect, 0, 0, 152, 56));
        // To a shape below it, below and a little to its right, as far below
        // as to its right, to its right, and to its right but nearer than its
        // label is long; each label reaches past the line's own box. In Latin
        // letters, on four lines, and in Japanese and Arabic, whose type is
        // wider or taller. And three long lines beside a short slanting line:
        // a label smaller than the most its text could make sits nearer the
        // line, so this one reaches further back past the line's end than
        // the largest would.
        const QString label = "when the form has been filled in", japanese = "東京から大阪まで",
                      arabic = "مرحبا بالعالم",
                      steps = "once the form has been filled in and sent\nand someone has read it and said it is "
                              "fine\nthen and only then may the next step go on";
        const QVector<QPair<QPointF, QString>> lines{
            {{0, 300}, label},    {{100, 400}, label},    {{300, 300}, label},    {{170, 0}, label},
            {{0, 300}, japanese}, {{100, 400}, japanese}, {{300, 300}, japanese}, {{170, 0}, japanese},
            {{100, 400}, arabic}, {{600, 0}, "from here\nto there\nand back\nonce more"}, {{120, 120}, steps}};
        for (const auto &[to, text] : lines) {
            Item line = connector(from, board.add(shape(Item::Shape::Rect, to.x(), to.y(), 152, 56)));
            line.text = text;
            const quint32 id = board.add(line);
            const Item &added = *board.item(id);
            const TextBlock block(added.text, TextMetrics::styleFor(added, palette));
            const QRectF box(TextMetrics::connectorLabelCorner(added, block.size()), block.size());
            // The window goes by these for what it draws and for what fits in
            // view. The board lays out no type, so it cannot say how big the
            // label is, only how big it might be; but whatever the type, the
            // label is inside.
            QVERIFY2(board.bounds(id).contains(box), qPrintable(added.text));
            QVERIFY(board.contentBounds().contains(box));
        }
    }

    void svgsAreAPixelToTheUnit() {
        QTemporaryDir dir;
        Board board;
        // With the margin, 217 units by 150: no whole number of points, nor
        // of millimeters.
        board.add(shape(Item::Shape::Rect, 0, 0, 153, 86));
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        QCOMPARE(Exporter::area(board, {}, options.margin).size(), QSizeF(217, 150));
        options.format = "svg";
        // The size and the view box of the drawing, as its root says.
        auto size = [](const QByteArray &svg) {
            QXmlStreamReader root(svg);
            if (!root.readNextStartElement() || root.name() != QLatin1String("svg"))
                return QStringList();
            return QStringList{root.attributes().value("width").toString(),
                               root.attributes().value("height").toString(),
                               root.attributes().value("viewBox").toString()};
        };
        QCOMPARE(Exporter::write(dir.filePath("board.svg"), board, {}, palette, {}, options), QString());
        // A size with no unit is in CSS pixels: as many as a PNG has at a
        // pixel to the unit.
        QCOMPARE(size(read(dir.filePath("board.svg"))), (QStringList{"217", "150", "0 0 217 150"}));
        options.format = "png";
        options.scale = 1;
        QCOMPARE(Exporter::write(dir.filePath("board.png"), board, {}, palette, {}, options), QString());
        QCOMPARE(QImage(dir.filePath("board.png")).size(), QSize(217, 150));

        // Whole units still, on a board more than a million of them across.
        board.add(shape(Item::Shape::Rect, 1234567, 0, 100, 60));
        QCOMPARE(Exporter::area(board, {}, options.margin).size(), QSizeF(1234731, 150));
        options.format = "svg";
        QCOMPARE(Exporter::write(dir.filePath("wide.svg"), board, {}, palette, {}, options), QString());
        QCOMPARE(size(read(dir.filePath("wide.svg"))), (QStringList{"1234731", "150", "0 0 1234731 150"}));
    }

    void pdfPagesAreThreeQuartersOfAPointToTheUnit() {
        QTemporaryDir dir;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        options.format = "pdf";
        // How big a PDF's page is, in points, as the file says in one of the
        // page's boxes: its media box, or the trim box Qt writes with it.
        auto page = [](const QByteArray &pdf, const QString &box) {
            const QRegularExpression size("/" + box + R"(\s*\[\s*0\s+0\s+([\d.]+)\s+([\d.]+)\s*\])");
            const QRegularExpressionMatch found = size.match(QString::fromLatin1(pdf));
            return QSizeF(found.captured(1).toDouble(), found.captured(2).toDouble());
        };
        // The boxes a PDF's page fills and their colors, read from its
        // drawing so that no reader is needed. Qt writes the drawing
        // compressed as zlib does; in it a box is "x y width height re",
        // filled by "f", or by "B" with an edge too ("f*" and "B*" for the
        // other rule of what is inside), in the color last set ("scn", or
        // "rg") and in points from the page's bottom left once every
        // transform in force ("cm"; "q" keeps them and "Q" goes back to what
        // was kept) is applied.
        auto fills = [](const QByteArray &pdf) {
            const QByteArray object = QRegularExpression(R"(/Contents (\d+) 0 R)")
                                          .match(QString::fromLatin1(pdf))
                                          .captured(1)
                                          .toLatin1();
            const qsizetype start = pdf.indexOf("stream\n", pdf.indexOf("\n" + object + " 0 obj")) + 7;
            // Qt takes zlib's data with a guess at how big it comes to first.
            const QByteArray drawing =
                qUncompress(QByteArray::fromHex("00000010") + pdf.mid(start, pdf.indexOf("endstream", start) - start));
            QVector<QPair<QRectF, QColor>> filled;
            QVector<qreal> operands;
            QTransform transform;
            QVector<QTransform> kept;
            QColor color;
            QRectF box;
            for (const QByteArray &word : drawing.simplified().split(' ')) {
                bool number = false;
                const qreal value = word.toDouble(&number);
                if (number) {
                    operands.append(value);
                    continue;
                }
                if (word == "cm" && operands.size() == 6)
                    transform = QTransform(operands[0], operands[1], operands[2], operands[3], operands[4],
                                           operands[5]) * transform;
                else if (word == "q")
                    kept.append(transform);
                else if (word == "Q" && !kept.isEmpty())
                    transform = kept.takeLast();
                else if ((word == "scn" || word == "rg") && operands.size() == 3)
                    color = QColor::fromRgbF(operands[0], operands[1], operands[2]);
                else if (word == "re" && operands.size() == 4)
                    box = transform.mapRect(QRectF(operands[0], operands[1], operands[2], operands[3]));
                else if ((word == "f" || word == "f*" || word == "B" || word == "B*") && !box.isNull())
                    filled.append({std::exchange(box, QRectF()), color});
                operands.clear();
            }
            return filled;
        };
        // A shape, and the page it comes to with the margin: 217 by 150 units
        // is 162.75 by 112.5 points, and 219 by 151 is 164.25 by 113.25, which
        // no page of whole points is. And 792 by 1120 is 594 by 840, near
        // enough A4 to be taken for it, but not A4.
        const QVector<QPair<QSizeF, QSizeF>> boards{
            {{153, 86}, {162.75, 112.5}}, {{155, 87}, {164.25, 113.25}}, {{728, 1056}, {594, 840}}};
        for (const auto &[size, points] : boards) {
            Board board;
            board.add(shape(Item::Shape::Rect, 0, 0, size.width(), size.height()));
            const QString path = dir.filePath("board.pdf");
            for (const bool background : {true, false}) {
                options.background = background;
                QCOMPARE(Exporter::write(path, board, {}, palette, {}, options), QString());
                const QByteArray pdf = read(path);
                QCOMPARE(page(pdf, "MediaBox"), points);
                QCOMPARE(page(pdf, "TrimBox"), points);
                // The canvas color over all of the page, if it is asked for,
                // and then the shape.
                const auto filled = fills(pdf);
                QCOMPARE(filled.size(), background ? 2 : 1);
                if (background) {
                    QVERIFY(filled.first().first.contains(QRectF(QPointF(0, 0), points)));
                    QCOMPARE(filled.first().second.rgb(), palette.background.rgb());
                }
                // As far from every edge of the page as from the others: the
                // margin and half its border, 32.5 units.
                const QRectF box = filled.last().first;
                for (const qreal from : {box.left(), box.top(), points.width() - box.right(),
                                         points.height() - box.bottom()})
                    QCOMPARE(from, 32.5 * 0.75);
            }
        }
    }

    void pdfPagesAreTheBoardToTheirEdges() {
        // Looked at as a reader shows them, which takes a reader Qt has:
        // QtPdf's, which comes with qt6-webengine. Where there is none, as in
        // CI, this is skipped, and the test above reads the same from the
        // file itself.
        if (!QImageReader::supportedImageFormats().contains("pdf"))
            QSKIP("Qt has no PDF reader here to look at the pages with.");
        QTemporaryDir dir;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        options.format = "pdf";
        const QColor canvas = palette.background;
        auto isCanvas = [&canvas](QRgb pixel) {
            return qAlpha(pixel) == 255 && qAbs(qRed(pixel) - canvas.red()) <= 2 &&
                   qAbs(qGreen(pixel) - canvas.green()) <= 2 && qAbs(qBlue(pixel) - canvas.blue()) <= 2;
        };
        // 217 by 150 units, 162.75 by 112.5 points, which no page of whole
        // points is; and 792 by 1120, near enough A4 to be taken for it.
        const QVector<QPair<QSizeF, QSizeF>> boards{{{153, 86}, {162.75, 112.5}}, {{728, 1056}, {594, 840}}};
        for (const auto &[size, points] : boards) {
            Board board;
            board.add(shape(Item::Shape::Rect, 0, 0, size.width(), size.height()));
            const QString path = dir.filePath("board.pdf");
            QCOMPARE(Exporter::write(path, board, {}, palette, {}, options), QString());
            // Four pixels to the point, so that a strip a quarter of a point
            // wide is a whole pixel.
            QImageReader reader(path, "pdf");
            reader.setScaledSize((points * 4).toSize());
            const QImage page = reader.read();
            QCOMPARE(page.size(), (points * 4).toSize());
            // Every pixel along its edges is the canvas color, and the shape
            // is as far from each edge as from the others.
            int other = 0;
            QRect drawn;
            for (int y = 0; y < page.height(); ++y)
                for (int x = 0; x < page.width(); ++x)
                    if (!isCanvas(page.pixel(x, y))) {
                        drawn |= QRect(x, y, 1, 1);
                        other += x == 0 || y == 0 || x == page.width() - 1 || y == page.height() - 1;
                    }
            QCOMPARE(other, 0);
            for (const int from : {drawn.top(), page.width() - 1 - drawn.right(), page.height() - 1 - drawn.bottom()})
                QCOMPARE(from, drawn.left());
        }
    }

    void exportsHoldWritingThatRunsOutOfItsItem() {
        QTemporaryDir dir;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        options.background = false;
        options.scale = 1;
        const QString words = "Pack the tent, the stove and the water filter. Check the weather the night "
                              "before, and leave the route with someone who is staying behind.";
        // As a file has them: a text item that wraps, as big as the board
        // guesses until the window measures it, which is a line where its
        // words take several; and a note too short for what is written in
        // it, as one written in smaller type than the desktop's is now.
        const QVector<QJsonObject> items{
            {{"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 160}, {"text", words}},
            {{"type", "note"}, {"x", 0}, {"y", 0}, {"w", 152}, {"h", 60}, {"text", words}}};
        for (const QJsonObject &item : items) {
            Board board;
            QCOMPARE(board.fromJson({{"sprawl", 1}, {"items", QJsonArray{item}}}), QStringList());
            const Item &loaded = *board.item(board.order().first());
            const TextBlock text(loaded.text, TextMetrics::styleFor(loaded, palette));
            const QRectF box(loaded.rect.topLeft() + text.origin(), text.size());
            QVERIFY(!loaded.rect.contains(box));
            const QRectF area = Exporter::area(board, {}, options.margin);
            QVERIFY2(area.marginsRemoved(QMarginsF(1, 1, 1, 1) * options.margin).contains(box),
                     qPrintable(item.value("type").toString()));
            // Its last line is in the picture, the margin below it.
            QCOMPARE(Exporter::write(dir.filePath("words.png"), board, {}, palette, {}, options), QString());
            const QImage image(dir.filePath("words.png"));
            QRect drawn;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 0)
                        drawn |= QRect(x, y, 1, 1);
            QVERIFY(drawn.bottom() > box.bottom() - area.top() - 16);
            for (const int blank : {drawn.left(), drawn.top(), image.width() - 1 - drawn.right(),
                                    image.height() - 1 - drawn.bottom()})
                QVERIFY2(blank >= 16, qPrintable(QString("%1: %2").arg(item.value("type").toString()).arg(blank)));
        }
    }

    void theWindowExportsTextItHasNotMeasuredYet() {
        Desk desk;
        QVERIFY(desk.ready());
        // A board opened and exported before a frame has measured its text
        // items, which until then are as big as the board guesses: one that
        // wraps, which the guess has as a line where its words take several;
        // and one with its accents as characters of their own, as some
        // systems write them, which the guess has wider than it is.
        const QString words = "Pack the tent, the stove and the water filter. Check the weather the night before.",
                      accents = QString::fromUtf8("Cafe\xcc\x81 cre\xcc\x80me and re\xcc\x81sume\xcc\x81s");
        const QJsonObject file{
            {"sprawl", 1},
            {"items", QJsonArray{QJsonObject{{"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 160}, {"text", words}},
                                 QJsonObject{{"type", "text"}, {"x", 0}, {"y", 200}, {"text", accents}}}}};
        QCOMPARE(desk.board().fromJson(file), QStringList());
        const quint32 wrapped = desk.board().order().first(), accented = desk.board().order().last();
        const QRectF guessedWrapped = desk.board().item(wrapped)->rect,
                     guessedAccented = desk.board().item(accented)->rect;
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("before.png"));
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.backend.problem(), QString());
        // Once a frame has measured them the board knows how big they are,
        // and the export is the same.
        desk.window->grabWindow();
        QVERIFY(desk.board().item(wrapped)->rect.height() > guessedWrapped.height() * 2);
        QVERIFY(desk.board().item(accented)->rect.width() < guessedAccented.width() - 10);
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("after.png"));
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.backend.problem(), QString());
        const QImage before(desk.home.filePath("before.png")), after(desk.home.filePath("after.png"));
        QVERIFY(!after.isNull());
        QCOMPARE(before.size(), after.size());
        QVERIFY(before == after);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardsNameMeansThatBoardWhateverIsHere() {
        QTemporaryDir home, here;
        Library library(home.path());
        QCOMPARE(library.create("plans"), QString());
        QCOMPARE(library.create("trip"), QString());
        // A board's folder made by hand, with a name no command gives one.
        const QByteArray empty = R"({"sprawl": 1, "items": []})";
        QVERIFY(QDir(library.boardsFolder()).mkpath("old.json"));
        write(library.boardFile("old.json"), empty);
        QVERIFY(library.exists("old.json"));
        // The folder a command is run from has a file called plans that is
        // no board, a board's folder called trip, another called what no
        // board in the library is, and a .json file called what the board
        // above is.
        const QString cwd = QFileInfo(here.path()).canonicalFilePath();
        write(cwd + "/plans", "buy milk\n");
        QVERIFY(QDir(cwd).mkpath("trip") && QDir(cwd).mkpath("ideas"));
        write(cwd + "/trip/board.json", empty);
        write(cwd + "/ideas/board.json", empty);
        write(cwd + "/old.json", empty);
        const QString was = QDir::currentPath();
        QVERIFY(QDir::setCurrent(cwd));
        const auto back = qScopeGuard([was] { QDir::setCurrent(was); });

        // A board's name is that board.
        for (const QString &name : QStringList{"plans", "trip"}) {
            const Library::Location found = library.locate(name);
            QCOMPARE(found.name, name);
            QCOMPARE(found.file, library.boardFile(name));
            QCOMPARE(found.assets, library.assetsFolder(name));
        }
        // A path is a path, however it is written, and the board is called
        // by its folder's name.
        const QStringList paths{"./trip", "trip/", cwd + "/trip", "./trip/../trip/", "trip/board.json"};
        for (const QString &path : paths) {
            const Library::Location found = library.locate(path);
            QCOMPARE(found.name, QString("trip"));
            QCOMPARE(found.file, cwd + "/trip/board.json");
            QCOMPARE(found.assets, cwd + "/trip/assets");
        }
        // So is a name with .json at its end, and a name no board has.
        QCOMPARE(library.locate("old.json").file, cwd + "/old.json");
        QCOMPARE(library.locate("old.json").name, QString("old"));
        QCOMPARE(library.locate("ideas").file, cwd + "/ideas/board.json");
        QVERIFY(!library.locate("nowhere").isValid());
        // From inside a board's folder it is ".", called by its name, and
        // nothing named is still no board.
        QVERIFY(QDir::setCurrent(cwd + "/trip"));
        QCOMPARE(library.locate(".").name, QString("trip"));
        QCOMPARE(library.locate(".").file, cwd + "/trip/board.json");
        QVERIFY(!library.locate("").isValid());
        // Where the path leads to no board, even beside a folder of that name
        // with no board in it, the library's old.json is had by its name.
        QCOMPARE(library.locate("old.json").file, library.boardFile("old.json"));
        QVERIFY(QDir(cwd + "/trip").mkpath("old.json"));
        QCOMPARE(library.locate("old.json").name, QString("old.json"));
        QCOMPARE(library.locate("old.json").file, library.boardFile("old.json"));
        QCOMPARE(library.locate("old.json").assets, library.assetsFolder("old.json"));
    }

    void commandsAndTheWindowTakeANameAlike() {
        // Run from a folder with a file called plans that is no board, and a
        // folder called trip with a board of its own.
        QTemporaryDir here;
        const QString cwd = QFileInfo(here.path()).canonicalFilePath();
        write(cwd + "/plans", "buy milk\n");
        Board local;
        local.add(note(0, 0, "trip, here"));
        QCOMPARE(Library::write(cwd + "/trip/board.json", local.toJson()), QString());
        const QString was = QDir::currentPath();
        QVERIFY(QDir::setCurrent(cwd));
        const auto back = qScopeGuard([was] { QDir::setCurrent(was); });
        // A library with boards of the same names, each saying which it is.
        const auto boards = [](const QString &home) {
            for (const QString &name : QStringList{"plans", "trip"}) {
                Board board;
                board.add(note(0, 0, name + ", in the library"));
                Library::write(home + "/boards/" + name + "/board.json", board.toJson());
            }
        };

        // `sprawl open plans`, and `sprawl open trip`.
        for (const QString &name : QStringList{"plans", "trip"}) {
            Desk desk(name, boards);
            QVERIFY(desk.ready());
            QCOMPARE(desk.backend.problem(), QString());
            QCOMPARE(desk.backend.boardName(), name);
            QCOMPARE(desk.backend.assetFolder(), desk.library.assetsFolder(name));
            QCOMPARE(desk.board().count(), 1);
            QCOMPARE(desk.last()->text, name + ", in the library");
            QCOMPARE(desk.warnings, QStringList());
        }
        // And `sprawl open trip/`, the folder here, called by its name.
        {
            Desk desk("trip/", boards);
            QVERIFY(desk.ready());
            QCOMPARE(desk.backend.problem(), QString());
            QCOMPARE(desk.backend.boardName(), QString("trip"));
            QCOMPARE(desk.backend.assetFolder(), cwd + "/trip/assets");
            QCOMPARE(desk.board().count(), 1);
            QCOMPARE(desk.last()->text, QString("trip, here"));
            QCOMPARE(desk.warnings, QStringList());
        }

        // The commands, on the same boards.
        QTemporaryDir home;
        boards(home.path());
        qputenv("SPRAWL_HOME", home.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        auto run = [](QStringList arguments) {
            arguments.prepend("sprawl");
            return runCli(arguments);
        };
        auto count = [](const QString &file) {
            QJsonObject root;
            return Library::read(file, root).isEmpty() ? root.value("items").toArray().size() : -1;
        };
        const Library library;
        QCOMPARE(run({"check", "plans"}), 0);
        QCOMPARE(run({"add", "trip", "note", "--text", "Added"}), 0);
        QCOMPARE(count(library.boardFile("trip")), 2);
        QCOMPARE(count(cwd + "/trip/board.json"), 1);
        // The folder here is had by its path, as the shell completes it, and
        // is called by its name.
        QCOMPARE(run({"add", "trip/", "note", "--text", "Added here"}), 0);
        QCOMPARE(count(cwd + "/trip/board.json"), 2);
        QCOMPARE(count(library.boardFile("trip")), 2);
        QCOMPARE(run({"export", "trip/"}), 0);
        QVERIFY(QFile::exists(cwd + "/trip.png"));
        QVERIFY(!QFile::exists(cwd + "/.png"));
    }

    void anythingAskedOfTheWindowOpensOneOfItsOwn() {
        // What `sprawl open` can be asked: each of its options, with a value
        // for those that take one.
        const QList<QPair<QString, QString>> options{
            {"new-window", {}}, {"screenshot", "shot.png"}, {"size", "800x600"}, {"show", "keys"},
            {"tool", "pen"},    {"select", "1,2"},          {"zoom", "0.5"},     {"default-theme", {}},
            {"theme", "nord"},  {"bench", "100"},           {"probe", "8"},      {"probe-with", "notes"}};
        // Read by a parser that knows them, as main()'s does.
        auto ownWindow = [&options](const QStringList &arguments) {
            QCommandLineParser args;
            args.addPositionalArgument("open", "Open the window", "open");
            args.addPositionalArgument("board", "The board to open", "[board]");
            for (const auto &[name, value] : options)
                args.addOption(value.isEmpty() ? QCommandLineOption(name) : QCommandLineOption(name, name, "value"));
            return args.parse(QStringList{"sprawl", "open"} + arguments) && wantsOwnWindow(args);
        };

        // Launched again with nothing asked, Sprawl brings the window that is
        // open to the front.
        QVERIFY(!ownWindow({}));
        // Whatever is asked would be left undone there, so it is done in a
        // window of its own.
        QVERIFY(ownWindow({"plans"}));
        for (const auto &[name, value] : options) {
            QStringList arguments{"--" + name};
            if (!value.isEmpty())
                arguments << value;
            QVERIFY2(ownWindow(arguments), qPrintable(arguments.join(' ')));
        }
        QVERIFY(ownWindow({"--theme=nord"}));
        QVERIFY(ownWindow({"plans", "--zoom", "2"}));
    }

    void aThemesNameMeansThatThemeWhateverIsHere() {
        // Where themes are kept: the person's own, then those Omarchy comes
        // with. A theme is a folder with a colors.toml in it.
        QTemporaryDir kept, here;
        const QString own = kept.filePath("own"), shipped = kept.filePath("shipped");
        auto theme = [](const QString &folder) {
            QVERIFY(QDir().mkpath(folder));
            write(folder + "/colors.toml", "accent = \"#112233\"\n");
        };
        theme(own + "/nord");
        theme(shipped + "/nord");
        theme(shipped + "/gruvbox");
        // The folder Sprawl is started from has a theme's folder called what
        // one of those is, and another called what none is.
        const QString cwd = QFileInfo(here.path()).canonicalFilePath();
        theme(cwd + "/gruvbox");
        theme(cwd + "/mine");
        const QString was = QDir::currentPath();
        QVERIFY(QDir::setCurrent(cwd));
        const auto back = qScopeGuard([was] { QDir::setCurrent(was); });
        const QStringList folders{own, shipped};

        // A theme's name is that theme, the person's own before Omarchy's.
        QCOMPARE(findTheme("gruvbox", folders), shipped + "/gruvbox");
        QCOMPARE(findTheme("nord", folders), own + "/nord");
        // A path is a path, however it is written, and so is a name no theme
        // has.
        for (const QString &path : QStringList{"./gruvbox", "gruvbox/", cwd + "/gruvbox"})
            QCOMPARE(findTheme(path, folders), cwd + "/gruvbox");
        QCOMPARE(findTheme("mine", folders), cwd + "/mine");
        QCOMPARE(findTheme("nowhere", folders), QString());
        // Nothing named is no theme, and "." and ".." are the folders here:
        // none of them is a name, even with colors where themes are kept and
        // in the folder above.
        theme(own);
        theme(kept.path());
        QCOMPARE(findTheme("", folders), QString());
        QVERIFY(QDir::setCurrent(cwd + "/mine"));
        QCOMPARE(findTheme(".", folders), cwd + "/mine");
        QCOMPARE(findTheme("..", folders), QString());
    }

    // Each kind of picture the dialog names is one Sprawl takes and draws
    // with the packages it depends on, which CI installs from the recipe:
    // all but AVIF, which takes two that the package has as optional.
    // Nothing links to the plugin that reads WebP, so only a picture shows
    // it is there.
    void everyKindOfPictureTheDialogNamesIsAddedAndDrawn() {
        // Turned away, a WebP would say only that it is not a picture.
        QVERIFY2(QImageReader::supportedImageFormats().contains("webp"),
                 "Qt has no WebP reader here: install qt6-imageformats, as CONTRIBUTING.md says.");
        Desk desk;
        QVERIFY(desk.ready());
        const QColor blue("#7aa2f7");
        // As other programs write them: Qt writes no GIFs or SVGs, and a WebP
        // written here would take the very plugin this looks for. Qt reads no
        // WebP shorter than 40 bytes, so this one has a corner of another blue.
        const QHash<QString, QByteArray> written{
            {"gif", QByteArray::fromHex("47494638396128001e00f000007aa2f700000021f90400000000002c0000000028001e"
                                        "00000221848fa9cbed0fa39cb4da8bb3debcfb0f86e24896e689a6eacab6ee0bc7f24c"
                                        "7305003b")},
            {"webp", QByteArray::fromHex("524946462a000000574542505650384c1d0000002f274007000f7065eceb025fa07e"
                                         "fec30362c1c45f7a197447f43ff50400")},
            {"svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"30\">"
                    "<rect width=\"40\" height=\"30\" fill=\"#7aa2f7\"/></svg>"},
        };
        QList<QUrl> picked;
        for (const QString &suffix : FilePicker::pictureSuffixes()) {
            if (suffix == "avif")
                continue;
            const QString path = desk.home.filePath("blue." + suffix);
            if (written.contains(suffix))
                write(path, written.value(suffix));
            else
                picture(path, QSize(40, 30), blue);
            // A kind the dialog comes to name that Qt cannot write needs one
            // above.
            QVERIFY2(QFile::exists(path),
                     qPrintable(QString("The dialog names .%1 pictures, and there is none here to add.").arg(suffix)));
            picked << QUrl::fromLocalFile(path);
        }
        desk.picker.images = picked;
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_I);
        QCOMPARE(desk.backend.problem(), QString());
        QVector<quint32> added;
        for (const Item *image : desk.all(Item::Kind::Image))
            added << image->id;
        QCOMPARE(added.size(), picked.size());

        // Each at its own size, and exported as itself rather than as a
        // picture that is missing: two pixels to the unit, inside a margin of
        // 32.
        for (const quint32 id : std::as_const(added)) {
            const Item image = *desk.board().item(id);
            QCOMPARE(image.rect.size(), QSizeF(40, 30));
            const QString file = desk.home.filePath("export-" + image.asset + ".png");
            desk.picker.target = QUrl::fromLocalFile(file);
            desk.backend.exportBoard("png", {QVariant(id)});
            QCOMPARE(desk.backend.problem(), QString());
            const QColor drawn = QImage(file).pixelColor((32 + 20) * 2, (32 + 15) * 2);
            QVERIFY2(qAbs(drawn.red() - blue.red()) <= 6 && qAbs(drawn.green() - blue.green()) <= 6 &&
                         qAbs(drawn.blue() - blue.blue()) <= 6,
                     qPrintable(QString("%1 is drawn %2").arg(image.asset, drawn.name())));
        }
        QCOMPARE(desk.warnings, QStringList());
    }

    void exportsAndTheWindowHoldAllOfAStrokesCurve() {
        // Strokes as a file has them, or a board from before corners were
        // kept, each corner given once: an L with legs of a thousand, a box,
        // an L with legs of two hundred, and a ring through eight points,
        // none of them at its top, its foot or its sides. The curve through
        // each bends out past the box around its points: round the corners,
        // and between the points of the ring.
        QVector<QPointF> ring;
        for (int i = 0; i <= 8; ++i)
            ring.append(turnedBy(QPointF(100, 0), 22.5 + 45 * i));
        const QVector<QVector<QPointF>> lines{{{0, 0}, {1000, 0}, {1000, 1000}},
                                             {{0, 0}, {300, 0}, {300, 300}, {0, 300}, {0, 0}},
                                             {{0, 0}, {200, 0}, {200, 200}},
                                             ring};
        QTemporaryDir dir;
        const Palette palette = Palette::fallback();
        Exporter::Options options;
        options.background = false;
        options.scale = 1;
        options.margin = 0;
        for (const QVector<QPointF> &points : lines) {
            Board board;
            Item line = stroke(points, 40, 40);
            line.width = 4;
            const quint32 id = board.add(line);
            // What is drawn: the curve, and half the stroke's width round it.
            const Item added = *board.item(id);
            const QRectF drawn = Geo::boundsOf(Geo::smooth(added.points, Geo::strokeTolerance))
                                     .translated(added.rect.topLeft())
                                     .adjusted(-2, -2, 2, 2);
            QVERIFY(!added.rect.adjusted(-3, -3, 3, 3).contains(drawn));
            // The window goes by these for what is in view, what is near the
            // pointer, and what fits the board in it; moved, as a drag moves
            // it, the stroke takes them with it.
            QVERIFY(board.bounds(id).contains(drawn));
            QVERIFY(board.contentBounds().contains(drawn));
            Item moved = added;
            moved.rect.translate(37, -12);
            board.set(moved);
            QVERIFY(board.bounds(id).contains(drawn.translated(37, -12)));
            QCOMPARE(board.bounds(id), Board::boundsFor(moved));
            // Exported with no margin, a pixel to the unit: nothing drawn is
            // cut off at an edge, and nothing is further than a unit or two
            // from one.
            QCOMPARE(Exporter::write(dir.filePath("stroke.png"), board, {}, palette, {}, options), QString());
            const QImage png(dir.filePath("stroke.png"));
            QRect inked;
            for (int y = 0; y < png.height(); ++y)
                for (int x = 0; x < png.width(); ++x)
                    if (qAlpha(png.pixel(x, y)) > 0)
                        inked |= QRect(x, y, 1, 1);
            for (const int blank : {inked.left(), inked.top(), png.width() - 1 - inked.right(),
                                    png.height() - 1 - inked.bottom()})
                QVERIFY2(blank >= 1 && blank <= 2, qPrintable(QString("%1 pixels blank at an edge").arg(blank)));
        }
    }

    void exportsHoldALineWiderThanTheShapeItIsWrittenOn() {
        // A shape narrower than a letter of what is written on it, as a file
        // can have one. The line is set across the middle of the shape, and
        // reaches out past it on both sides.
        Board board;
        Item narrow = shape(Item::Shape::Triangle, 0, 0, 4, 56);
        narrow.text = "W";
        const quint32 id = board.add(narrow);
        const Item &added = *board.item(id);
        const Palette palette = Palette::fallback();
        const TextBlock text(added.text, TextMetrics::styleFor(added, palette));
        const QRectF line = text.paragraphs().front().layout->lineAt(0).naturalTextRect().translated(
            added.rect.topLeft() + text.origin());
        QVERIFY(line.left() < added.rect.left() && line.right() > added.rect.right());
        // The export is of the line and the shape, with the margin round
        // them: nothing cut off on one side, and nothing blank past the
        // margin on the other.
        Exporter::Options options;
        options.background = false;
        options.scale = 8;
        options.margin = 8;
        const QRectF area = Exporter::area(board, {}, options.margin);
        QCOMPARE(area.left(), std::floor(line.left() - options.margin));
        QCOMPARE(area.right(), std::ceil(line.right() + options.margin));
        // And the letter is drawn there, out past the shape on both sides.
        QTemporaryDir dir;
        QCOMPARE(Exporter::write(dir.filePath("narrow.png"), board, {}, palette, {}, options), QString());
        const QImage png(dir.filePath("narrow.png"));
        QCOMPARE(png.size(), (area.size() * options.scale).toSize());
        int left = png.width(), right = -1;
        for (int y = 0; y < png.height(); ++y)
            for (int x = 0; x < png.width(); ++x)
                if (qAlpha(png.pixel(x, y)) > 0) {
                    left = qMin(left, x);
                    right = qMax(right, x);
                }
        const qreal shapeLeft = (added.rect.left() - area.left()) * options.scale,
                    shapeRight = (added.rect.right() - area.left()) * options.scale;
        QVERIFY2(left < shapeLeft - 2 && right > shapeRight + 2, qPrintable(QString("%1 to %2").arg(left).arg(right)));
        QVERIFY(left >= (options.margin - 1) * options.scale);
        QVERIFY(png.width() - 1 - right >= (options.margin - 1) * options.scale);
    }

    void aLineToTextNotMeasuredYetIsExportedToWhereTheTextEnds() {
        Desk desk;
        QVERIFY(desk.ready());
        // A board read before a frame has measured its text: a text item that
        // wraps, which the board guesses is a line where its words take
        // several, and below it a shape with a line to the text, which ends
        // at the foot of the text as the board has it.
        const QString words = "Pack the tent, the stove and the water filter. Check the weather the night before.";
        const QJsonObject file{
            {"sprawl", 1},
            {"items",
             QJsonArray{QJsonObject{{"id", 1}, {"type", "text"}, {"x", 0}, {"y", 0}, {"wrap", 160}, {"text", words}},
                        QJsonObject{{"id", 2}, {"type", "shape"}, {"x", 0}, {"y", 300}, {"w", 160}, {"h", 56}},
                        QJsonObject{{"id", 3}, {"type", "connector"}, {"from", 2}, {"to", 1}, {"text", "then"}}}}};
        QCOMPARE(desk.board().fromJson(file), QStringList());
        const qreal guessed = desk.board().item(1)->rect.bottom();
        QCOMPARE(desk.board().item(3)->p2.y(), guessed);
        // Exported then, the text is as big as it is set and the line ends
        // at its foot, as once a frame has measured it.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("before.png"));
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.backend.problem(), QString());
        desk.window->grabWindow();
        const qreal foot = desk.board().item(1)->rect.bottom();
        QVERIFY(foot > guessed + 32);
        QCOMPARE(desk.board().item(3)->p2.y(), foot);
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("after.png"));
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.backend.problem(), QString());
        const QImage before(desk.home.filePath("before.png")), after(desk.home.filePath("after.png"));
        QVERIFY(!after.isNull());
        QVERIFY(before == after);
        QCOMPARE(desk.warnings, QStringList());
    }

    void qtsOwnOptionsAndBoardsKeptElsewhereOpenAWindowOfTheirOwn() {
        // `sprawl open` with nothing of Sprawl's own asked, read as main()
        // reads it, and the boards where they always are.
        QCommandLineParser args;
        args.addPositionalArgument("open", "Open the window", "open");
        args.addPositionalArgument("board", "The board to open", "[board]");
        args.addOption(QCommandLineOption("new-window"));
        QVERIFY(args.parse({"sprawl", "open"}));
        qunsetenv("SPRAWL_HOME");
        QVERIFY(!wantsOwnWindow(args));
        // Qt's own options, a window's size or title or right to left, which
        // Qt takes out of the arguments before the parser sees them.
        QVERIFY(wantsOwnWindow(args, true));
        // Boards kept somewhere else, which the window that is open does not
        // have: what is tried there is never done to the real ones.
        qputenv("SPRAWL_HOME", QDir::temp().filePath("elsewhere").toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        QVERIFY(wantsOwnWindow(args));
    }

    // --- Commands ------------------------------------------------------------------

    void commandsWorkWithNoWindow() {
        QTemporaryDir dir;
        qputenv("SPRAWL_HOME", dir.path().toUtf8());
        auto run = [](QStringList arguments) {
            arguments.prepend("sprawl");
            return runCli(arguments);
        };
        QCOMPARE(run({"new", "Weekend Trip", "--json"}), 0);
        QCOMPARE(run({"new", "Weekend Trip"}), 1);
        QCOMPARE(run({"add", "weekend-trip", "note", "--text", "Pack\\nboots, map", "--color", "green", "--json"}), 0);
        QCOMPARE(run({"add", "weekend-trip", "shape", "--shape", "diamond", "--text", "Go?", "--width", "200",
                      "--height", "90", "--json"}), 0);
        QCOMPARE(run({"add", "weekend-trip", "connector", "--from", "1", "--to", "2", "--route", "elbow", "--start",
                      "dot", "--end", "open", "--dash", "dashed", "--json"}), 0);
        QCOMPARE(run({"add", "weekend-trip", "connector", "--from", "1", "--to", "2", "--route", "loopy"}), 1);
        // What cannot be on a board is refused, and the board is left alone.
        QCOMPARE(run({"add", "weekend-trip", "note", "--color", "purple"}), 1);
        QCOMPARE(run({"add", "weekend-trip", "connector", "--from", "1", "--to", "99"}), 1);
        QCOMPARE(run({"add", "nowhere", "note"}), 1);
        QCOMPARE(run({"check", "weekend-trip", "--json"}), 0);
        QCOMPARE(run({"check", "nowhere", "--json"}), 1);

        const Library library;
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("weekend-trip"), root), QString());
        Board board;
        QCOMPARE(board.fromJson(root), QStringList());
        QCOMPARE(board.count(), 3);
        QCOMPARE(board.item(1)->text, QString("Pack\nboots, map"));
        QCOMPARE(board.item(1)->color, QString("green"));
        // With no place given, each thing goes below what is there.
        QVERIFY(board.item(2)->rect.top() >= board.item(1)->rect.bottom());
        QCOMPARE(board.item(2)->rect.size(), QSizeF(200, 90));
        QCOMPARE(board.item(3)->from, quint32(1));
        QCOMPARE(board.item(3)->route, Item::Route::Elbow);
        QCOMPARE(board.item(3)->start, Item::Head::Dot);
        QCOMPARE(board.item(3)->end, Item::Head::Open);
        QCOMPARE(board.item(3)->dash, Item::Dash::Dashed);

        QCOMPARE(run({"export", "weekend-trip", "-o", dir.filePath("trip.svg"), "--json"}), 0);
        QVERIFY(read(dir.filePath("trip.svg")).contains("<svg"));
        QCOMPARE(run({"export", "weekend-trip", "-o", dir.filePath("trip.gif")}), 0);
        QVERIFY(QImage(dir.filePath("trip.gif"), "png").width() > 0);
        QCOMPARE(run({"export", "weekend-trip", "-f", "gif"}), 1);
        // Removing a thing takes its connector too.
        QCOMPARE(run({"remove", "weekend-trip", "1", "--json"}), 0);
        QCOMPARE(Library::read(library.boardFile("weekend-trip"), root), QString());
        QCOMPARE(root.value("items").toArray().size(), 1);
        QCOMPARE(run({"remove", "weekend-trip", "42"}), 1);
        QCOMPARE(run({"list", "--json"}), 0);
        qunsetenv("SPRAWL_HOME");
    }

    // --- What comes from outside ---------------------------------------------

    void anItemHoldsOnlySoMuchText() {
        Board board;
        const QString pages(25'000, 'a');
        const quint32 id = board.add(note(0, 0, pages));
        QCOMPARE(board.item(id)->text.size(), Item::longestText);
        Item more = *board.item(id);
        more.text = pages + "b";
        board.set(more);
        QCOMPARE(board.item(id)->text.size(), Item::longestText);
        // Never half of a character that takes two.
        const QString face = QString(Item::longestText - 1, 'a') + QString::fromUtf8("\xF0\x9F\x99\x82") + "b";
        QCOMPARE(Item::fitted(face).size(), Item::longestText - 1);
        QCOMPARE(Item::fitted("short"), QString("short"));

        // A file with more in it is not a board, and says how much is too much.
        auto problems = [&board](const QString &text) {
            const QJsonObject item{{"type", "note"}, {"x", 0}, {"y", 0}, {"text", text}};
            return board.fromJson(QJsonObject{{"sprawl", 1}, {"items", QJsonArray{item}}}).join(" ");
        };
        QVERIFY(problems(QString(Item::longestText + 1, 'a')).contains("holds 10000"));
        QCOMPARE(board.item(id)->text.size(), Item::longestText);
        QCOMPARE(problems(QString(Item::longestText, 'a')), QString());
    }

    void whatAFileSaysIsQuotedBriefly() {
        Board board;
        auto problems = [&board](const QJsonObject &item) {
            return board.fromJson(QJsonObject{{"sprawl", 1}, {"items", QJsonArray{item}}}).join(" ");
        };
        // However much a file says where a word was wanted, a few words of
        // it are said back, and nothing in them that a terminal would obey.
        const QString wrong = problems({{"type", QString(5000, 'x')}, {"x", 0}, {"y", 0}});
        QVERIFY(wrong.contains("no item type"));
        QVERIFY2(wrong.size() < 120, qPrintable(QString::number(wrong.size())));
        const QString loud = problems({{"type", "note"}, {"x", 0}, {"y", 0}, {"color", "\x1b[31mred\x07"}});
        QVERIFY(loud.contains("no color"));
        QVERIFY(!loud.contains(QChar(0x1b)) && !loud.contains(QChar(0x07)));
        QVERIFY(problems({{"type", "shape"}, {"x", 0}, {"y", 0}, {"shape", QString(5000, 'x')}}).size() < 400);
        QVERIFY(problems({{"type", "connector"}, {"x1", 0}, {"y1", 0}, {"x2", 9}, {"y2", 9}, {"route", QString(5000, 'x')}})
                    .size() < 200);
    }

    void whatIsPrintedCannotDriveATerminal() {
        const QString text =
            QString::fromUtf8("plain \x1b[2Jcleared\rover\x07 del\x7f \xC2\x9B" "31m tab\tline\nnext");
        const QString printed = printableText(text);
        for (const QChar c : printed)
            QVERIFY2(c.category() != QChar::Other_Control || c == '\n' || c == '\t',
                     qPrintable(QString::number(c.unicode(), 16)));
        QVERIFY(printed.contains("cleared") && printed.contains("tab\tline\nnext"));
        QCOMPARE(printed.size(), text.size());
        // In JSON nothing is lost: what a terminal would obey is written as
        // an escape, and reads back as what it was.
        const QByteArray json = printableJson(QJsonDocument(QJsonObject{{"text", text}}).toJson());
        for (const QChar c : QString::fromUtf8(json))
            QVERIFY2(c.category() != QChar::Other_Control || c == '\n', qPrintable(QString::number(c.unicode(), 16)));
        QCOMPARE(QJsonDocument::fromJson(json).object().value("text").toString(), text);
    }

    void aBoardsNameIsNotAPath() {
        QTemporaryDir dir;
        Library library(dir.path());
        QCOMPARE(library.create("plans"), QString());
        QVERIFY(library.exists("plans"));
        QVERIFY(!library.exists("../boards/plans"));
        QVERIFY(!library.exists("plans/../plans"));
        QVERIFY(!library.exists(".."));
        QVERIFY(library.remove("../boards/plans").contains("no board"));
        QVERIFY(library.exists("plans"));
    }

    void onlyPicturesAreKeptAsPictures() {
        QTemporaryDir dir;
        const QString assets = dir.filePath("board/assets");
        QString problem;
        // What is not a picture is not taken, whatever it is called...
        write(dir.filePath("notes.png"), "not a picture at all");
        QCOMPARE(Library::importFile(assets, dir.filePath("notes.png"), &problem), QString());
        QVERIFY2(problem.contains("not a picture"), qPrintable(problem));
        // ...and what is not a file is not so much as opened: a pipe that
        // nothing writes to would never answer.
        QVERIFY(QDir(dir.path()).mkdir("folder.png"));
        QCOMPARE(Library::importFile(assets, dir.filePath("folder.png"), &problem), QString());
        QVERIFY2(problem.contains("not a file"), qPrintable(problem));
        QCOMPARE(mkfifo(QFile::encodeName(dir.filePath("pipe.png")).constData(), 0600), 0);
        QCOMPARE(Library::importFile(assets, dir.filePath("pipe.png"), &problem), QString());
        QVERIFY2(problem.contains("not a file"), qPrintable(problem));
        QVERIFY(QDir(assets).entryList(QDir::Files).isEmpty());
    }

    void pastedPicturesComeAlongAndNothingElseDoes() {
        Desk desk;
        QVERIFY(desk.ready());
        // Somewhere that is no board: a picture, and something private.
        QTemporaryDir elsewhere;
        picture(elsewhere.filePath("photo.png"), QSize(40, 30));
        write(elsewhere.filePath("id_rsa"), "-----BEGIN OPENSSH PRIVATE KEY-----");
        auto image = [](const QString &asset, qreal x) {
            return QJsonObject{{"type", "image"}, {"x", x}, {"y", 0}, {"w", 40}, {"h", 30}, {"asset", asset}};
        };
        // What anything running could have put on the clipboard.
        auto *data = new QMimeData;
        data->setData("application/x-sprawl-items",
                      QJsonDocument(QJsonObject{{"sprawl", 1},
                                                {"items", QJsonArray{image("photo.png", 0), image("id_rsa", 60)}},
                                                {"assets", elsewhere.path()}})
                          .toJson());
        QGuiApplication::clipboard()->setMimeData(data);
        desk.hover(QPointF(400, 300));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        const auto pictures = desk.all(Item::Kind::Image);
        QCOMPARE(pictures.size(), 2);
        // The picture came along, kept as any picture is. The other thing
        // stayed where it was, and its item says its picture is missing.
        const QDir assets(desk.backend.assetFolder());
        QCOMPARE(assets.entryList(QDir::Files).size(), 1);
        QCOMPARE(read(assets.filePath(pictures[0]->asset)), read(elsewhere.filePath("photo.png")));
        QVERIFY(!assets.exists("id_rsa"));
        QCOMPARE(pictures[1]->asset, QString("id_rsa"));

        // A pipe handed over as a picture is not opened to find out.
        const QString pipe = elsewhere.filePath("pipe.png");
        QCOMPARE(mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
        desk.backend.addFiles({QUrl::fromLocalFile(pipe)});
        QVERIFY2(desk.backend.problem().contains("not a picture"), qPrintable(desk.backend.problem()));
        QCOMPARE(desk.all(Item::Kind::Image).size(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void aBoardShowsOnlyThePicturesItsOwnFolderHolds() {
        // A board from somewhere else, and beside it a picture the person
        // keeps to themselves, in red.
        QTemporaryDir dir;
        const QString secret = picture(dir.filePath("secret.png"), QSize(10, 10), Qt::red);
        QVERIFY(QDir(dir.path()).mkpath("board/assets"));
        const QString assets = dir.filePath("board/assets");
        picture(assets + "/own.png", QSize(10, 10), Qt::blue);
        QCOMPARE(ImageStore::read(assets, "own.png").pixelColor(5, 5), QColor(Qt::blue));
        // A picture of the board that is a link to it is not read, nor one
        // in an assets folder that is itself a link to where it is, nor a
        // way out of the folder, nor a pipe, which is not waited on.
        QVERIFY(QFile::link(secret, assets + "/linked.png"));
        QVERIFY(ImageStore::read(assets, "linked.png").isNull());
        QVERIFY(QDir(dir.path()).mkpath("other"));
        QVERIFY(QFile::link(dir.path(), dir.filePath("other/assets")));
        QVERIFY(ImageStore::read(dir.filePath("other/assets"), "secret.png").isNull());
        QVERIFY(ImageStore::read(assets, "../../secret.png").isNull());
        QCOMPARE(mkfifo(QFile::encodeName(assets + "/pipe.png").constData(), 0600), 0);
        QVERIFY(ImageStore::read(assets, "pipe.png").isNull());

        // An SVG that draws the picture into itself by its path, however it
        // says so, packed or not: Qt would show it, so it is not shown.
        auto svg = [](const QString &attribute, const QString &href, const QByteArray &before = {}) {
            return before + QString("<svg xmlns='http://www.w3.org/2000/svg' "
                                    "xmlns:xlink='http://www.w3.org/1999/xlink' width='10' height='10'>"
                                    "<image x='0' y='0' width='10' height='10' %1='%2'/></svg>")
                                .arg(attribute, href)
                                .toUtf8();
        };
        const QList<QByteArray> drawingIn{
            svg("xlink:href", secret),
            svg("href", secret),
            svg("xlink:href", "../../secret.png"),
            svg("href", "  " + secret),
            svg("href", "data/../../secret.png"),
            svg("xlink:href", "&where;", "<!DOCTYPE svg [<!ENTITY where '" + secret.toUtf8() + "'>]>"),
            svg("xlink:href", "data:image/png;base64,AAAA",
                "<?xml-stylesheet type='text/css' href='" + dir.filePath("style.css").toUtf8() + "'?>"),
        };
        write(assets + "/shown.svg", drawingIn.first());
        QCOMPARE(QImage(assets + "/shown.svg").pixelColor(5, 5), QColor(Qt::red));
        for (qsizetype i = 0; i < drawingIn.size(); ++i) {
            const QString name = QString("drawing%1.svg").arg(i);
            write(assets + "/" + name, drawingIn[i]);
            QVERIFY2(ImageStore::read(assets, name).isNull(), drawingIn[i].constData());
            // Whatever it is called.
            write(assets + "/" + name + ".png", drawingIn[i]);
            QVERIFY2(ImageStore::read(assets, name + ".png").isNull(), drawingIn[i].constData());
        }
        write(assets + "/packed.svgz", gzipped(drawingIn.first()));
        QVERIFY(ImageStore::read(assets, "packed.svgz").isNull());
        // One that holds its picture itself is shown, packed or not.
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImage blue(10, 10, QImage::Format_RGB32);
        blue.fill(Qt::blue);
        blue.save(&buffer, "PNG");
        const QByteArray holding = svg("xlink:href", "data:image/png;base64," + QString::fromLatin1(png.toBase64()));
        write(assets + "/holding.svg", holding);
        QCOMPARE(ImageStore::read(assets, "holding.svg").pixelColor(5, 5), QColor(Qt::blue));
        write(assets + "/holding.svgz", gzipped(holding));
        QCOMPARE(ImageStore::read(assets, "holding.svgz").pixelColor(5, 5), QColor(Qt::blue));

        // An export of the board shows each as missing, with none of the red.
        Board board;
        int x = 0;
        for (const QString &asset : {QString("linked.png"), QString("drawing0.svg"), QString("packed.svgz")}) {
            Item image;
            image.kind = Item::Kind::Image;
            image.rect = QRectF(x, 0, 40, 40);
            image.asset = asset;
            board.add(image);
            x += 60;
        }
        Exporter::Options options;
        options.scale = 1;
        options.margin = 0;
        QCOMPARE(Exporter::write(dir.filePath("out.png"), board, {}, Palette::fallback(), assets, options), QString());
        const QImage out(dir.filePath("out.png"));
        QVERIFY(!out.isNull());
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                QVERIFY(out.pixelColor(x, y) != QColor(Qt::red));

        // Nor is such an SVG taken in as a picture, from a file or a board.
        QTemporaryDir other;
        QString error;
        QCOMPARE(Library::importFile(other.path(), assets + "/drawing0.svg", &error), QString());
        QVERIFY2(error.contains("not a picture"), qPrintable(error));
        QCOMPARE(Library::importAsset(other.path(), assets, "drawing0.svg"), QString());
        QCOMPARE(Library::importAsset(other.path(), assets, "linked.png"), QString());
        QVERIFY(!Library::importFile(other.path(), assets + "/holding.svg").isEmpty());
        QCOMPARE(QDir(other.path()).entryList(QDir::Files).size(), 1);
    }

    void picturesNoLongerOnTheBoardAreLetGoOf() {
        QTemporaryDir dir;
        const QStringList names{"a.png", "b.png", "c.png", "d.png"};
        for (const QString &name : names)
            picture(dir.filePath(name), QSize(100, 100));
        const qint64 one = 100 * 100 * 4;
        ImageStore store;
        store.setFolder(dir.path());
        store.setUnusedBudget(2 * one);
        // On the board, they are all kept, whatever the budget.
        store.setInUse(QSet<QString>(names.cbegin(), names.cend()));
        for (const QString &name : names)
            store.image(name);
        QTRY_VERIFY(!store.loading());
        QCOMPARE(store.bytesHeld(), 4 * one);
        QCOMPARE(store.unusedBytesHeld(), qint64(0));
        // Taken off it, as many are kept as the budget has room for.
        store.setInUse({"a.png"});
        QCOMPARE(store.unusedBytesHeld(), 2 * one);
        QCOMPARE(store.bytesHeld(), 3 * one);
        QVERIFY(!store.alreadyRead("a.png").isNull());
        // The one just taken off is the last to go.
        store.setInUse({});
        QCOMPARE(store.bytesHeld(), 2 * one);
        QVERIFY(!store.alreadyRead("a.png").isNull());
        store.setUnusedBudget(0);
        QCOMPARE(store.bytesHeld(), qint64(0));
        // Put back, a picture is read again.
        store.setInUse({"b.png"});
        QVERIFY(store.image("b.png").isNull());
        QTRY_VERIFY(!store.alreadyRead("b.png").isNull());
        QCOMPARE(store.bytesHeld(), one);
        // And one read while it is on nothing goes once it has been read.
        store.image("c.png");
        QTRY_VERIFY(!store.loading());
        QCOMPARE(store.bytesHeld(), one);
        QVERIFY(store.alreadyRead("c.png").isNull());
    }

    void groupsMadeApartAreKeptApart() {
        // Four notes, open in two places.
        Board start;
        for (int i = 0; i < 4; ++i)
            start.add(note(i * 200, 0, QString::number(i + 1)));
        const QJsonObject file = start.toJson();
        QVector<Item> base;
        QCOMPARE(Board::read(file, base), QStringList());
        auto groupOf = [](Board &board, const QVector<quint32> &ids) {
            const quint32 group = board.newGroup();
            board.begin();
            for (quint32 id : ids) {
                Item item = *board.item(id);
                item.group = group;
                board.set(item);
            }
            board.commit();
            return group;
        };
        auto members = [](const Board &board, quint32 group) {
            QVector<quint32> found = board.membersOf(group);
            std::sort(found.begin(), found.end());
            return found;
        };
        // There, the first two are grouped; here, the other two, and both
        // come to the same number for their group.
        Board theirs, mine;
        QCOMPARE(theirs.fromJson(file), QStringList());
        QCOMPARE(mine.fromJson(file), QStringList());
        const quint32 group = groupOf(theirs, {1, 2});
        QCOMPARE(groupOf(mine, {3, 4}), group);
        mine.merge(base, theirs.items(), theirs.lastId());
        QCOMPARE(members(mine, group), (QVector<quint32>{1, 2}));
        const quint32 own = mine.item(3)->group;
        QVERIFY(own != 0 && own != group);
        QCOMPARE(members(mine, own), (QVector<quint32>{3, 4}));
        QVERIFY(mine.newGroup() > own);
        // Undone and done again, what was grouped here is that group still.
        mine.undo();
        QCOMPARE(mine.item(3)->group, quint32(0));
        QCOMPARE(members(mine, group), (QVector<quint32>{1, 2}));
        mine.redo();
        QCOMPARE(members(mine, own), (QVector<quint32>{3, 4}));
        // Written and read back: two groups of two.
        Board again;
        QCOMPARE(again.fromJson(mine.toJson()), QStringList());
        QCOMPARE(members(again, again.item(1)->group), (QVector<quint32>{1, 2}));
        QCOMPARE(members(again, again.item(3)->group), (QVector<quint32>{3, 4}));

        // The same two grouped in both places are one group.
        Board there, here;
        QCOMPARE(there.fromJson(file), QStringList());
        QCOMPARE(here.fromJson(file), QStringList());
        groupOf(there, {1, 2});
        groupOf(here, {1, 2});
        here.merge(base, there.items(), there.lastId());
        QVERIFY(here.item(1)->group != 0);
        QCOMPARE(members(here, here.item(1)->group), (QVector<quint32>{1, 2}));
    }

    void groupsMadeInTwoWindowsAtOnceStayApart() {
        QTemporaryDir home;
        Library library(home.path());
        QVERIFY(QDir(home.path()).mkpath("boards/desk"));
        write(library.boardFile("desk"), R"({"sprawl": 1, "items": [
            {"id": 1, "type": "note", "x": 0, "y": 0}, {"id": 2, "type": "note", "x": 200, "y": 0},
            {"id": 3, "type": "note", "x": 400, "y": 0}, {"id": 4, "type": "note", "x": 600, "y": 0}]})");
        FakePicker picker;
        Theme theme(home.path(), nullptr, {}, home.filePath("shell.toml"));
        Backend first(&library, &picker, &theme), second(&library, &picker, &theme);
        first.openInitial("desk");
        second.openInitial("desk");
        auto groupOf = [](Board &board, const QVector<quint32> &ids) {
            const quint32 group = board.newGroup();
            board.begin();
            for (quint32 id : ids) {
                Item item = *board.item(id);
                item.group = group;
                board.set(item);
            }
            board.commit();
        };
        groupOf(*first.board(), {1, 2});
        groupOf(*second.board(), {3, 4});
        QVERIFY(first.flush());
        QVERIFY(second.flush());
        QJsonObject root;
        QCOMPARE(Library::read(library.boardFile("desk"), root), QString());
        Board reopened;
        QCOMPARE(reopened.fromJson(root), QStringList());
        QVERIFY(reopened.item(1)->group != 0);
        QCOMPARE(reopened.item(2)->group, reopened.item(1)->group);
        QVERIFY(reopened.item(3)->group != 0);
        QCOMPARE(reopened.item(4)->group, reopened.item(3)->group);
        QVERIFY(reopened.item(1)->group != reopened.item(3)->group);
    }

    void aBoardRenamedInAnotherWindowIsFollowed() {
        QTemporaryDir home;
        Library library(home.path());
        QCOMPARE(library.create("desk"), QString());
        FakePicker picker;
        Theme theme(home.path(), nullptr, {}, home.filePath("shell.toml"));
        Backend first(&library, &picker, &theme), second(&library, &picker, &theme);
        first.openInitial("desk");
        second.openInitial("desk");
        auto items = [&library](const QString &name) {
            QJsonObject root;
            Library::read(library.boardFile(name), root);
            QStringList texts;
            for (const QJsonValue &item : root.value("items").toArray())
                texts << item.toObject().value("text").toString();
            return texts;
        };
        // Renamed in the first window, and written in the second, which
        // was not told: what it writes goes to the board it has open, and
        // the old name is not made again for it.
        QVERIFY(first.renameBoard("renamed"));
        second.board()->add(note(0, 0, "Second"));
        QTRY_COMPARE(second.saveState(), QString("saved"));
        QVERIFY(!QFileInfo::exists(library.boardFolder("desk")));
        QCOMPARE(second.boardName(), QString("renamed"));
        QCOMPARE(second.assetFolder(), library.assetsFolder("renamed"));
        QCOMPARE(items("renamed"), QStringList{"Second"});
        QTRY_COMPARE(first.board()->count(), 1);
        // Renamed again, and written at once, as on quitting.
        QVERIFY(first.renameBoard("again"));
        second.board()->add(note(300, 0, "Quitting"));
        QVERIFY(second.flush());
        QVERIFY(!QFileInfo::exists(library.boardFolder("renamed")));
        QCOMPARE(second.boardName(), QString("again"));
        QCOMPARE(items("again"), (QStringList{"Second", "Quitting"}));
        // Taken away for good, it is made again with what it had, as before.
        QVERIFY(QDir(library.boardFolder("again")).removeRecursively());
        second.board()->add(note(600, 0, "Kept"));
        QVERIFY(second.flush());
        QCOMPARE(items("again"), (QStringList{"Second", "Quitting", "Kept"}));
    }

    void aBoardTrashedRightAfterAnEditTakesTheEditWithIt() {
        QTemporaryDir home;
        Library library(home.path());
        QCOMPARE(library.create("last-edit"), QString());
        FakePicker picker;
        Theme theme(home.path(), nullptr, {}, home.filePath("shell.toml"));
        Backend backend(&library, &picker, &theme);
        backend.openInitial("last-edit");
        backend.board()->add(note(0, 0, "Just now"));
        QCOMPARE(backend.saveState(), QString("unsaved"));
        QVERIFY(backend.deleteBoard());
        QVERIFY(!library.exists("last-edit"));
        const QDir trash(qEnvironmentVariable("XDG_DATA_HOME") + "/Trash/files");
        const QStringList trashed = trash.entryList({"last-edit*"}, QDir::Dirs);
        QCOMPARE(trashed.size(), 1);
        QJsonObject root;
        QCOMPARE(Library::read(trash.filePath(trashed.first() + "/board.json"), root), QString());
        const QJsonArray items = root.value("items").toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value("text").toString(), QString("Just now"));
    }

    void aCopyOfABoardHasOnlyThePicturesOnIt() {
        QTemporaryDir dir;
        qputenv("SPRAWL_HOME", dir.path().toUtf8());
        const auto unset = qScopeGuard([] { qunsetenv("SPRAWL_HOME"); });
        const Library library(dir.path());
        const QString kept = picture(dir.filePath("kept.png"), QSize(20, 20), Qt::blue);
        const QString gone = picture(dir.filePath("gone.png"), QSize(20, 20), Qt::red);
        QCOMPARE(runCli({"sprawl", "new", "trip"}), 0);
        QCOMPARE(runCli({"sprawl", "add", "trip", "image", "--file", kept}), 0);
        QCOMPARE(runCli({"sprawl", "add", "trip", "image", "--file", gone}), 0);
        QCOMPARE(runCli({"sprawl", "remove", "trip", "2"}), 0);
        // The board's own folder keeps the picture taken off it, for a window
        // that can still undo that.
        const QDir assets(library.assetsFolder("trip"));
        QCOMPARE(assets.entryList(QDir::Files).size(), 2);
        // A copy to give away has only the one on the board.
        const QString copy = dir.filePath("to-give/trip");
        QCOMPARE(runCli({"sprawl", "copy", "trip", copy}), 0);
        const QStringList copied = QDir(copy + "/assets").entryList(QDir::Files);
        QCOMPARE(copied.size(), 1);
        QCOMPARE(read(copy + "/assets/" + copied.first()), read(kept));
        QCOMPARE(runCli({"sprawl", "check", copy}), 0);
        QJsonObject root;
        QCOMPARE(Library::read(copy + "/board.json", root), QString());
        QCOMPARE(root.value("items").toArray().size(), 1);
        QCOMPARE(assets.entryList(QDir::Files).size(), 2);
        // Never over what is there already.
        QCOMPARE(runCli({"sprawl", "copy", "trip", copy}), 1);
        QCOMPARE(QDir(copy + "/assets").entryList(QDir::Files), copied);
        // A picture of the board that is a link to somewhere else is not
        // copied from there.
        QVERIFY(QFile::remove(assets.filePath(copied.first())));
        QVERIFY(QFile::link(gone, assets.filePath(copied.first())));
        const QString linked = dir.filePath("linked");
        QCOMPARE(runCli({"sprawl", "copy", "trip", linked}), 0);
        QVERIFY(!QFile::exists(linked + "/assets/" + copied.first()));
        QCOMPARE(runCli({"sprawl", "check", linked}), 1);
    }

    void anExportOfWhatWasSelectedIsNeverOfTheWholeBoard() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 chosen = desk.add(note(100, 100, "Chosen"));
        desk.add(note(500, 400, "Not to be shown"));
        desk.click(QPointF(150, 150));
        QCOMPARE(desk.canvas->selectionCount(), 1);
        // The formats are up when the note is taken away elsewhere, by
        // another window or a command: what was selected is gone, and so is
        // the selection.
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("chosen.png"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(desk.panel(), QString("export"));
        desk.board().remove(chosen);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.panel(), QString());
        QCOMPARE(desk.picker.asked, 0);
        QVERIFY(!QFile::exists(desk.home.filePath("chosen.png")));
        const QString gone = "Nothing was exported: what was selected is no longer on the board.";
        QCOMPARE(desk.backend.problem(), gone);
        // Taken away once the dialog is asking where to save it: nothing.
        desk.backend.dismissProblem();
        const quint32 again = desk.add(note(100, 100, "Again"));
        desk.picker.target = {};
        desk.backend.exportBoard("png", {QVariant(again)});
        QCOMPARE(desk.picker.asked, 1);
        desk.board().remove(again);
        emit desk.picker.exportSelected(QUrl::fromLocalFile(desk.home.filePath("again.png")), "png");
        QVERIFY(!QFile::exists(desk.home.filePath("again.png")));
        QCOMPARE(desk.backend.problem(), gone);
        // What was selected and is still there is what is exported.
        desk.backend.dismissProblem();
        const quint32 left = desk.add(note(100, 100, "Left"));
        desk.click(QPointF(150, 150));
        desk.picker.target = QUrl::fromLocalFile(desk.home.filePath("left.png"));
        desk.key(Qt::Key_E, Qt::ControlModifier);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.backend.problem(), QString());
        QCOMPARE(QImage(desk.home.filePath("left.png")).size(),
                 (Exporter::area(desk.board(), {left}, 32).size() * 2).toSize());
        QCOMPARE(desk.warnings, QStringList());
    }

    void aPicturePastedFromAnotherBoardIsTheOneThatWasCopied() {
        // Two boards with a picture called the same, and different: red on
        // the one copied from, blue on the one pasted onto.
        Desk desk("blue", [](const QString &home) {
            const QByteArray board = R"({"sprawl": 1, "items": [
                {"id": 1, "type": "image", "x": 0, "y": 0, "w": 20, "h": 20, "asset": "photo.png"}]})";
            for (const QString &name : {QString("red"), QString("blue")}) {
                QDir(home).mkpath("boards/" + name + "/assets");
                picture(home + "/boards/" + name + "/assets/photo.png", QSize(20, 20), QColor(name));
                write(home + "/boards/" + name + "/board.json", board);
            }
        });
        QVERIFY(desk.ready());
        const QByteArray red = read(desk.library.assetsFolder("red") + "/photo.png");
        const QByteArray blue = read(desk.library.assetsFolder("blue") + "/photo.png");
        QVERIFY(desk.backend.openBoard("red"));
        desk.canvas->selectAll();
        desk.canvas->copy();
        QVERIFY(desk.backend.openBoard("blue"));
        desk.hover(QPointF(400, 300));
        QVERIFY(desk.canvas->paste());
        QCOMPARE(desk.all(Item::Kind::Image).size(), 2);
        const QString pasted = desk.last()->asset;
        QVERIFY(pasted != "photo.png");
        const QDir assets(desk.backend.assetFolder());
        QCOMPARE(read(assets.filePath(pasted)), red);
        QCOMPARE(read(assets.filePath("photo.png")), blue);
        // Pasted again, it is the same picture, kept once.
        QVERIFY(desk.canvas->paste());
        QCOMPARE(desk.last()->asset, pasted);
        QCOMPARE(assets.entryList(QDir::Files).size(), 2);
        // Where it was copied from, it is now a link to somewhere else: it
        // cannot be had from there, and it is not taken for the blue one
        // here, but left out.
        QTemporaryDir elsewhere;
        const QString secret = picture(elsewhere.filePath("secret.png"), QSize(20, 20), Qt::green);
        QVERIFY(QFile::remove(desk.library.assetsFolder("red") + "/photo.png"));
        QVERIFY(QFile::link(secret, desk.library.assetsFolder("red") + "/photo.png"));
        QSignalSpy leftOut(desk.canvas, &CanvasView::itemsLeftOut);
        QVERIFY(desk.canvas->paste());
        QTRY_COMPARE(leftOut.count(), 1);
        QCOMPARE(desk.all(Item::Kind::Image).size(), 3);
        QCOMPARE(assets.entryList(QDir::Files).size(), 2);
        QCOMPARE(desk.warnings, QStringList());
    }

    void exportsOnlyGoToFiles() {
        Desk desk;
        QVERIFY(desk.ready());
        desk.add(note(0, 0, "One"));
        desk.picker.target = QUrl("https://example.com/board.png");
        desk.backend.exportBoard("png", {});
        QCOMPARE(desk.picker.asked, 1);
        QVERIFY2(desk.backend.problem().contains("files on this computer"), qPrintable(desk.backend.problem()));
        QVERIFY(!QFile::exists(".png"));
    }

    void theEditorHoldsNoMoreThanAnItem() {
        Desk desk;
        QVERIFY(desk.ready());
        const quint32 id = desk.add(note(224, 124, "Call"));
        desk.doubleClick(QPointF(300, 200));
        QVERIFY(desk.canvas->editing());
        // Far more than an item holds, pasted in where the cursor is.
        QGuiApplication::clipboard()->setText(QString(30'000, 'x'));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QQuickItem *editor = desk.item("editor");
        QVERIFY(editor);
        QCOMPARE(editor->property("length").toInt(), Item::longestText);
        QCOMPARE(desk.board().item(id)->text.size(), Item::longestText);
        QVERIFY(desk.board().item(id)->text.startsWith("Callxxx"));
        QVERIFY2(desk.backend.notice().contains("characters"), qPrintable(desk.backend.notice()));
        // And typing on from there goes nowhere.
        desk.type("yz");
        QCOMPARE(editor->property("length").toInt(), Item::longestText);
        QVERIFY(!desk.board().item(id)->text.contains('y'));
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.canvas->editing());
        QCOMPARE(desk.board().item(id)->text.size(), Item::longestText);

        // Pasted onto the board instead, it is text of the same length.
        desk.hover(QPointF(600, 400));
        desk.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(desk.last()->kind, Item::Kind::Text);
        QCOMPARE(desk.last()->text.size(), Item::longestText);
        QCOMPARE(desk.warnings, QStringList());
    }

    void whatIsBeingWrittenIsPartOfTheFrame() {
        // Three thousand notes and, beside them, one with as much written
        // on it as a note holds.
        Desk desk("pile", [](const QString &home) {
            QJsonArray items;
            for (int i = 0; i < 3000; ++i)
                items.append(QJsonObject{{"type", "note"}, {"x", (i % 40) * 30.0}, {"y", (i / 40 % 30) * 30.0}});
            items.append(QJsonObject{{"type", "note"}, {"x", 1400}, {"y", 0}, {"text", QString(Item::longestText, 'a')}});
            QDir(home).mkpath("boards/pile");
            write(home + "/boards/pile/board.json", QJsonDocument(QJsonObject{{"sprawl", 1}, {"items", items}}).toJson());
        });
        QVERIFY(desk.ready());
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas->pendingItems(), 0, 30000);
        desk.canvas->setView(QPointF(-100, -100), 0.5);
        desk.window->grabWindow();
        const SceneLoad whole = desk.canvas->load();
        QCOMPARE(whole.items, 3001);
        QVERIFY(!whole.reduced);
        QCOMPARE(whole.editor, qint64(0));

        // The note being written in is the editor's to draw, and the editor
        // draws every letter it holds, in view or not. That is counted...
        const qint64 budget = whole.bytes - 500'000;
        desk.canvas->setLoadBudget(budget);
        desk.doubleClick(QPointF(1476, 76));
        QVERIFY(desk.canvas->editing());
        desk.window->grabWindow();
        const SceneLoad writing = desk.canvas->load();
        QCOMPARE(writing.editor, qint64(Item::longestText) * 2 * 96);
        // ...and the board gives way to it: the frame is what it may be.
        QVERIFY(writing.reduced);
        QVERIFY2(writing.bytes + writing.editor <= budget, qPrintable(QString::number(writing.bytes)));
        QVERIFY(writing.items > 0);
        // With the writing done, nothing is counted for it.
        desk.key(Qt::Key_Escape);
        desk.window->grabWindow();
        QCOMPARE(desk.canvas->load().editor, qint64(0));
    }

    void nothingWrittenInTheWindowIsReadAsMarkup() {
        // A board that will not open, in a folder named to look like markup
        // and saying why in words made to look like it too.
        Desk desk("<b>loud", [](const QString &home) {
            QDir(home).mkpath("boards/<b>loud");
            write(home + "/boards/<b>loud/board.json",
                  R"({"sprawl": 1, "items": [{"type": "<i>sticker", "x": 0, "y": 0}]})");
        });
        QVERIFY(desk.ready());
        QCOMPARE(desk.backend.boardName(), QString("<b>loud"));
        QQuickItem *problem = desk.item("problem");
        QVERIFY(problem && problem->isVisible());
        QVERIFY2(problem->property("text").toString().contains("<i>sticker"),
                 qPrintable(problem->property("text").toString()));
        // Every piece of text there is, with each panel open in turn: the
        // characters it was given, and nothing made of them.
        for (const QString &panel : QStringList{"", "keys", "boards", "export", "quit"}) {
            if (!panel.isEmpty())
                QMetaObject::invokeMethod(desk.window, "openPanel", Q_ARG(QVariant, panel));
            desk.window->grabWindow();
            QVector<QQuickItem *> texts;
            Desk::texts(desk.window->contentItem(), texts);
            QVERIFY2(texts.size() > 5, qPrintable(panel));
            QStringList marked;
            for (const QQuickItem *text : std::as_const(texts))
                if (text->property("textFormat").toInt() != Qt::PlainText)
                    marked << text->property("text").toString();
            QCOMPARE(marked, QStringList());
        }
    }

    void nothingIsEverFetched() {
        auto picture = [](const QTcpServer &server) {
            return QString("import QtQuick\nImage { source: \"http://127.0.0.1:%1/picture.png\" }")
                .arg(server.serverPort())
                .toUtf8();
        };
        // Left to itself, an engine goes and asks for it...
        {
            QTcpServer asked;
            QVERIFY(asked.listen(QHostAddress::LocalHost));
            QQmlEngine engine;
            QQmlComponent component(&engine);
            component.setData(picture(asked), QUrl());
            const std::unique_ptr<QObject> image(component.create());
            QVERIFY2(image, qPrintable(component.errorString()));
            QTRY_VERIFY(asked.hasPendingConnections());
        }
        // ...and the app's is told no before anything leaves. Somewhere of
        // its own to listen, so nothing the other engine sent is heard here.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        Desk desk;
        QVERIFY(desk.ready());
        QQmlComponent component(&desk.engine);
        component.setData(picture(server), QUrl());
        const std::unique_ptr<QObject> image(component.create());
        QVERIFY2(image, qPrintable(component.errorString()));
        // Image.Error
        QTRY_COMPARE(image->property("status").toInt(), 3);
        QTest::qWait(200);
        QVERIFY(!server.hasPendingConnections());
    }

    // --- Translator ----------------------------------------------------------

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json",
              R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // --- The tests themselves ------------------------------------------------

    void theTestsDoNotStartWithoutAFolderOfTheirOwn() {
        // The tests keep everything they write in a temporary folder of their
        // own (see main()). Where none can be made (here, where temporary
        // folders go is a file), the folders an app falls back to are the
        // person's own: their boards, and which of them opens next. A run
        // that went on would write among them. So it does not start, and
        // writes nothing at all, not even in the home folder.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath("temporary");
        write(file, {});
        QProcessEnvironment session = QProcessEnvironment::systemEnvironment();
        session.insert("TMPDIR", file);
        session.insert("HOME", dir.filePath("home"));
        QProcess tests;
        tests.setProcessEnvironment(session);
        tests.setProcessChannelMode(QProcess::MergedChannels);
        // Asked for a test that opens a window on a board, and so writes one.
        tests.start(QCoreApplication::applicationFilePath(), {"theWindowOpensOnABoard"});
        QVERIFY(tests.waitForFinished(60000));
        const QString said = QString::fromLocal8Bit(tests.readAll());
        QVERIFY2(tests.exitStatus() == QProcess::NormalExit && tests.exitCode() != 0, qPrintable(said));
        QVERIFY2(!said.contains("Totals:"), qPrintable(said));
        QVERIFY2(!QFileInfo::exists(dir.filePath("home")), qPrintable(said));
    }

    void aDeskWithoutAFolderOfItsOwnOpensNothing() {
        // Nor does a desk that cannot have a folder of its own, once the
        // tests have started: it prepares nothing, opens no board in the
        // library everything falls back to, and is not ready. Here where
        // temporary folders go becomes a file, and the home folder, where a
        // theme and a language are looked for, an empty one, with no Hyprland
        // to ask.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath("temporary");
        write(file, {});
        // All three as they were, however the test ends.
        QHash<QByteArray, std::optional<QByteArray>> had;
        for (const char *name : {"TMPDIR", "HOME", "HYPRLAND_INSTANCE_SIGNATURE"})
            had.insert(name, qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        const auto restore = qScopeGuard([&had] {
            for (auto it = had.cbegin(); it != had.cend(); ++it) {
                if (it.value())
                    qputenv(it.key().constData(), *it.value());
                else
                    qunsetenv(it.key().constData());
            }
        });
        qputenv("TMPDIR", file.toUtf8());
        qputenv("HOME", dir.path().toUtf8());
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");

        bool prepared = false;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^No folder for a desk"));
        {
            Desk desk("homeless", [&prepared](const QString &) { prepared = true; });
            QVERIFY(!desk.ready());
        }
        QVERIFY(!prepared);
        const QString library = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sprawl";
        QVERIFY(!QFileInfo::exists(library + "/boards/homeless"));
    }

    void theTestsDrawWithTheProcessor() {
        // What the tests draw is drawn by the processor, never the GPU (see
        // main()).
        Desk desk;
        QVERIFY(desk.ready());
        desk.window->grabWindow();
        QCOMPARE(desk.window->rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
    }

    void theTestsDrawWithTheProcessorHoweverTheyAreStarted() {
        // However they are started: here as a process of their own, from a
        // session that asks Qt Quick to draw with the GPU, with no display to
        // reach one through, and with Qt's null GPU, which draws nothing, as
        // the one it would get. That process is asked only for the test
        // above, which starts nothing, so this test never has to tell which
        // process it is in, and whatever these tests were started with, it
        // starts one.
        QProcessEnvironment session = QProcessEnvironment::systemEnvironment();
        session.remove("DISPLAY");
        session.remove("WAYLAND_DISPLAY");
        session.insert("QT_QUICK_BACKEND", "rhi");
        session.insert("QSG_RHI_BACKEND", "null");
        QProcess tests;
        tests.setProcessEnvironment(session);
        tests.setProcessChannelMode(QProcess::MergedChannels);
        tests.start(QCoreApplication::applicationFilePath(), {"theTestsDrawWithTheProcessor"});
        QVERIFY(tests.waitForFinished(60000));
        const QString said = QString::fromLocal8Bit(tests.readAll());
        QVERIFY2(tests.exitStatus() == QProcess::NormalExit && tests.exitCode() == 0, qPrintable(said));
        QVERIFY2(said.contains("PASS   : SprawlTests::theTestsDrawWithTheProcessor()"), qPrintable(said));
    }
};

int main(int argc, char *argv[]) {
    // A command run by a test as a process of its own, beside others, the
    // way an agent runs them: sprawl_tests --cli add <board> note ... It
    // only ever works on the library the test made for it.
    if (argc > 1 && qstrcmp(argv[1], "--cli") == 0) {
        if (qEnvironmentVariableIsEmpty("SPRAWL_HOME"))
            return 2;
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        app.setApplicationName("sprawl");
        QStringList arguments = app.arguments();
        arguments.removeAt(1);
        return runCli(arguments);
    }

    // Nothing a test does reaches the real boards, their state, the trash,
    // or the desktop's theme and language: the folders an app keeps its
    // things in are all somewhere that goes away with the tests.
    QTemporaryDir sandbox;
    // With nowhere to make that, there are no tests: left empty, each of
    // those folders would be the person's own.
    if (!sandbox.isValid()) {
        fprintf(stderr, "sprawl_tests: no folder for the tests in %s: %s\n",
                qPrintable(qEnvironmentVariable("TMPDIR", "/tmp")), qPrintable(sandbox.errorString()));
        return 1;
    }
    for (const char *folder : {"XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"}) {
        const QString path = sandbox.filePath(QString::fromLatin1(folder).toLower());
        QDir().mkpath(path);
        qputenv(folder, path.toUtf8());
    }
    // Omarchy keeps the current theme, and the person's own settings for the
    // shell, in the home folder itself.
    QDir().mkpath(sandbox.filePath("home"));
    qputenv("HOME", sandbox.filePath("home").toUtf8());
    qunsetenv("SPRAWL_HOME");
    // And nothing shows on the desktop or reaches its clipboard, whatever
    // platform the session asks Qt apps to use: the tests' windows are drawn
    // nowhere, however the binary is started.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    // Nor are they drawn by the GPU. Offscreen, Qt Quick still draws with
    // one when it can reach a display; some of the tests' frames are far
    // heavier than any a person would ask for, and on some machines a GPU
    // asked for too much takes the whole desktop down with it.
    qputenv("QT_QUICK_BACKEND", "software");

    QGuiApplication app(argc, argv);
    app.setApplicationName("sprawl");
    SprawlTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "sprawl_tests.moc"
