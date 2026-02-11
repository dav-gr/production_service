#include "client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDebug>
#include <QFile>
#include <QCoreApplication>

// ============================================================================
// Construction / Destruction
// ============================================================================

ExampleClient::ExampleClient(QObject* parent)
    : QObject(parent)
{
}

ExampleClient::~ExampleClient() {
    if (ackFlushTimer_) {
        ackFlushTimer_->stop();
    }
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

    // Load resume state from file (overrides parameter if file exists)
    qint64 savedId = loadResumeState(clientId);
    if (savedId > resumeFrom) {
        qInfo() << "Resuming from saved state: event_id" << savedId;
        resumeFrom = savedId;
    }
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

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(5000);
    connect(reconnectTimer_, &QTimer::timeout,
            this, &ExampleClient::onReconnectTimer);

    // Periodic flush to catch partial final batches (500ms)
    ackFlushTimer_ = new QTimer(this);
    ackFlushTimer_->setInterval(500);
    connect(ackFlushTimer_, &QTimer::timeout, this, [this]() {
        flushPendingAcks();
        // Log final progress when events stopped arriving
        if (eventsReceived_ > 0 && eventsReceived_ != lastLoggedCount_) {
            qInfo() << "Progress:" << eventsReceived_
                    << "events received (last_id:" << lastEventId_ << ")";
            lastLoggedCount_ = eventsReceived_;
        }
    });
    ackFlushTimer_->start();

    qInfo() << "Connecting to server...";
    socket_->connectToHost(serverHost, serverPort);
}

void ExampleClient::onPermanentConnected() {
    qInfo() << "Connected to server, sending subscribe request...";

    pendingAcks_.clear();

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
            processMessage(msg);

            pendingAcks_.append(msg["event_id"].toInteger());

            if (pendingAcks_.size() >= ACK_BATCH_SIZE) {
                sendBatchAck(socket_, pendingAcks_);
                pendingAcks_.clear();
            }

        } else if (cmd == "ping") {
            socket_->write(QByteArrayLiteral("{\"cmd\":\"pong\"}\n"));
            socket_->flush();

        } else if (cmd == "ok" || msg.contains("status")) {
            QString status = msg["status"].toString();
            if (status == "ok") {
                qInfo() << "Subscribed successfully";
                emit connected();
            } else {
                qWarning() << "Subscription failed:" << msg["message"].toString();
            }
        }
    }

    // Flush remaining ACKs after processing all available data
    if (!pendingAcks_.isEmpty()) {
        sendBatchAck(socket_, pendingAcks_);
        pendingAcks_.clear();
    }
}

void ExampleClient::onPermanentDisconnected() {
    // Flush any pending ACKs before disconnect
    flushPendingAcks();
    
    qInfo() << "Disconnected. Total received:" << eventsReceived_ 
            << "last_event_id:" << lastEventId_;
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
    qint64 eventId = msg["event_id"].toInteger();
    eventsReceived_++;

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

void ExampleClient::sendBatchAck(QTcpSocket* socket, const QVector<qint64>& eventIds) {
    if (eventIds.isEmpty()) return;

    qint64 maxEventId = eventIds.last();
    for (qint64 id : eventIds) {
        if (id > maxEventId) maxEventId = id;
    }

    QJsonObject ack;
    ack["cmd"] = QStringLiteral("ack");
    ack["client_id"] = clientId_;
    ack["ack"] = maxEventId;

    socket->write(QJsonDocument(ack).toJson(QJsonDocument::Compact) + "\n");
    socket->flush();

    // Log every 1000 events
    int prevThousand = (eventsReceived_ - eventIds.size()) / 1000;
    int currThousand = eventsReceived_ / 1000;
    if (currThousand > prevThousand || eventsReceived_ == eventIds.size()) {
        qInfo() << "Progress:" << eventsReceived_
                << "events received (last_id:" << maxEventId << ")";
        lastLoggedCount_ = eventsReceived_;
    }

    // Persist resume state
    saveResumeState();
}

void ExampleClient::flushPendingAcks() {
    if (!pendingAcks_.isEmpty() && socket_ && socket_->isOpen()) {
        sendBatchAck(socket_, pendingAcks_);
        pendingAcks_.clear();
    }
}

void ExampleClient::saveResumeState() {
    QString path = QCoreApplication::applicationDirPath()
                   + "/" + clientId_ + ".resume";
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QByteArray::number(lastEventId_));
    }
}

qint64 ExampleClient::loadResumeState(const QString& clientId) {
    QString path = QCoreApplication::applicationDirPath()
                   + "/" + clientId + ".resume";
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        bool ok = false;
        qint64 id = file.readAll().trimmed().toLongLong(&ok);
        if (ok) return id;
    }
    return 0;
}

void ExampleClient::logEvent(const QJsonObject& msg) {
    qint64 eventId = msg["event_id"].toInteger();
    QString table = msg["table"].toString();
    QString type = msg["type"].toString();
    QString createdAt = msg["created_at"].toString();

    qInfo().noquote() << QString("<< EVENT #%1 [%2.%3] at %4")
        .arg(eventId).arg(table).arg(type).arg(createdAt);
}
