#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QMimeData>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <sys/stat.h>
#include <utility>

#include "exporter.h"
#include "filepicker.h"
#include "textlayout.h"
#include "theme.h"

namespace {

// How long after the last change a board is written. Long enough that
// typing, or a drag, is written when it pauses and not at every key or step
// of the mouse; short enough that nothing is ever lost.
const int saveDelay = 400;
// However long changes go on without a pause, steady typing or a drag kept
// moving, they and what came before them are written at least this often.
const int saveDeadline = 2000;
// How many times the window, letting go of a board, takes it in and writes
// it again when a whole file was renamed over it while it was being written
// (by an agent, which takes no lock). Once is rare; this many times in a row
// is something writing it without end.
const int flushTries = 5;

QString homeRelative(const QString &path) {
    const QString home = QDir::homePath();
    return path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path;
}

// Why a file cannot be read as a board: what reading it came to, `problem`,
// or the first few things wrong with what it holds, `problems`.
QString unreadableBecause(const QString &problem, const QStringList &problems) {
    if (!problem.isEmpty())
        return problem;
    if (problems.size() <= 3)
        return problems.join(" ");
    return problems.mid(0, 3).join(" ") + Backend::tr(" And %1 more.").arg(problems.size() - 3);
}

// Whether an export written to `path` would take the place of what is there.
// It is written beside the file and renamed over it (see Exporter::write()),
// which is never done to a folder, to a file it may not change, or to one in
// a folder it may not write in: of those it says it cannot write them, and
// nothing is lost. Through a link, the file replaced is the one the link
// leads to, and it is beside that one that the export is written.
bool wouldReplace(const QString &path) {
    const QFileInfo there(path);
    if (!there.exists() || there.isDir() || !there.isWritable())
        return false;
    return QFileInfo(QFileInfo(there.canonicalFilePath()).absolutePath()).isWritable();
}

} // namespace

Backend::Backend(Library *library, FilePicker *picker, Theme *theme, QObject *parent)
    : QObject(parent), m_library(library), m_picker(picker), m_theme(theme) {
    m_pool.setMaxThreadCount(1);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(saveDelay);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);
    m_saveDeadline.setSingleShot(true);
    m_saveDeadline.setInterval(saveDeadline);
    connect(&m_saveDeadline, &QTimer::timeout, this, &Backend::save);
    connect(&m_board, &Board::modified, this, &Backend::scheduleSave);
    // An export waiting on its dialog keeps the items it was asked for.
    connect(&m_board, &Board::renumbered, this, [this](quint32 from, quint32 to) {
        std::replace(m_exportIds.begin(), m_exportIds.end(), from, to);
    });

    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(5000);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] { setNotice({}); });

    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(1500);
    connect(&m_stateTimer, &QTimer::timeout, this, &Backend::writeState);

    // A board edited from outside (by `sprawl` commands' users: agents and
    // scripts) shows up in the window as it is written.
    m_watchTimer.setSingleShot(true);
    m_watchTimer.setInterval(120);
    connect(&m_watchTimer, &QTimer::timeout, this, &Backend::fileChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_watchTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_watchTimer.start(); });

    if (m_picker) {
        connect(m_picker, &FilePicker::imagesSelected, this, &Backend::addFiles);
        connect(m_picker, &FilePicker::failed, this, &Backend::setProblem);
        connect(m_picker, &FilePicker::exportSelected, this, [this](const QUrl &url, const QString &format) {
            if (!url.isLocalFile()) {
                setProblem(tr("Sprawl exports to files on this computer, and %1 is not one.").arg(url.toDisplayString()));
                return;
            }
            // The dialog leaves the window free to be used until it answers:
            // what it answers for is the board it was asked for, and once
            // another has been opened, nothing is written.
            if (!m_exportAsked) {
                setProblem(tr("Nothing was exported: another board was opened before the export was given a name."));
                return;
            }
            QString path = url.toLocalFile();
            if (!path.endsWith("." + format, Qt::CaseInsensitive)) {
                path += "." + format;
                // A name the dialog never asked about: a file that has it
                // already is written over only once that is agreed to. What
                // the export could not write over, or when there is nothing
                // to export, is not asked about.
                if (wouldReplace(path) && !Exporter::area(m_board, m_exportIds, 0).isEmpty()) {
                    const QFileInfo there(path);
                    m_replacing = {path, format};
                    emit replaceAsked(there.fileName(), homeRelative(there.absolutePath()));
                    return;
                }
            }
            writeExport(path, format);
        });
    }
    readState();
}

