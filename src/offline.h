#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>

// Sprawl has nothing to fetch: its interface comes with it, and its boards
// are files. So that nothing ever is fetched, whatever a board or a name
// turns out to hold, the QML engine is given this in place of the network.
// Whatever is asked for that is not a file on this computer is refused
// before anything leaves it.
class Offline : public QQmlNetworkAccessManagerFactory {
public:
    // Called from whichever thread wants a manager, so it keeps nothing.
    QNetworkAccessManager *create(QObject *parent) override { return new Manager(parent); }

private:
    class Refusal : public QNetworkReply {
    public:
        Refusal(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QObject *parent)
            : QNetworkReply(parent) {
            setOperation(operation);
            setRequest(request);
            setUrl(request.url());
            setError(ConnectionRefusedError, QStringLiteral("Sprawl does not use the network"));
            setFinished(true);
            // Said once whoever asked has had the chance to listen.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    emit errorOccurred(error());
                    emit finished();
                },
                Qt::QueuedConnection);
        }
        void abort() override {}

    protected:
        qint64 readData(char *, qint64) override { return -1; }
    };

    class Manager : public QNetworkAccessManager {
    public:
        using QNetworkAccessManager::QNetworkAccessManager;

    protected:
        QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override {
            const QUrl url = request.url();
            if (url.isLocalFile() || url.scheme() == QLatin1String("qrc") || url.scheme() == QLatin1String("data"))
                return QNetworkAccessManager::createRequest(operation, request, data);
            return new Refusal(operation, request, this);
        }
    };
};
