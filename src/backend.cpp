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

#include "exporter.h"
#include "filepicker.h"
#include "textlayout.h"
#include "theme.h"

namespace {

// How long after the last change a board is written. Long enough that a
// drag is one write, short enough that nothing is ever lost.
const int saveDelay = 400;

QString homeRelative(const QString &path) {
    const QString home = QDir::homePath();
    return path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path;
}

} // namespace

Backend::Backend(Library *library, FilePicker *picker, Theme *theme, QObject *parent)
    : QObject(parent), m_library(library), m_picker(picker), m_theme(theme) {
    m_pool.setMaxThreadCount(1);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(saveDelay);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);
    connect(&m_board, &Board::modified, this, &Backend::scheduleSave);

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
            QString path = url.toLocalFile();
            if (!path.endsWith("." + format, Qt::CaseInsensitive))
                path += "." + format;
            const QString problem = exportTo(path, format, m_exportIds);
            if (problem.isEmpty())
                setNotice(tr("Exported to %1").arg(homeRelative(path)));
            else
                setProblem(problem);
        });
    }
    readState();
}

Backend::~Backend() {
    flush();
    writeState();
}

// --- Boards -----------------------------------------------------------------------

void Backend::openInitial(const QString &argument) {
    if (!argument.isEmpty()) {
        const Library::Location location = m_library->locate(argument);
        if (location.isValid()) {
            open(location);
            return;
        }
        // Asked for by a name that is free: that is a new board.
        if (Library::slug(argument) == argument && newBoard(argument))
            return;
        setProblem(tr("There is no board %1.").arg(argument));
    }
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
    newBoard("untitled");
}

bool Backend::open(const Library::Location &location) {
    // Whatever was open is safely on disk before it is let go of.
    if (m_location.isValid() && !flush())
        return false;
    m_saveTimer.stop();

    QJsonObject root;
    QString problem = Library::read(location.file, root);
    QStringList problems;
    if (problem.isEmpty())
        problems = m_board.fromJson(root);
    m_location = location;
    m_readOnly = !problem.isEmpty() || !problems.isEmpty();
    if (m_readOnly) {
        // Say what is wrong, show an empty board, and never save over the
        // file that could not be read.
        m_board.clear();
        if (problem.isEmpty())
            problem = problems.size() > 3 ? problems.mid(0, 3).join(" ") + tr(" And %1 more.").arg(problems.size() - 3)
                                          : problems.join(" ");
        setProblem(tr("Could not open %1. %2 Nothing will be saved over it.").arg(location.name, problem));
    } else if (!m_problem.isEmpty()) {
        dismissProblem();
    }
    m_savedRevision = m_board.revision();
    const QFileInfo info(location.file);
    m_ownModified = info.lastModified();
    m_ownSize = info.size();
    setSaveState("saved");
    watch();

    if (m_library->exists(location.name) && m_library->boardFile(location.name) == location.file) {
        m_lastBoard = location.name;
        m_stateTimer.start();
    }
    emit boardChanged();
    emit boardOpened();
    return true;
}

