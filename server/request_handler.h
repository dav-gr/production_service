#ifndef SERVER_REQUEST_HANDLER_H
#define SERVER_REQUEST_HANDLER_H

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QDateTime>
#include "protocol.h"
#include "core/db/db_service.h"

namespace server {

// ============================================================================
// Session (stored in memory)
// ============================================================================

struct Session {
    QString token;
    qint64 userId = 0;
    QString username;
    QStringList permissions;
    QDateTime expiresAt;
    
    bool isValid() const { return QDateTime::currentDateTime() < expiresAt; }
    bool hasPermission(const QString& p) const { return permissions.contains(p); }
};

// ============================================================================
// Request Handler
// ============================================================================

class RequestHandler : public QObject {
    Q_OBJECT

public:
    explicit RequestHandler(core::DbService* db, int sessionMinutes = 480, 
                            QObject* parent = nullptr);
    
    Response handle(const Request& request);

private:
    // Actions
    Response doLogin(const Request& req);
    Response doLogout(const Request& req);
    Response doValidate(const Request& req);
    Response doProcess(const Request& req);
    
    // Session management
    QString createSession(qint64 userId, const QString& username, 
                          const QStringList& permissions);
    Session* getSession(const QString& token);
    void cleanExpiredSessions();
    
    // Database operations
    Response lookupBarcode(const QString& barcode);
    Response processItemInBox(qint64 itemId, qint64 boxId, 
                              const QString& itemBarcode, qint64 userId);
    Response processLooseItem(qint64 itemId, const QString& barcode, qint64 userId);
    Response processBox(qint64 boxId, const QString& barcode, qint64 userId);
    
    bool unsealBox(qint64 boxId);
    int removeItemsFromBox(qint64 boxId);
    bool resetItem(qint64 itemId);
    void writeAudit(const QString& entityType, const QString& barcode,
                    const QString& action, qint64 userId, const QString& reason);

    core::DbService* db_;
    QHash<QString, Session> sessions_;
    QMutex mutex_;
    int sessionMinutes_;
};

} // namespace server

#endif // SERVER_REQUEST_HANDLER_H