Backend::~Backend() {
    // Whatever is not on disk yet goes there, an edit still under way
    // included: the window was showing it.
    flush();
    writeState();
}

// --- Boards -----------------------------------------------------------------------

void Backend::openInitial(const QString &argument) {
    if (argument.isEmpty()) {
        openLast();
        return;
    }
    const Library::Location location = m_library->locate(argument);
    if (location.isValid()) {
        open(location);
        return;
    }
    // Asked for by a name that is free: that is a new board. When it cannot
    // be made, what is said is why. What was asked for is quoted, so that it
    // is never read as part of the sentence: it can be anything, ".." even.
    QString missing = tr("There is no board \"%1\".").arg(argument);
    if (Library::slug(argument) == argument) {
        if (newBoard(argument))
            return;
        missing = m_problem;
    }
    // Another board opens in its place, and opening a board puts away what
    // was said before it: what became of the one asked for is said once it
    // is open, before whatever is said of it.
    openLast();
    setProblem(m_problem.isEmpty() ? missing : missing + " " + m_problem);
}

void Backend::openLast() {
    if (m_library->exists(m_lastBoard) && openBoard(m_lastBoard))
        return;
    const auto boards = m_library->boards();
    if (!boards.isEmpty() && openBoard(boards.first().name))
        return;

    // The very first time: a board that shows what a board can hold.
    const QString name = "welcome";
    QFile bundled(":/welcome.json");
    if (bundled.open(QIODevice::ReadOnly)) {
        const QString problem =
            Library::write(m_library->boardFile(name), QJsonDocument::fromJson(bundled.readAll()).object());
        if (problem.isEmpty() && openBoard(name))
            return;
    }
    if (newBoard("untitled"))
        return;
    // Not even that could be made, for what was just said: a library that
    // cannot be written to.
    openNone(m_problem);
}

bool Backend::open(const Library::Location &location) {
    // Whatever was open is safely on disk before it is let go of.
    if (m_location.isValid() && !flush())
        return false;
    m_saveTimer.stop();
    m_saveDeadline.stop();

    // Watched from before it is read, so that nothing written to it after
    // that goes unseen. An export asked of the board before is not of this
    // one.
    m_location = location;
    m_folderId = folderIdOf(boardFolder());
    m_exportAsked = false;
    watch();
    QJsonObject root;
    QByteArray fingerprint;
    const QString problem = Library::read(location.file, root, &fingerprint);
    QStringList problems;
    bool numbered = false;
    if (problem.isEmpty())
        problems = m_board.fromJson(root, &numbered);
    m_unreadable.clear();
    if (!problem.isEmpty() || !problems.isEmpty()) {
        // Say what is wrong, show an empty board, and never save over the
        // file that could not be read.
        setReadOnly(cannotOpen(unreadableBecause(problem, problems), Board::newer(root)));
        m_board.clear();
    } else {
        setReadOnly({});
        if (!m_problem.isEmpty())
            dismissProblem();
    }
    m_savedRevision = m_board.revision();
    // Items the file holds with no id are written back soon, with the ids
    // they have here (see takeIn()).
    m_idsToWrite = numbered;
    m_base = m_board.items();
    // A file that could not be read is not one the board was read from, but
    // one found it cannot be read: as it is, there is nothing more to say.
    m_baseFingerprint = m_readOnly ? QByteArray() : fingerprint;
    m_unreadableFingerprint = m_readOnly ? fingerprint : QByteArray();
    // A board that could not be read says so for as long as it is open:
    // nothing of it is ever saved.
    if (m_readOnly)
        setSaveState("readonly");
    else
        settle();

    if (m_library->exists(location.name) && m_library->boardFile(location.name) == location.file) {
        m_lastBoard = location.name;
        m_stateTimer.start();
    }
    emit boardChanged();
    emit boardOpened();
    return true;
}

