#include "library.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "imagestore.h"

namespace {

// How long a writer waits for another to be done with a board's file: far
// longer than reading and writing even a large board takes.
const int lockWait = 20000;

QByteArray fingerprintOf(const QByteArray &contents) {
    return QCryptographicHash::hash(contents, QCryptographicHash::Sha1);
}

Library::FileId idOf(const struct stat &status) {
    return {quint64(status.st_dev), quint64(status.st_ino)};
}

Library::Picture pictureFrom(QFile &input, const QString &name, QString *error) {
    QByteArray bytes = input.readAll();
    if (input.error() != QFileDevice::NoError) {
        if (error)
            *error = QString("Could not read %1: %2.").arg(name, input.errorString());
        return {};
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    const QString suffix = QFileInfo(name).suffix().toLower();
    const QSize size = ImageStore::validatedSize(&buffer, suffix.toLatin1());
    if (size.isEmpty()) {
        if (error)
            *error = QString("%1 is not a picture Sprawl can show.").arg(name);
        return {};
    }
    const QString asset = QString::fromLatin1(fingerprintOf(bytes).toHex().left(16)) +
                          (suffix.isEmpty() ? "" : "." + suffix);
    return {asset, std::move(bytes), size};
}

} // namespace

Library::Library(const QString &root) : m_root(root) {
    // SPRAWL_HOME puts the boards somewhere else, for trying things without
    // touching the real ones.
    if (m_root.isEmpty())
        m_root = qEnvironmentVariable("SPRAWL_HOME");
    if (m_root.isEmpty()) {
        m_root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sprawl";
        m_standard = true;
    }
}

QString Library::stateFile() const {
    // A library kept somewhere else (SPRAWL_HOME, a benchmark, a test) keeps
    // its state with it, so it never disturbs what the real one remembers.
    return m_standard ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/sprawl/state.json"
                      : m_root + "/state.json";
}

QVector<Library::Entry> Library::boards() const {
    QVector<Entry> entries;
    const QDir folder(boardsFolder());
    for (const QFileInfo &info : folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QFileInfo file(info.filePath() + "/board.json");
        if (file.isFile())
            entries.append({info.fileName(), file.lastModified()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    return entries;
}

bool Library::exists(const QString &name) const {
    // A name is the name of a folder among the boards, never a way out of them.
    if (name.isEmpty() || name.contains('/') || name == "." || name == "..")
        return false;
    return QFile::exists(boardFile(name));
}

QString Library::slug(const QString &text) {
    // Letters and digits of any script, in lower case, joined by single
    // dashes: a name that is easy to type, safe as a folder, and never
    // taken for an option or a path on a command line.
    QString name;
    for (const char32_t c : text.trimmed().toLower().toUcs4()) {
        if (QChar::isLetterOrNumber(c) || c == '_')
            name += QString::fromUcs4(&c, 1);
        else if (!name.isEmpty() && !name.endsWith('-'))
            name += '-';
        if (name.size() >= 64)
            break;
    }
    name.truncate(64);
    if (!name.isEmpty() && name.back().isHighSurrogate())
        name.chop(1);
    while (name.endsWith('-'))
        name.chop(1);
    return name;
}

QString Library::nameFor(const QString &text) {
    const QString name = slug(text);
    return name.isEmpty() ? QStringLiteral("untitled") : name;
}

QString Library::freeName(const QString &base) const {
    const QString name = nameFor(base);
    if (!QFile::exists(boardFolder(name)))
        return name;
    for (int i = 2;; ++i) {
        const QString numbered = QString("%1-%2").arg(name).arg(i);
        if (!QFile::exists(boardFolder(numbered)))
            return numbered;
    }
}

QString Library::taken(const QString &name) const {
    if (exists(name))
        return QString("There is already a board called %1.").arg(name);
    // A folder with no board in it (its file taken out by hand), or a file:
    // no board, and not to be made one.
    if (QFile::exists(boardFolder(name)))
        return QString("%1 is there already and is not a board.").arg(boardFolder(name));
    return {};
}

QString Library::create(const QString &name) {
    if (name.isEmpty() || name != slug(name))
        return QString("\"%1\" is not a name a board can have.").arg(name);
    const QString why = taken(name);
    if (!why.isEmpty())
        return why;
    const QDir boards(boardsFolder());
    if (!QDir().mkpath(boards.path()))
        return QString("Could not make the folder %1.").arg(boardFolder(name));
    if (!boards.mkdir(name)) {
        const QString why = taken(name);
        return why.isEmpty() ? QString("Could not make the folder %1.").arg(boardFolder(name)) : why;
    }
    const QString problem = write(boardFile(name), QJsonObject{{"sprawl", Board::formatVersion}, {"items", QJsonArray()}});
    if (!problem.isEmpty())
        boards.rmdir(name);
    return problem;
}

Library::FileId Library::identity(const QString &path) {
    struct stat status;
    return ::stat(QFile::encodeName(path).constData(), &status) == 0 ? idOf(status) : FileId{};
}

Library::FileId Library::folderId(const QString &folder) {
    struct stat status;
    return ::stat(QFile::encodeName(folder).constData(), &status) == 0 && S_ISDIR(status.st_mode)
               ? idOf(status) : FileId{};
}

std::unique_ptr<Library::Lock> Library::lockBoardToMove(const QString &name, FileId expected, QString &error) const {
    if (!exists(name)) {
        error = QString("There is no board called %1.").arg(name);
        return {};
    }
    const QString folder = boardFolder(name);
    if (!expected.isValid())
        expected = folderId(folder);
    const auto stillThere = [&] { return expected.isValid() && folderId(folder) == expected; };
    auto held = stillThere() ? lock(boardFile(name), &error) : nullptr;
    if (!stillThere() || (held && held->folderId() != expected)) {
        error = QString("The folder for %1 no longer holds the open board.").arg(name);
        return {};
    }
    if (held && !exists(name)) {
        error = QString("There is no board called %1.").arg(name);
        return {};
    }
    return held;
}

QString Library::rename(const QString &from, const QString &to, FileId expected) {
    if (to.isEmpty() || to != slug(to))
        return QString("\"%1\" is not a name a board can have.").arg(to);
    QString error;
    const auto held = lockBoardToMove(from, expected, error);
    if (!held)
        return error;
    const QString why = taken(to);
    if (!why.isEmpty())
        return why;
    if (!QDir().rename(boardFolder(from), boardFolder(to)))
        return QString("Could not rename %1.").arg(from);
    return {};
}

QString Library::remove(const QString &name, FileId expected) {
    QString error;
    const auto held = lockBoardToMove(name, expected, error);
    if (!held)
        return error;
    if (!QFile::moveToTrash(boardFolder(name)))
        return QString("Could not move %1 to the trash.").arg(name);
    return {};
}

QString Library::read(const QString &file, QJsonObject &root, QByteArray *fingerprint) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return QString("Could not read %1: %2.").arg(file, input.errorString());
    const QByteArray contents = input.readAll();
    if (input.error() != QFileDevice::NoError)
        return QString("Could not read %1: %2.").arg(file, input.errorString());
    if (fingerprint)
        *fingerprint = fingerprintOf(contents);
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError)
        return QString("%1 is not valid JSON: %2 (at character %3).").arg(file, error.errorString()).arg(error.offset);
    if (!document.isObject())
        return QString("%1 does not hold a board.").arg(file);
    root = document.object();
    return {};
}

QString Library::write(const QString &file, const QJsonObject &root, QByteArray *fingerprint) {
    return write(file, contentsOf(root), fingerprint);
}

QString Library::write(const QString &file, const QByteArray &contents, QByteArray *fingerprint,
                       const std::function<bool()> &still) {
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return QString("Could not make the folder for %1.").arg(file);
    QSaveFile output(file);
    if (!output.open(QIODevice::WriteOnly))
        return QString("Could not write %1: %2.").arg(file, output.errorString());
    output.write(contents);
    if (still) {
        // Writing, and above all getting it onto the disk, can take a while,
        // and a file renamed over this one meanwhile by something that takes
        // no lock would be replaced unseen: it is all done before the asking.
        if (!output.flush())
            return QString("Could not write %1: %2.").arg(file, output.errorString());
        ::fsync(output.handle());
        // Not to be replaced: what was written beside it goes.
        if (!still())
            return {};
    }
    if (!output.commit())
        return QString("Could not write %1: %2.").arg(file, output.errorString());
    if (fingerprint)
        *fingerprint = fingerprintOf(contents);
    return {};
}

QByteArray Library::contentsOf(const QJsonObject &root) {
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QByteArray Library::fingerprint(const QString &file) {
    QFile input(file);
    return input.open(QIODevice::ReadOnly) ? fingerprintOf(input.readAll()) : QByteArray();
}

Library::Lock::Lock(int directory, FileId folderId, const QString &name)
    : m_directory(directory), m_folderId(folderId),
      m_folder(QString("/proc/self/fd/%1").arg(directory)), m_lock(m_folder + "/" + name + ".lock") {}

Library::Lock::~Lock() {
    m_lock.unlock();
    ::close(m_directory);
}

std::unique_ptr<Library::Lock> Library::lock(const QString &file, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::unique_ptr<Lock>();
    };
    const QFileInfo info(file);
    // Keep the directory open so a rename cannot strand the lock in it
    // or release a different lock at the old name.
    const int directory = ::open(QFile::encodeName(info.absolutePath()).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return fail(QString("Could not open the folder for %1.").arg(file));
    struct stat status;
    if (::fstat(directory, &status) != 0) {
        ::close(directory);
        return fail(QString("Could not open the folder for %1.").arg(file));
    }
    auto held = std::unique_ptr<Lock>(new Lock(directory, idOf(status), info.fileName()));
    QLockFile &lock = held->m_lock;
    // Tried often, so that a queue of writers (an agent's commands, run side
    // by side) is soon through.
    const QDeadlineTimer deadline(lockWait);
    while (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::PermissionError)
            return fail(QString("Could not write %1: its folder cannot be written to.").arg(file));
        if (lock.error() != QLockFile::LockFailedError)
            return fail(QString("Could not write %1: %1.lock could not be made.").arg(file));
        if (deadline.hasExpired())
            return fail(QString("Could not write %1: something else has been writing it for too long.").arg(file));
        QThread::msleep(10);
    }
    return held;
}

int Library::score(const QString &name, const QString &query) {
    if (query.isEmpty())
        return 0;
    const QString text = name.toLower(), wanted = query.toLower().remove(' ');
    const int n = int(text.size()), m = int(wanted.size());
    if (m == 0)
        return 0;
    if (m > n)
        return -1;
    // The letters can often be found in a name more than one way ("set" in
    // desk-setup), so every way is weighed and the best one counts.
    // best[i] is the most that the letters so far can score with the last
    // of them at i; none is "not there".
    const int none = INT_MIN / 2;
    QVector<int> best(n, none), next(n, none);
    auto worth = [&text](int i) {
        // A letter that starts the name or a word of it is what was most
        // likely meant.
        return 10 + (i == 0 || !text[i - 1].isLetterOrNumber() ? 12 : 0);
    };
    for (int i = 0; i < n; ++i)
        if (text[i] == wanted[0])
            best[i] = worth(i) - qMin(i, 6);
    for (int j = 1; j < m; ++j) {
        next.fill(none);
        for (int i = j; i < n; ++i) {
            if (text[i] != wanted[j])
                continue;
            for (int k = j - 1; k < i; ++k) {
                if (best[k] == none)
                    continue;
                // Letters that follow on from the last one are worth more
                // than letters with others between them.
                const int step = k == i - 1 ? 15 : -qMin(i - k - 1, 6);
                next[i] = qMax(next[i], best[k] + worth(i) + step);
            }
        }
        best.swap(next);
    }
    const int total = *std::max_element(best.constBegin(), best.constEnd());
    if (total == none)
        return -1;
    // Shorter names are closer matches.
    return qMax(1, total - n / 4);
}

Library::Picture Library::preparePicture(const QString &source, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Picture{};
    };
    const QFileInfo info(source);
    if (info.exists() && !info.isFile())
        return fail(QString("%1 is not a file.").arg(source));
    // Only read a regular file. Open without waiting in case a pipe takes
    // its place, and use this one handle for the bytes to be kept.
    const int handle = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (handle < 0)
        return fail(QString("Could not read %1: %2.").arg(source, QString::fromLocal8Bit(std::strerror(errno))));
    struct stat status;
    QFile input;
    if (::fstat(handle, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(handle);
        return fail(QString("%1 is not a file.").arg(source));
    }
    if (!input.open(handle, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(handle);
        return fail(QString("Could not read %1: %2.").arg(source, input.errorString()));
    }
    return pictureFrom(input, source, error);
}

QString Library::savePicture(const QString &assetsFolder, const Picture &picture, QString *error,
                             const Lock *held) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (picture.name.isEmpty())
        return {};
    // One with this name already is the same picture, whatever it was
    // called where it came from.
    const QString folder = held ? held->m_folder + "/assets" : assetsFolder;
    const QString target = folder + "/" + picture.name;
    if (QFile::exists(target))
        return picture.name;
    if (!QDir().mkpath(folder))
        return fail(QString("Could not make the folder %1.").arg(assetsFolder));
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly) || output.write(picture.bytes) != picture.bytes.size() || !output.commit())
        return fail(QString("Could not write %1/%2.").arg(assetsFolder, picture.name));
    return picture.name;
}

