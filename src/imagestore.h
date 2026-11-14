#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

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
    bool failed(const QString &asset) const { return m_entries.value(asset).failed; }
    // Whether any picture is still being read.
    bool loading() const;

    // Reads a picture right now, for exports. Null when it cannot be read.
    static QImage read(const QString &path);
    // A picture is never kept bigger than this on its long side: more than
    // any screen shows, and safely within what a GPU takes as one texture.
    static constexpr int longestSide = 4096;

signals:
    void loaded(const QString &asset);

private:
    struct Entry {
        QImage image;
        bool loading = false, failed = false;
    };
    QString m_folder;
    QHash<QString, Entry> m_entries;
    // Bumped when the folder changes, so a late read for the old board is
    // dropped.
    quint64 m_generation = 0;
};
