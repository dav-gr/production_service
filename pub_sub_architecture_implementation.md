# Pub/Sub Architecture — Implementation Specification

**production_service ? production_line event subscription system**

---

# 1. Overview

Extend the existing `production_service` TCP server with a pub/sub capability so the
**production_line** desktop app receives real-time notifications when `items` or `boxes`
change (insert, delete, mark as deleted, status 1?0, status 2?0, bulk import finished).

**Default ports:**
- Request/Response server: **8080**
- Pub/Sub event publisher: **9000**

**Design pillars:**
- Durable `event_log` table — source of truth, survives crashes
- PostgreSQL `LISTEN/NOTIFY` — lightweight wake-up only (no payload)
- Session-scoped `app.bulk_import` flag — suppresses per-row triggers during imports
- Soft-delete via `is_deleted` column — items/boxes can be marked as deleted without hard removal
- Two **connection modes**: permanent (keep-alive TCP) and one-shot (TCP callback)
- Two **subscription modes**: full (row data in payload) and notify_only (id only)
- Multi-threaded delivery — dedicated QThread per permanent client, QThreadPool for one-shot
- Resume from `last_event_id` after disconnect/crash

---

# 2. Scope

| Concern | Detail |
|---------|--------|
| Monitored tables | `items`, `boxes` |
| Event types | `insert`, `delete`, `marked_as_deleted`, `status_to_0`, `bulk_import_finished` |
| Status transitions | Only **1?0** and **2?0** |
| Subscribers | 1 initially (production_line), scales to a handful |
| Protocol | JSON over raw TCP (newline-delimited), same style as existing `server::Protocol` |

---

# 3. Database Changes

### 3.1 `event_log`

```sql
CREATE TABLE event_log (
    id          BIGSERIAL PRIMARY KEY,
    table_name  TEXT NOT NULL,
    event_type  TEXT NOT NULL,
    row_id      BIGINT,
    payload     JSONB DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_event_log_id      ON event_log(id);
CREATE INDEX idx_event_log_table   ON event_log(table_name);
```

### 3.2 `subscribers`

```sql
CREATE TABLE subscribers (
    id                SERIAL PRIMARY KEY,
    client_id         TEXT UNIQUE NOT NULL,
    connection_mode   TEXT NOT NULL,          -- 'permanent' | 'one_shot'
    subscription_mode TEXT NOT NULL,          -- 'full' | 'notify_only'
    callback_host     TEXT,                   -- ip:port for one_shot
    last_event_id     BIGINT DEFAULT 0 NOT NULL,
    updated_at        TIMESTAMPTZ DEFAULT now()
);
```

### 3.3 Schema changes — soft-delete column

Add `is_deleted` to `items` and `boxes`:

```sql
ALTER TABLE items ADD COLUMN is_deleted BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE boxes ADD COLUMN is_deleted BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX idx_items_not_deleted ON items(production_line, status) WHERE NOT is_deleted;
CREATE INDEX idx_boxes_not_deleted ON boxes(production_line, status) WHERE NOT is_deleted;
```

**`is_deleted = TRUE`** means the item/box is logically removed.  
The row remains in the database for audit trail and export history.  
All existing queries that list available items/boxes must add `AND NOT is_deleted`.

### 3.4 Trigger function

Attached to `items` and `boxes`. Skipped when `app.bulk_import = 'on'`.

