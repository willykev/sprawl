#pragma once

#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>

// The desktop's own open and save dialogs. Abstract, so tests can stand in a
// picker that answers without a desktop.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    // The endings the open dialog names pictures by, as well as their type.
    // Qt reads each with the packages Sprawl depends on, but AVIF, which
    // takes two that the package has as optional.
    static QStringList pictureSuffixes() { return {"png", "jpg", "jpeg", "webp", "gif", "svg", "bmp", "avif"}; }

    virtual void openImages() = 0;
    // `format` is the kind of file being written: png, svg or pdf.
    virtual void saveExport(const QUrl &suggestedUrl, const QString &format) = 0;

signals:
    void imagesSelected(const QList<QUrl> &urls);
    void exportSelected(const QUrl &url, const QString &format);
    void failed(const QString &message);
};