QString Backend::cannotOpen(const QString &why, bool newer) const {
    // Nothing is wrong with a file from a newer Sprawl: that is what opens it.
    if (newer)
        return tr("Could not open %1. %2 Nothing will be saved over it. Update Sprawl to open it, or open another "
                  "board with Ctrl+O.")
            .arg(m_location.name, why);
    // Which file to fix is said once: by what is wrong with it, or here.
    if (why.contains(m_location.file))
        return tr("Could not open %1. %2 Nothing will be saved over it. Fix the file and Sprawl opens it by itself, "
                  "or open another board with Ctrl+O.")
            .arg(m_location.name, why);
    return tr("Could not open %1. %2 Nothing will be saved over it. Fix %3 and Sprawl opens it by itself, or open "
              "another board with Ctrl+O.")
        .arg(m_location.name, why, homeRelative(m_location.file));
}

void Backend::openNone(const QString &why) {
    m_saveTimer.stop();
    m_saveDeadline.stop();
    m_location = {};
    m_folderId = {};
    m_exportAsked = false;
    watch();
    // Whatever the window had goes: nothing done to it could be kept.
    m_board.clear();
    m_unreadable.clear();
    m_unreadableFingerprint.clear();
    m_base.clear();
    m_baseFingerprint.clear();
    m_savedRevision = m_board.revision();
    m_idsToWrite = false;
    setReadOnly(tr("%1 There is no board to keep anything in, so nothing can be changed here: open one with "
                   "Ctrl+O, or make one with Ctrl+N.")
                    .arg(why));
    setSaveState("readonly");
    emit boardChanged();
    emit boardOpened();
}

Backend::FolderId Backend::folderIdOf(const QString &folder) {
    struct stat status;
    if (folder.isEmpty() || ::stat(QFile::encodeName(folder).constData(), &status) != 0 || !S_ISDIR(status.st_mode))
        return {};
    return {quint64(status.st_dev), quint64(status.st_ino)};
}

QString Backend::boardFolder() const {
    const QString file = QStringLiteral("/board.json");
    if (!m_location.file.endsWith(file))
        return {};
    const QString folder = m_location.file.chopped(file.size());
    return m_location.assets == folder + "/assets" ? folder : QString();
}

bool Backend::followMove() {
    const QString folder = boardFolder();
    if (folder.isEmpty() || !m_folderId.isValid() || QFileInfo::exists(folder))
        return false;
    // Renamed, it is the same folder, beside where it was and called
    // something else.
    const QString parent = folder.left(folder.lastIndexOf('/'));
    for (const QString &name : QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString there = parent + "/" + name;
        if (folderIdOf(there) == m_folderId && QFileInfo(there + "/board.json").isFile()) {
            const QString was = m_location.name;
            moveTo({name, there + "/board.json", there + "/assets"});
            setNotice(tr("%1 was renamed %2 elsewhere, and is saved as %2").arg(was, name));
            return true;
        }
    }
    return false;
}

void Backend::moveTo(const Library::Location &location) {
    // The view it was left at goes with it.
    if (m_views.contains(viewKey()))
        m_views.insert(location.file, m_views.take(viewKey()));
    m_location = location;
    if (m_library->exists(location.name) && m_library->boardFile(location.name) == location.file)
        m_lastBoard = location.name;
    watch();
    // What is said of a board that could not be read names it, and its file:
    // read again where it is now, it is said anew.
    if (m_readOnly) {
        m_unreadableFingerprint.clear();
        fileChanged();
    }
    writeState();
    emit boardChanged();
}

bool Backend::openBoard(const QString &name) {
    if (name == m_location.name && m_library->boardFile(name) == m_location.file)
        return true;
    if (!m_library->exists(name)) {
        setProblem(tr("There is no board \"%1\".").arg(name));
        return false;
    }
    return open({name, m_library->boardFile(name), m_library->assetsFolder(name)});
}

bool Backend::newBoard(const QString &name) {
    const QString free = m_library->freeName(name);
    const QString problem = m_library->create(free);
    if (!problem.isEmpty()) {
        setProblem(problem);
        return false;
    }
    if (!openBoard(free))
        return false;
    // Numbered for something that is not a board, which no list of the
    // boards shows: the board is not called what was asked for, and why is
    // said once it is open (opening it put away what was said before).
    const QString asked = Library::nameFor(name);
    if (free != asked && !m_library->exists(asked))
        setProblem(tr("%1 is there already and is not a board, so the new board is %2.")
                       .arg(homeRelative(m_library->boardFolder(asked)), free));
    return true;
}

