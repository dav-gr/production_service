#ifndef SERVER_TCP_SERVER_H
#define SERVER_TCP_SERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include "session_manager.h"
#include "protocol.h"
#include "pipeline/entity_resolver.h"
#include "pipeline/state_resolver.h"
#include "pipeline/capability_engine.h"
#include "pipeline/response_builder.h"
#include "pipeline/action_executor.h"
#include "core/db/db_service.h"

namespace server {

/**
 * @brief TCP server with rule-driven pipeline
 *
 * Flow: connect -> receive request -> route to handler -> send response -> close
 * No persistent connections. Each request is independent.
 *
 * Message routing:
 *   "login"  -> SessionManager::login
 *   "logout" -> SessionManager::logout
 *   "scan"   -> EntityResolver -> StateResolver -> CapabilityEngine -> ResponseBuilder
 *   "action" -> EntityResolver -> StateResolver -> ActionExecutor
 */
class TcpServer : public QTcpServer {
    Q_OBJECT

public:
    explicit TcpServer(QObject* parent = nullptr);
    ~TcpServer() override;

    bool connectDatabase(const QString& host, int port, const QString& db,
                         const QString& user, const QString& password);
    bool loadCapabilityRules(const QString& rulesPath);
    bool startServer(quint16 port);
    void stopServer();

    void setSessionExpiration(int minutes);
    void setReadTimeout(int msec);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void onReadyRead();
    void onDisconnected();
    void onTimeout();

private:
    void processClient(QTcpSocket* socket, const QByteArray& data);

    // Route handlers
    QByteArray handleLogin(const Request& req);
    QByteArray handleLogout(const Request& req);
    QByteArray handleScan(const Request& req);
    QByteArray handleAction(const Request& req);

    core::DbService* db_;
    SessionManager* sessionMgr_;
    pipeline::EntityResolver* entityResolver_;
    pipeline::StateResolver* stateResolver_;
    pipeline::CapabilityEngine* capabilityEngine_;
    pipeline::ActionExecutor* actionExecutor_;

    int readTimeoutMsec_ = 5000;
    int sessionMinutes_ = 480;
};

} // namespace server

#endif // SERVER_TCP_SERVER_H