```sql
CREATE OR REPLACE FUNCTION log_items_boxes_events()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    bulk text;
    p    jsonb;
BEGIN
    bulk := COALESCE(current_setting('app.bulk_import', true), 'off');
    IF bulk = 'on' THEN
        RETURN COALESCE(NEW, OLD);
    END IF;

    IF TG_OP = 'INSERT' THEN
        p := jsonb_build_object(
            'id', NEW.id, 'bar_code', NEW.bar_code,
            'status', NEW.status, 'production_line', NEW.production_line,
            'imported_at', NEW.imported_at);
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'insert', NEW.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'DELETE' THEN
        p := jsonb_build_object('id', OLD.id, 'bar_code', OLD.bar_code);
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'delete', OLD.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'UPDATE' THEN
        -- Soft-delete: is_deleted toggled FALSE ? TRUE
        IF NOT OLD.is_deleted AND NEW.is_deleted THEN
            p := jsonb_build_object(
                'id', NEW.id, 'bar_code', NEW.bar_code,
                'status', NEW.status, 'production_line', NEW.production_line);
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'marked_as_deleted', NEW.id, p);
            PERFORM pg_notify('db_events', '');

        -- Status rollback: 1?0 or 2?0
        ELSIF OLD.status IN (1,2) AND NEW.status = 0 THEN
            p := jsonb_build_object(
                'id', NEW.id, 'bar_code', NEW.bar_code,
                'old_status', OLD.status, 'new_status', NEW.status,
                'production_line', NEW.production_line);
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'status_to_0', NEW.id, p);
            PERFORM pg_notify('db_events', '');
        END IF;
    END IF;

    RETURN COALESCE(NEW, OLD);
END;
$$;

CREATE TRIGGER trg_items_event
    AFTER INSERT OR UPDATE OR DELETE ON items
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();

CREATE TRIGGER trg_boxes_event
    AFTER INSERT OR UPDATE OR DELETE ON boxes
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();
```

---

# 4. Bulk Import Integration

Update the existing `DbService::doImport()` to wrap the work with the session flag:

```cpp
// before batch loop
QSqlQuery(db).exec("SET app.bulk_import = 'on'");

// ... existing batch INSERT logic ...

// after commit
QSqlQuery fin(db);
fin.prepare("INSERT INTO event_log(table_name, event_type, payload) "
            "VALUES (:t, 'bulk_import_finished', :p)");
fin.bindValue(":t", tableName);
QJsonObject p;
p["rows_affected"] = result.importedCount;
p["production_line"] = lineId;
fin.bindValue(":p", QJsonDocument(p).toJson(QJsonDocument::Compact));
fin.exec();

QSqlQuery(db).exec("SELECT pg_notify('db_events','')");
QSqlQuery(db).exec("SET app.bulk_import = 'off'");
```

No new tables, no schema changes beyond section 3.

---

# 5. Threading Model

```
Main thread
?? QSocketNotifier on libpq LISTEN fd  ?  fetchAndDeliver()
?? QTcpServer (subscriber port)        ?  accept subscribe messages
?? SubscriberRegistry (QMutex-guarded map)

Per permanent subscriber
?? QThread  ?  PermanentWorker  ?  owns QTcpSocket, reads ACKs

One-shot deliveries
?? QThreadPool  ?  QtConcurrent::run()  ?  create QTcpSocket, send, wait ACK, close
```

**Why separate threads?**
- A permanent subscriber's socket I/O must not block the main event loop.
- One-shot deliveries are fire-and-forget tasks that fit a thread pool naturally.
- The LISTEN notifier stays on the main thread — it fires rarely and does only a SELECT.

---

# 6. Server Components

## 6.1 Event struct

```cpp
struct Event {
    qint64      id;
    QString     table;      // "items" | "boxes"
    QString     type;       // "insert" | "delete" | "marked_as_deleted" | "status_to_0" | "bulk_import_finished"
    qint64      rowId;
    QJsonObject payload;
    QDateTime   createdAt;
};
Q_DECLARE_METATYPE(Event)
```

## 6.2 SubscriberInfo

```cpp
struct SubscriberInfo {
    QString  clientId;
    QString  connectionMode;    // "permanent" | "one_shot"
    QString  subscriptionMode;  // "full" | "notify_only"
    QString  callbackHost;      // "ip:port"  — used only for one_shot
    qint64   lastEventId = 0;

    // permanent mode only
    QThread*                thread = nullptr;
    PermanentWorker*        worker = nullptr;
};
```

## 6.3 LISTEN/NOTIFY wiring

```cpp
bool EventPublisher::startListening() {
    listenConn_ = PQconnectdb(connStr_.toUtf8().constData());
    if (PQstatus(listenConn_) != CONNECTION_OK) return false;

    PQexec(listenConn_, "LISTEN db_events");
    int fd = PQsocket(listenConn_);

    notifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, &EventPublisher::onNotify);
    return true;
}

void EventPublisher::onNotify() {
    PQconsumeInput(listenConn_);
    while (PGnotify* n = PQnotifies(listenConn_)) {
        PQfreemem(n);
    }
    fetchAndDeliver();
}
```