bool Backend::renameBoard(const QString &name) {
    const QString tidy = Library::slug(name);
    if (tidy.isEmpty() || tidy == m_location.name)
        return tidy == m_location.name;
    if (!m_library->exists(m_location.name) || m_library->boardFile(m_location.name) != m_location.file) {
        setProblem(tr("Only boards in the library can be renamed here."));
        return false;
    }
    if (!flush())
        return false;
    const QString problem = m_library->rename(m_location.name, tidy);
    if (!problem.isEmpty()) {
        setProblem(problem);
        return false;
    }
    moveTo({tidy, m_library->boardFile(tidy), m_library->assetsFolder(tidy)});
    return true;
}

bool Backend::deleteBoard() {
    if (!m_library->exists(m_location.name) || m_library->boardFile(m_location.name) != m_location.file) {
        setProblem(tr("Only boards in the library can be deleted here."));
        return false;
    }
    // What was done here goes into the trash with it: what is had back from
    // there is the board as it was left. Not written, it is not moved.
    if (!flush())
        return false;
    const QString gone = m_location.name;
    const QString problem = m_library->remove(gone);
    if (!problem.isEmpty()) {
        setProblem(problem);
        return false;
    }
    m_views.remove(viewKey());
    // Nothing to flush: the board that was open is in the trash.
    m_location = {};
    m_savedRevision = m_board.revision();
    // In its place the newest board, or a new one; or, when neither can be
    // had, for what was just said, none.
    const auto boards = m_library->boards();
    if (!(boards.isEmpty() ? newBoard("untitled") : openBoard(boards.first().name)))
        openNone(m_problem);
    setNotice(tr("Moved %1 to the trash").arg(gone));
    return true;
}

QVariantList Backend::findBoards(const QString &query) const {
    struct Match {
        QString name;
        int score;
    };
    QVector<Match> matches;
    for (const Library::Entry &entry : m_library->boards()) {
        const int score = Library::score(entry.name, query);
        if (score >= 0)
            matches.append({entry.name, score});
    }
    if (query.trimmed().isEmpty()) {
        // The open board first, then the rest as they were last changed.
        std::stable_sort(matches.begin(), matches.end(), [this](const Match &a, const Match &b) {
            return (a.name == m_location.name) > (b.name == m_location.name);
        });
    } else {
        std::stable_sort(matches.begin(), matches.end(),
                         [](const Match &a, const Match &b) { return a.score > b.score; });
    }
    QVariantList found;
    for (const Match &match : std::as_const(matches))
        found.append(QVariantMap{{"name", match.name}, {"current", match.name == m_location.name}});
    return found;
}

// --- Saving -----------------------------------------------------------------------

void Backend::setSaveState(const QString &state) {
    if (state == m_saveState)
        return;
    m_saveState = state;
    emit saveStateChanged();
}

void Backend::scheduleSave() {
    if (m_readOnly || !m_location.isValid())
        return;
    if (m_saving) {
        // What is being written may be out of date already: saved() looks
        // again when it is done.
        setSaveState("saving");
        m_saveTimer.start();
    } else {
        settle();
    }
}

// Saved when the file holds just what the board does, which a step rolled
// back can come back to; written once the board has been still for a
// moment when it does not, or before long when it never is.
void Backend::settle() {
    if (!m_unreadable.isEmpty()) {
        // Nothing is written over a file that cannot be read.
        m_saveTimer.stop();
        m_saveDeadline.stop();
        setSaveState("paused");
        return;
    }
    if (m_board.revision() == m_savedRevision) {
        m_saveDeadline.stop();
        setSaveState("saved");
        if (m_saveFailed)
            dismissProblem();
        m_saveFailed = false;
        // Nothing done here is still to be written. Ids the file is still to
        // be given are written once the board is still.
        if (m_idsToWrite)
            m_saveTimer.start();
        else
            m_saveTimer.stop();
    } else {
        setSaveState("unsaved");
        m_saveTimer.start();
        // Every change starts the wait again; this one counts from the first
        // change the file does not have yet, and is not started again.
        if (!m_saveDeadline.isActive())
            m_saveDeadline.start();
    }
}

