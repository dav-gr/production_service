#include "tcp_server.h"
#include <QTimer>
#include <QDebug>

namespace server {

TcpServer::TcpServer(QObject* parent)
    : QTcpServer(parent)
    , db_(new core::DbService(this))
    , sessionMgr_(nullptr)
    , entityResolver_(nullptr)
    , stateResolver_(nullptr)
    , capabilityEngine_(nullptr)
    , actionExecutor_(nullptr)
{
}

TcpServer::~TcpServer() {
    stopServer();
}

bool TcpServer::connectDatabase(const QString& host, int port, const QString& database,
                                 const QString& user, const QString& password)
{
    if (!db_->connect(host, port, database, user, password)) {
        qCritical() << "[Server] DB connect failed:" << db_->lastError();
        return false;
    }

    qInfo() << "[Server] Database connected";
    return true;
}

bool TcpServer::loadCapabilityRules(const QString& rulesPath) {
    if (!capabilityEngine_) {
        capabilityEngine_ = new pipeline::CapabilityEngine();
    }
    if (!capabilityEngine_->loadRules(rulesPath)) {
        qCritical() << "[Server] Failed to load capability rules from" << rulesPath;
        return false;
    }
    qInfo() << "[Server] Capability rules loaded from" << rulesPath;
    return true;
}

bool TcpServer::startServer(quint16 port) {
    if (!db_->isConnected()) {
        qCritical() << "[Server] Database not connected";
        return false;
    }

    // Initialize pipeline components
    if (!sessionMgr_) {
        sessionMgr_ = new SessionManager(db_, sessionMinutes_, this);
    }
    if (!entityResolver_) {
        entityResolver_ = new pipeline::EntityResolver(db_);
    }
    if (!stateResolver_) {
        stateResolver_ = new pipeline::StateResolver(db_);
    }
    if (!capabilityEngine_) {
        capabilityEngine_ = new pipeline::CapabilityEngine();
        qWarning() << "[Server] No capability rules loaded — actions will be empty";
    }
    if (!actionExecutor_) {
        actionExecutor_ = new pipeline::ActionExecutor(db_);
    }

    if (!listen(QHostAddress::Any, port)) {
        qCritical() << "[Server] Listen failed:" << errorString();
        return false;
    }

    qInfo() << "[Server] Listening on port" << port;
    return true;
}

void TcpServer::stopServer() {
    if (isListening()) {
        close();
        qInfo() << "[Server] Stopped";
    }
}

void TcpServer::setSessionExpiration(int minutes) {
    sessionMinutes_ = minutes;
    delete sessionMgr_;
    sessionMgr_ = new SessionManager(db_, minutes, this);
}

void TcpServer::setReadTimeout(int msec) {
    readTimeoutMsec_ = msec;
}

void TcpServer::incomingConnection(qintptr socketDescriptor) {
    auto* socket = new QTcpSocket(this);

    if (!socket->setSocketDescriptor(socketDescriptor)) {
        delete socket;
        return;
    }

    // Setup timeout timer
    auto* timer = new QTimer(socket);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &TcpServer::onTimeout);
    timer->start(readTimeoutMsec_);

    // Store socket in timer for retrieval
    timer->setProperty("socket", QVariant::fromValue(static_cast<void*>(socket)));
    socket->setProperty("timer", QVariant::fromValue(static_cast<void*>(timer)));
    socket->setProperty("buffer", QByteArray());

    connect(socket, &QTcpSocket::readyRead, this, &TcpServer::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &TcpServer::onDisconnected);

    qDebug() << "[Server] Client connected:"
             << socket->peerAddress().toString() << socket->peerPort();
}

void TcpServer::onReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    // Append to buffer
    QByteArray buffer = socket->property("buffer").toByteArray();
    buffer.append(socket->readAll());

    // Check for complete message (newline terminated)
    int newlinePos = buffer.indexOf('\n');
    if (newlinePos != -1) {
        // Stop timeout
        auto* timer = static_cast<QTimer*>(socket->property("timer").value<void*>());
        if (timer) timer->stop();

        // Process message
        QByteArray message = buffer.left(newlinePos);
        processClient(socket, message);
    } else if (buffer.size() > 65536) {
        // Too large, reject
        socket->write(ActionResponse::error("Request too large").toJson());
        socket->flush();
        socket->disconnectFromHost();
    } else {
        // Save buffer, wait for more data
        socket->setProperty("buffer", buffer);
    }
}

