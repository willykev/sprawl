#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <sys/stat.h>

#include "backend.h"
#include "board.h"
#include "canvasview.h"
#include "cli.h"
#include "exporter.h"
#include "filepicker.h"
#include "geometry.h"
#include "library.h"
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
        if (before)
            before(home);
        return 0;
    }

    QTemporaryDir home;
    // What `before` put in the folder is there before anything reads it.
    int prepared;
    Library library;
    FakePicker picker;
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
        : prepared(run(before, home.path())), library(home.path()), theme(home.path()),
          translator(home.filePath("language"), ":/i18n"), backend(&library, &picker, &theme) {
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

        // Moving a thing takes its end of the connector along.
        desk.click(QPointF(120, 120));
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
};

int main(int argc, char *argv[]) {
    // Nothing a test does reaches the real boards, their state, the trash,
    // or the desktop's theme and language: the folders an app keeps its
    // things in are all somewhere that goes away with the tests.
    QTemporaryDir sandbox;
    for (const char *folder : {"XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"}) {
        const QString path = sandbox.filePath(QString::fromLatin1(folder).toLower());
        QDir().mkpath(path);
        qputenv(folder, path.toUtf8());
    }
    qunsetenv("SPRAWL_HOME");
    // And nothing shows on the desktop or reaches its clipboard, whatever
    // platform the session asks Qt apps to use: the tests' windows are drawn
    // nowhere, however the binary is started.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    QGuiApplication app(argc, argv);
    app.setApplicationName("sprawl");
    SprawlTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "sprawl_tests.moc"