void Backend::save() {
    if (m_readOnly || !m_unreadable.isEmpty() || !m_location.isValid() ||
        (m_board.revision() == m_savedRevision && !m_idsToWrite))
        return;
    if (m_saving) {
        // One at a time; saved() looks again when this one is done.
        return;
    }
    m_saving = true;
    // This write takes everything so far; what changes next counts afresh.
    m_saveDeadline.stop();
    setSaveState("saving");
    // What the window shows, an edit or a drag still under way included.
    // The items are copied here, cheaply, and turned into a file on another
    // thread, so even a board of thousands saves without a dropped frame.
    Written written;
    written.items = m_board.itemsToWrite();
    written.lastId = m_board.lastId();
    written.revision = m_board.revision();
    // A board whose folder has gone was renamed or moved elsewhere, and is
    // followed there; or it was taken away, and is made again, folder and
    // all, so that nothing done here is lost. Never is the folder it had
    // made again when it went somewhere else: what is done here would go
    // to a board of the old name, and not to the one that is open.
    followMove();
    const QString file = m_location.file;
    const QByteArray base = m_baseFingerprint;
    QString folder = boardFolder();
    if (!QFileInfo::exists(folder))
        folder.clear();
    // `this` outlives the write: the pool is a member, and both flush() and
    // its destructor wait for it.
    m_pool.start([this, written, file, base, folder]() mutable {
        // Gone since it was looked for: where to is looked for again first.
        if (!folder.isEmpty() && !QFileInfo::exists(folder)) {
            written.moved = true;
            QMetaObject::invokeMethod(this, [this, written] { saved(written); }, Qt::QueuedConnection);
            return;
        }
        // Under the lock every writer takes, so no command comes in between.
        // What is to be written is made beforehand, so the lock is held no
        // longer than it must be.
        const QByteArray contents = Library::contentsOf(Board::toJson(written.items, written.lastId));
        if (const auto lock = Library::lock(file, &written.error))
            writeOver(file, contents, base, written);
        QMetaObject::invokeMethod(this, [this, written] { saved(written); }, Qt::QueuedConnection);
    });
}

// Writes over the file only while it is as this app last read or wrote it,
// `base` (or there is none). Changed by someone else since, it is left
// alone, and `written` says so: what they wrote is taken in first, and then
// saved with the rest. It is looked at the moment before it would be
// replaced, so that not even a whole file renamed over it while this one was
// written (by an agent, which takes no lock) is replaced unseen.
void Backend::writeOver(const QString &file, const QByteArray &contents, const QByteArray &base, Written &written) {
    written.changed = false;
    written.error = Library::write(file, contents, &written.fingerprint, [&] {
        written.changed = QFileInfo::exists(file) && Library::fingerprint(file) != base;
        return !written.changed;
    });
}

void Backend::saved(const Written &written) {
    m_saving = false;
    if (written.moved) {
        // Looked for, and written again where it is now (see save()).
        followMove();
        settle();
        return;
    }
    if (written.changed) {
        fileChanged();
        settle();
        return;
    }
    if (!written.error.isEmpty()) {
        // What came from outside while it was being written is taken in all
        // the same (see fileChanged()).
        m_watchTimer.start();
        if (written.revision == m_savedRevision) {
            // Only the ids the file is to be given were written, and whatever
            // reads it gives its items those same ids: nothing done here is
            // lost, and nothing is said. They go with what is saved next.
            m_idsToWrite = false;
            settle();
            return;
        }
        m_saveFailed = true;
        setSaveState("failed");
        setProblem(tr("The board is not saved. %1").arg(written.error));
        return;
    }
    m_savedRevision = written.revision;
    m_idsToWrite = false;
    m_base = written.items;
    m_baseFingerprint = written.fingerprint;
    // A new file took the old one's place, and the watch is set on it. What
    // was written to it before then, by a command run the moment after this
    // save while the window was too busy to look, the watch never saw: the
    // file is looked at once more all the same.
    watch();
    m_watchTimer.start();
    settle();
}

