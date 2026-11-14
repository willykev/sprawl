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

// Text is as big as its type; a file only says where it starts.
void fitText(Board &board) {
    for (quint32 id : board.order()) {
        Item item = *board.item(id);
        if (item.kind == Item::Kind::Text && TextMetrics::fit(item))
            board.setMeasuredRect(id, item.rect);
    }
}

// Reads the board named on the command line: one in the library, a board's
// folder, or a .json file. Says what is wrong when it cannot.
bool load(const Library &library, const QString &name, Library::Location &location, Board &board) {
    location = library.locate(name);
    if (!location.isValid()) {
        print(stderr, QString("There is no board %1. %2").arg(name, boardNames(library)));
        return false;
    }
    QJsonObject root;
    const QString problem = Library::read(location.file, root);
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
    fitText(board);
    return true;
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
    command.parser.addPositionalArgument("board", "A board's name, its folder, or a .json file", "<board>");
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
    command.parser.addPositionalArgument("board", "A board's name, its folder, or a .json file", "<board>");
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
        problems << QString("There is no board %1. %2").arg(name, boardNames(library));
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
    command.parser.addPositionalArgument("board", "A board's name, its folder, or a .json file", "<board>");
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
    Command command("add", "Add an item to a board. Without --x and --y it goes below everything else.");
    command.parser.addPositionalArgument("board", "A board's name, its folder, or a .json file", "<board>");
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
    const Library library;
    Library::Location location;
    Board board;
    if (!load(library, words[0], location, board))
        return 1;

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
            if (!ok)
                return fail(QString("--%1 takes a number.").arg(option));
            object.insert(key, number);
        }
    for (const char *key : {"color", "shape", "route", "start", "end", "dash"})
        if (parser.isSet(key))
            object.insert(key, parser.value(key));
    if (parser.isSet("text"))
        object.insert("text", parser.value("text").replace("\\n", "\n"));
    if (parser.isSet("bold"))
        object.insert("bold", true);

    const bool placed = parser.isSet("x") && parser.isSet("y");
    if (!placed) {
        object.insert("x", 0);
        object.insert("y", 0);
    }
    if (words[1] == "image") {
        if (!parser.isSet("file"))
            return fail("An image needs --file <picture>.");
        QImageReader reader(parser.value("file"));
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (!reader.canRead() || !size.isValid())
            return fail(QString("%1 is not a picture Sprawl can show.").arg(parser.value("file")));
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            size.transpose();
        QString problem;
        const QString asset = Library::importFile(location.assets, parser.value("file"), &problem);
        if (asset.isEmpty())
            return fail(problem);
        object.insert("asset", asset);
        QSizeF fitted = size;
        if (fitted.width() > 420 || fitted.height() > 320)
            fitted.scale(420, 320, Qt::KeepAspectRatio);
        if (!parser.isSet("width"))
            object.insert("w", qRound(fitted.width()));
        if (!parser.isSet("height"))
            object.insert("h", qRound(object.value("w").toDouble() * size.height() / size.width()));
    }
    if (words[1] == "connector" && !(parser.isSet("from") && parser.isSet("to")))
        return fail("A connector needs --from <id> and --to <id>: the ids are in sprawl show <board> --json.");
    if (words[1] == "stroke")
        return fail("Strokes are drawn with the pen, in the window.");

    Item item;
    const QString problem = Board::itemFromJson(object, item);
    if (!problem.isEmpty())
        return fail(QString("That item cannot be added: %1.").arg(problem));
    for (quint32 end : {item.from, item.to}) {
        const Item *attached = end ? board.item(end) : nullptr;
        if (end && (!attached || !Board::canAttach(*attached)))
            return fail(QString("There is no item %1 to attach to: the ids are in sprawl show <board> --json.").arg(end));
    }
    TextMetrics::fit(item);
    if (!placed && item.kind != Item::Kind::Connector) {
        // Below everything, lined up with its left edge.
        QRectF content;
        for (quint32 other : board.order())
            content |= board.item(other)->rect;
        item.rect.moveTo(content.isNull() ? QPointF(0, 0)
                                          : QPointF(std::round(content.left()), std::round(content.bottom() + 24)));
    }
    const quint32 id = board.add(item);
    const QString written = Library::write(location.file, board.toJson());
    if (!written.isEmpty())
        return fail(written);
    if (command.wantsJson())
        print(Board::itemToJson(*board.item(id)));
    else
        print(stdout, "Added " + describe(*board.item(id)));
    return 0;
}

int removeItems(const QStringList &arguments) {
    Command command("remove", "Remove items from a board, with the connectors attached to them.");
    command.parser.addPositionalArgument("board", "A board's name, its folder, or a .json file", "<board>");
    command.parser.addPositionalArgument("ids", "The ids of the items, from sprawl show", "<id>...");
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.arguments();
    if (words.size() < 2)
        return fail("Say which board and which items: sprawl remove <board> <id>...");
    const Library library;
    Library::Location location;
    Board board;
    if (!load(library, words[0], location, board))
        return 1;
    QVector<quint32> ids;
    for (const QString &word : words.mid(1)) {
        bool ok = false;
        const quint32 id = word.toUInt(&ok);
        if (!ok || !board.item(id))
            return fail(QString("There is no item %1 on %2.").arg(word, location.name));
        ids.append(id);
    }
    const int before = board.count();
    board.begin();
    for (quint32 id : std::as_const(ids))
        board.remove(id);
    board.commit();
    const QString written = Library::write(location.file, board.toJson());
    if (!written.isEmpty())
        return fail(written);
    if (command.wantsJson())
        print(QJsonObject{{"removed", before - board.count()}, {"items", board.count()}});
    else
        print(stdout, QString("Removed %1 items.").arg(before - board.count()));
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
           "  skill [install]          Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"list", "new", "show", "add", "remove", "check", "export", "skill", "help"}.contains(word);
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
    if (command == "skill")
        return skill(arguments);
    return help(arguments);
}