bool Backend::openBoard(const QString &name) {
    if (name == m_location.name && m_library->boardFile(name) == m_location.file)
        return true;
    if (!m_library->exists(name)) {
        setProblem(tr("There is no board %1.").arg(name));
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
    return openBoard(free);
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
    // The view it was left at goes with it.
    if (m_views.contains(viewKey()))
        m_views.insert(m_library->boardFile(tidy), m_views.take(viewKey()));
    m_location = {tidy, m_library->boardFile(tidy), m_library->assetsFolder(tidy)};
    m_lastBoard = tidy;
    watch();
    writeState();
    emit boardChanged();
    return true;
}

bool Backend::deleteBoard() {
    if (!m_library->exists(m_location.name) || m_library->boardFile(m_location.name) != m_location.file) {
        setProblem(tr("Only boards in the library can be deleted here."));
        return false;
    }
    m_saveTimer.stop();
    m_pool.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
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
    const auto boards = m_library->boards();
    const bool opened = boards.isEmpty() ? newBoard("untitled") : openBoard(boards.first().name);
    if (opened)
        setNotice(tr("Moved %1 to the trash").arg(gone));
    return opened;
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
    setSaveState(m_saving ? "saving" : "unsaved");
    m_saveTimer.start();
}

void Backend::save() {
    if (m_readOnly || !m_location.isValid() || m_board.revision() == m_savedRevision)
        return;
    if (m_saving) {
        // One at a time; saved() looks again when this one is done.
        return;
    }
    m_saving = true;
    setSaveState("saving");
    // The items are copied here, cheaply, and turned into a file on another
    // thread, so even a board of thousands saves without a dropped frame.
    const QVector<Item> items = m_board.items();
    const quint64 revision = m_board.revision();
    const QString file = m_location.file;
    // `this` outlives the write: the pool is a member, and both flush() and
    // its destructor wait for it.
    m_pool.start([this, items, revision, file] {
        const QString error = Library::write(file, Board::toJson(items));
        QMetaObject::invokeMethod(this, [this, revision, error] { saved(revision, error); }, Qt::QueuedConnection);
    });
}

void Backend::saved(quint64 revision, const QString &error) {
    m_saving = false;
    if (!error.isEmpty()) {
        m_saveFailed = true;
        setSaveState("failed");
        setProblem(tr("The board is not saved. %1").arg(error));
        return;
    }
    m_savedRevision = revision;
    const QFileInfo info(m_location.file);
    m_ownModified = info.lastModified();
    m_ownSize = info.size();
    watch();
    if (m_board.revision() == m_savedRevision) {
        setSaveState("saved");
        if (m_saveFailed)
            dismissProblem();
        m_saveFailed = false;
    } else {
        // Changed again while it was being written.
        setSaveState("unsaved");
        m_saveTimer.start();
    }
}

bool Backend::flush() {
    m_saveTimer.stop();
    // Let a save that is under way finish and report.
    m_pool.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    if (m_readOnly || !m_location.isValid() || m_board.revision() == m_savedRevision)
        return true;
    m_saving = true;
    saved(m_board.revision(), Library::write(m_location.file, m_board.toJson()));
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
    if (QFileInfo::exists(m_location.file))
        m_watcher.addPath(m_location.file);
}

void Backend::fileChanged() {
    watch();
    if (!m_location.isValid())
        return;
    const QFileInfo info(m_location.file);
    if (!info.exists() || (info.lastModified() == m_ownModified && info.size() == m_ownSize))
        return;
    // Work in the window that is not yet on disk wins: it is about to be
    // saved, and is never thrown away for a file that changed behind it.
    if (m_saving || m_board.revision() != m_savedRevision)
        return;
    m_ownModified = info.lastModified();
    m_ownSize = info.size();
    QJsonObject root;
    QString problem = Library::read(m_location.file, root);
    const QStringList problems = problem.isEmpty() ? m_board.fromJson(root) : QStringList();
    if (!problem.isEmpty() || !problems.isEmpty()) {
        if (problem.isEmpty())
            problem = problems.mid(0, 3).join(" ");
        setProblem(tr("%1 changed on disk and can no longer be read. %2 The board here is as it was.")
                       .arg(m_location.name, problem));
        return;
    }
    m_readOnly = false;
    m_savedRevision = m_board.revision();
    setSaveState("saved");
    setNotice(tr("Reloaded %1: it changed on disk").arg(m_location.name));
}

// --- What is said in the window -------------------------------------------------------

void Backend::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

void Backend::dismissProblem() { setProblem({}); }

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
    if (!m_location.isValid())
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
        QSize size = reader.size();
        if (!reader.canRead() || !size.isValid()) {
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
    m_exportIds.clear();
    for (const QVariant &id : ids)
        m_exportIds.append(id.toUInt());
    // Pictures go where pictures go, documents where documents go.
    const auto place = format == "pdf" ? QStandardPaths::DocumentsLocation : QStandardPaths::PicturesLocation;
    QString folder = QStandardPaths::writableLocation(place);
    if (!QDir(folder).exists())
        folder = QDir::homePath();
    m_picker->saveExport(QUrl::fromLocalFile(folder + "/" + m_location.name + "." + format), format);
}

QString Backend::exportTo(const QString &path, const QString &format, const QVector<quint32> &ids) {
    Exporter::Options options;
    options.format = format;
    options.radius = m_theme ? m_theme->radius() : 0;
    return Exporter::write(path, m_board, ids, m_theme ? m_theme->palette() : Palette::fallback(), m_location.assets,
                           options);
}