## 6.4 Fetch and deliver

```cpp
void EventPublisher::fetchAndDeliver() {
    qint64 minId = 0;
    {
        QMutexLocker lk(&mutex_);
        for (auto* s : subscribers_)
            minId = (minId == 0) ? s->lastEventId
                                 : qMin(minId, s->lastEventId);
    }

    QSqlQuery q(workerDb_);
    q.prepare("SELECT id, table_name, event_type, row_id, payload, created_at "
              "FROM event_log WHERE id > :min "
              "ORDER BY id LIMIT 1000");
    q.bindValue(":min", minId);
    if (!q.exec()) return;

    QVector<Event> batch;
    while (q.next()) {
        Event e;
        e.id        = q.value(0).toLongLong();
        e.table     = q.value(1).toString();
        e.type      = q.value(2).toString();
        e.rowId     = q.value(3).toLongLong();
        e.payload   = QJsonDocument::fromJson(
                          q.value(4).toByteArray()).object();
        e.createdAt = q.value(5).toDateTime();
        batch.append(e);
    }

    QMutexLocker lk(&mutex_);
    for (auto* sub : subscribers_) {
        for (const Event& e : batch) {
            if (e.id <= sub->lastEventId) continue;

            if (sub->connectionMode == "permanent" && sub->worker) {
                QMetaObject::invokeMethod(
                    sub->worker, "deliverEvent",
                    Qt::QueuedConnection, Q_ARG(Event, e));
            } else if (sub->connectionMode == "one_shot") {
                deliverOneShot(sub, e);
            }
        }
    }
}
```

## 6.5 PermanentWorker (runs in its own QThread)

```cpp
class PermanentWorker : public QObject {
    Q_OBJECT
public:
    PermanentWorker(QTcpSocket* sock, const QString& clientId,
                    const QString& subMode, qint64 lastEventId);
public slots:
    void deliverEvent(const Event& e);
signals:
    void ackReceived(const QString& clientId, qint64 eventId);
    void clientDisconnected(const QString& clientId);
private slots:
    void onReadyRead();
    void onDisconnected();
private:
    QTcpSocket*  socket_;
    QString      clientId_;
    QString      subMode_;
    qint64       lastEventId_;
};

void PermanentWorker::deliverEvent(const Event& e) {
    if (e.id <= lastEventId_) return;

    QJsonObject msg;
    msg["cmd"]        = QStringLiteral("event");
    msg["event_id"]   = e.id;
    msg["table"]      = e.table;
    msg["type"]       = e.type;
    msg["row_id"]     = e.rowId;
    msg["created_at"] = e.createdAt.toString(Qt::ISODate);

    if (subMode_ == "full")
        msg["payload"] = e.payload;
    else
        msg["payload"] = QJsonObject{{"id", e.rowId}};

    socket_->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
    socket_->flush();
}

void PermanentWorker::onReadyRead() {
    while (socket_->canReadLine()) {
        auto doc = QJsonDocument::fromJson(socket_->readLine());
        auto obj = doc.object();
        if (obj["cmd"].toString() == "ack") {
            qint64 id = obj["ack"].toInteger();
            lastEventId_ = qMax(lastEventId_, id);
            emit ackReceived(clientId_, id);
        }
    }
}
```

## 6.6 One-shot delivery (thread pool)