QString Library::importFile(const QString &assetsFolder, const QString &source, QString *error) {
    return savePicture(assetsFolder, preparePicture(source, error), error);
}

QString Library::importAsset(const QString &assetsFolder, const QString &fromFolder, const QString &asset,
                             QString *error, const Lock *held) {
    QFile input;
    if (!ImageStore::openAsset(fromFolder, asset, input)) {
        if (error)
            *error = QString("Could not read the picture %1 in %2.").arg(asset, fromFolder);
        return {};
    }
    return savePicture(assetsFolder, pictureFrom(input, asset, error), error, held);
}

QString Library::importImage(const QString &assetsFolder, const QImage &image, QString *error, const Lock *held) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer, "PNG")) {
        if (error)
            *error = "Could not read the picture.";
        return {};
    }
    const QString name = QString::fromLatin1(fingerprintOf(bytes).toHex().left(16)) + ".png";
    return savePicture(assetsFolder, {name, bytes, image.size()}, error, held);
}

Library::Location Library::locate(const QString &nameOrPath) const {
    // Nothing named is no board, not the folder the command is run from.
    if (nameOrPath.isEmpty())
        return {};
    // A board's name is that board, whatever the folder a command is run
    // from has in it. Anything else is a path: what has a slash in it (never
    // a name), or a .json at its end, or is the name of no board.
    const bool named = exists(nameOrPath);
    if (named && !nameOrPath.endsWith(".json", Qt::CaseInsensitive))
        return {nameOrPath, boardFile(nameOrPath), assetsFolder(nameOrPath)};
    // Made whole and tidy first, so that "plans/" (as the shell completes a
    // folder) and "." have the names of their folders.
    const QFileInfo info(QDir::cleanPath(QDir::current().absoluteFilePath(nameOrPath)));
    // A folder holding a board.json, or the file itself.
    if (info.isDir() && QFile::exists(info.filePath() + "/board.json"))
        return {info.fileName(), info.absoluteFilePath() + "/board.json", info.absoluteFilePath() + "/assets"};
    if (info.isFile())
        return {info.fileName() == "board.json" ? info.absoluteDir().dirName() : info.completeBaseName(),
                info.absoluteFilePath(), info.absolutePath() + "/assets"};
    // A board in the library whose folder was given a file's name by hand is
    // had by that name where the path leads to no board.
    if (named)
        return {nameOrPath, boardFile(nameOrPath), assetsFolder(nameOrPath)};
    return {};
}
