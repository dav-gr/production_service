#ifndef EXAMPLES_CLIENT_H
#define EXAMPLES_CLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTcpServer>
#include <QTimer>
#include <QJsonObject>

/**
 * @brief Example pub/sub client demonstrating both connection modes
 * 
 * Permanent mode:
 *   Keeps a TCP connection open to the server. Receives events as they arrive,
 *   responds to pings, and sends ACKs.
 * 
 * One-shot mode:
 *   Registers with the server (providing a callback host:port), then listens
 *   on that port. The server opens a new TCP connection for each event delivery.
 */
class ExampleClient : public QObject {
    Q_OBJECT

public:
    explicit ExampleClient(QObject* parent = nullptr);
    ~ExampleClient() override;

    /**
     * @brief Connect in permanent mode
     * @param serverHost Publisher server host
     * @param serverPort Publisher server port
     * @param clientId Unique client identifier
     * @param subscriptionMode "full" or "notify_only"
     * @param resumeFrom Resume from this event ID (0 = all events)
     */
    void connectPermanent(const QString& serverHost, quint16 serverPort,
                          const QString& clientId, const QString& subscriptionMode,
                          qint64 resumeFrom = 0);

    /**
     * @brief Connect in one-shot mode
     * @param serverHost Publisher server host
     * @param serverPort Publisher server port
     * @param clientId Unique client identifier
     * @param subscriptionMode "full" or "notify_only"
     * @param callbackPort Local port to listen for callbacks
     * @param resumeFrom Resume from this event ID (0 = all events)
     */
    void connectOneShot(const QString& serverHost, quint16 serverPort,
                        const QString& clientId, const QString& subscriptionMode,
                        quint16 callbackPort, qint64 resumeFrom = 0);

signals:
    void connected();
    void disconnected();
    void eventReceived(const QJsonObject& event);
    void errorOccurred(const QString& error);

private slots:
    // Permanent mode
    void onPermanentConnected();
    void onPermanentReadyRead();
    void onPermanentDisconnected();
    void onPermanentError(QAbstractSocket::SocketError error);
    void onReconnectTimer();

    // One-shot mode
    void onOneShotRegistered();
    void onCallbackConnection();

private:
    void processMessage(const QJsonObject& msg);
    void sendAck(QTcpSocket* socket, qint64 eventId);
    void sendBatchAck(QTcpSocket* socket, const QVector<qint64>& eventIds);
    void flushPendingAcks();
    void logEvent(const QJsonObject& msg);
    void saveResumeState();
    static qint64 loadResumeState(const QString& clientId);

    // Connection info
    QString serverHost_;
    quint16 serverPort_ = 0;
    QString clientId_;
    QString connectionMode_;
    QString subscriptionMode_;
    quint16 callbackPort_ = 0;
    qint64  resumeFrom_ = 0;
    qint64  lastEventId_ = 0;

    // Permanent mode
    QTcpSocket* socket_ = nullptr;
    QTimer*     reconnectTimer_ = nullptr;
    QTimer*     ackFlushTimer_ = nullptr;
    QByteArray  buffer_;

    // Batched ACK
    QVector<qint64> pendingAcks_;
    static constexpr int ACK_BATCH_SIZE = 100;

    // One-shot mode
    QTcpServer* callbackServer_ = nullptr;

    // Stats
    int eventsReceived_ = 0;
    int lastLoggedCount_ = 0;
};

#endif // EXAMPLES_CLIENT_H
