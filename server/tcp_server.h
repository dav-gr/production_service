#ifndef SERVER_TCP_SERVER_H
#define SERVER_TCP_SERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include "request_handler.h"
#include "core/db/db_service.h"

namespace server {

/**
 * @brief Simple TCP server with one-shot connections
 * 
 * Flow: connect → receive request → send response → close
 * No persistent connections. Each request is independent.
 */
class TcpServer : public QTcpServer {
    Q_OBJECT

public:
    explicit TcpServer(QObject* parent = nullptr);
    ~TcpServer() override;
    
    bool connectDatabase(const QString& host, int port, const QString& db,
                         const QString& user, const QString& password);
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
    
    core::DbService* db_;
    RequestHandler* handler_;
    int readTimeoutMsec_ = 5000;
};

} // namespace server

#endif // SERVER_TCP_SERVER_H
