#ifndef PUBSUB_EVENT_PUBLISHER_H
#define PUBSUB_EVENT_PUBLISHER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QMutex>
#include <QHash>
#include <QTimer>
#include <QSocketNotifier>
#include <QSqlDatabase>

#include "core/db/types.h"
#include "permanent_worker.h"

namespace pubsub {

/**
 * @brief Manages pub/sub event distribution to subscribers
 * 
 * Design:
 * - Uses PostgreSQL LISTEN/NOTIFY for wake-up signals
 * - Maintains event_log in PostgreSQL for durability
 * - Supports two connection modes:
 *   - permanent: keep-alive TCP with dedicated QThread per client
 *   - one_shot: TCP callback to client's server
 * - Supports two subscription modes:
 *   - full: complete row data in payload
 *   - notify_only: only row ID
 * 
 * Threading:
 * - Main thread: LISTEN/NOTIFY + subscriber accept + registry management
 * - Per permanent client: dedicated QThread with PermanentWorker
 * - One-shot deliveries: QThreadPool tasks
 */
class EventPublisher : public QObject {
    Q_OBJECT

public:
    explicit EventPublisher(const core::AppConfig& config, QObject* parent = nullptr);
    ~EventPublisher() override;

    /**
     * @brief Start the publisher
     * @param subscribePort Port to listen for subscribe requests
     * @return true if started successfully
     */
    bool start(quint16 subscribePort);

    /**
     * @brief Stop the publisher and cleanup
     */
    void stop();

    /**
     * @brief Check if publisher is running
     */
    bool isRunning() const { return running_; }

    /**
     * @brief Manually emit a bulk_import_finished event
     * 
     * Called by DbService after bulk import completes.
     * 
     * @param tableName "items" or "boxes"
     * @param lineId Production line ID
     * @param rowsAffected Number of rows imported
     */
    void emitBulkImportFinished(const QString& tableName, 
                                 qint64 lineId, 
                                 int rowsAffected);

    /**
     * @brief Get current subscriber count
     */
    int subscriberCount() const;

signals:
    /**
     * @brief Emitted when a new subscriber connects
     */
    void subscriberConnected(const QString& clientId);

    /**
     * @brief Emitted when a subscriber disconnects
     */
    void subscriberDisconnected(const QString& clientId);

    /**
     * @brief Emitted on errors
     */
    void errorOccurred(const QString& error);

private slots:
    void onNewSubscriberConnection();
    void onNotify();
    void onPingTimer();
    void onAckReceived(const QString& clientId, qint64 eventId);
    void onClientDisconnected(const QString& clientId);

private:
    // Internal subscriber state (extends SubscriberInfo with runtime data)
    struct Subscriber : public core::SubscriberInfo {
        QThread*          thread = nullptr;
        PermanentWorker*  worker = nullptr;
    };

    bool setupListenNotify();
    void cleanupListenNotify();
    bool loadSubscribersFromDb();
    void upsertSubscriber(const QString& clientId, const QString& connMode,
                          const QString& subMode, const QString& callbackHost,
                          qint64 resumeFrom);
    void updateLastEventId(const QString& clientId, qint64 eventId);

    void handleSubscribeRequest(QTcpSocket* socket);
    void setupPermanentSubscriber(Subscriber* sub, QTcpSocket* socket);
    void replayEvents(Subscriber* sub, qint64 fromEventId);
    void replayEventsBatch(Subscriber* sub, qint64 fromEventId, int batchNum, int batchCount);

    void fetchAndDeliver();
    void deliverOneShot(Subscriber* sub, const core::Event& event);

    QSqlDatabase getWorkerDatabase();

    // Configuration
    core::AppConfig config_;
    QString         connectionString_;

    // LISTEN/NOTIFY (libpq) or polling fallback
    void*             listenConn_ = nullptr;   // PGconn* when libpq available
    QSocketNotifier*  notifier_ = nullptr;
    QTimer*           pollTimer_ = nullptr;   // Fallback when libpq unavailable

    // Subscribe server
    QTcpServer*       subscribeServer_ = nullptr;

    // Subscribers
    QHash<QString, Subscriber*> subscribers_;
    mutable QMutex              mutex_;

    // One-shot delivery pool
    QThreadPool*      oneShotPool_ = nullptr;

    // Heartbeat timer
    QTimer*           pingTimer_ = nullptr;

    // State
    bool              running_ = false;
};

} // namespace pubsub

#endif // PUBSUB_EVENT_PUBLISHER_H
