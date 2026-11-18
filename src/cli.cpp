// The command layout follows Hype's src/cli.cpp (MIT, David Heinemeier Hansson).
#include "cli.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <cstdio>

#include "board.h"
#include "exporter.h"
#include "imagestore.h"
#include "library.h"
#include "textlayout.h"
#include "theme.h"

QString printableText(const QString &text) {
    QString safe = text;
    for (QChar &c : safe)
        if (c.category() == QChar::Other_Control && c != '\n' && c != '\t')
            c = QChar(QChar::ReplacementCharacter);
    return safe;
}

QByteArray printableJson(const QByteArray &json) {
    // QJsonDocument escapes what is below a space. Delete and the controls
    // above it are written as they are, and can only be inside a string.
    const QString text = QString::fromUtf8(json);
    QString safe;
    safe.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() >= 0x7f && c.unicode() <= 0x9f)
            safe += QString("\\u%1").arg(int(c.unicode()), 4, 16, QChar('0'));
        else
            safe += c;
    }
    return safe.toUtf8();
}

namespace {

void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(printableText(text))); }
void print(const QJsonObject &object) {
    fprintf(stdout, "%s", printableJson(QJsonDocument(object).toJson()).constData());
}
void print(const QJsonArray &array) { fprintf(stdout, "%s", printableJson(QJsonDocument(array).toJson()).constData()); }
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool wantsJson() const { return parser.isSet("json"); }
    QStringList arguments() const { return parser.positionalArguments().mid(1); }
};

QString boardNames(const Library &library) {
    QStringList names;
    for (const Library::Entry &entry : library.boards())
        names << entry.name;
    return names.isEmpty() ? QString("There are no boards yet: sprawl new <name> makes one.")
                           : "The boards are: " + names.join(", ");
}

// What the help of each command that takes a board says it can be given. A
// board's name is that board whatever the folder a command is run from has
// in it, so a folder there with the same name is had only by its path.
const char *const boardArgument = "A board's name, or a path to a board's folder or .json file. A board's name means "
                                  "that board; a folder here with the same name is given as ./name";

// How many times a command that changes a board reads it again and does it
// over, when a whole board was renamed over the file as it was written back
// (by an agent, which takes no lock). Once is rare; this many times in a row
// is something writing it without end.
const int writeTries = 5;

// Reads the board named on the command line: one in the library, a board's
// folder, or a .json file. Says what is wrong when it cannot. A command that
// changes the board asks for `lock`, which it holds until it has written the
// board back: nothing else (the window, a command run beside it) writes the
// board in between, to be written over. Read again, it is read under the
// lock it holds already. `fingerprint`, when asked for, is that of the file
// as it was read.
bool load(const Library &library, const QString &name, Library::Location &location, Board &board,
          std::unique_ptr<QLockFile> *lock = nullptr, QByteArray *fingerprint = nullptr) {
    location = library.locate(name);
    if (!location.isValid()) {
        print(stderr, QString("There is no board \"%1\". %2").arg(name, boardNames(library)));
        return false;
    }
    if (lock && !*lock) {
        QString error;
        *lock = Library::lock(location.file, &error);
        if (!*lock) {
            print(stderr, error);
            return false;
        }
    }
    QJsonObject root;
    const QString problem = Library::read(location.file, root, fingerprint);
    if (!problem.isEmpty()) {
        print(stderr, problem);
        return false;
    }
    const QStringList problems = board.fromJson(root);
    if (!problems.isEmpty()) {
        print(stderr, QString("%1 is not a board Sprawl can open:").arg(location.file));
        for (const QString &line : problems)
            print(stderr, "  " + line);
        return false;
    }
    TextMetrics::fitAll(board);
    return true;
}

