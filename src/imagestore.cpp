#include "imagestore.h"

#include <QCoreApplication>
#include <QImageReader>
#include <QPointer>
#include <QThreadPool>

void ImageStore::setFolder(const QString &folder) {
    if (folder == m_folder)
        return;
    m_folder = folder;
    m_entries.clear();
    ++m_generation;
}

QImage ImageStore::read(const QString &path) {
    QImageReader reader(path);
    // Photos carry their rotation beside the pixels.
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && qMax(size.width(), size.height()) > longestSide)
        reader.setScaledSize(size.scaled(longestSide, longestSide, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    // The format the GPU takes as it is.
    return image.isNull() ? image : image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

bool ImageStore::loading() const {
    for (const Entry &entry : m_entries)
        if (entry.loading)
            return true;
    return false;
}

QImage ImageStore::image(const QString &asset) {
    Entry &entry = m_entries[asset];
    if (!entry.image.isNull() || entry.loading || entry.failed)
        return entry.image;
    if (m_folder.isEmpty() || asset.isEmpty()) {
        entry.failed = true;
        return {};
    }
    entry.loading = true;
    const QString path = m_folder + "/" + asset;
    const quint64 generation = m_generation;
    QPointer<ImageStore> store(this);
    QThreadPool::globalInstance()->start([store, path, asset, generation] {
        const QImage image = read(path);
        // Back on the thread the store lives on, if it still does.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [store, asset, generation, image] {
                if (!store || store->m_generation != generation)
                    return;
                Entry &entry = store->m_entries[asset];
                entry.loading = false;
                entry.failed = image.isNull();
                entry.image = image;
                emit store->loaded(asset);
            },
            Qt::QueuedConnection);
    });
    return {};
}
