#ifndef PUBSUB_PERMANENT_WORKER_H
#define PUBSUB_PERMANENT_WORKER_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include "core/db/types.h"

namespace pubsub {

/**
 * @brief Worker for permanent TCP connections
 * 
 * Runs in its own QThread, owns the QTcpSocket.
 * Receives events via queued signal and delivers them to the client.
 * Reads ACKs from the client and emits ackReceived.
 */
class PermanentWorker : public QObject {
    Q_OBJECT

public:
    PermanentWorker(qintptr socketDescriptor, 
                    const QString& clientId,
                    const QString& subscriptionMode, 
                    qint64 lastEventId);
    ~PermanentWorker() override;

public slots:
    /**
     * @brief Initialize the socket (must be called from worker thread)
     */
    void initialize();

    /**
     * @brief Deliver an event to the connected client
     * 
     * Called via QMetaObject::invokeMethod from the main thread.
     * The event is formatted as JSON and written to the socket.
     */
    void deliverEvent(const core::Event& event);

    /**
     * @brief Send a ping to check if client is alive
     */
    void sendPing();

signals:
    /**
     * @brief Emitted when client acknowledges events
     * @param clientId The client identifier
     * @param eventId The highest event ID acknowledged
     */
    void ackReceived(const QString& clientId, qint64 eventId);

    /**
     * @brief Emitted when client disconnects
     * @param clientId The client identifier
     */
    void clientDisconnected(const QString& clientId);

    /**
     * @brief Emitted on socket errors
     * @param clientId The client identifier
     * @param error Error description
     */
    void socketError(const QString& clientId, const QString& error);

    /**
     * @brief Emitted when initialization is complete
     */
    void initialized();

private slots:
    void onReadyRead();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);

private:
    void processMessage(const QByteArray& line);

    qintptr         socketDescriptor_;
    QTcpSocket*     socket_ = nullptr;
    QString         clientId_;
    QString         subscriptionMode_;
    qint64          lastEventId_;
    QByteArray      buffer_;
};

} // namespace pubsub

#endif // PUBSUB_PERMANENT_WORKER_H
