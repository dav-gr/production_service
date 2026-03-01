#ifndef SERVER_SESSION_MANAGER_H
#define SERVER_SESSION_MANAGER_H

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QDateTime>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
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
// Session Manager
// ============================================================================

class SessionManager : public QObject {
    Q_OBJECT

public:
    explicit SessionManager(core::DbService* db, int sessionMinutes = 480,
                            QObject* parent = nullptr);

    // Authentication
    struct LoginResult {
        bool success = false;
        QString error;
        Session session;
        QJsonObject responseData;  // token, user_id, username, full_name, permissions
    };

    LoginResult login(const QString& username, const QString& pin);
    bool logout(const QString& token);

    // Session access
    Session* getSession(const QString& token);
    void cleanExpiredSessions();

private:
    QString createSession(qint64 userId, const QString& username,
                          const QStringList& permissions);

    core::DbService* db_;
    QHash<QString, Session> sessions_;
    QMutex mutex_;
    int sessionMinutes_;
};

} // namespace server

#endif // SERVER_SESSION_MANAGER_H