```cpp
void EventPublisher::deliverOneShot(SubscriberInfo* sub, const Event& e) {
    QString host  = sub->callbackHost;
    QString mode  = sub->subscriptionMode;
    QString cid   = sub->clientId;

    QtConcurrent::run(oneShotPool_, [=]() {
        QStringList parts = host.split(':');
        if (parts.size() != 2) return;

        QTcpSocket sock;
        sock.connectToHost(parts[0], parts[1].toUShort());
        if (!sock.waitForConnected(5000)) return;

        QJsonObject msg;
        msg["cmd"]        = QStringLiteral("event");
        msg["event_id"]   = e.id;
        msg["table"]      = e.table;
        msg["type"]       = e.type;
        msg["row_id"]     = e.rowId;
        msg["created_at"] = e.createdAt.toString(Qt::ISODate);
        msg["payload"]    = (mode == "full") ? e.payload
                                             : QJsonObject{{"id", e.rowId}};

        sock.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
        sock.flush();

        // Wait for ACK
        if (sock.waitForReadyRead(10000)) {
            auto doc = QJsonDocument::fromJson(sock.readLine());
            if (doc.object()["cmd"].toString() == "ack") {
                qint64 ackId = doc.object()["ack"].toInteger();
                // persist ack (thread-safe DB call)
                updateLastEventId(cid, ackId);
            }
        }
        sock.close();
    });
}
```

## 6.7 ACK persistence (shared by both modes)

```cpp
void EventPublisher::onAckReceived(const QString& clientId, qint64 eventId) {
    QSqlQuery q(workerDb_);
    q.prepare("UPDATE subscribers SET last_event_id = GREATEST(last_event_id, :eid), "
              "updated_at = now() WHERE client_id = :cid");
    q.bindValue(":eid", eventId);
    q.bindValue(":cid", clientId);
    q.exec();

    QMutexLocker lk(&mutex_);
    if (auto* s = subscribers_.value(clientId))
        s->lastEventId = qMax(s->lastEventId, eventId);
}
```

---

# 7. Wire Protocol (JSON-over-TCP, newline-delimited)

### Subscribe (client ? server)

```json
{"cmd":"subscribe","client_id":"production_line","connection_mode":"permanent","subscription_mode":"full","resume_from":0,"callback_host":null}
```
For one-shot add `"callback_host":"192.168.1.50:9000"`.

### Event (server ? client)

```json
{"cmd":"event","event_id":123,"table":"items","type":"insert","row_id":99,"payload":{...},"created_at":"2026-02-10T14:23:45Z"}
```
In `notify_only` mode the payload is `{"id":99}`.

### ACK (client ? server)

```json
{"cmd":"ack","client_id":"production_line","ack":123}
```
Means: "I have processed all events up to and including 123."

### Heartbeat (permanent mode, every 30 s)

```json
{"cmd":"ping"}   ?  server
{"cmd":"pong"}   ?  client
```

---

# 8. Subscriber Handling on Connect

```
Client connects ? sends subscribe JSON
                      ?
        ?????????????????????????????
        ? permanent                  ? one_shot
        ?                            ?
   create QThread +             upsert subscribers row
   PermanentWorker              with callback_host
   move socket to thread        return ok, close socket
   upsert subscribers row
   replay events > resume_from
```

One-shot subscribers register once; the server stores `callback_host` and
opens a new TCP connection to that address every time it has an event to deliver.

---

# 9. Client Side (production_line)

### 9.1 Permanent mode

```cpp
void ProductionLineClient::onReadyRead() {
    while (socket_->canReadLine()) {
        auto obj = QJsonDocument::fromJson(socket_->readLine()).object();
        QString cmd = obj["cmd"].toString();

        if (cmd == "event") {
            QString type = obj["type"].toString();
            if (type == "bulk_import_finished") {
                // full reload or incremental sync
            } else if (type == "marked_as_deleted") {
                // remove row from local model (soft-deleted on server)
            } else {
                // apply insert / delete / status_to_0
            }
            // ACK
            QJsonObject ack;
            ack["cmd"] = "ack";
            ack["client_id"] = clientId_;
            ack["ack"] = obj["event_id"];
            socket_->write(QJsonDocument(ack).toJson(QJsonDocument::Compact)+"\n");
        } else if (cmd == "ping") {
            socket_->write("{\"cmd\":\"pong\"}\n");
        }
    }
}
```

### 9.2 One-shot mode

Client runs a `QTcpServer` on its callback port. When a connection arrives:

