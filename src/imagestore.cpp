#include "imagestore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPointer>
#include <QThreadPool>
#include <QXmlStreamReader>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace {

// An SVG bigger than this, unpacked, is not read: far bigger than any
// drawing, and more than is worth looking through.
const qint64 largestSvg = qint64(64) << 20;

bool isSvg(const QByteArray &format) { return format == "svg" || format == "svgz"; }

// What a gzipped SVG (an .svgz) holds. Nothing when it is not whole, or
// would unpack to more than an SVG is read at.
QByteArray gunzip(const QByteArray &packed) {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
    stream.avail_in = uInt(packed.size());
    QByteArray unpacked;
    char chunk[16384];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(chunk);
        stream.avail_out = sizeof chunk;
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            break;
        unpacked.append(chunk, qsizetype(sizeof chunk - stream.avail_out));
        if (unpacked.size() > largestSvg)
            status = Z_MEM_ERROR;
    }
    inflateEnd(&stream);
    return status == Z_STREAM_END ? unpacked : QByteArray();
}

// Whether an SVG draws only what it holds. Qt's SVG reader draws in a
// picture that an <image> names by a path, reading it from wherever that
// is, and takes in a style sheet that an xml-stylesheet instruction names
// the same way; nothing tells it not to. Neither is let through: an
// <image> here may only hold its picture itself, as a data: URL. The SVG is
// read as Qt reads it, an element and an attribute known by the name they
// are written with.
bool selfContained(const QByteArray &svg) {
    QXmlStreamReader xml(svg);
    xml.setNamespaceProcessing(false);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isProcessingInstruction() && xml.processingInstructionTarget() == QLatin1String("xml-stylesheet"))
            return false;
        if (!xml.isStartElement())
            continue;
        const QStringView name = xml.name();
        if (name != QLatin1String("image") && !name.endsWith(QLatin1String(":image")))
            continue;
        for (const QXmlStreamAttribute &attribute : xml.attributes()) {
            const QStringView key = attribute.qualifiedName();
            if ((key == QLatin1String("href") || key.endsWith(QLatin1String(":href"))) &&
                !attribute.value().trimmed().startsWith(QLatin1String("data:")))
                return false;
        }
    }
    // What could not be looked through to its end is not drawn either.
    return !xml.hasError();
}

// What format the picture `device` holds is in: as what it holds says, or,
// when that says nothing, as `hint`, its name, does.
QByteArray formatOf(QIODevice *device, const QByteArray &hint) {
    QByteArray format;
    {
        QImageReader probe(device);
        format = probe.format();
    }
    device->seek(0);
    return format.isEmpty() ? hint.toLower() : format;
}

// What an SVG in `device` draws from, unpacked: nothing when it draws
// anything it does not hold (see selfContained()), or is too big.
QByteArray svgSource(QIODevice *device) {
    if (device->size() > largestSvg)
        return {};
    QByteArray source = device->readAll();
    device->seek(0);
    if (source.startsWith("\x1f\x8b"))
        source = gunzip(source);
    return !source.isEmpty() && selfContained(source) ? source : QByteArray();
}

void scaleReader(QImageReader &reader, const QSize &size, int longestSide) {
    // Brought down to the longest side kept, a picture far longer than it is
    // wide is still a pixel across: at none, it would not be read at all.
    if (size.isValid() && qMax(size.width(), size.height()) > longestSide)
        reader.setScaledSize(size.scaled(longestSide, longestSide, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
}

// Reads or measures the picture only after an SVG has been looked through,
// whatever the file is called.
template <typename Read>
auto withReader(QIODevice *device, const QByteArray &hint, Read read) -> decltype(read(std::declval<QImageReader &>())) {
    const QByteArray format = formatOf(device, hint);
    if (format.isEmpty())
        return {};
    if (isSvg(format)) {
        QByteArray source = svgSource(device);
        if (source.isEmpty())
            return {};
        QBuffer buffer(&source);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "svg");
        reader.setAutoTransform(true);
        return read(reader);
    }
    QImageReader reader(device, format);
    reader.setAutoTransform(true);
    return read(reader);
}

QImage decode(QIODevice *device, const QByteArray &hint) {
    return withReader(device, hint, [](QImageReader &reader) {
        scaleReader(reader, reader.size(), ImageStore::longestSide);
        const QImage image = reader.read();
        // The format the GPU takes as it is.
        return image.isNull() ? image : image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    });
}

QByteArray suffixOf(const QString &name) { return QFileInfo(name).suffix().toLower().toLatin1(); }

} // namespace

