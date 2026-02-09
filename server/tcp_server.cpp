#include "tcp_server.h"
#include <QTimer>
#include <QDebug>

namespace server {

TcpServer::TcpServer(QObject* parent)
    : QTcpServer(parent)
    , db_(new core::DbService(this))
    , handler_(nullptr)
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

bool TcpServer::startServer(quint16 port) {
    if (!db_->isConnected()) {
        qCritical() << "[Server] Database not connected";
        return false;
    }
    
    if (!handler_) {
        handler_ = new RequestHandler(db_, 480, this);
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
    if (handler_) {
        // Handler created with fixed expiration, would need recreation
        // For simplicity, set before startServer()
    }
    delete handler_;
    handler_ = new RequestHandler(db_, minutes, this);
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
        socket->write(Response::error("Request too large").toJson());
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
        socket->write(Response::error("Read timeout").toJson());
        socket->flush();
        socket->disconnectFromHost();
    }
}

void TcpServer::processClient(QTcpSocket* socket, const QByteArray& data) {
    QString error;
    auto request = Request::fromJson(data, &error);
    
    Response response;
    if (request) {
        response = handler_->handle(*request);
    } else {
        response = Response::error(error);
    }
    
    // Send response and close
    socket->write(response.toJson());
    socket->flush();
    socket->disconnectFromHost();
}

} // namespace server
