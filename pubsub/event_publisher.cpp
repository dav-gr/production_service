#include "event_publisher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QtConcurrent>

// PostgreSQL libpq for LISTEN/NOTIFY (optional, falls back to polling)
// Define PUBSUB_USE_LIBPQ=1 to enable native LISTEN/NOTIFY
#if defined(PUBSUB_USE_LIBPQ) && PUBSUB_USE_LIBPQ
    #ifdef _WIN32
        #pragma comment(lib, "libpq.lib")
    #endif
    extern "C" {
        #include <libpq-fe.h>
    }
    #define HAS_LIBPQ 1
#else
    // libpq not available - will use polling mode
    #define HAS_LIBPQ 0
#endif

namespace pubsub {

// ============================================================================
// Construction / Destruction
// ============================================================================

EventPublisher::EventPublisher(const core::AppConfig& config, QObject* parent)
    : QObject(parent)
    , config_(config)
{
    // Build libpq connection string
    connectionString_ = QString("host=%1 port=%2 dbname=%3 user=%4 password=%5")
        .arg(config.host)
        .arg(config.port)
        .arg(config.database)
        .arg(config.user)
        .arg(config.password);

    // Register metatype for cross-thread signal/slot
    qRegisterMetaType<core::Event>("core::Event");
}

EventPublisher::~EventPublisher() {
    stop();
}

// ============================================================================
// Start / Stop
// ============================================================================

bool EventPublisher::start(quint16 subscribePort) {
    if (running_) {
        qWarning() << "EventPublisher: Already running";
        return false;
    }

    qDebug() << "EventPublisher: Starting on port" << subscribePort;

    // Setup LISTEN/NOTIFY connection
    if (!setupListenNotify()) {
        emit errorOccurred("Failed to setup LISTEN/NOTIFY connection");
        return false;
    }

    // Create thread pool for one-shot deliveries
    oneShotPool_ = new QThreadPool(this);
    oneShotPool_->setMaxThreadCount(4);

    // Create subscribe server
    subscribeServer_ = new QTcpServer(this);
    connect(subscribeServer_, &QTcpServer::newConnection,
            this, &EventPublisher::onNewSubscriberConnection);

    if (!subscribeServer_->listen(QHostAddress::Any, subscribePort)) {
        emit errorOccurred("Failed to listen on port " + QString::number(subscribePort));
        cleanupListenNotify();
        return false;
    }

    // Load existing subscribers from database
    loadSubscribersFromDb();

    // Setup heartbeat timer (30 seconds)
    pingTimer_ = new QTimer(this);
    connect(pingTimer_, &QTimer::timeout, this, &EventPublisher::onPingTimer);
    pingTimer_->start(30000);

    running_ = true;
    qDebug() << "EventPublisher: Started successfully";
    return true;
}

void EventPublisher::stop() {
    if (!running_) return;

    qDebug() << "EventPublisher: Stopping...";

    running_ = false;

    // Stop ping timer
    if (pingTimer_) {
        pingTimer_->stop();
        delete pingTimer_;
        pingTimer_ = nullptr;
    }

    // Stop subscribe server
    if (subscribeServer_) {
        subscribeServer_->close();
        delete subscribeServer_;
        subscribeServer_ = nullptr;
    }

    // Stop all permanent workers
    {
        QMutexLocker lk(&mutex_);
        for (auto* sub : subscribers_) {
            if (sub->thread) {
                sub->thread->quit();
                sub->thread->wait(5000);
                delete sub->thread;
            }
            delete sub;
        }
        subscribers_.clear();
    }

    // Wait for one-shot tasks
    if (oneShotPool_) {
        oneShotPool_->waitForDone(5000);
        delete oneShotPool_;
        oneShotPool_ = nullptr;
    }

    // Cleanup LISTEN/NOTIFY
    cleanupListenNotify();

    qDebug() << "EventPublisher: Stopped";
}

// ============================================================================
// LISTEN/NOTIFY Setup
// ============================================================================

bool EventPublisher::setupListenNotify() {
#if HAS_LIBPQ
    PGconn* conn = PQconnectdb(connectionString_.toUtf8().constData());
    listenConn_ = conn;

    if (PQstatus(conn) != CONNECTION_OK) {
        qWarning() << "EventPublisher: LISTEN connection failed:" 
                   << PQerrorMessage(conn);
        PQfinish(conn);
        listenConn_ = nullptr;
        // Fall through to polling mode
    } else {
        // Set non-blocking mode
        PQsetnonblocking(conn, 1);

        // Execute LISTEN command
        PGresult* res = PQexec(conn, "LISTEN db_events");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            qWarning() << "EventPublisher: LISTEN failed:" << PQerrorMessage(conn);
            PQclear(res);
            PQfinish(conn);
            listenConn_ = nullptr;
        } else {
            PQclear(res);

            // Get socket file descriptor and create notifier
            int fd = PQsocket(conn);
            if (fd >= 0) {
                notifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
                connect(notifier_, &QSocketNotifier::activated, this, &EventPublisher::onNotify);
                qDebug() << "EventPublisher: LISTEN/NOTIFY setup complete, fd=" << fd;
                return true;
            } else {
                qWarning() << "EventPublisher: Invalid socket from libpq";
                PQfinish(conn);
                listenConn_ = nullptr;
            }
        }
    }
#endif

