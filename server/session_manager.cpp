#include "session_manager.h"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDebug>

namespace server {

SessionManager::SessionManager(core::DbService* db, int sessionMinutes, QObject* parent)
    : QObject(parent)
    , db_(db)
    , sessionMinutes_(sessionMinutes)
{
}

SessionManager::LoginResult SessionManager::login(const QString& username, const QString& pin) {
    LoginResult result;

    if (username.isEmpty() || pin.isEmpty()) {
        result.error = "Username and PIN required";
        return result;
    }

    // Hash PIN
    QString pinHash = QString::fromLatin1(
        QCryptographicHash::hash(pin.toUtf8(), QCryptographicHash::Sha256).toHex());

    // Authenticate
    auto auth = db_->authenticate(username, pinHash);
    if (!auth) {
        qWarning() << "[SessionManager] Login failed:" << username;
        result.error = "Invalid username or PIN";
        return result;
    }

    if (!auth->user.active) {
        result.error = "Account disabled";
        return result;
    }

    // Collect permissions
    QStringList perms;
    if (auth->user.superuser) {
        perms << "postprod.mark_damaged" << "postprod.remove_item"
              << "postprod.unseal_box" << "postprod.view_audit";
    } else {
        for (const auto& p : auth->permissions) {
            perms << p.name;
        }
    }

    // Create session
    QString token = createSession(auth->user.id, username, perms);

    // Build response data
    QJsonObject data;
    data["token"] = token;
    data["user_id"] = auth->user.id;
    data["username"] = username;
    data["full_name"] = auth->user.fullName;
    data["permissions"] = QJsonArray::fromStringList(perms);

    result.success = true;
    result.session = *getSession(token);
    result.responseData = data;

    qInfo() << "[SessionManager] Login OK:" << username;
    return result;
}

bool SessionManager::logout(const QString& token) {
    QMutexLocker lock(&mutex_);
    return sessions_.remove(token) > 0;
}

Session* SessionManager::getSession(const QString& token) {
    QMutexLocker lock(&mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return nullptr;
    if (!it->isValid()) {
        sessions_.erase(it);
        return nullptr;
    }
    return &(*it);
}

void SessionManager::cleanExpiredSessions() {
    QMutexLocker lock(&mutex_);
    QDateTime now = QDateTime::currentDateTime();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now > it->expiresAt) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

QString SessionManager::createSession(qint64 userId, const QString& username,
                                       const QStringList& permissions) {
    QMutexLocker lock(&mutex_);

    // Generate token
    QByteArray rand(32, 0);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(rand.data()), 8);
    QString token = QString::fromLatin1(
        QCryptographicHash::hash(rand, QCryptographicHash::Sha256).toHex());

    Session s;
    s.token = token;
    s.userId = userId;
    s.username = username;
    s.permissions = permissions;
    s.expiresAt = QDateTime::currentDateTime().addSecs(sessionMinutes_ * 60);

    sessions_.insert(token, s);
    return token;
}

} // namespace server
