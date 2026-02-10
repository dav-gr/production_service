#include "client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDebug>

// ============================================================================
// Construction / Destruction
// ============================================================================

ExampleClient::ExampleClient(QObject* parent)
    : QObject(parent)
{
}

ExampleClient::~ExampleClient() {
    if (reconnectTimer_) {
        reconnectTimer_->stop();
    }
    if (socket_) {
        socket_->disconnectFromHost();
    }
    if (callbackServer_) {
        callbackServer_->close();
    }
}

// ============================================================================
// Permanent Mode
// ============================================================================

void ExampleClient::connectPermanent(const QString& serverHost, quint16 serverPort,
                                      const QString& clientId, const QString& subscriptionMode,
                                      qint64 resumeFrom) {
    serverHost_ = serverHost;
    serverPort_ = serverPort;
    clientId_ = clientId;
    connectionMode_ = "permanent";
    subscriptionMode_ = subscriptionMode;
    resumeFrom_ = resumeFrom;
    lastEventId_ = resumeFrom;

    qInfo() << "=== Permanent Mode ===";
    qInfo() << "Server:" << serverHost << ":" << serverPort;
    qInfo() << "Client ID:" << clientId;
    qInfo() << "Subscription:" << subscriptionMode;
    qInfo() << "Resume from:" << resumeFrom;

    socket_ = new QTcpSocket(this);

    connect(socket_, &QTcpSocket::connected,
            this, &ExampleClient::onPermanentConnected);
    connect(socket_, &QTcpSocket::readyRead,
            this, &ExampleClient::onPermanentReadyRead);
    connect(socket_, &QTcpSocket::disconnected,
            this, &ExampleClient::onPermanentDisconnected);
    connect(socket_, &QTcpSocket::errorOccurred,
            this, &ExampleClient::onPermanentError);

    // Reconnect timer
    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(5000);
    connect(reconnectTimer_, &QTimer::timeout,
            this, &ExampleClient::onReconnectTimer);

    qInfo() << "Connecting to server...";
    socket_->connectToHost(serverHost, serverPort);
}

void ExampleClient::onPermanentConnected() {
    qInfo() << "Connected to server, sending subscribe request...";

    // Send subscribe message
    QJsonObject sub;
    sub["cmd"] = QStringLiteral("subscribe");
    sub["client_id"] = clientId_;
    sub["connection_mode"] = QStringLiteral("permanent");
    sub["subscription_mode"] = subscriptionMode_;
    sub["resume_from"] = lastEventId_;
    sub["callback_host"] = QJsonValue::Null;

    QByteArray data = QJsonDocument(sub).toJson(QJsonDocument::Compact) + "\n";
    socket_->write(data);
    socket_->flush();

    qInfo() << "Subscribe request sent:" << data.trimmed();
}

void ExampleClient::onPermanentReadyRead() {
    buffer_.append(socket_->readAll());

    int idx;
    while ((idx = buffer_.indexOf('\n')) != -1) {
        QByteArray line = buffer_.left(idx).trimmed();
        buffer_.remove(0, idx + 1);

        if (line.isEmpty()) continue;

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "JSON parse error:" << parseError.errorString();
            continue;
        }

        QJsonObject msg = doc.object();
        QString cmd = msg["cmd"].toString();

        if (cmd == "event") {
            logEvent(msg);
            processMessage(msg);

            qint64 eventId = msg["event_id"].toInteger();
            sendAck(socket_, eventId);

        } else if (cmd == "ping") {
            qDebug() << "<< PING";
            QByteArray pong = QByteArrayLiteral("{\"cmd\":\"pong\"}\n");
            socket_->write(pong);
            socket_->flush();
            qDebug() << ">> PONG";

        } else if (cmd == "ok" || msg.contains("status")) {
            // Subscribe response
            QString status = msg["status"].toString();
            QString message = msg["message"].toString();
            qInfo() << "Server response:" << status << "-" << message;
            if (status == "ok") {
                emit connected();
            }
        } else {
            qDebug() << "<< Unknown command:" << cmd << line;
        }
    }
}

void ExampleClient::onPermanentDisconnected() {
    qWarning() << "Disconnected from server. Reconnecting in 5 seconds...";
    reconnectTimer_->start();
    emit disconnected();
}