```cpp
void OneShotReceiver::onNewConnection() {
    auto* sock = callbackServer_->nextPendingConnection();
    connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
        if (!sock->canReadLine()) return;
        auto obj = QJsonDocument::fromJson(sock->readLine()).object();

        // process event …

        // ACK back on same socket
        QJsonObject ack;
        ack["cmd"] = "ack";
        ack["client_id"] = clientId_;
        ack["ack"] = obj["event_id"];
        sock->write(QJsonDocument(ack).toJson(QJsonDocument::Compact)+"\n");
        sock->flush();
        sock->disconnectFromHost();
    });
}
```

---

# 10. Failure & Recovery

| Scenario | Behaviour |
|----------|-----------|
| Server crash | `event_log` is durable. On restart, load `subscribers`, resume from `min(last_event_id)`. |
| Permanent client disconnect | Worker thread detects `disconnected()` signal. Server keeps events. Client reconnects with `resume_from`. |
| One-shot callback unreachable | Thread pool task times out after 5 s. Event stays unacked. Next `fetchAndDeliver()` will retry. |
| Duplicate delivery | Client deduplicates by `event_id` (at-least-once semantics). |
| DB crash mid-import | No `bulk_import_finished` emitted — correct, import was incomplete. |

---

# 11. Event Log Cleanup

```sql
DELETE FROM event_log
WHERE id < (SELECT COALESCE(MIN(last_event_id),0) FROM subscribers)
  AND created_at < now() - interval '7 days';
```

Run from a scheduled task or a `QTimer` inside the server (e.g. daily).

---

# 12. Testing Checklist

| # | Test | Expected |
|---|------|----------|
| 1 | Insert one item normally | `event_log` row with type `insert`, NOTIFY fires |
| 2 | Delete one box (hard) | `event_log` row with type `delete` |
| 3 | Mark item as deleted (`is_deleted = TRUE`) | `event_log` row with type `marked_as_deleted` |
| 4 | Update item status 1?0 | `event_log` row with type `status_to_0` |
| 5 | Update item status 0?1 | No event (not a monitored transition) |
| 6 | Mark item as deleted + status change in same UPDATE | Only `marked_as_deleted` event (takes priority) |
| 7 | Import 10 000 items via `doImport` | 0 per-row events, 1 `bulk_import_finished` |
| 8 | Permanent client connects, resumes from id 50 | Receives events 51+ |
| 9 | Kill server, restart | Client reconnects, no events lost |
| 10 | One-shot callback port unreachable | Delivery fails, `last_event_id` not advanced, retry on next cycle |
| 11 | Full vs notify_only | Full payload vs `{"id": N}` |

---

# 13. Implementation Plan

| Phase | Work | Week |
|-------|------|------|
| 1 | SQL migration: `event_log`, `subscribers`, trigger function, attach triggers | 1 |
| 2 | `EventPublisher` class: LISTEN/NOTIFY + QSocketNotifier + `fetchAndDeliver` | 2 |
| 3 | Permanent mode: `PermanentWorker` in QThread, subscribe flow, ACK | 3 |
| 4 | One-shot mode: QThreadPool delivery, callback registration | 4 |
| 5 | `DbService::doImport()` bulk flag + `bulk_import_finished` event | 5 |
| 6 | Client-side handling (permanent + one-shot) | 6 |
| 7 | Heartbeat, cleanup job, integration tests | 7 |

---

# 14. Migration Script (003_add_event_subscription.sql)

