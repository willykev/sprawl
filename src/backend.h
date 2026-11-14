#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "board.h"
#include "library.h"

class FilePicker;
class Theme;

// The app around the canvas: which board is open, keeping it saved, the
// other boards, and the files that come in (pictures) and go out (exports).
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(Board *board READ board CONSTANT)
    Q_PROPERTY(QString boardName READ boardName NOTIFY boardChanged)
    Q_PROPERTY(QString assetFolder READ assetFolder NOTIFY boardChanged)
    // saved, unsaved (about to be saved), saving, or failed.
    Q_PROPERTY(QString saveState READ saveState NOTIFY saveStateChanged)
    // Something that went wrong, said in the window until it is dismissed.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // Something that went right, said for a moment.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    Backend(Library *library, FilePicker *picker, Theme *theme, QObject *parent = nullptr);
    ~Backend() override;

    Board *board() { return &m_board; }
    QString boardName() const { return m_location.name; }
    QString assetFolder() const { return m_location.assets; }
    QString saveState() const { return m_saveState; }
    QString problem() const { return m_problem; }
    QString notice() const { return m_notice; }

    // Opens the board named on the command line, or the one that was open
    // last, or the newest, or, the very first time, a welcome board.
    void openInitial(const QString &argument);

    // The boards whose names answer what was typed, best first; all of them,
    // newest first, when nothing was.
    Q_INVOKABLE QVariantList findBoards(const QString &query) const;
    Q_INVOKABLE bool openBoard(const QString &name);
    // Makes a board and opens it. The name is tidied into a folder name and
    // numbered when taken; empty is "untitled".
    Q_INVOKABLE bool newBoard(const QString &name);
    Q_INVOKABLE bool renameBoard(const QString &name);
    // Moves the open board to the trash and opens another.
    Q_INVOKABLE bool deleteBoard();
    Q_INVOKABLE QString tidyName(const QString &name) const { return Library::slug(name); }

    Q_INVOKABLE void rememberView(QPointF origin, qreal zoom);
    // x, y and zoom of the open board as it was last left; empty when it
    // has not been seen before.
    Q_INVOKABLE QVariantMap rememberedView() const;

    Q_INVOKABLE void pickImages();
    Q_INVOKABLE void addFiles(const QList<QUrl> &urls);
    // Takes a picture, files, or text from the system clipboard. False when
    // it holds none of them.
    Q_INVOKABLE bool pasteClipboard();

    // Asks where to save, then writes the items (all of them when `ids` is
    // empty) as a png, svg or pdf.
    Q_INVOKABLE void exportBoard(const QString &format, const QVariantList &ids);
    QString exportTo(const QString &path, const QString &format, const QVector<quint32> &ids);

    // Writes the board now. False when it could not be written.
    Q_INVOKABLE bool flush();
    Q_INVOKABLE void dismissProblem();
    void setProblem(const QString &problem);

signals:
    void boardChanged();
    // A board was opened: the view goes to where it was last left.
    void boardOpened();
    void saveStateChanged();
    void problemChanged();
    void noticeChanged();
    // A picture is in the board's assets and ready to be placed. `position`
    // counts the pictures of one batch, so they can be fanned out.
    void imageAdded(const QString &asset, const QSize &size, int position);
    void textPasted(const QString &text);

private:
    bool open(const Library::Location &location);
    void scheduleSave();
    void save();
    void saved(quint64 revision, const QString &error);
    void setSaveState(const QString &state);
    void setNotice(const QString &notice);
    void watch();
    void fileChanged();
    QString viewKey() const;
    void readState();
    void writeState();

    Library *m_library;
    FilePicker *m_picker;
    Theme *m_theme;
    Board m_board;
    Library::Location m_location;
    // The board could not be read, so nothing is ever written over it.
    bool m_readOnly = false;

    QString m_saveState = QStringLiteral("saved");
    QString m_problem, m_notice;
    QTimer m_saveTimer, m_noticeTimer, m_stateTimer, m_watchTimer;
    // One thread, so saves happen in the order they were asked for.
    QThreadPool m_pool;
    bool m_saving = false, m_saveFailed = false;
    quint64 m_savedRevision = 0;
    // The file as this app last wrote or read it, to tell its own saves
    // from someone else's.
    QDateTime m_ownModified;
    qint64 m_ownSize = -1;
    QFileSystemWatcher m_watcher;

    QString m_lastBoard;
    QHash<QString, QVariantMap> m_views;
    QVector<quint32> m_exportIds;
};