bool Backend::flush() {
    m_saveTimer.stop();
    m_saveDeadline.stop();
    // Let a save that is under way finish and report.
    m_pool.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    // Nothing to write; or a board that could not be read, which has nothing
    // of its own and whose file is left as it is.
    if (m_readOnly || !m_location.isValid() || (m_board.revision() == m_savedRevision && !m_idsToWrite))
        return true;
    // Under the lock, over the file as it was last read or written: what was
    // written to it since is taken in first, and a file that cannot be read
    // is not written over. A whole file renamed over it by an agent while it
    // is being written is taken in too, and the board written again: this
    // may be the last chance (the app is ending).
    Written written;
    // What it is a write of, which saved() is told even when the lock cannot
    // be had: one of ids alone that fails is nothing to say.
    written.revision = m_board.revision();
    // Moved elsewhere, it is written where it is now (see save()).
    followMove();
    if (const auto lock = Library::lock(m_location.file, &written.error)) {
        for (int tries = 0; tries < flushTries; ++tries) {
            QJsonObject root;
            QByteArray fingerprint;
            const QString problem = Library::read(m_location.file, root, &fingerprint);
            if (!QFileInfo::exists(m_location.file)) {
                resume();
            } else if (fingerprint != m_baseFingerprint && !takeIn(problem, root, fingerprint)) {
                // Asked to write it now, it says why it cannot. Ids alone
                // are no loss: only what was done here is.
                sayWhy();
                return m_board.revision() == m_savedRevision;
            }
            if (m_board.revision() == m_savedRevision && !m_idsToWrite)
                return true;
            written.items = m_board.itemsToWrite();
            written.lastId = m_board.lastId();
            written.revision = m_board.revision();
            writeOver(m_location.file, Library::contentsOf(Board::toJson(written.items, written.lastId)),
                      m_baseFingerprint, written);
            if (!written.changed)
                break;
        }
    }
    m_saving = true;
    saved(written);
    return m_board.revision() == m_savedRevision;
}

// --- Changes from outside ------------------------------------------------------------

void Backend::watch() {
    // Saving replaces the file, which drops it from the watch, so the folder
    // is watched too and both are re-armed after every change.
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!m_location.isValid())
        return;
    const QString folder = QFileInfo(m_location.file).absolutePath();
    if (QFileInfo::exists(folder))
        m_watcher.addPath(folder);
    // A file this app may not read cannot be watched itself: its folder sees
    // what becomes of it, its permissions given back included.
    if (QFileInfo(m_location.file).isReadable())
        m_watcher.addPath(m_location.file);
}

void Backend::fileChanged() {
    watch();
    if (!m_location.isValid())
        return;
    // Its own save is told from someone else's once it is done, when saved()
    // looks again.
    if (m_saving)
        return;
    // Gone, there is nothing to take in or to write over: whatever the board
    // has that the file did is written anew. Gone with its folder, it may
    // only have been renamed, and then it is looked at where it is now.
    if (!QFileInfo::exists(m_location.file)) {
        if (followMove()) {
            fileChanged();
            return;
        }
        resume();
        return;
    }
    // Its own save, or nothing new (a file written beside it, a command
    // that took the lock and wrote nothing): told by the file's fingerprint
    // alone, before any of it is read as a board, which on a board of
    // thousands of items would hold up the window each time.
    const QByteArray now = Library::fingerprint(m_location.file);
    if (!now.isEmpty() && now == m_baseFingerprint) {
        resume();
        return;
    }
    if (!now.isEmpty() && now == m_unreadableFingerprint)
        return;
    QJsonObject root;
    QByteArray fingerprint;
    const QString problem = Library::read(m_location.file, root, &fingerprint);
    takeIn(problem, root, fingerprint);
}