    // Fallback: Use polling mode (check for new events every 500ms)
    qDebug() << "EventPublisher: Using polling mode (libpq not available or failed)";
    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &EventPublisher::fetchAndDeliver);
    pollTimer_->start(500);
    return true;
}

void EventPublisher::cleanupListenNotify() {
    if (pollTimer_) {
        pollTimer_->stop();
        delete pollTimer_;
        pollTimer_ = nullptr;
    }

    if (notifier_) {
        notifier_->setEnabled(false);
        delete notifier_;
        notifier_ = nullptr;
    }

#if HAS_LIBPQ
    if (listenConn_) {
        PQfinish(static_cast<PGconn*>(listenConn_));
        listenConn_ = nullptr;
    }
#endif
}

// ============================================================================
// Notification Handler
// ============================================================================

void EventPublisher::onNotify() {
#if HAS_LIBPQ
    if (!listenConn_) return;

    PGconn* conn = static_cast<PGconn*>(listenConn_);

    // Consume any pending input
    PQconsumeInput(conn);

    // Process all notifications
    while (PGnotify* n = PQnotifies(conn)) {
        qDebug() << "EventPublisher: Received NOTIFY on channel" << n->relname;
        PQfreemem(n);
    }
#endif

    // Fetch and deliver events
    fetchAndDeliver();
}

// ============================================================================
// Fetch and Deliver Events
// ============================================================================

void EventPublisher::fetchAndDeliver() {
    qint64 minId = 0;
    QList<Subscriber*> activeSubscribers;

    {
        QMutexLocker lk(&mutex_);
        for (auto* sub : subscribers_) {
            if (minId == 0 || sub->lastEventId < minId) {
                minId = sub->lastEventId;
            }
            activeSubscribers.append(sub);
        }
    }

    if (activeSubscribers.isEmpty()) {
        return;
    }

    // Query events from database
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) {
        qWarning() << "EventPublisher: Database not available for fetchAndDeliver";
        return;
    }

    QSqlQuery q(db);
    q.prepare(
        "SELECT id, table_name, event_type, row_id, payload, created_at "
        "FROM event_log WHERE id > :min_id "
        "ORDER BY id LIMIT 1000"
    );
    q.bindValue(":min_id", minId);

    if (!q.exec()) {
        qWarning() << "EventPublisher: Failed to fetch events:" << q.lastError().text();
        return;
    }

    QVector<core::Event> events;
    while (q.next()) {
        core::Event e;
        e.id        = q.value(0).toLongLong();
        e.table     = q.value(1).toString();
        e.type      = q.value(2).toString();
        e.rowId     = q.value(3).toLongLong();
        e.payload   = QJsonDocument::fromJson(q.value(4).toByteArray()).object();
        e.createdAt = q.value(5).toDateTime();
        events.append(e);
    }

    if (events.isEmpty()) {
        return;
    }

    qDebug() << "EventPublisher: Fetched" << events.size() << "events to deliver";

    // Deliver to each subscriber
    QMutexLocker lk(&mutex_);
    for (auto* sub : activeSubscribers) {
        for (const core::Event& e : events) {
            if (e.id <= sub->lastEventId) continue;

            if (sub->connectionMode == "permanent" && sub->worker) {
                QMetaObject::invokeMethod(
                    sub->worker, "deliverEvent",
                    Qt::QueuedConnection,
                    Q_ARG(core::Event, e)
                );
            } else if (sub->connectionMode == "one_shot") {
                deliverOneShot(sub, e);
            }
        }
    }
}

// ============================================================================
// One-Shot Delivery
// ============================================================================

