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
#include <optional>

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
    // saved, unsaved (about to be saved), saving, failed, paused (the file
    // changed into one that cannot be read, which nothing is written over
    // until it can be), or readonly: the board could not be read when it was
    // opened, or there is none, and nothing is ever written (see readOnly).
    Q_PROPERTY(QString saveState READ saveState NOTIFY saveStateChanged)
    // Something that went wrong, said in the window until it is dismissed.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // Something that went right, said for a moment.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    // The open board could not be read: an empty board is shown in its place,
    // nothing can be changed on it, and nothing is ever saved over its file.
    // Once the file can be read, the board opens as it is. Or no board could
    // be opened or made at all, and there is nowhere to keep anything.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)

public:
    Backend(Library *library, FilePicker *picker, Theme *theme, QObject *parent = nullptr);
    ~Backend() override;

    Board *board() { return &m_board; }
    QString boardName() const { return m_location.name; }
    QString assetFolder() const { return m_location.assets; }
    QString saveState() const { return m_saveState; }
    QString problem() const { return m_problem; }
    QString notice() const { return m_notice; }
    bool readOnly() const { return m_readOnly; }

    // Opens the board named on the command line, by its name or a path to it,
    // as the commands find it; a free name is a new board. With none named,
    // or in place of one that is not there, it is the one that was open last,
    // or the newest, or, the very first time, a welcome board, and what
    // became of the one asked for is said.
    void openInitial(const QString &argument);

    // The boards whose names answer what was typed, best first; all of them,
    // newest first, when nothing was.
    Q_INVOKABLE QVariantList findBoards(const QString &query) const;
    Q_INVOKABLE bool openBoard(const QString &name);
    // Makes a board and opens it. The name is tidied into a folder name and
    // numbered when taken; empty is "untitled". Taken by something that is
    // not a board, that is said.
    Q_INVOKABLE bool newBoard(const QString &name);
    Q_INVOKABLE bool renameBoard(const QString &name);
    // Moves the open board to the trash and opens another, or, when none can
    // be opened or made, leaves the window with none. False when it is not
    // in the trash.
    Q_INVOKABLE bool deleteBoard();
    Q_INVOKABLE QString tidyName(const QString &name) const { return Library::slug(name); }

    Q_INVOKABLE void rememberView(QPointF origin, qreal zoom);
    // x, y and zoom of the open board where its view was last moved to;
    // empty when nobody has moved it.
    Q_INVOKABLE QVariantMap rememberedView() const;

    Q_INVOKABLE void pickImages();
    Q_INVOKABLE void addFiles(const QList<QUrl> &urls);
    // Takes a picture, files, or text from the system clipboard. False when
    // it holds none of them.
    Q_INVOKABLE bool pasteClipboard();

    // Asks where to save, then writes the items (all of them when `ids` is
    // empty) as a png, svg or pdf. A name chosen without the format's ending
    // is given it, and a file that has that name already, which the dialog
    // never asked about, is written over only once that is agreed to (see
    // replaceAsked()).
    Q_INVOKABLE void exportBoard(const QString &format, const QVariantList &ids);
    // The answer to replaceAsked(): the export is written over the file, or,
    // not agreed to, not written at all.
    Q_INVOKABLE void replaceWithExport(bool agreed);
    QString exportTo(const QString &path, const QString &format, const QVector<quint32> &ids);

    // Writes the board now, as the window shows it: an edit or a drag still
    // under way included, and whatever was written to the file from outside
    // taken in first. False when it could not be written.
    Q_INVOKABLE bool flush();
    Q_INVOKABLE void dismissProblem();
    // Says again why nothing is being saved, once that has been put away:
    // why the board is read only, or why its file is not written over.
    Q_INVOKABLE void sayWhy();
    // Something worth saying once, in the status line, that then goes away.
    Q_INVOKABLE void notify(const QString &notice) { setNotice(notice); }
    void setProblem(const QString &problem);

