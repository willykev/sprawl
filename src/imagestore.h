#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

class QFile;
class QIODevice;

// The pictures of the board that is open. They are read and scaled on
// worker threads, so a board full of photos opens as quickly as an empty
// one and the pictures appear as they arrive.
class ImageStore : public QObject {
    Q_OBJECT

public:
    explicit ImageStore(QObject *parent = nullptr) : QObject(parent) {}

    // The folder the board keeps its pictures in. Forgets what was read.
    void setFolder(const QString &folder);
    QString folder() const { return m_folder; }

    // The picture, or a null image while it is still being read (`loaded`
    // follows) or when it cannot be (see failed()).
    QImage image(const QString &asset);
    // The picture if it has been read, and a null image if not, without
    // having it read.
    QImage alreadyRead(const QString &asset) const { return m_entries.value(asset).image; }
    bool failed(const QString &asset) const { return m_entries.value(asset).failed; }
    // Whether any picture is still being read.
    bool loading() const;

    // The pictures the board shows now. The rest, taken off it or never on
    // it, are kept only while together they take up no more than
    // unusedBudget, the one longest out of use let go of first: a picture
    // just taken off comes back at once with an undo, and a board that has
    // been through many pictures does not hold on to them all.
    void setInUse(const QSet<QString> &assets);
    static constexpr qint64 unusedBudget = qint64(256) << 20;
    void setUnusedBudget(qint64 bytes);
    // What the pictures read take up: all of them, and those not in use.
    qint64 bytesHeld() const;
    qint64 unusedBytesHeld() const;

    // Reads a picture of a board right now, for exports: `asset`, a file
    // right in `folder`, its assets folder. Null when it cannot be read, or
    // when it is not in that folder (see openAsset()).
    static QImage read(const QString &folder, const QString &asset);
    // Reads a picture from a file that was chosen, wherever it is. Null when
    // it cannot be read.
    static QImage read(const QString &path);
    // Opens `asset` only when it is a plain file right in `folder`: neither
    // the folder nor the file is a link, which could lead anywhere, and the
    // file is no pipe or device. A board from somewhere else shows only
    // what it brought along.
    static bool openAsset(const QString &folder, const QString &asset, QFile &file);
    // Whether what `device` holds is a picture Sprawl shows. `hint` is what
    // its name says it is (its suffix). An SVG that draws other files into
    // itself is not: Qt would read them from wherever it says, and nothing
    // else of a board reaches outside it.
    static bool canShow(QIODevice *device, const QByteArray &hint = {});
    static bool canShow(const QString &path);
    // A picture is never kept bigger than this on its long side: more than
    // any screen shows, and safely within what a GPU takes as one texture.
    static constexpr int longestSide = 4096;

signals:
    void loaded(const QString &asset);

private:
    struct Entry {
        QImage image;
        bool loading = false, failed = false;
        // When it was last asked for, or went out of use.
        quint64 used = 0;
    };
    // Lets go of pictures not in use, the one longest out of use first,
    // until those left are within the budget.
    void trim();

    QString m_folder;
    QHash<QString, Entry> m_entries;
    QSet<QString> m_inUse;
    qint64 m_unusedBudget = unusedBudget;
    quint64 m_clock = 0;
    // Bumped when the folder changes, so a late read for the old board is
    // dropped.
    quint64 m_generation = 0;
};
