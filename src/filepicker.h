#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

// The desktop's own open and save dialogs. Abstract, so tests can stand in a
// picker that answers without a desktop.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openImages() = 0;
    // `format` is the kind of file being written: png, svg or pdf.
    virtual void saveExport(const QUrl &suggestedUrl, const QString &format) = 0;

signals:
    void imagesSelected(const QList<QUrl> &urls);
    void exportSelected(const QUrl &url, const QString &format);
    void failed(const QString &message);
};