void TcpServer::onDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        qDebug() << "[Server] Client disconnected";
        socket->deleteLater();
    }
}

void TcpServer::onTimeout() {
    auto* timer = qobject_cast<QTimer*>(sender());
    if (!timer) return;

    auto* socket = static_cast<QTcpSocket*>(timer->property("socket").value<void*>());
    if (socket && socket->isOpen()) {
        qDebug() << "[Server] Client timeout";
        socket->write(ActionResponse::error("Read timeout").toJson());
        socket->flush();
        socket->disconnectFromHost();
    }
}

void TcpServer::processClient(QTcpSocket* socket, const QByteArray& data) {
    QString error;
    auto request = Request::fromJson(data, &error);

    QByteArray response;
    if (!request) {
        response = LoginResponse::error(error).toJson();
    } else if (request->type == MessageType::Login) {
        response = handleLogin(*request);
    } else if (request->type == MessageType::Logout) {
        response = handleLogout(*request);
    } else if (request->type == MessageType::Scan) {
        response = handleScan(*request);
    } else if (request->type == MessageType::Action) {
        response = handleAction(*request);
    } else {
        response = ActionResponse::error("Unknown type: " + request->type).toJson();
    }

    // Send response and close
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

// ============================================================================
// Route Handlers
// ============================================================================

QByteArray TcpServer::handleLogin(const Request& req) {
    QString username = req.getString("username");
    QString pin = req.getString("pin");

    // Support both nested data and flat format
    if (username.isEmpty()) {
        QJsonObject data = req.data.value("data").toObject();
        username = data.value("username").toString();
        pin = data.value("pin").toString();
    }

    auto result = sessionMgr_->login(username, pin);
    if (!result.success) {
        return LoginResponse::error(result.error).toJson();
    }

    return LoginResponse::ok("Login successful", result.responseData).toJson();
}

QByteArray TcpServer::handleLogout(const Request& req) {
    QString token = req.getString("token");
    if (token.isEmpty()) {
        QJsonObject data = req.data.value("data").toObject();
        token = data.value("token").toString();
    }

    sessionMgr_->logout(token);
    return LoginResponse::ok("Logged out").toJson();
}

QByteArray TcpServer::handleScan(const Request& req) {
    QString token = req.getString("token");
    auto* session = sessionMgr_->getSession(token);
    if (!session)
        return ActionResponse::error("Invalid or expired session").toJson();

    QString barcode = req.getString("barcode");
    if (barcode.isEmpty())
        return ActionResponse::error("Barcode required").toJson();

    // Pipeline: resolve -> state -> capabilities -> response
    auto entity = entityResolver_->resolve(barcode);
    if (!entity)
        return ActionResponse::error("Barcode not found").toJson();

    auto state = stateResolver_->computeState(*entity);
    auto actions = capabilityEngine_->evaluate(state);
    auto scanResp = pipeline::ResponseBuilder::buildScanResponse(state, actions);

    qInfo() << "[Server] Scan:" << barcode
            << "-> type:" << scanResp.entityType
            << "actions:" << actions.size();

    return scanResp.toJson();
}

QByteArray TcpServer::handleAction(const Request& req) {
    QString token = req.getString("token");
    auto* session = sessionMgr_->getSession(token);
    if (!session)
        return ActionResponse::error("Invalid or expired session").toJson();

    QString action = req.getString("action");
    QString barcode = req.getString("barcode");
    QJsonObject params = req.data.value("params").toObject();

    if (action.isEmpty())
        return ActionResponse::error("Action required").toJson();
    if (barcode.isEmpty())
        return ActionResponse::error("Barcode required for action context").toJson();

    // Re-resolve the entity to get current state
    auto entity = entityResolver_->resolve(barcode);
    if (!entity)
        return ActionResponse::error("Entity not found for barcode").toJson();

    auto state = stateResolver_->computeState(*entity);

    qInfo() << "[Server] Action:" << action
            << "on" << barcode
            << "by user:" << session->userId;

    auto result = actionExecutor_->execute(state, action, params, session->userId);
    return result.toJson();
}

} // namespace server
