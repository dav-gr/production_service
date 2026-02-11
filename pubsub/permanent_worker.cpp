#include "permanent_worker.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace pubsub {

PermanentWorker::PermanentWorker(QTcpSocket* socket,
                                 const QString& clientId,
                                 const QString& subscriptionMode,
                                 qint64 lastEventId)
    : socket_(socket)
    , clientId_(clientId)
    , subscriptionMode_(subscriptionMode)
    , lastEventId_(lastEventId)
{
    // Socket will be moved to this worker's thread
}

PermanentWorker::~PermanentWorker() {
    if (socket_) {
        socket_->disconnectFromHost();
        socket_->deleteLater();
        socket_ = nullptr;
    }
}

void PermanentWorker::initialize() {
    if (!socket_) {
        qWarning() << "PermanentWorker: Socket is null for" << clientId_;
        emit socketError(clientId_, "Socket is null");
        return;
    }

    // Socket has been moved to this thread via moveToThread
    socket_->setParent(this);

    connect(socket_, &QTcpSocket::readyRead, this, &PermanentWorker::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &PermanentWorker::onDisconnected);
    connect(socket_, &QTcpSocket::errorOccurred, this, &PermanentWorker::onError);

    qDebug() << "PermanentWorker: Initialized for client" << clientId_ 
             << "mode:" << subscriptionMode_ 
             << "resumeFrom:" << lastEventId_;

    // Send OK response now that we're in the worker thread
    QJsonObject resp;
    resp["status"] = "ok";
    resp["message"] = "Subscribed in permanent mode";
    socket_->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
    socket_->flush();

    emit initialized();
}

void PermanentWorker::deliverEvent(const core::Event& event) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "PermanentWorker: Cannot deliver event, socket not connected";
        return;
    }

    // Skip events already processed
    if (event.id <= lastEventId_) {
        return;
    }

    QJsonObject msg;
    msg["cmd"]        = QStringLiteral("event");
    msg["event_id"]   = event.id;
    msg["table"]      = event.table;
    msg["type"]       = event.type;
    msg["row_id"]     = event.rowId;
    msg["created_at"] = event.createdAt.toString(Qt::ISODate);

    if (subscriptionMode_ == "full") {
        msg["payload"] = event.payload;
    } else {
        // notify_only mode: only send the row ID
        QJsonObject minPayload;
        minPayload["id"] = event.rowId;
        msg["payload"] = minPayload;
    }

    QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    socket_->write(data);
    socket_->flush();

    // Event delivered - no per-event logging (use batch logs instead)
}

void PermanentWorker::sendPing() {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    QJsonObject ping;
    ping["cmd"] = QStringLiteral("ping");
    socket_->write(QJsonDocument(ping).toJson(QJsonDocument::Compact) + "\n");
    socket_->flush();
}

void PermanentWorker::onReadyRead() {
    buffer_.append(socket_->readAll());

    // Process complete lines
    int idx;
    while ((idx = buffer_.indexOf('\n')) != -1) {
        QByteArray line = buffer_.left(idx).trimmed();
        buffer_.remove(0, idx + 1);

        if (!line.isEmpty()) {
            processMessage(line);
        }
    }
}

void PermanentWorker::processMessage(const QByteArray& line) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "PermanentWorker: JSON parse error:" << parseError.errorString();
        return;
    }

    QJsonObject obj = doc.object();
    QString cmd = obj["cmd"].toString();

    if (cmd == "ack") {
        qint64 ackId = obj["ack"].toInteger();
        if (ackId > lastEventId_) {
            lastEventId_ = ackId;
            emit ackReceived(clientId_, ackId);
            // ACK processed - no per-ACK logging (client uses batched ACKs)
        }
    } else if (cmd == "pong") {
        // Pong received - heartbeat OK, no logging needed
    } else {
        qWarning() << "PermanentWorker: Unknown command:" << cmd;
    }
}

void PermanentWorker::onDisconnected() {
    qDebug() << "PermanentWorker: Client disconnected:" << clientId_;
    emit clientDisconnected(clientId_);
}

void PermanentWorker::onError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error)
    if (socket_) {
        qWarning() << "PermanentWorker: Socket error for" << clientId_ 
                   << ":" << socket_->errorString();
        emit socketError(clientId_, socket_->errorString());
    }
}

} // namespace pubsub