void ExampleClient::onPermanentError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error)
    qWarning() << "Socket error:" << socket_->errorString();
    if (!reconnectTimer_->isActive()) {
        reconnectTimer_->start();
    }
}

void ExampleClient::onReconnectTimer() {
    if (socket_->state() == QAbstractSocket::ConnectedState) {
        reconnectTimer_->stop();
        return;
    }

    qInfo() << "Attempting reconnect... (resume_from:" << lastEventId_ << ")";
    resumeFrom_ = lastEventId_;
    socket_->connectToHost(serverHost_, serverPort_);
}

// ============================================================================
// One-Shot Mode
// ============================================================================

void ExampleClient::connectOneShot(const QString& serverHost, quint16 serverPort,
                                    const QString& clientId, const QString& subscriptionMode,
                                    quint16 callbackPort, qint64 resumeFrom) {
    serverHost_ = serverHost;
    serverPort_ = serverPort;
    clientId_ = clientId;
    connectionMode_ = "one_shot";
    subscriptionMode_ = subscriptionMode;
    callbackPort_ = callbackPort;
    resumeFrom_ = resumeFrom;
    lastEventId_ = resumeFrom;

    qInfo() << "=== One-Shot Mode ===";
    qInfo() << "Server:" << serverHost << ":" << serverPort;
    qInfo() << "Client ID:" << clientId;
    qInfo() << "Subscription:" << subscriptionMode;
    qInfo() << "Callback port:" << callbackPort;
    qInfo() << "Resume from:" << resumeFrom;

    // Start callback server first
    callbackServer_ = new QTcpServer(this);
    connect(callbackServer_, &QTcpServer::newConnection,
            this, &ExampleClient::onCallbackConnection);

    if (!callbackServer_->listen(QHostAddress::Any, callbackPort)) {
        QString err = "Failed to listen on callback port " + QString::number(callbackPort)
                     + ": " + callbackServer_->errorString();
        qCritical() << err;
        emit errorOccurred(err);
        return;
    }

    qInfo() << "Callback server listening on port" << callbackPort;

    // Now register with the publisher server
    QTcpSocket* regSocket = new QTcpSocket(this);

    connect(regSocket, &QTcpSocket::connected, this, [this, regSocket]() {
        onOneShotRegistered();

        // Determine our IP address visible to the server
        QString localAddr = regSocket->localAddress().toString();

        // Build callback_host as ip:port
        QString callbackHost = localAddr + ":" + QString::number(callbackPort_);

        QJsonObject sub;
        sub["cmd"] = QStringLiteral("subscribe");
        sub["client_id"] = clientId_;
        sub["connection_mode"] = QStringLiteral("one_shot");
        sub["subscription_mode"] = subscriptionMode_;
        sub["resume_from"] = lastEventId_;
        sub["callback_host"] = callbackHost;

        QByteArray data = QJsonDocument(sub).toJson(QJsonDocument::Compact) + "\n";
        regSocket->write(data);
        regSocket->flush();

        qInfo() << "Subscribe request sent:" << data.trimmed();
        qInfo() << "Callback host:" << callbackHost;
    });

    connect(regSocket, &QTcpSocket::readyRead, this, [this, regSocket]() {
        if (!regSocket->canReadLine()) return;

        QByteArray line = regSocket->readLine().trimmed();
        QJsonDocument doc = QJsonDocument::fromJson(line);
        QJsonObject resp = doc.object();

        QString status = resp["status"].toString();
        QString message = resp["message"].toString();
        qInfo() << "Server response:" << status << "-" << message;

        if (status == "ok") {
            emit connected();
        } else {
            emit errorOccurred(message);
        }

        // Registration done, close the registration socket
        regSocket->disconnectFromHost();
        regSocket->deleteLater();
    });

    connect(regSocket, &QTcpSocket::errorOccurred, this,
            [this, regSocket](QAbstractSocket::SocketError) {
        qWarning() << "Registration failed:" << regSocket->errorString();
        emit errorOccurred(regSocket->errorString());
        regSocket->deleteLater();
    });

    qInfo() << "Connecting to server for registration...";
    regSocket->connectToHost(serverHost, serverPort);
}

