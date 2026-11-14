// sprawl — an infinite canvas for the Omarchy desktop: notes, shapes,
// connectors, pen strokes and pictures on boards that never end.

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "backend.h"
#include "canvasview.h"
#include "cli.h"
#include "geometry.h"
#include "library.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

namespace {

// Brings the Sprawl window that is already open to the front. True when
// there was one and it now has the focus.
bool focusOpenWindow() {
    QProcess clients;
    clients.start("hyprctl", {"clients", "-j"});
    if (!clients.waitForFinished(1500))
        return false;
    for (const QJsonValue &value : QJsonDocument::fromJson(clients.readAllStandardOutput()).array()) {
        const QJsonObject client = value.toObject();
        if (client.value("class").toString() != "sprawl")
            continue;
        const QString window = "address:" + client.value("address").toString();
        // Hyprland's Lua dispatchers, then the syntax of the versions before them.
        const QList<QStringList> attempts{{"dispatch", QString("hl.dsp.focus({ window = \"%1\" })").arg(window)},
                                          {"dispatch", "focuswindow", window}};
        bool asked = false;
        for (const QStringList &arguments : attempts) {
            QProcess focus;
            focus.start("hyprctl", arguments);
            asked = focus.waitForFinished(1500) && focus.readAllStandardOutput().trimmed() == "ok";
            if (asked)
                break;
        }
        // Hyprland answers "ok" even when the window stayed where it was (on a
        // hidden special workspace, say), so look at what has the focus now.
        QProcess active;
        active.start("hyprctl", {"-j", "activewindow"});
        if (asked && active.waitForFinished(1500) &&
            QJsonDocument::fromJson(active.readAllStandardOutput()).object().value("address") == client.value("address"))
            return true;
    }
    return false;
}

// A scribble of a hundred points or so, as the pen leaves one.
Item scribble(QPointF at, int seed) {
    Item item;
    item.kind = Item::Kind::Stroke;
    QVector<QPointF> points;
    double px = 0, py = 0;
    for (int p = 0; p < 120; ++p) {
        px += 1.2;
        py += std::sin(p * 0.35 + seed) * 4;
        points.append(QPointF(px, py));
    }
    const QRectF bounds = Geo::boundsOf(points);
    for (QPointF &point : points)
        point -= bounds.topLeft();
    item.points = points;
    item.rect = QRectF(at, bounds.size());
    return item;
}

// A board of made-up things, laid out in clusters like a real one, for
// measuring how the canvas holds up: `sprawl open --bench 10000`.
QJsonObject madeUpBoard(int count) {
    QRandomGenerator random(7);
    const QStringList colors = Item::colorNames();
    const QStringList words{"plan", "ship", "sketch", "review", "idea", "later", "owner", "risk", "note", "draft"};
    QJsonArray items;
    const int columns = qMax(1, int(std::ceil(std::sqrt(count * 1.6))));
    int id = 0;
    for (int i = 0; i < count; ++i) {
        const double x = (i % columns) * 190.0 + random.bounded(24), y = (i / columns) * 190.0 + random.bounded(24);
        const QString text = QString("%1 %2\n%3 %4, %5")
                                 .arg(words[random.bounded(words.size())], QString::number(i),
                                      words[random.bounded(words.size())], words[random.bounded(words.size())],
                                      words[random.bounded(words.size())]);
        switch (i % 5) {
        case 0:
        case 1:
            items.append(QJsonObject{{"id", ++id}, {"type", "note"}, {"x", x}, {"y", y},
                                     {"color", colors[random.bounded(colors.size())]}, {"text", text}});
            break;
        case 2:
            items.append(QJsonObject{{"id", ++id}, {"type", "shape"}, {"x", x}, {"y", y + 40},
                                     {"shape", QStringList{"rect", "ellipse", "diamond"}[random.bounded(3)]},
                                     {"color", colors[random.bounded(colors.size())]}, {"text", text}});
            // Joined to the note before it.
            items.append(QJsonObject{{"id", ++id}, {"type", "connector"}, {"from", id - 2}, {"to", id - 1}});
            break;
        case 3: {
            Item pen = scribble(QPointF(x, y + 40), i);
            pen.id = ++id;
            pen.color = colors[random.bounded(colors.size())];
            items.append(Board::itemToJson(pen));
            break;
        }
        default:
            items.append(QJsonObject{{"id", ++id}, {"type", "text"}, {"x", x}, {"y", y + 50}, {"text", text},
                                     {"size", i % 15 == 4 ? 28 : 12}, {"bold", i % 15 == 4}});
            break;
        }
    }
    return {{"sprawl", Board::formatVersion}, {"items", items}};
}

QString megabytes(qint64 bytes) { return QString::number(bytes / 1e6, 'f', 1); }

// Times every frame while the canvas pans, zooms and drags by itself, then
// says how it went. What it measures is what a person would feel: how long
// each frame took to prepare and draw, and how evenly frames arrived. And
// what the GPU was asked for, which has to stay small however big the board.
class Benchmark {
public:
    Benchmark(QQuickWindow *window, CanvasView *canvas, Board *board) : m_window(window), m_canvas(canvas), m_board(board) {
        // These three are sent from the render thread; they only read clocks.
        QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [this] { m_syncStart = m_clock.nsecsElapsed(); },
                         Qt::DirectConnection);
        QObject::connect(window, &QQuickWindow::afterRendering, window,
                         [this] { m_work = m_clock.nsecsElapsed() - m_syncStart; }, Qt::DirectConnection);
        QObject::connect(
            window, &QQuickWindow::frameSwapped, window,
            [this] {
                const QMutexLocker lock(&m_mutex);
                const qint64 now = m_clock.nsecsElapsed();
                if (m_recording) {
                    m_intervals.append(now - m_lastSwap);
                    m_works.append(m_work);
                }
                m_lastSwap = now;
            },
            Qt::DirectConnection);
        // And this one runs on the GUI thread, once per frame, to move on.
        QObject::connect(window, &QQuickWindow::frameSwapped, canvas, [this] { step(); }, Qt::QueuedConnection);
    }

