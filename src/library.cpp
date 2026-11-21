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
#include <climits>
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
    if (!QDir().mkpath(boardFolder(name)))
        return QString("Could not make the folder %1.").arg(boardFolder(name));
    return write(boardFile(name), QJsonObject{{"sprawl", Board::formatVersion}, {"items", QJsonArray()}});
}

QString Library::rename(const QString &from, const QString &to) {
    if (to.isEmpty() || to != slug(to))
        return QString("\"%1\" is not a name a board can have.").arg(to);
    if (!exists(from))
        return QString("There is no board called %1.").arg(from);
    const QString why = taken(to);
    if (!why.isEmpty())
        return why;
    if (!QDir().rename(boardFolder(from), boardFolder(to)))
        return QString("Could not rename %1.").arg(from);
    return {};
}

QString Library::remove(const QString &name) {
    if (!exists(name))
        return QString("There is no board called %1.").arg(name);
    if (!QFile::moveToTrash(boardFolder(name)))
        return QString("Could not move %1 to the trash.").arg(name);
    return {};
}

QString Library::read(const QString &file, QJsonObject &root, QByteArray *fingerprint) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return QString("Could not read %1: %2.").arg(file, input.errorString());
    const QByteArray contents = input.readAll();
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

std::unique_ptr<QLockFile> Library::lock(const QString &file, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::unique_ptr<QLockFile>();
    };
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return fail(QString("Could not make the folder for %1.").arg(file));
    auto lock = std::make_unique<QLockFile>(file + ".lock");
    // Tried often, so that a queue of writers (an agent's commands, run side
    // by side) is soon through.
    const QDeadlineTimer deadline(lockWait);
    while (!lock->tryLock(0)) {
        if (lock->error() == QLockFile::PermissionError)
            return fail(QString("Could not write %1: its folder cannot be written to.").arg(file));
        if (lock->error() != QLockFile::LockFailedError)
            return fail(QString("Could not write %1: %2 could not be made.").arg(file, lock->fileName()));
        if (deadline.hasExpired())
            return fail(QString("Could not write %1: something else has been writing it for too long.").arg(file));
        QThread::msleep(10);
    }
    return lock;
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

QString Library::importFile(const QString &assetsFolder, const QString &source, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    // Only ever a picture, wherever the asking came from: a folder, a pipe
    // or a device is not opened, and a file that is something else is not
    // copied.
    const QFileInfo info(source);
    if (info.exists() && !info.isFile())
        return fail(QString("%1 is not a file.").arg(source));
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return fail(QString("Could not read %1: %2.").arg(source, input.errorString()));
    const QString suffix = info.suffix().toLower();
    if (!ImageStore::canShow(&input, suffix.toLatin1()))
        return fail(QString("%1 is not a picture Sprawl can show.").arg(source));
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&input))
        return fail(QString("Could not read %1.").arg(source));
    const QString name = QString::fromLatin1(hash.result().toHex().left(16)) + (suffix.isEmpty() ? "" : "." + suffix);
    const QString target = assetsFolder + "/" + name;
    if (QFile::exists(target))
        return name;
    if (!QDir().mkpath(assetsFolder))
        return fail(QString("Could not make the folder %1.").arg(assetsFolder));
    // The original is only ever read; the board keeps a copy of its own.
    // One put there meanwhile, whole, by whatever else was adding the same
    // picture (the window, a command beside it) is that same picture.
    input.close();
    if (!QFile::copy(source, target) && !QFile::exists(target))
        return fail(QString("Could not copy %1 into the board.").arg(source));
    return name;
}

QString Library::importAsset(const QString &assetsFolder, const QString &fromFolder, const QString &asset,
                             QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    // Only what is in that folder, as a board's own pictures are read.
    QFile input;
    if (!ImageStore::openAsset(fromFolder, asset, input))
        return fail(QString("Could not read the picture %1 in %2.").arg(asset, fromFolder));
    const QByteArray bytes = input.readAll();
    if (input.error() != QFileDevice::NoError)
        return fail(QString("Could not read the picture %1 in %2.").arg(asset, fromFolder));
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    const QString suffix = QFileInfo(asset).suffix().toLower();
    if (!ImageStore::canShow(&buffer, suffix.toLatin1()))
        return fail(QString("%1 is not a picture Sprawl can show.").arg(asset));
    // Named for what it holds, as every picture taken in is: one of the same
    // name here is the same picture, and one called the same where it came
    // from but different is not taken for it.
    const QString name =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16)) +
        (suffix.isEmpty() ? "" : "." + suffix);
    const QString target = assetsFolder + "/" + name;
    if (QFile::exists(target))
        return name;
    if (!QDir().mkpath(assetsFolder))
        return fail(QString("Could not make the folder %1.").arg(assetsFolder));
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return fail(QString("Could not write %1.").arg(target));
    return name;
}

QString Library::importImage(const QString &assetsFolder, const QImage &image, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer, "PNG"))
        return fail("Could not read the picture.");
    const QString name =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16)) + ".png";
    const QString target = assetsFolder + "/" + name;
    if (QFile::exists(target))
        return name;
    if (!QDir().mkpath(assetsFolder))
        return fail(QString("Could not make the folder %1.").arg(assetsFolder));
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return fail(QString("Could not write %1.").arg(target));
    return name;
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