// Takes in the file as it is now: read with `problem`, as `root`, holding
// what `fingerprint` says. Work in the window is never thrown away for it:
// the two are merged (see Board::merge()), and the board then writes what
// it has that the file does not. False when the file cannot be read.
bool Backend::takeIn(const QString &problem, const QJsonObject &root, const QByteArray &fingerprint) {
    QVector<Item> theirs;
    bool numbered = false;
    quint32 lastId = 0;
    const QStringList problems = problem.isEmpty() ? Board::read(root, theirs, &numbered, &lastId) : QStringList();
    if (!problem.isEmpty() || !problems.isEmpty()) {
        const QString why = unreadableBecause(problem, problems);
        m_unreadableFingerprint = fingerprint;
        // Changed, and still not a board: what is wrong with it now is said
        // in place of what was. Found so again for the same reason, as a
        // file that cannot be read at all is at every change beside it (it
        // has no fingerprint to know it by), what was said and put away
        // stays put away.
        if (m_readOnly) {
            const QString again = cannotOpen(why, Board::newer(root));
            if (again != m_whyReadOnly)
                setReadOnly(again);
            return false;
        }
        const QString unreadable = tr("%1 changed on disk and can no longer be read. %2 Nothing is written over it: "
                                      "what is here is kept, and saved once it can be read again.")
                                       .arg(m_location.name, why);
        if (unreadable != m_unreadable) {
            m_unreadable = unreadable;
            settle();
            setProblem(m_unreadable);
        }
        return false;
    }
    const bool resumed = !m_unreadable.isEmpty();
    resume();
    if (m_readOnly) {
        // A board that could not be read when it was opened, and now can be,
        // opens as it would have then: what stood in for it was never to be
        // kept, there is nothing left to say of the file, and items it holds
        // with no id are written back with theirs soon.
        m_board.fromJson(root);
        setReadOnly({});
        dismissProblem();
        m_base = m_board.items();
        m_baseFingerprint = fingerprint;
        m_savedRevision = m_board.revision();
        m_idsToWrite = numbered;
        settle();
        setNotice(tr("Reloaded %1: it changed on disk").arg(m_location.name));
        emit boardOpened();
        return true;
    }
    const Board::Merged merged = m_board.merge(m_base, theirs, lastId);
    m_base = theirs;
    m_baseFingerprint = fingerprint;
    // When the file holds just what the board does, the board is saved;
    // when not, it has something to write, whether or not anything here
    // changed for it. Items the file holds with no id are written back soon
    // with the ids they have here, and in what commands print: left
    // without, the next whole file written from outside, giving new items
    // those ids, would number them anew.
    if (merged.ahead)
        m_savedRevision.reset();
    else
        m_savedRevision = m_board.revision();
    m_idsToWrite = numbered;
    settle();
    if (merged.kept > 0)
        setNotice(tr("Updated %1 from outside, but kept %n items as they are here", nullptr, merged.kept)
                      .arg(m_location.name));
    else if (merged.taken > 0 || merged.restacked || resumed)
        setNotice(tr("Updated %1 from outside").arg(m_location.name));
    return true;
}

// The file can be read again, or is gone: saving goes on.
void Backend::resume() {
    m_unreadableFingerprint.clear();
    if (m_unreadable.isEmpty())
        return;
    if (m_problem == m_unreadable)
        dismissProblem();
    m_unreadable.clear();
    settle();
}

// --- What is said in the window -------------------------------------------------------

void Backend::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

void Backend::dismissProblem() { setProblem({}); }

void Backend::sayWhy() {
    if (m_readOnly)
        setProblem(m_whyReadOnly);
    else if (!m_unreadable.isEmpty())
        setProblem(m_unreadable);
}

void Backend::setReadOnly(const QString &why) {
    m_whyReadOnly = why;
    if (!why.isEmpty())
        setProblem(why);
    const bool readOnly = !why.isEmpty();
    if (readOnly == m_readOnly)
        return;
    m_readOnly = readOnly;
    emit readOnlyChanged();
}