    void start() {
        m_clock.start();
        m_started = true;
        m_phaseClock.start();
        printf("%d items on the board, window %dx%d at scale %g, at most %s MB to a frame\n", m_board->count(),
               m_window->width(), m_window->height(), m_window->effectiveDevicePixelRatio(),
               qPrintable(megabytes(m_canvas->loadBudget())));
        fflush(stdout);
        // From the middle of the board at 100%, as a person works on one.
        m_canvas->setView(m_board->contentBounds().center() - QPointF(m_canvas->width(), m_canvas->height()) / 2, 1);
        // Something to drag in the last phase: whatever is nearest the middle.
        QVector<quint32> near;
        m_board->itemsIn(m_canvas->visibleArea(), near);
        for (quint32 id : std::as_const(near))
            if (m_board->item(id)->kind == Item::Kind::Note) {
                m_dragged = id;
                break;
            }
        m_canvas->update();
    }

private:
    void step() {
        if (!m_started || m_finished)
            return;
        const int frame = m_frame++;
        const QPointF middle(m_canvas->width() / 2, m_canvas->height() / 2);
        static const char *const names[] = {"pan", "zoom", "drag"};
        if (frame == 0) {
            const QMutexLocker lock(&m_mutex);
            m_recording = true;
            m_intervals.clear();
            m_works.clear();
            m_most = SceneLoad();
        }
        // What the frame before this one came to, once the frames on screen
        // are this phase's own.
        const SceneLoad load = m_canvas->load();
        if (frame >= 3) {
            if (load.bytes > m_most.bytes)
                m_most = load;
            m_reduced |= load.reduced;
        }
        if (m_phase == 0) {
            // A wide loop across the board.
            m_canvas->panBy(14 * std::cos(frame * 0.02), 9 * std::sin(frame * 0.031) + 3);
        } else if (m_phase == 1) {
            // All the way out, where the whole board is in view, and back in.
            m_canvas->zoomBy(frame < framesPerPhase / 2 ? 0.985 : 1 / 0.985, middle);
        } else if (m_dragged && m_board->item(m_dragged)) {
            // A note carried around, as a mouse would carry it.
            Item item = *m_board->item(m_dragged);
            item.rect.translate(3 * std::cos(frame * 0.05), 3 * std::sin(frame * 0.05));
            m_board->set(item);
        } else {
            m_canvas->update();
        }
        if (m_frame < framesPerPhase)
            return;

        QVector<qint64> intervals, works;
        {
            const QMutexLocker lock(&m_mutex);
            intervals = m_intervals;
            works = m_works;
            m_recording = false;
        }
        report(names[m_phase], intervals, works);
        printf("      most in a frame: %d of %d items, %lld triangles, %s MB%s\n", m_most.items, m_board->count(),
               static_cast<long long>(m_most.triangles), qPrintable(megabytes(m_most.bytes)),
               m_reduced ? " (detail was left out to stay within the frame)" : "");
        fflush(stdout);
        m_reduced = false;
        m_frame = 0;
        if (++m_phase == 3) {
            m_finished = true;
            QCoreApplication::quit();
        }
    }