// Writes the board back over its file, read as `fingerprint`, while the file
// is still that one at the moment before it would be replaced (see
// Library::write()). An agent, which takes no lock, may have renamed a whole
// board over it meanwhile, or the board may have been renamed or deleted:
// then nothing is written, and `changed` says so.
QString writeBack(const QString &file, const Board &board, const QByteArray &fingerprint, bool &changed) {
    // Gone, it is not made again: whatever took it away has it now.
    changed = !QFileInfo::exists(file);
    if (changed)
        return {};
    return Library::write(file, Library::contentsOf(board.toJson()), nullptr, [&] {
        changed = Library::fingerprint(file) != fingerprint;
        return !changed;
    });
}

// Reads the board named on the command line, as load() does, has `change`
// change it, and writes it back, under the board's lock all along. Written
// over meanwhile, or taken away, the board is read and changed again, so
// that what is done is done to the board as it is. `change` says what is
// wrong when it cannot be done. False, said why, when nothing was written.
bool rewrite(const Library &library, const QString &name, Library::Location &location, Board &board,
             const std::function<QString()> &change) {
    std::unique_ptr<QLockFile> lock;
    for (int tries = 1;; ++tries) {
        QByteArray fingerprint;
        if (!load(library, name, location, board, &lock, &fingerprint))
            return false;
        bool changed = false;
        QString problem = change();
        if (problem.isEmpty())
            problem = writeBack(location.file, board, fingerprint, changed);
        if (problem.isEmpty() && changed && tries == writeTries)
            problem = QString("Could not write %1: something else keeps writing it.").arg(location.file);
        if (!problem.isEmpty()) {
            print(stderr, problem);
            return false;
        }
        if (!changed)
            return true;
    }
}

QString describe(const Item &item) {
    QString line = QString("#%1 %2").arg(item.id).arg(Board::kindName(item.kind));
    if (item.kind == Item::Kind::Shape)
        line += " " + Board::shapeName(item.shape);
    if (item.kind == Item::Kind::Connector) {
        auto end = [](quint32 id, QPointF at) {
            return id ? QString("#%1").arg(id) : QString("(%1, %2)").arg(qRound(at.x())).arg(qRound(at.y()));
        };
        line += QString(" %1 -> %2").arg(end(item.from, item.p1), end(item.to, item.p2));
    } else {
        line += QString(" at (%1, %2) %3x%4")
                    .arg(qRound(item.rect.x()))
                    .arg(qRound(item.rect.y()))
                    .arg(qRound(item.rect.width()))
                    .arg(qRound(item.rect.height()));
    }
    if (!item.color.isEmpty())
        line += " " + item.color;
    if (item.group)
        line += QString(" group %1").arg(item.group);
    if (!item.text.isEmpty()) {
        QString text = item.text;
        text.replace('\n', " / ");
        line += " \"" + (text.size() > 60 ? text.left(57) + "..." : text) + "\"";
    }
    return line;
}

// The corners windows have right now, asked of Hyprland with a short wait:
// an export should look like the board does on screen.
int currentRounding() {
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return 0;
    QProcess hyprctl;
    hyprctl.start("hyprctl", {"-j", "getoption", "decoration:rounding"});
    if (!hyprctl.waitForFinished(400))
        return 0;
    return qMax(0, QJsonDocument::fromJson(hyprctl.readAllStandardOutput()).object().value("int").toInt());
}

int list(const QStringList &arguments) {
    Command command("list", "List the boards, newest first.");
    command.json();
    command.parser.process(arguments);
    const Library library;
    QJsonArray boards;
    for (const Library::Entry &entry : library.boards()) {
        QJsonObject root;
        QVector<Item> items;
        const bool readable = Library::read(library.boardFile(entry.name), root).isEmpty() &&
                              Board::parse(root, items).isEmpty();
        boards.append(QJsonObject{{"name", entry.name},
                                  {"file", library.boardFile(entry.name)},
                                  {"items", readable ? QJsonValue(int(items.size())) : QJsonValue()},
                                  {"modified", entry.modified.toString(Qt::ISODate)}});
    }
    if (command.wantsJson()) {
        print(boards);
        return 0;
    }
    if (boards.isEmpty())
        print(stdout, "There are no boards yet: sprawl new <name> makes one.");
    for (const QJsonValue &value : std::as_const(boards)) {
        const QJsonObject board = value.toObject();
        const QString count = board.value("items").isNull() ? QString("unreadable")
                                                             : QString("%1 items").arg(board.value("items").toInt());
        print(stdout, QString("%1  %2  %3")
                          .arg(board.value("name").toString(), -28)
                          .arg(count, -12)
                          .arg(board.value("modified").toString().replace('T', ' ')));
    }
    return 0;
}