void EventPublisher::deliverOneShot(Subscriber* sub, const core::Event& event) {
    QString host = sub->callbackHost;
    QString mode = sub->subscriptionMode;
    QString clientId = sub->clientId;
    qint64 eventId = event.id;

    // Capture event data for lambda
    QString table = event.table;
    QString type = event.type;
    qint64 rowId = event.rowId;
    QJsonObject payload = event.payload;
    QDateTime createdAt = event.createdAt;

    // Capture 'this' explicitly to avoid MSVC C3791
    EventPublisher* self = this;

    QtConcurrent::run(oneShotPool_, [=]() {
        QStringList parts = host.split(':');
        if (parts.size() != 2) {
            qWarning() << "EventPublisher: Invalid callback_host:" << host;
            return;
        }

        QTcpSocket socket;
        socket.connectToHost(parts[0], parts[1].toUShort());
        
        if (!socket.waitForConnected(5000)) {
            qWarning() << "EventPublisher: One-shot connect failed to" << host;
            return;
        }

        // Build event message
        QJsonObject msg;
        msg["cmd"]        = QStringLiteral("event");
        msg["event_id"]   = eventId;
        msg["table"]      = table;
        msg["type"]       = type;
        msg["row_id"]     = rowId;
        msg["created_at"] = createdAt.toString(Qt::ISODate);

        if (mode == "full") {
            msg["payload"] = payload;
        } else {
            QJsonObject minPayload;
            minPayload["id"] = rowId;
            msg["payload"] = minPayload;
        }

        socket.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
        socket.flush();

        // Wait for ACK
        if (socket.waitForReadyRead(10000)) {
            QByteArray response = socket.readLine();
            QJsonDocument doc = QJsonDocument::fromJson(response);
            QJsonObject obj = doc.object();

            if (obj["cmd"].toString() == "ack") {
                qint64 ackId = obj["ack"].toInteger();
                qDebug() << "EventPublisher: One-shot ACK received for event" << ackId;
                
                // Update last_event_id (thread-safe)
                QMetaObject::invokeMethod(self, [self, clientId, ackId]() {
                    self->onAckReceived(clientId, ackId);
                }, Qt::QueuedConnection);
            }
        } else {
            qWarning() << "EventPublisher: One-shot ACK timeout for" << clientId;
        }

        socket.disconnectFromHost();
    });
}

// ============================================================================
// Subscribe Handling
// ============================================================================

void EventPublisher::onNewSubscriberConnection() {
    while (subscribeServer_->hasPendingConnections()) {
        QTcpSocket* socket = subscribeServer_->nextPendingConnection();
        
        // Read the subscribe request (with timeout)
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleSubscribeRequest(socket);
        });

        // Timeout after 10 seconds
        QTimer::singleShot(10000, socket, [socket]() {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                qWarning() << "EventPublisher: Subscribe timeout, closing socket";
                socket->disconnectFromHost();
            }
        });
    }
}