void ExampleClient::onOneShotRegistered() {
    qInfo() << "Connected to server for one-shot registration";
}

void ExampleClient::onCallbackConnection() {
    while (callbackServer_->hasPendingConnections()) {
        QTcpSocket* sock = callbackServer_->nextPendingConnection();

        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            if (!sock->canReadLine()) return;

            QByteArray line = sock->readLine().trimmed();
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

            if (parseError.error != QJsonParseError::NoError) {
                qWarning() << "Callback JSON parse error:" << parseError.errorString();
                sock->disconnectFromHost();
                sock->deleteLater();
                return;
            }

            QJsonObject msg = doc.object();

            logEvent(msg);
            processMessage(msg);

            // ACK back on same socket
            qint64 eventId = msg["event_id"].toInteger();
            sendAck(sock, eventId);

            // Close after ACK
            sock->flush();
            sock->disconnectFromHost();
            sock->deleteLater();
        });

        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
}

// ============================================================================
// Shared Logic
// ============================================================================

void ExampleClient::processMessage(const QJsonObject& msg) {
    QString type = msg["type"].toString();
    QString table = msg["table"].toString();
    qint64 eventId = msg["event_id"].toInteger();
    qint64 rowId = msg["row_id"].toInteger();
    QJsonObject payload = msg["payload"].toObject();

    eventsReceived_++;

    if (type == "insert") {
        QString barcode = payload["bar_code"].toString();
        qInfo().noquote() << QString("  -> [%1] INSERT %2 row_id=%3 bar_code=%4")
            .arg(table).arg(type).arg(rowId).arg(barcode);

    } else if (type == "delete") {
        QString barcode = payload["bar_code"].toString();
        qInfo().noquote() << QString("  -> [%1] DELETE row_id=%2 bar_code=%3")
            .arg(table).arg(rowId).arg(barcode);

    } else if (type == "marked_as_deleted") {
        QString barcode = payload["bar_code"].toString();
        int status = payload["status"].toInt();
        qInfo().noquote() << QString("  -> [%1] MARKED_AS_DELETED row_id=%2 bar_code=%3 status=%4")
            .arg(table).arg(rowId).arg(barcode).arg(status);

    } else if (type == "status_to_0") {
        int oldStatus = payload["old_status"].toInt();
        int newStatus = payload["new_status"].toInt();
        QString barcode = payload["bar_code"].toString();
        qInfo().noquote() << QString("  -> [%1] STATUS_TO_0 row_id=%2 bar_code=%3 %4->%5")
            .arg(table).arg(rowId).arg(barcode).arg(oldStatus).arg(newStatus);

    } else if (type == "bulk_import_finished") {
        int rows = payload["rows_affected"].toInt();
        qint64 line = payload["production_line"].toInteger();
        qInfo().noquote() << QString("  -> [%1] BULK_IMPORT_FINISHED production_line=%2 rows=%3")
            .arg(table).arg(line).arg(rows);

    } else {
        qInfo().noquote() << QString("  -> [%1] UNKNOWN TYPE '%2' row_id=%3")
            .arg(table).arg(type).arg(rowId);
    }

    // Track last processed event
    if (eventId > lastEventId_) {
        lastEventId_ = eventId;
    }

    emit eventReceived(msg);
}

void ExampleClient::sendAck(QTcpSocket* socket, qint64 eventId) {
    QJsonObject ack;
    ack["cmd"] = QStringLiteral("ack");
    ack["client_id"] = clientId_;
    ack["ack"] = eventId;

    QByteArray data = QJsonDocument(ack).toJson(QJsonDocument::Compact) + "\n";
    socket->write(data);
    socket->flush();

    qDebug() << ">> ACK event_id:" << eventId;
}

void ExampleClient::logEvent(const QJsonObject& msg) {
    qint64 eventId = msg["event_id"].toInteger();
    QString table = msg["table"].toString();
    QString type = msg["type"].toString();
    QString createdAt = msg["created_at"].toString();

    qInfo().noquote() << QString("<< EVENT #%1 [%2.%3] at %4")
        .arg(eventId).arg(table).arg(type).arg(createdAt);
}