void Backend::setNotice(const QString &notice) {
    if (notice.isEmpty())
        m_noticeTimer.stop();
    else
        m_noticeTimer.start();
    if (notice == m_notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

// --- Where each board was left ------------------------------------------------------

QString Backend::viewKey() const { return m_location.file; }

void Backend::readState() {
    QFile file(m_library->stateFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    m_lastBoard = state.value("last").toString();
    const QJsonObject views = state.value("views").toObject();
    for (auto it = views.constBegin(); it != views.constEnd(); ++it)
        m_views.insert(it.key(), it.value().toObject().toVariantMap());
}

void Backend::writeState() {
    m_stateTimer.stop();
    QJsonObject views;
    for (auto it = m_views.constBegin(); it != m_views.constEnd(); ++it)
        // Boards that are gone are forgotten.
        if (QFile::exists(it.key()))
            views.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    const QString file = m_library->stateFile();
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile output(file);
    if (!output.open(QIODevice::WriteOnly))
        return;
    output.write(QJsonDocument(QJsonObject{{"last", m_lastBoard}, {"views", views}}).toJson());
    output.commit();
}

void Backend::rememberView(QPointF origin, qreal zoom) {
    // Where a board that could not be read was left is kept for when it can
    // be: the empty board shown in its place has nothing to go back to.
    if (!m_location.isValid() || m_readOnly)
        return;
    m_views.insert(viewKey(), {{"x", origin.x()}, {"y", origin.y()}, {"zoom", zoom}});
    m_stateTimer.start();
}

QVariantMap Backend::rememberedView() const { return m_views.value(viewKey()); }

// --- Pictures and the clipboard --------------------------------------------------------

void Backend::pickImages() {
    if (m_picker)
        m_picker->openImages();
}

void Backend::addFiles(const QList<QUrl> &urls) {
    if (!m_location.isValid() || m_readOnly)
        return;
    int position = 0;
    for (const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        if (path.isEmpty())
            continue;
        QImageReader reader(path);
        reader.setAutoTransform(true);
        // Only a file is looked into: a pipe or a device would never answer.
        // A picture of no width or height, as an SVG can say it is, has
        // nothing to show, and is refused before it is copied in.
        QSize size = QFileInfo(path).isFile() ? reader.size() : QSize();
        if (size.isEmpty() || !reader.canRead()) {
            setProblem(tr("%1 is not a picture Sprawl can show.").arg(QFileInfo(path).fileName()));
            continue;
        }
        // A photo taken on its side is as tall as it says it is wide.
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            size.transpose();
        QString problem;
        const QString asset = Library::importFile(m_location.assets, path, &problem);
        if (asset.isEmpty()) {
            setProblem(problem);
            continue;
        }
        emit imageAdded(asset, size, position++);
    }
}

bool Backend::pasteClipboard() {
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || !m_location.isValid() || m_readOnly)
        return false;
    if (data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        QString problem;
        const QString asset = Library::importImage(m_location.assets, image, &problem);
        if (asset.isEmpty()) {
            setProblem(problem);
            return true;
        }
        emit imageAdded(asset, image.size(), 0);
        return true;
    }
    if (data->hasUrls()) {
        QList<QUrl> files;
        for (const QUrl &url : data->urls())
            if (url.isLocalFile())
                files.append(url);
        if (!files.isEmpty()) {
            addFiles(files);
            return true;
        }
    }
    if (data->hasText() && !data->text().trimmed().isEmpty()) {
        emit textPasted(data->text());
        return true;
    }
    return false;
}

// --- Exporting ---------------------------------------------------------------------------

void Backend::exportBoard(const QString &format, const QVariantList &ids) {
    if (!m_picker || !m_location.isValid())
        return;
    // What was selected when the export was asked for, as much of it as is
    // still on the board. All of it taken away meanwhile (in another window,
    // by a command), there is nothing to export: never the whole board,
    // which is what no ids at all would mean.
    m_exportIds.clear();
    for (const QVariant &id : ids)
        if (m_board.item(id.toUInt()))
            m_exportIds.append(id.toUInt());
    if (!ids.isEmpty() && m_exportIds.isEmpty()) {
        setProblem(selectionGone());
        return;
    }
    m_exportAsked = true;
    // Pictures go where pictures go, documents where documents go.
    const auto place = format == "pdf" ? QStandardPaths::DocumentsLocation : QStandardPaths::PicturesLocation;
    QString folder = QStandardPaths::writableLocation(place);
    if (!QDir(folder).exists())
        folder = QDir::homePath();
    m_picker->saveExport(QUrl::fromLocalFile(folder + "/" + m_location.name + "." + format), format);
}

void Backend::replaceWithExport(bool agreed) {
    const auto [path, format] = std::exchange(m_replacing, {});
    if (agreed && !path.isEmpty())
        writeExport(path, format);
}

QString Backend::selectionGone() const {
    return tr("Nothing was exported: what was selected is no longer on the board.");
}

void Backend::writeExport(const QString &path, const QString &format) {
    // Gone once it has been given a name, what was asked for is not
    // exported either: ids none of which is on the board cover nothing, and
    // the exporter says there is nothing to export.
    const QString problem = exportTo(path, format, m_exportIds);
    if (problem.isEmpty())
        setNotice(tr("Exported to %1").arg(homeRelative(path)));
    else
        setProblem(problem);
}

QString Backend::exportTo(const QString &path, const QString &format, const QVector<quint32> &ids) {
    // Just after a board is read, the canvas has not yet made all of it big
    // enough for what is written on it. That is done here first, so the file
    // has the board as the window is about to draw it.
    TextMetrics::fitAll(m_board);
    Exporter::Options options;
    options.format = format;
    options.radius = m_theme ? m_theme->radius() : 0;
    return Exporter::write(path, m_board, ids, m_theme ? m_theme->palette() : Palette::fallback(), m_location.assets,
                           options);
}