signals:
    void boardChanged();
    // A board was opened: the view goes to where it was last left.
    void boardOpened();
    void saveStateChanged();
    void problemChanged();
    void noticeChanged();
    void readOnlyChanged();
    // A picture is in the board's assets and ready to be placed. `position`
    // counts the pictures of one batch, so they can be fanned out.
    void imageAdded(const QString &asset, const QSize &size, int position);
    void textPasted(const QString &text);
    // An export is to be written over a file, `name` in `folder`, that the
    // dialog never asked about (see exportBoard()). It waits on
    // replaceWithExport().
    void replaceAsked(const QString &name, const QString &folder);

private:
    // What a save came to.
    struct Written {
        quint64 revision = 0;
        // What was written, and the fingerprint of the file it made.
        QVector<Item> items;
        quint32 lastId = 0;
        QByteArray fingerprint;
        // Nothing was written: the file had been changed from outside.
        bool changed = false;
        QString error;
    };

    bool open(const Library::Location &location);
    // The board that was open last, or the newest, or, the very first time,
    // a welcome board; or an untitled one, or, when not even that can be
    // made, none.
    void openLast();
    // What is said when the open board's file cannot be read, for `why`:
    // that nothing is saved over it, and what can be done about it. `newer`
    // is a file made by a newer Sprawl, which there is nothing wrong with.
    QString cannotOpen(const QString &why, bool newer) const;
    // No board could be opened or made, for `why`: there is none in the
    // window, and nowhere to keep anything done there.
    void openNone(const QString &why);
    void scheduleSave();
    void settle();
    void save();
    void saved(const Written &written);
    void setSaveState(const QString &state);
    void setNotice(const QString &notice);
    // Read only for `why`, which is said; or, for no reason, not.
    void setReadOnly(const QString &why);
    static void writeOver(const QString &file, const QByteArray &contents, const QByteArray &base, Written &written);
    void watch();
    void fileChanged();
    bool takeIn(const QString &problem, const QJsonObject &root, const QByteArray &fingerprint);
    void resume();
    QString viewKey() const;
    void readState();
    void writeState();
    // Writes the export asked for to `path`, and says how that went.
    void writeExport(const QString &path, const QString &format);

    Library *m_library;
    FilePicker *m_picker;
    Theme *m_theme;
    Board m_board;
    Library::Location m_location;
    // The board could not be read, or there is none, so nothing is ever
    // written; and why, as it was said, to be said again when asked.
    bool m_readOnly = false;
    QString m_whyReadOnly;

    QString m_saveState = QStringLiteral("saved");
    QString m_problem, m_notice;
    QTimer m_saveTimer, m_saveDeadline, m_noticeTimer, m_stateTimer, m_watchTimer;
    // One thread, so saves happen in the order they were asked for.
    QThreadPool m_pool;
    bool m_saving = false, m_saveFailed = false;
    // The revision of the board that the file holds; none when it holds
    // something the board has never been, as when the board kept changes of
    // its own over the file's.
    std::optional<quint64> m_savedRevision;
    // The file is still to be given the ids that items it holds without one
    // were given here. It reads as the board all the same, so the board is
    // saved: they are written once it is still, for whatever reads the file
    // next (see takeIn()), and nothing done here is lost when they cannot be.
    bool m_idsToWrite = false;
    // The file as this app last read or wrote it: what tells its own saves
    // from someone else's, and what someone else's are taken in against.
    QVector<Item> m_base;
    QByteArray m_baseFingerprint;
    // What is said of the file when it changed into one that cannot be read,
    // and nothing while it can be. Nothing is written over it until it can
    // be read again, and what it holds then is taken in first.
    QString m_unreadable;
    // The file as it was when it was last found it cannot be read: found so
    // again, there is nothing new to say of it.
    QByteArray m_unreadableFingerprint;
    QFileSystemWatcher m_watcher;

    QString m_lastBoard;
    QHash<QString, QVariantMap> m_views;
    QVector<quint32> m_exportIds;
    // An export was asked for, of the board that is open: answered once
    // another has been opened, it is not written.
    bool m_exportAsked = false;
    // The export waiting on replaceAsked(): the file it is to be written
    // over, and as what. Nothing while none is.
    struct Waiting {
        QString path, format;
    } m_replacing;
};