    static void report(const char *name, QVector<qint64> intervals, QVector<qint64> works) {
        if (intervals.size() < 3)
            return;
        // The first frame of a phase still belongs to the last one.
        intervals.removeFirst();
        works.removeFirst();
        auto summarize = [](QVector<qint64> values, double &mean, double &p99, double &worst) {
            std::sort(values.begin(), values.end());
            double sum = 0;
            for (qint64 value : values)
                sum += value;
            mean = sum / values.size() / 1e6;
            p99 = values[qMin(values.size() - 1, values.size() * 99 / 100)] / 1e6;
            worst = values.last() / 1e6;
        };
        double mean, p99, worst, work, workP99, workWorst;
        summarize(intervals, mean, p99, worst);
        summarize(works, work, workP99, workWorst);
        printf("%-5s %4lld frames  %6.1f fps  frame %5.2f ms (99%%: %5.2f, worst %5.2f)  "
               "work per frame %5.2f ms (99%%: %5.2f, worst %5.2f)\n",
               name, static_cast<long long>(intervals.size()), 1000.0 / mean, mean, p99, worst, work, workP99,
               workWorst);
        fflush(stdout);
    }

    static constexpr int framesPerPhase = 360;
    QQuickWindow *m_window;
    CanvasView *m_canvas;
    Board *m_board;
    QElapsedTimer m_clock, m_phaseClock;
    QMutex m_mutex;
    QVector<qint64> m_intervals, m_works;
    qint64 m_lastSwap = 0, m_syncStart = 0, m_work = 0;
    bool m_recording = false, m_started = false, m_finished = false;
    int m_frame = 0, m_phase = 0;
    quint32 m_dragged = 0;
    SceneLoad m_most;
    bool m_reduced = false;
};

// Finds out what a frame costs this machine's GPU: `sprawl open --probe 8`.
//
// A tile-based GPU keeps every triangle of a frame in a buffer until the
// frame is drawn. The Asahi driver says so in the kernel log the first two
// times that buffer has to grow, long before it is anywhere near full. This
// puts pen strokes on an empty board a few at a time, listening to the
// kernel as it goes, and stops the moment it hears the second: what was on
// screen at each of the two is how many triangles a block of that buffer
// holds, which is what the frame budget (CanvasView::m_loadBudget) is
// reckoned from. It never draws more than the megabytes it is given.
class Probe : public QObject {
public:
    Probe(QQuickWindow *window, CanvasView *canvas, Board *board, double megabytes, const QString &kind)
        : m_window(window), m_canvas(canvas), m_board(board), m_limit(qint64(megabytes * 1e6)),
          m_notes(kind == "notes"), m_far(kind == "far"), m_unseen(kind == "unseen") {
        connect(window, &QQuickWindow::frameSwapped, this, &Probe::frame, Qt::QueuedConnection);
        connect(&m_kernel, &QProcess::readyReadStandardOutput, this, &Probe::heard);
        m_kernel.start("journalctl", {"-k", "-f", "-n", "0", "-o", "cat"});
    }
    ~Probe() override {
        m_kernel.kill();
        m_kernel.waitForFinished(500);
    }