void EventPublisher::handleSubscribeRequest(QTcpSocket* socket) {
    if (!socket->canReadLine()) return;

    // Disconnect readyRead to avoid re-entry
    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);

    QByteArray line = socket->readLine().trimmed();
    QJsonDocument doc = QJsonDocument::fromJson(line);
    QJsonObject req = doc.object();

    QString cmd = req["cmd"].toString();
    if (cmd != "subscribe") {
        QJsonObject resp;
        resp["status"] = "error";
        resp["message"] = "Expected subscribe command";
        socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        socket->disconnectFromHost();
        return;
    }

    QString clientId = req["client_id"].toString();
    QString connMode = req["connection_mode"].toString();
    QString subMode  = req["subscription_mode"].toString();
    qint64 resumeFrom = req["resume_from"].toInteger();
    QString callbackHost = req["callback_host"].toString();

    // Validate
    if (clientId.isEmpty() || 
        (connMode != "permanent" && connMode != "one_shot") ||
        (subMode != "full" && subMode != "notify_only")) {
        QJsonObject resp;
        resp["status"] = "error";
        resp["message"] = "Invalid subscribe parameters";
        socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        socket->disconnectFromHost();
        return;
    }

    if (connMode == "one_shot" && callbackHost.isEmpty()) {
        QJsonObject resp;
        resp["status"] = "error";
        resp["message"] = "callback_host required for one_shot mode";
        socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        socket->disconnectFromHost();
        return;
    }

    qDebug() << "EventPublisher: Subscribe request from" << clientId 
             << "mode:" << connMode << "/" << subMode;

    // Upsert subscriber in database
    upsertSubscriber(clientId, connMode, subMode, callbackHost, resumeFrom);

    // Create/update subscriber in memory
    QMutexLocker lk(&mutex_);

    // Clean up existing subscriber if present
    if (Subscriber* existing = subscribers_.value(clientId)) {
        if (existing->thread) {
            existing->thread->quit();
            existing->thread->wait(1000);
            delete existing->thread;
        }
        delete existing;
        subscribers_.remove(clientId);
    }

    Subscriber* sub = new Subscriber();
    sub->clientId = clientId;
    sub->connectionMode = connMode;
    sub->subscriptionMode = subMode;
    sub->callbackHost = callbackHost;
    sub->lastEventId = resumeFrom;

    subscribers_.insert(clientId, sub);

    if (connMode == "permanent") {
        // Send OK response first
        QJsonObject resp;
        resp["status"] = "ok";
        resp["message"] = "Subscribed in permanent mode";
        socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        socket->flush();

        // Setup permanent worker (transfers socket ownership)
        setupPermanentSubscriber(sub, socket->socketDescriptor());
        
        // Detach socket from this thread (will be recreated in worker thread)
        socket->setParent(nullptr);
        socket->deleteLater();

        // Replay events from resume_from
        lk.unlock();
        replayEvents(sub, resumeFrom);

    } else {
        // one_shot mode: just store and close
        QJsonObject resp;
        resp["status"] = "ok";
        resp["message"] = "Subscribed in one_shot mode";
        socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        socket->flush();
        socket->disconnectFromHost();
    }

    emit subscriberConnected(clientId);
}

void EventPublisher::setupPermanentSubscriber(Subscriber* sub, qintptr socketDescriptor) {
    QThread* thread = new QThread();
    PermanentWorker* worker = new PermanentWorker(
        socketDescriptor,
        sub->clientId,
        sub->subscriptionMode,
        sub->lastEventId
    );

    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &PermanentWorker::initialize);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &PermanentWorker::ackReceived, this, &EventPublisher::onAckReceived);
    connect(worker, &PermanentWorker::clientDisconnected, this, &EventPublisher::onClientDisconnected);

    sub->thread = thread;
    sub->worker = worker;

    thread->start();
}

void EventPublisher::replayEvents(Subscriber* sub, qint64 fromEventId) {
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(
        "SELECT id, table_name, event_type, row_id, payload, created_at "
        "FROM event_log WHERE id > :from_id "
        "ORDER BY id LIMIT 1000"
    );
    q.bindValue(":from_id", fromEventId);

    if (!q.exec()) {
        qWarning() << "EventPublisher: Failed to replay events:" << q.lastError().text();
        return;
    }

    QMutexLocker lk(&mutex_);
    while (q.next()) {
        core::Event e;
        e.id        = q.value(0).toLongLong();
        e.table     = q.value(1).toString();
        e.type      = q.value(2).toString();
        e.rowId     = q.value(3).toLongLong();
        e.payload   = QJsonDocument::fromJson(q.value(4).toByteArray()).object();
        e.createdAt = q.value(5).toDateTime();

        if (sub->worker) {
            QMetaObject::invokeMethod(
                sub->worker, "deliverEvent",
                Qt::QueuedConnection,
                Q_ARG(core::Event, e)
            );
        }
    }
}

// ============================================================================
// ACK and Disconnect Handling
// ============================================================================

void EventPublisher::onAckReceived(const QString& clientId, qint64 eventId) {
    updateLastEventId(clientId, eventId);

    QMutexLocker lk(&mutex_);
    if (Subscriber* sub = subscribers_.value(clientId)) {
        sub->lastEventId = qMax(sub->lastEventId, eventId);
    }
}

void EventPublisher::onClientDisconnected(const QString& clientId) {
    qDebug() << "EventPublisher: Client disconnected:" << clientId;

    {
        QMutexLocker lk(&mutex_);
        if (Subscriber* sub = subscribers_.value(clientId)) {
            if (sub->thread) {
                sub->thread->quit();
                // Don't wait here, let it clean up asynchronously
            }
            // Keep subscriber in memory for reconnection
            sub->thread = nullptr;
            sub->worker = nullptr;
        }
    }

    emit subscriberDisconnected(clientId);
}

// ============================================================================
// Heartbeat
// ============================================================================

