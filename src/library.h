#pragma once

#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector>

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
    // `base`, or base-2, base-3... when it is taken.
    QString freeName(const QString &base) const;

    // These return what went wrong, or nothing when it worked.
    QString create(const QString &name);
    QString rename(const QString &from, const QString &to);
    // To the trash, so a board deleted by mistake can be had back.
    QString remove(const QString &name);

    static QString read(const QString &file, QJsonObject &root);
    // Written beside the file and renamed over it, so a crash or a full disk
    // never leaves half a board.
    static QString write(const QString &file, const QJsonObject &root);

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
    // .json file. Where its file and its pictures are.
    struct Location {
        QString name, file, assets;
        bool isValid() const { return !file.isEmpty(); }
    };
    Location locate(const QString &nameOrPath) const;

private:
    QString m_root;
    // In the desktop's own data folder, not one that was asked for.
    bool m_standard = false;
};