    void start() {
        // At a size where strokes are drawn in full; at the smallest at
        // which type still is, which is where the most of it fits; or all
        // the way out, where strokes are specks and there are the most.
        const qreal pixels = m_window->effectiveDevicePixelRatio();
        m_canvas->setView(QPointF(0, 0), m_far ? 0.1 : (m_notes ? 0.5 : 1) / pixels);
        if (m_unseen) {
            // Strokes that are handed to the GPU but are nowhere in the
            // window: whether those cost it anything is the question.
            m_canvas->setDrawingEverything(true);
            m_canvas->setLoadBudget(m_limit * 4);
        }
        m_started = true;
        say(QString("%5: window %1x%2 at scale %3, up to %4 MB")
                .arg(m_window->width())
                .arg(m_window->height())
                .arg(m_window->effectiveDevicePixelRatio())
                .arg(megabytes(m_limit),
                     m_notes ? "notes" : m_far ? "strokes from far off" : m_unseen ? "strokes out of view" : "strokes"));
        m_canvas->update();
    }

private:
    static void say(const QString &line) {
        printf("%s %s\n", qPrintable(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")), qPrintable(line));
        fflush(stdout);
    }

    void frame() {
        if (!m_started || m_done)
            return;
        m_canvas->update();
        // A step every thirty frames, so each has settled before the next.
        if (++m_frames % 30 != 0)
            return;
        const SceneLoad load = m_canvas->load();
        say(QString("items=%1 triangles=%2 bytes=%3").arg(m_strokes).arg(load.triangles).arg(load.bytes));
        if (load.bytes >= m_limit || load.reduced)
            return finish("reached the limit without hearing from the kernel twice");
        const QRectF area = m_canvas->visibleArea();
        const QSizeF pitch = m_notes ? QSizeF(160, 200) : QSizeF(170, 70);
        const int columns = qMax(1, int((area.width() - 20) / pitch.width()));
        for (int i = 0; i < (m_far || m_unseen ? 40 : 2); ++i, ++m_strokes) {
            // Out of view is off to the right of the window, a window away.
            const QPointF at(area.left() + 10 + (m_strokes % columns) * pitch.width() + (m_unseen ? 2 * area.width() : 0),
                             area.top() + 10 + (m_strokes / columns) * pitch.height());
            if (!m_unseen && at.y() + pitch.height() > area.bottom())
                return finish("ran out of room in the window");
            if (m_notes) {
                // A note written full: a title and nine lines.
                Item note;
                note.kind = Item::Kind::Note;
                note.color = Item::colorNames()[m_strokes % Item::colorNames().size()];
                note.rect = QRectF(at, QSizeF(152, 192));
                note.text = QString("Note %1").arg(m_strokes) + QString("\nwritten all over").repeated(9);
                m_board->add(note);
            } else {
                m_board->add(scribble(at, m_strokes));
            }
        }
    }

    void heard() {
        while (m_kernel.canReadLine()) {
            const QString line = QString::fromUtf8(m_kernel.readLine()).trimmed();
            if (!line.contains("gpu", Qt::CaseInsensitive) && !line.contains("asahi", Qt::CaseInsensitive))
                continue;
            const SceneLoad load = m_canvas->load();
            say(QString("KERNEL at items=%1 triangles=%2 bytes=%3: %4")
                    .arg(m_strokes)
                    .arg(load.triangles)
                    .arg(load.bytes)
                    .arg(line));
            // The second growth is all there is to learn; anything about a
            // fault is a reason to stop at once.
            if (line.contains("PB growth request 1") || line.contains("fault", Qt::CaseInsensitive) ||
                line.contains("FAILED") || line.contains("recovery"))
                return finish("heard what it was listening for");
        }
    }

    void finish(const char *why) {
        if (m_done)
            return;
        m_done = true;
        say(QString("done: %1").arg(why));
        QCoreApplication::quit();
    }

    QQuickWindow *m_window;
    CanvasView *m_canvas;
    Board *m_board;
    QProcess m_kernel;
    qint64 m_limit;
    bool m_notes, m_far, m_unseen;
    int m_frames = 0, m_strokes = 0;
    bool m_started = false, m_done = false;
};

} // namespace

int main(int argc, char *argv[]) {
    // Sprawl follows the Omarchy theme itself; Qt's gtk3 platform theme would
    // only fight it.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    const QString first = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Commands open no window, but they lay out text and paint pictures,
    // which takes the toolkit: on a platform that needs no display. Bare
    // sprawl prints help, as a command line tool should; the launcher says
    // sprawl open.
    if (first != "open") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        app.setApplicationName("sprawl");
        app.setApplicationVersion(SPRAWL_VERSION);
        if (first == "--version" || first == "-v") {
            printf("sprawl %s\n", SPRAWL_VERSION);
            return 0;
        }
        if (first.isEmpty() || first == "--help" || first == "-h")
            return runCli({app.arguments().value(0), "help"});
        if (!isCliCommand(first)) {
            fprintf(stderr, "sprawl: there is no command %s\n\n%s\n", qPrintable(first), qPrintable(cliSummary()));
            return 1;
        }
        return runCli(app.arguments());
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("sprawl");
    app.setApplicationVersion(SPRAWL_VERSION);

    // Matches the window to sprawl.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("sprawl");
    app.setWindowIcon(QIcon::fromTheme("sprawl"));

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    QCommandLineParser args;
    args.setApplicationDescription("An infinite canvas for the Omarchy desktop.\n\n" + cliSummary());
    args.addHelpOption();
    args.addPositionalArgument("open", "Open the window", "open");
    args.addPositionalArgument("board", "The board to open: a name, a board's folder, or a .json file", "[board]");
    args.addOption({"new-window", "Open another window even when one is open"});
    // For looking at the app without a person at it.
    args.addOption({"screenshot", "Save a picture of the window and exit", "file"});
    args.addOption({"size", "With --screenshot or --bench: the size of the window", "WxH"});
    args.addOption({"show", "Start with a panel open: keys, boards, export; or, with --tool connector, start, route, dash, end", "panel"});
    args.addOption({"tool", "Start with a tool: pen, note, shape...", "tool"});
    args.addOption({"select", "Start with these items selected", "id,id"});
    args.addOption({"zoom", "Start at this zoom, like 0.5", "factor"});
    args.addOption({"default-theme", "Ignore the desktop's theme and show Omarchy's default"});
    args.addOption({"theme", "Show another Omarchy theme than the desktop's: its name, or its folder", "theme"});
    args.addOption({"bench", "Measure frames on a made-up board of this many items, and exit", "items"});
    args.addOption({"probe", "Measure what a frame costs the GPU, drawing no more than this, and exit", "megabytes"});
    args.addOption({"probe-with", "What --probe draws: strokes (the default), notes, far (strokes, zoomed out) or unseen", "kind"});
    args.process(app);
    const QString requested = args.positionalArguments().value(1);
    const bool screenshot = args.isSet("screenshot"), probe = args.isSet("probe");
    const bool bench = args.isSet("bench") || probe;

    // A board is one thing to look at, so launching Sprawl again brings the
    // window that is open to the front. Asking for a particular board still
    // opens it, in a window of its own.
    if (!screenshot && !bench && !args.isSet("new-window") && requested.isEmpty() && focusOpenWindow())
        return 0;

    // The plainest style, as Hype uses: every control is drawn by Sprawl's
    // own components, in the colors and states of the Omarchy theme.
    QQuickStyle::setStyle("Basic");

    // The desktop's theme, unless another was asked for: a folder is made to
    // stand in for Omarchy's own, with that theme as the current one.
    QTemporaryDir themeHome;
    QString themeFolder;
    if (args.isSet("default-theme")) {
        themeFolder = "/nonexistent";
    } else if (args.isSet("theme")) {
        const QString asked = args.value("theme");
        QString found;
        for (const QString &candidate : {asked, QDir::homePath() + "/.config/omarchy/themes/" + asked,
                                         "/usr/share/omarchy/themes/" + asked})
            if (found.isEmpty() && QFile::exists(candidate + "/colors.toml"))
                found = QFileInfo(candidate).absoluteFilePath();
        if (found.isEmpty() || !QFile::link(found, themeHome.filePath("theme"))) {
            fprintf(stderr, "sprawl: there is no theme %s\n", qPrintable(asked));
            return 1;
        }
        themeFolder = themeHome.path();
    }
    Theme theme(themeFolder);

    // The Omarchy font: "monospace" is the alias `omarchy font set` points at
    // the chosen family, at the size the theme gives the shell.
    auto adoptFont = [&theme] {
        QFont font(theme.fontFamily());
        font.setStyleHint(QFont::Monospace);
        font.setPixelSize(theme.fontSize());
        QGuiApplication::setFont(font);
    };
    adoptFont();
    QObject::connect(&theme, &Theme::changed, &app, adoptFont);

    // The benchmark's board lives in a folder of its own that goes away with
    // it; nobody's boards are touched.
    QTemporaryDir scratch;
    Library library(bench ? scratch.path() : QString());
    if (bench) {
        const QString problem =
            Library::write(library.boardFile("bench"), madeUpBoard(probe ? 0 : args.value("bench").toInt()));
        if (!problem.isEmpty()) {
            fprintf(stderr, "%s\n", qPrintable(problem));
            return 1;
        }
    }
    PortalFilePicker picker;
    Backend backend(&library, &picker, &theme);
    backend.openInitial(bench ? QString("bench") : requested);

    qmlRegisterType<CanvasView>("Sprawl", 1, 0, "CanvasView");
    qmlRegisterUncreatableType<Board>("Sprawl", 1, 0, "Board", "Boards belong to the backend");

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    QSize size(1200, 760);
    if (args.isSet("size")) {
        const QStringList parts = args.value("size").split('x');
        if (parts.size() == 2 && parts[0].toInt() > 0 && parts[1].toInt() > 0)
            size = QSize(parts[0].toInt(), parts[1].toInt());
    }
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("startHidden", screenshot);
    engine.rootContext()->setContextProperty("startSize", size);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *canvas = window ? window->findChild<CanvasView *>() : nullptr;
    if (!window || !canvas)
        return 1;
    if (args.isSet("tool"))
        canvas->setTool(args.value("tool"));
    if (args.isSet("zoom"))
        canvas->zoomBy(args.value("zoom").toDouble() / canvas->zoom(), QPointF(size.width() / 2.0, size.height() / 2.0));
    if (args.isSet("select")) {
        QVector<quint32> ids;
        for (const QString &id : args.value("select").split(',', Qt::SkipEmptyParts))
            ids.append(id.toUInt());
        canvas->select(ids);
    }
    if (args.isSet("show"))
        QMetaObject::invokeMethod(window, "openPanel", Q_ARG(QVariant, args.value("show")));

    Benchmark *benchmark = nullptr;
    Probe *prober = nullptr;
    if (probe) {
        prober = new Probe(window, canvas, backend.board(), args.value("probe").toDouble(), args.value("probe-with"));
        // Once the window has been given its place on the desktop.
        QTimer::singleShot(1500, prober, [prober] { prober->start(); });
    } else if (bench) {
        benchmark = new Benchmark(window, canvas, backend.board());
        // Once every item is on the GPU, so what is measured is the canvas
        // at rest, not the board still arriving.
        QObject::connect(canvas, &CanvasView::settled, canvas, [benchmark] { benchmark->start(); },
                         Qt::SingleShotConnection);
    }
    QTimer shutter;
    if (screenshot) {
        // The window is never shown, so the picture is one frame drawn off
        // screen, start to finish. It is taken once the board's pictures
        // have been read, so that one frame has everything in it.
        int tries = 0;
        QObject::connect(&shutter, &QTimer::timeout, &app, [&] {
            if (canvas->loadingImages() && ++tries < 400)
                return;
            shutter.stop();
            const QImage image = window->grabWindow();
            if (image.isNull())
                fprintf(stderr, "sprawl: the window could not be drawn\n");
            app.exit(!image.isNull() && image.save(args.value("screenshot")) ? 0 : 1);
        });
        shutter.start(50);
    }
    const int code = app.exec();
    delete benchmark;
    delete prober;
    return code;
}