int create(const QStringList &arguments) {
    Command command("new", "Make an empty board and print where its file is.");
    command.parser.addPositionalArgument("name", "What to call it, like weekend-trip", "<name>");
    command.json();
    command.parser.process(arguments);
    const QString asked = command.arguments().value(0);
    const QString name = Library::slug(asked);
    if (name.isEmpty())
        return fail("Give the board a name: sprawl new weekend-trip");
    Library library;
    const QString problem = library.create(name);
    if (!problem.isEmpty())
        return fail(problem);
    if (command.wantsJson())
        print(QJsonObject{{"name", name}, {"file", library.boardFile(name)}});
    else
        print(stdout, library.boardFile(name));
    return 0;
}

int show(const QStringList &arguments) {
    Command command("show", "Describe a board: what is on it, and where.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.json();
    command.parser.process(arguments);
    if (command.arguments().isEmpty())
        return fail("Say which board: sprawl show <board>");
    const Library library;
    Library::Location location;
    Board board;
    if (!load(library, command.arguments().first(), location, board))
        return 1;
    if (command.wantsJson()) {
        QJsonObject root = board.toJson();
        root.insert("name", location.name);
        root.insert("file", location.file);
        print(root);
        return 0;
    }
    print(stdout, QString("%1: %2 items, back to front (%3)").arg(location.name).arg(board.count()).arg(location.file));
    for (quint32 id : board.order())
        print(stdout, describe(*board.item(id)));
    return 0;
}

int check(const QStringList &arguments) {
    Command command("check", "Check that a board is one Sprawl can open, and say what is wrong when it is not.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.json();
    command.parser.process(arguments);
    if (command.arguments().isEmpty())
        return fail("Say which board: sprawl check <board>");
    const Library library;
    const QString name = command.arguments().first();
    const Library::Location location = library.locate(name);
    QStringList problems;
    QVector<Item> items;
    if (!location.isValid()) {
        problems << QString("There is no board \"%1\". %2").arg(name, boardNames(library));
    } else {
        QJsonObject root;
        const QString problem = Library::read(location.file, root);
        if (!problem.isEmpty())
            problems << problem;
        else
            problems = Board::parse(root, items);
        // A picture the board shows has to be in its assets folder.
        if (problems.isEmpty())
            for (const Item &item : std::as_const(items))
                if (item.kind == Item::Kind::Image && !QFile::exists(location.assets + "/" + item.asset))
                    problems << QString("The picture %1 is missing from %2.").arg(item.asset, location.assets);
    }
    if (command.wantsJson()) {
        print(QJsonObject{{"ok", problems.isEmpty()},
                          {"file", location.file},
                          {"items", int(items.size())},
                          {"problems", QJsonArray::fromStringList(problems)}});
        return problems.isEmpty() ? 0 : 1;
    }
    if (problems.isEmpty()) {
        print(stdout, QString("%1 is fine: %2 items.").arg(location.file).arg(items.size()));
        return 0;
    }
    for (const QString &problem : std::as_const(problems))
        print(stderr, problem);
    return 1;
}

int exportBoard(const QStringList &arguments) {
    Command command("export", "Write a board as a picture or a document, with no display.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.parser.addOption({{"f", "format"}, "png, svg or pdf (the default is the output's, or png)", "format"});
    command.parser.addOption({{"o", "output"}, "The file to write (the default is <board>.<format> here)", "file"});
    command.parser.addOption({"scale", "PNG: pixels per unit of the board (the default is 2)", "number", "2"});
    command.parser.addOption({"transparent", "Leave out the canvas color behind the board"});
    command.parser.addOption({"margin", "Space around the board (the default is 32)", "units", "32"});
    command.json();
    command.parser.process(arguments);
    if (command.arguments().isEmpty())
        return fail("Say which board: sprawl export <board>");
    const Library library;
    Library::Location location;
    Board board;
    if (!load(library, command.arguments().first(), location, board))
        return 1;

    Exporter::Options options;
    QString output = command.parser.value("output");
    options.format = command.parser.value("format").toLower();
    if (options.format.isEmpty())
        options.format = Exporter::formats().contains(QFileInfo(output).suffix().toLower())
                             ? QFileInfo(output).suffix().toLower()
                             : QString("png");
    if (!Exporter::formats().contains(options.format))
        return fail(QString("Sprawl exports %1, not %2.").arg(Exporter::formats().join(", "), options.format));
    if (output.isEmpty())
        output = location.name + "." + options.format;
    bool scaleOk = false, marginOk = false;
    options.scale = command.parser.value("scale").toDouble(&scaleOk);
    options.margin = command.parser.value("margin").toDouble(&marginOk);
    if (!scaleOk || options.scale <= 0)
        return fail("--scale takes a number above 0, like 2.");
    if (!marginOk || options.margin < 0)
        return fail("--margin takes a number, like 32.");
    options.background = !command.parser.isSet("transparent");
    options.radius = currentRounding();

    // In the colors of the theme that is on, as the window would show it.
    const Theme theme;
    const QString problem = Exporter::write(output, board, {}, theme.palette(), location.assets, options);
    if (!problem.isEmpty())
        return fail(problem);
    const QString written = QFileInfo(output).absoluteFilePath();
    if (command.wantsJson())
        print(QJsonObject{{"file", written}, {"format", options.format}, {"items", board.count()}});
    else
        print(stdout, written);
    return 0;
}

int add(const QStringList &arguments) {
    Command command("add", "Add an item to a board. Without --y it goes below everything else, and without --x at "
                           "its left edge.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.parser.addPositionalArgument("type", "note, text, shape, frame, connector or image", "<type>");
    command.parser.addOption({"text", "What it says (\\n starts a new line; a note's first line is its title)", "text"});
    command.parser.addOption({"x", "Where its left edge is", "number"});
    command.parser.addOption({"y", "Where its top edge is", "number"});
    // Not --w and --h: -h is help.
    command.parser.addOption({"width", "How wide it is", "number"});
    command.parser.addOption({"height", "How tall it is", "number"});
    command.parser.addOption({"color", "yellow, orange, red, magenta, blue, cyan or green", "name"});
    command.parser.addOption({"shape", "Shape: " + Board::shapes().join(", "), "name"});
    command.parser.addOption({"size", "Text: the size of the type (12 is body, 16 a heading, 28 a title)", "number"});
    command.parser.addOption({"bold", "Text: bold"});
    command.parser.addOption({"from", "Connector: the id of the item it starts at", "id"});
    command.parser.addOption({"to", "Connector: the id of the item it points to", "id"});
    command.parser.addOption({"route", "Connector: " + Board::routes().join(", "), "name"});
    command.parser.addOption({"start", "Connector: what is at its start: " + Board::heads().join(", "), "name"});
    command.parser.addOption({"end", "Connector: what is at its end (an arrow, unless said)", "name"});
    command.parser.addOption({"dash", "Connector: " + Board::dashes().join(", "), "name"});
    command.parser.addOption({"file", "Image: the picture to add (it is copied into the board)", "file"});
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.arguments();
    if (words.size() < 2)
        return fail("Say which board and what to add: sprawl add <board> note --text \"Call Sam\"");

    // The item is described the way a board file describes it, so it is
    // checked by the same rules.
    const QCommandLineParser &parser = command.parser;
    QJsonObject object{{"type", words[1]}};
    // Each option, and the key a board file knows it by.
    const QList<QPair<QString, QString>> numbers{{"x", "x"}, {"y", "y"}, {"width", "w"}, {"height", "h"},
                                                 {"size", "size"}, {"from", "from"}, {"to", "to"}};
    for (const auto &[option, key] : numbers)
        if (parser.isSet(option)) {
            bool ok = false;
            const double number = parser.value(option).toDouble(&ok);
            // "nan" and "inf" are read as numbers, but no file can say them.
            if (!ok || !std::isfinite(number))
                return fail(QString("--%1 takes a number.").arg(option));
            // A file may say a box is less than a unit across or down, from
            // before boxes were kept to one, and it is read as a unit. Asked
            // for here, it is a slip.
            if ((key == "w" || key == "h") && number < 1)
                return fail(QString("--%1 takes a number of 1 or more, like 200.").arg(option));
            object.insert(key, number);
        }
    for (const char *key : {"color", "shape", "route", "start", "end", "dash"})
        if (parser.isSet(key))
            object.insert(key, parser.value(key));
    if (parser.isSet("text"))
        object.insert("text", parser.value("text").replace("\\n", "\n"));
    if (parser.isSet("bold"))
        object.insert("bold", true);

    // A file says where every item is. Where --x or --y does not, it is
    // decided once the board is read (see below).
    for (const char *key : {"x", "y"})
        if (!parser.isSet(key))
            object.insert(key, 0);
    if (words[1] == "image") {
        if (!parser.isSet("file"))
            return fail("An image needs --file <picture>.");
        // Only ever a picture: a folder, a pipe or a device is not opened even
        // to be looked at (see Library::importFile()), and is refused before
        // the board is read, so a pipe that never sends anything holds up
        // nothing else that writes the board.
        const QFileInfo info(parser.value("file"));
        if (info.exists() && !info.isFile())
            return fail(QString("%1 is not a file.").arg(parser.value("file")));
        QImageReader reader(parser.value("file"));
        reader.setAutoTransform(true);
        QSize size = reader.size();
        // A picture of no width or height, as an SVG can say it is, has
        // nothing to show and no proportions to keep.
        if (!reader.canRead() || size.isEmpty())
            return fail(QString("%1 is not a picture Sprawl can show.").arg(parser.value("file")));
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            size.transpose();
        // It starts at a size to arrange and to see: neither a screenful nor
        // a sliver. A width asked for takes the height along with it, in
        // proportion, and a unit at least.
        QSizeF start = Defaults::pictureSize(size);
        if (parser.isSet("width"))
            start.setHeight(qMax(1.0, std::round(object.value("w").toDouble() * size.height() / size.width())));
        else
            object.insert("w", start.width());
        if (!parser.isSet("height"))
            object.insert("h", start.height());
        // The picture is copied into the board last, once nothing can refuse
        // it, so that one refused leaves nothing behind. A copy is never
        // taken back, for other items may show it already. Until it is made,
        // the picture goes by a name like the one it will be kept under.
        object.insert("asset", "picture.png");
    }
    if (words[1] == "connector" && !(parser.isSet("from") && parser.isSet("to")))
        return fail("A connector needs --from <id> and --to <id>: the ids are in sprawl show <board> --json.");
    // A connector is where the items it joins are, and as big: where else it
    // was asked to be would be left out unsaid.
    for (const char *option : {"x", "y", "width", "height"})
        if (words[1] == "connector" && parser.isSet(option))
            return fail(QString("--%1 is not for a connector, which goes from one item to another: place those "
                                "items instead.")
                            .arg(option));
    if (words[1] == "stroke")
        return fail("Strokes are drawn with the pen, in the window.");

    Item item;
    const QString problem = Board::itemFromJson(object, item);
    if (!problem.isEmpty())
        return fail(QString("That item cannot be added: %1.").arg(problem));

    // Its corner is where --x and --y say, even once it has grown to hold its
    // text (a shape grows about its middle). Where either does not say, it
    // is where it would go with neither: below everything, lined up with its
    // left edge.
    const QPointF asked = item.rect.topLeft();
    TextMetrics::fit(item);
    // Only now is the board read, the command line and the picture checked
    // already: it is held from here until the item is written to it (see
    // load()), and nothing else that writes it waits on more than that.
    const Library library;
    Library::Location location;
    Board board;
    quint32 id = 0;
    const bool added = rewrite(library, words[0], location, board, [&] {
        for (quint32 end : {item.from, item.to}) {
            const Item *attached = end ? board.item(end) : nullptr;
            if (end && (!attached || !Board::canAttach(*attached)))
                return QString("There is no item %1 to attach to: the ids are in sprawl show <board> --json.").arg(end);
        }
        if (item.kind != Item::Kind::Connector) {
            QRectF content;
            for (quint32 other : board.order())
                content |= board.item(other)->rect;
            const QPointF below = content.isNull()
                                      ? QPointF(0, 0)
                                      : QPointF(std::round(content.left()), std::round(content.bottom() + 24));
            item.rect.moveTo(parser.isSet("x") ? asked.x() : below.x(), parser.isSet("y") ? asked.y() : below.y());
            // Below everything on a board that reaches as far down as a board
            // can, it would be further out than a file may say anything is,
            // and the board could not be read back: where it is put is
            // checked by the rules a board is read by, as what the command
            // line said was.
            Item placed;
            if (!Board::itemFromJson(Board::itemToJson(item), placed).isEmpty())
                return QString("There is no room for it below everything else: place it with --x and --y.");
        }
        if (item.kind == Item::Kind::Image) {
            QString error;
            item.asset = Library::importFile(location.assets, parser.value("file"), &error);
            if (item.asset.isEmpty())
                return error;
        }
        id = board.add(item);
        return QString();
    });
    if (!added)
        return 1;
    if (command.wantsJson())
        print(Board::itemToJson(*board.item(id)));
    else
        print(stdout, "Added " + describe(*board.item(id)));
    return 0;
}

int removeItems(const QStringList &arguments) {
    Command command("remove", "Remove items from a board, with the connectors attached to them.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.parser.addPositionalArgument("ids", "The ids of the items, from sprawl show", "<id>...");
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.arguments();
    if (words.size() < 2)
        return fail("Say which board and which items: sprawl remove <board> <id>...");
    const Library library;
    Library::Location location;
    Board board;
    int before = 0;
    const bool removed = rewrite(library, words[0], location, board, [&] {
        QVector<quint32> ids;
        for (const QString &word : words.mid(1)) {
            bool ok = false;
            const quint32 id = word.toUInt(&ok);
            if (!ok || !board.item(id))
                return QString("There is no item %1 on %2.").arg(word, location.name);
            ids.append(id);
        }
        before = board.count();
        board.begin();
        for (quint32 id : std::as_const(ids))
            board.remove(id);
        board.commit();
        return QString();
    });
    if (!removed)
        return 1;
    if (command.wantsJson())
        print(QJsonObject{{"removed", before - board.count()}, {"items", board.count()}});
    else
        print(stdout, QString("Removed %1 items.").arg(before - board.count()));
    return 0;
}

// A board's folder keeps every picture that was ever put on the board, so
// that one taken off comes back with an undo, or is there for another
// window that still shows it. A copy made to give to someone has only the
// pictures the board shows.
int copyBoard(const QStringList &arguments) {
    Command command("copy", "Copy a board to a new folder, with only the pictures that are on it.");
    command.parser.addPositionalArgument("board", boardArgument, "<board>");
    command.parser.addPositionalArgument("folder", "Where to put the copy: a folder that is not there yet",
                                         "<folder>");
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.arguments();
    if (words.size() != 2)
        return fail("Say which board and where to put the copy: sprawl copy <board> <folder>");
    const Library library;
    Library::Location location;
    Board board;
    if (!load(library, words[0], location, board))
        return 1;
    const QString target = QDir::cleanPath(QDir::current().absoluteFilePath(words[1]));
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink())
        return fail(QString("%1 is there already: give the copy a folder that is not.").arg(target));

    // What the board shows, each picture once, read only from its own
    // folder as the window reads it (see ImageStore::openAsset()).
    QStringList pictures, missing;
    for (quint32 id : board.order())
        if (const Item *item = board.item(id); item->kind == Item::Kind::Image && !pictures.contains(item->asset))
            pictures << item->asset;
    if (!QDir().mkpath(target))
        return fail(QString("Could not make the folder %1.").arg(target));
    // The folder was made here, and goes again when the copy cannot be had
    // whole.
    auto abandon = [&target](const QString &message) {
        QDir(target).removeRecursively();
        return fail(message);
    };
    int copied = 0;
    for (const QString &asset : std::as_const(pictures)) {
        QFile input;
        if (!ImageStore::openAsset(location.assets, asset, input)) {
            missing << asset;
            continue;
        }
        const QByteArray bytes = input.readAll();
        QSaveFile output(target + "/assets/" + asset);
        if (input.error() != QFileDevice::NoError || !QDir().mkpath(target + "/assets") ||
            !output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
            return abandon(QString("Could not copy the picture %1 to %2.").arg(asset, target));
        ++copied;
    }
    const QString problem = Library::write(target + "/board.json", board.toJson());
    if (!problem.isEmpty())
        return abandon(problem);
    // What the folder holds that the board no longer shows.
    int leftOut = 0;
    for (const QString &file : QDir(location.assets).entryList(QDir::Files | QDir::Hidden | QDir::System))
        leftOut += !pictures.contains(file);

    if (command.wantsJson()) {
        print(QJsonObject{{"folder", target},
                          {"file", target + "/board.json"},
                          {"pictures", copied},
                          {"leftOut", leftOut},
                          {"missing", QJsonArray::fromStringList(missing)}});
        return 0;
    }
    for (const QString &asset : std::as_const(missing))
        print(stderr, QString("The picture %1 is missing from %2, so the copy does not have it either.")
                          .arg(asset, location.assets));
    print(stdout, leftOut ? QString("Copied %1 to %2, leaving out %3 files that are no longer on it.")
                                .arg(location.name, target)
                                .arg(leftOut)
                          : QString("Copied %1 to %2.").arg(location.name, target));
    return 0;
}

// As Hype does: one shared copy, linked into Claude Code, found directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/sprawl and link into ~/.claude/skills",
                                         "[install]");
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return fail("The skill is missing from this build.");
    const QByteArray text = bundled.readAll();
    const QString action = command.arguments().value(0);
    if (action.isEmpty()) {
        fputs(text.constData(), stdout);
        return 0;
    }
    if (action != "install")
        return fail("Use sprawl skill to print the skill, or sprawl skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/sprawl");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return 0;
    const QString link = home.filePath(".claude/skills/sprawl");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/sprawl", link))
        return fail("Could not link " + link);
    print(stdout, "Linked " + link);
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: sprawl <command> [options]\n\n" + cliSummary());
    return 0;
}

} // namespace

QString cliSummary() {
    return "Window:\n"
           "  open [board]             Open the window, on a board by name or path\n\n"
           "Commands that need no display (sprawl help <command> for options):\n"
           "  list                     List the boards\n"
           "  new <name>               Make an empty board and print where its file is\n"
           "  show <board>             Describe a board: what is on it, and where\n"
           "  add <board> <type>       Add an item: sprawl add plan note --text \"Call Sam\" --color blue\n"
           "  remove <board> <id>...   Remove items, with the connectors attached to them\n"
           "  check <board>            Check that a board is one Sprawl can open\n"
           "  export <board>           Write a board as a png, svg or pdf\n"
           "  copy <board> <folder>    Copy a board, with only the pictures that are on it\n"
           "  skill [install]          Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"list", "new", "show", "add", "remove", "check", "export", "copy", "skill", "help"}.contains(
        word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    if (command == "list")
        return list(arguments);
    if (command == "new")
        return create(arguments);
    if (command == "show")
        return show(arguments);
    if (command == "add")
        return add(arguments);
    if (command == "remove")
        return removeItems(arguments);
    if (command == "check")
        return check(arguments);
    if (command == "export")
        return exportBoard(arguments);
    if (command == "copy")
        return copyBoard(arguments);
    if (command == "skill")
        return skill(arguments);
    return help(arguments);
}