```sql
BEGIN;

-- Soft-delete columns
ALTER TABLE items ADD COLUMN IF NOT EXISTS is_deleted BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE boxes ADD COLUMN IF NOT EXISTS is_deleted BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX IF NOT EXISTS idx_items_not_deleted
    ON items(production_line, status) WHERE NOT is_deleted;
CREATE INDEX IF NOT EXISTS idx_boxes_not_deleted
    ON boxes(production_line, status) WHERE NOT is_deleted;

-- Event log
CREATE TABLE event_log (
    id          BIGSERIAL PRIMARY KEY,
    table_name  TEXT NOT NULL,
    event_type  TEXT NOT NULL,
    row_id      BIGINT,
    payload     JSONB DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_event_log_id      ON event_log(id);
CREATE INDEX idx_event_log_table   ON event_log(table_name);
CREATE INDEX idx_event_log_created ON event_log(created_at);

-- Subscribers
CREATE TABLE subscribers (
    id                SERIAL PRIMARY KEY,
    client_id         TEXT UNIQUE NOT NULL,
    connection_mode   TEXT NOT NULL,
    subscription_mode TEXT NOT NULL,
    callback_host     TEXT,
    last_event_id     BIGINT DEFAULT 0 NOT NULL,
    updated_at        TIMESTAMPTZ DEFAULT now(),

    CONSTRAINT chk_connection_mode CHECK (connection_mode IN ('permanent', 'one_shot')),
    CONSTRAINT chk_subscription_mode CHECK (subscription_mode IN ('full', 'notify_only')),
    CONSTRAINT chk_callback_host_required CHECK (
        connection_mode != 'one_shot' OR callback_host IS NOT NULL
    )
);
CREATE INDEX idx_subscribers_client ON subscribers(client_id);

-- Trigger function (see section 3.4)
CREATE OR REPLACE FUNCTION log_items_boxes_events()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    bulk text;
    p    jsonb;
BEGIN
    bulk := COALESCE(current_setting('app.bulk_import', true), 'off');
    IF bulk = 'on' THEN RETURN COALESCE(NEW, OLD); END IF;

    IF TG_OP = 'INSERT' THEN
        p := jsonb_build_object(
            'id', NEW.id, 'bar_code', NEW.bar_code,
            'status', NEW.status, 'production_line', NEW.production_line,
            'imported_at', NEW.imported_at);
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'insert', NEW.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'DELETE' THEN
        p := jsonb_build_object('id', OLD.id, 'bar_code', OLD.bar_code);
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'delete', OLD.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'UPDATE' THEN
        -- Priority 1: Soft-delete (is_deleted toggled FALSE ? TRUE)
        IF NOT OLD.is_deleted AND NEW.is_deleted THEN
            p := jsonb_build_object(
                'id', NEW.id, 'bar_code', NEW.bar_code,
                'status', NEW.status, 'production_line', NEW.production_line);
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'marked_as_deleted', NEW.id, p);
            PERFORM pg_notify('db_events', '');

        -- Priority 2: Status rollback (1?0 or 2?0)
        ELSIF OLD.status IN (1,2) AND NEW.status = 0 THEN
            p := jsonb_build_object(
                'id', NEW.id, 'bar_code', NEW.bar_code,
                'old_status', OLD.status, 'new_status', NEW.status,
                'production_line', NEW.production_line);
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'status_to_0', NEW.id, p);
            PERFORM pg_notify('db_events', '');
        END IF;
    END IF;

    RETURN COALESCE(NEW, OLD);
END;
$$;

-- Attach triggers
DROP TRIGGER IF EXISTS trg_items_event ON items;
DROP TRIGGER IF EXISTS trg_boxes_event ON boxes;

CREATE TRIGGER trg_items_event
    AFTER INSERT OR UPDATE OR DELETE ON items
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();

CREATE TRIGGER trg_boxes_event
    AFTER INSERT OR UPDATE OR DELETE ON boxes
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();

COMMIT;
```

---

# 15. Summary

| Aspect | Decision |
|--------|----------|
| Durability | Append-only `event_log` in PostgreSQL |
| Wake-up | `LISTEN/NOTIFY` (no payload, just signal); polling fallback when libpq unavailable |
| Bulk imports | Session `SET app.bulk_import='on'` suppresses triggers; one `bulk_import_finished` event at end |
| Soft-delete | `is_deleted` column on `items`/`boxes`; triggers `marked_as_deleted` event |
| Permanent delivery | Dedicated QThread + PermanentWorker per client |
| One-shot delivery | QThreadPool tasks, raw TCP to callback ip:port |
| Subscription modes | `full` (row data in payload) / `notify_only` (id only) |
| ACK semantics | Monotonic `last_event_id`; acking N means all ?N processed |
| Delivery guarantee | At-least-once; client deduplicates by `event_id` |
| Protocol | JSON newline-delimited over raw TCP (no HTTP) |
| Resume | Client sends `resume_from` on subscribe; server replays from there |
