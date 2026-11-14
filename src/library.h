#pragma once

#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

// Where boards live. Each board is a folder of its own under
// ~/.local/share/sprawl/boards: a board.json, written by this app (and by
// agents, see `sprawl skill`), and an assets folder holding its pictures.
// A board can also be any such folder, or a .json file, elsewhere.
class Library {
public:
    explicit Library(const QString &root = {});

    QString root() const { return m_root; }
    // Where the app remembers which board was open and where each was left:
    // with the desktop's other state, or beside the boards when they have
    // been put somewhere of their own.
    QString stateFile() const;
    QString boardsFolder() const { return m_root + "/boards"; }
    QString boardFolder(const QString &name) const { return boardsFolder() + "/" + name; }
    QString boardFile(const QString &name) const { return boardFolder(name) + "/board.json"; }
    QString assetsFolder(const QString &name) const { return boardFolder(name) + "/assets"; }

    struct Entry {
        QString name;
        QDateTime modified;
    };
    // Most recently changed first.
    QVector<Entry> boards() const;
    bool exists(const QString &name) const;

    // A board's name as a folder is named: "Desk setup!" is desk-setup.
    static QString slug(const QString &text);
    // The name a new board is given for `text`: it tidied, or "untitled"
    // when nothing is left of it.
    static QString nameFor(const QString &text);
    // `base` as nameFor() names it, or, when that is taken, base-2, base-3...
    QString freeName(const QString &base) const;

    // These return what went wrong, or nothing when it worked.
    QString create(const QString &name);
    QString rename(const QString &from, const QString &to);
    // To the trash, so a board deleted by mistake can be had back.
    QString remove(const QString &name);

    // `fingerprint`, when asked for, is that of what was read (see
    // fingerprint()), whether or not it turned out to be a board.
    static QString read(const QString &file, QJsonObject &root, QByteArray *fingerprint = nullptr);
    // Written beside the file and renamed over it, so a crash or a full disk
    // never leaves half a board. `fingerprint` is that of what was written.
    static QString write(const QString &file, const QJsonObject &root, QByteArray *fingerprint = nullptr);
    // The same, of what the file is to hold, made of a board beforehand.
    // `still`, when given, is asked whether the file is still to be replaced
    // at the last moment, what is to replace it written and on disk by then,
    // so that nothing but the rename comes after the answer. No, and the file
    // is left as it is (and `fingerprint` too).
    static QString write(const QString &file, const QByteArray &contents, QByteArray *fingerprint = nullptr,
                         const std::function<bool()> &still = {});
    // What a board's file holds of it.
    static QByteArray contentsOf(const QJsonObject &root);
    // What a file holds, in short: the same for the same bytes, and nothing
    // when there is no file it can be read from.
    static QByteArray fingerprint(const QString &file);
    // Whoever changes a board's file, the window as it saves and every
    // command, holds this from reading the file to writing it back, so that
    // none of them writes over a change it never read. Waits while someone
    // else holds it, and takes over one left by a writer that is gone. Null,
    // with `error` saying why, when it cannot be had.
    static std::unique_ptr<QLockFile> lock(const QString &file, QString *error = nullptr);

    // How well a board's name answers what was typed in the switcher: the
    // letters in order, better when together and at the start of words.
    // Negative when it does not.
    static int score(const QString &name, const QString &query);

    // Copies a picture into a board's assets, named for its contents so the
    // same picture is only kept once. Returns the name it has there, or
    // nothing, with `error` saying why, for what is not a picture.
    static QString importFile(const QString &assetsFolder, const QString &source, QString *error = nullptr);
    static QString importImage(const QString &assetsFolder, const QImage &image, QString *error = nullptr);

    // A board named on the command line: one in the library, a folder, or a
    // .json file. Where its file and its pictures are. A board's name means
    // that board, even beside a file or folder of the same name; ./plans is
    // the way to that folder.
    struct Location {
        QString name, file, assets;
        bool isValid() const { return !file.isEmpty(); }
    };
    Location locate(const QString &nameOrPath) const;

private:
    // Why no board can be given `name`, a name one can have: a board has it,
    // or something that is not a board does. Nothing when it is free.
    QString taken(const QString &name) const;

    QString m_root;
    // In the desktop's own data folder, not one that was asked for.
    bool m_standard = false;
};