void ImageStore::setFolder(const QString &folder) {
    if (folder == m_folder)
        return;
    m_folder = folder;
    m_entries.clear();
    ++m_generation;
}

bool ImageStore::openAsset(const QString &folder, const QString &asset, QFile &file) {
    if (folder.isEmpty() || asset.isEmpty() || asset.contains('/') || asset == "." || asset == "..")
        return false;
    // The folder itself is no link either (O_NOFOLLOW on its last part,
    // which a slash after it would undo), and the file is opened in it.
    const QByteArray where = QFile::encodeName(QDir::cleanPath(folder));
    const int directory = ::open(where.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return false;
    // Not blocking, so that a pipe put there is not waited on before it is
    // found to be one.
    const int handle =
        ::openat(directory, QFile::encodeName(asset).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ::close(directory);
    if (handle < 0)
        return false;
    struct stat status;
    if (::fstat(handle, &status) != 0 || !S_ISREG(status.st_mode) ||
        !file.open(handle, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(handle);
        return false;
    }
    return true;
}

QImage ImageStore::read(const QString &folder, const QString &asset) {
    QFile file;
    if (!openAsset(folder, asset, file))
        return {};
    return decode(&file, suffixOf(asset));
}

QImage ImageStore::read(const QString &path) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return {};
    return decode(&file, suffixOf(path));
}

bool ImageStore::canShow(QIODevice *device, const QByteArray &hint) {
    return !validatedSize(device, hint).isEmpty();
}

QSize ImageStore::validatedSize(QIODevice *device, const QByteArray &hint) {
    const QSize size = withReader(device, hint, [](QImageReader &reader) -> QSize {
        const QSize natural = reader.size();
        if (natural.isEmpty())
            return {};
        // The decoded pixels are only used to tell whether the picture
        // can be read. Keep a thumbnail and skip conversion for the GPU.
        scaleReader(reader, natural, 128);
        const bool rotated = reader.transformation() & QImageIOHandler::TransformationRotate90;
        return reader.read().isNull() ? QSize() : rotated ? natural.transposed() : natural;
    });
    device->seek(0);
    return size;
}

bool ImageStore::loading() const {
    for (const Entry &entry : m_entries)
        if (entry.loading)
            return true;
    return false;
}

void ImageStore::setInUse(const QSet<QString> &assets) {
    // Those that go out of use now are the newest out of use.
    for (const QString &asset : std::as_const(m_inUse))
        if (!assets.contains(asset))
            if (const auto found = m_entries.find(asset); found != m_entries.end())
                found->used = ++m_clock;
    m_inUse = assets;
    trim();
}

void ImageStore::setUnusedBudget(qint64 bytes) {
    m_unusedBudget = qMax<qint64>(0, bytes);
    trim();
}

qint64 ImageStore::bytesHeld() const {
    qint64 bytes = 0;
    for (const Entry &entry : m_entries)
        bytes += entry.image.sizeInBytes();
    return bytes;
}

qint64 ImageStore::unusedBytesHeld() const {
    qint64 bytes = 0;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        if (!m_inUse.contains(it.key()))
            bytes += it->image.sizeInBytes();
    return bytes;
}

void ImageStore::trim() {
    qint64 unused = unusedBytesHeld();
    while (unused > m_unusedBudget) {
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (!it->image.isNull() && !m_inUse.contains(it.key()) &&
                (oldest == m_entries.end() || it->used < oldest->used))
                oldest = it;
        if (oldest == m_entries.end())
            return;
        unused -= oldest->image.sizeInBytes();
        // Asked for again, it is read again.
        m_entries.erase(oldest);
    }
}

QImage ImageStore::image(const QString &asset) {
    Entry &entry = m_entries[asset];
    entry.used = ++m_clock;
    if (!entry.image.isNull() || entry.loading || entry.failed)
        return entry.image;
    if (m_folder.isEmpty() || asset.isEmpty()) {
        entry.failed = true;
        return {};
    }
    entry.loading = true;
    const QString folder = m_folder;
    const quint64 generation = m_generation;
    QPointer<ImageStore> store(this);
    QThreadPool::globalInstance()->start([store, folder, asset, generation] {
        const QImage image = read(folder, asset);
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
                // What nothing shows any more is kept only within the budget.
                if (store && !store->m_inUse.contains(asset))
                    store->trim();
            },
            Qt::QueuedConnection);
    });
    return {};
}