void EventPublisher::onPingTimer() {
    QMutexLocker lk(&mutex_);
    for (auto* sub : subscribers_) {
        if (sub->connectionMode == "permanent" && sub->worker) {
            QMetaObject::invokeMethod(sub->worker, "sendPing", Qt::QueuedConnection);
        }
    }
}

// ============================================================================
// Database Operations
// ============================================================================

bool EventPublisher::loadSubscribersFromDb() {
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.exec("SELECT client_id, connection_mode, subscription_mode, callback_host, last_event_id "
           "FROM subscribers");

    QMutexLocker lk(&mutex_);
    while (q.next()) {
        Subscriber* sub = new Subscriber();
        sub->clientId = q.value(0).toString();
        sub->connectionMode = q.value(1).toString();
        sub->subscriptionMode = q.value(2).toString();
        sub->callbackHost = q.value(3).toString();
        sub->lastEventId = q.value(4).toLongLong();

        subscribers_.insert(sub->clientId, sub);
        qDebug() << "EventPublisher: Loaded subscriber" << sub->clientId 
                 << "lastEventId:" << sub->lastEventId;
    }

    return true;
}

void EventPublisher::upsertSubscriber(const QString& clientId, const QString& connMode,
                                       const QString& subMode, const QString& callbackHost,
                                       qint64 resumeFrom) {
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO subscribers (client_id, connection_mode, subscription_mode, "
        "callback_host, last_event_id, updated_at) "
        "VALUES (:cid, :conn, :sub, :cb, :last, now()) "
        "ON CONFLICT (client_id) DO UPDATE SET "
        "connection_mode = EXCLUDED.connection_mode, "
        "subscription_mode = EXCLUDED.subscription_mode, "
        "callback_host = EXCLUDED.callback_host, "
        "last_event_id = GREATEST(subscribers.last_event_id, EXCLUDED.last_event_id), "
        "updated_at = now()"
    );
    q.bindValue(":cid", clientId);
    q.bindValue(":conn", connMode);
    q.bindValue(":sub", subMode);
    q.bindValue(":cb", callbackHost.isEmpty() ? QVariant() : callbackHost);
    q.bindValue(":last", resumeFrom);

    if (!q.exec()) {
        qWarning() << "EventPublisher: Failed to upsert subscriber:" << q.lastError().text();
    }
}

void EventPublisher::updateLastEventId(const QString& clientId, qint64 eventId) {
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(
        "UPDATE subscribers SET last_event_id = GREATEST(last_event_id, :eid), "
        "updated_at = now() WHERE client_id = :cid"
    );
    q.bindValue(":eid", eventId);
    q.bindValue(":cid", clientId);

    if (!q.exec()) {
        qWarning() << "EventPublisher: Failed to update last_event_id:" << q.lastError().text();
    }
}

QSqlDatabase EventPublisher::getWorkerDatabase() {
    QString connName = "pubsub_" + QString::number(reinterpret_cast<quintptr>(QThread::currentThread()));
    
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName);
        if (db.isOpen()) return db;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connName);
    db.setHostName(config_.host);
    db.setPort(config_.port);
    db.setDatabaseName(config_.database);
    db.setUserName(config_.user);
    db.setPassword(config_.password);

    if (!db.open()) {
        qWarning() << "EventPublisher: Failed to open database:" << db.lastError().text();
    }

    return db;
}

// ============================================================================
// Public API
// ============================================================================

void EventPublisher::emitBulkImportFinished(const QString& tableName,
                                             qint64 lineId,
                                             int rowsAffected) {
    QSqlDatabase db = getWorkerDatabase();
    if (!db.isOpen()) return;

    QJsonObject payload;
    payload["rows_affected"] = rowsAffected;
    payload["production_line"] = lineId;

    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO event_log (table_name, event_type, payload) "
        "VALUES (:table, 'bulk_import_finished', :payload)"
    );
    q.bindValue(":table", tableName);
    q.bindValue(":payload", QJsonDocument(payload).toJson(QJsonDocument::Compact));

    if (!q.exec()) {
        qWarning() << "EventPublisher: Failed to emit bulk_import_finished:" << q.lastError().text();
        return;
    }

    // Trigger NOTIFY
    QSqlQuery notify(db);
    notify.exec("SELECT pg_notify('db_events', '')");

    qDebug() << "EventPublisher: Emitted bulk_import_finished for" << tableName 
             << "rows:" << rowsAffected;
}

int EventPublisher::subscriberCount() const {
    QMutexLocker lk(&mutex_);
    return subscribers_.size();
}

} // namespace pubsub
