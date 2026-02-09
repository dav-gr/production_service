#include "request_handler.h"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

namespace server {

RequestHandler::RequestHandler(core::DbService* db, int sessionMinutes, QObject* parent)
    : QObject(parent), db_(db), sessionMinutes_(sessionMinutes)
{
}

Response RequestHandler::handle(const Request& request) {
    cleanExpiredSessions();
    
    if (request.action == "login")            return doLogin(request);
    if (request.action == "logout")           return doLogout(request);
    if (request.action == "validate_barcode") return doValidate(request);
    if (request.action == "process_barcode")  return doProcess(request);
    
    return Response::error("Unknown action: " + request.action);
}

// ============================================================================
// Login
// ============================================================================

Response RequestHandler::doLogin(const Request& req) {
    QString username = req.getString("username");
    QString pin = req.getString("pin");
    
    if (username.isEmpty() || pin.isEmpty()) {
        return Response::error("Username and PIN required");
    }
    
    // Hash PIN
    QString pinHash = QString::fromLatin1(
        QCryptographicHash::hash(pin.toUtf8(), QCryptographicHash::Sha256).toHex());
    
    // Authenticate
    auto auth = db_->authenticate(username, pinHash);
    if (!auth) {
        qWarning() << "[Handler] Login failed:" << username;
        return Response::error("Invalid username or PIN");
    }
    
    if (!auth->user.active) {
        return Response::error("Account disabled");
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
    
    QJsonObject data;
    data["token"] = token;
    data["user_id"] = auth->user.id;
    data["username"] = username;
    data["full_name"] = auth->user.fullName;
    data["permissions"] = QJsonArray::fromStringList(perms);
    
    qInfo() << "[Handler] Login OK:" << username;
    return Response::ok("Login successful", data);
}

// ============================================================================
// Logout
// ============================================================================

Response RequestHandler::doLogout(const Request& req) {
    QString token = req.getString("token");
    
    QMutexLocker lock(&mutex_);
    if (sessions_.remove(token)) {
        return Response::ok("Logged out");
    }
    return Response::ok("Session not found");
}

// ============================================================================
// Validate Barcode
// ============================================================================

Response RequestHandler::doValidate(const Request& req) {
    QString token = req.getString("token");
    Session* session = getSession(token);
    if (!session) {
        return Response::error("Invalid or expired session");
    }
    
    QString barcode = req.getString("barcode");
    if (barcode.isEmpty()) {
        return Response::error("Barcode required");
    }
    
    return lookupBarcode(barcode);
}

// ============================================================================
// Process Barcode
// ============================================================================

Response RequestHandler::doProcess(const Request& req) {
    QString token = req.getString("token");
    Session* session = getSession(token);
    if (!session) {
        return Response::error("Invalid or expired session");
    }
    
    QString barcode = req.getString("barcode");
    if (barcode.isEmpty()) {
        return Response::error("Barcode required");
    }
    
    QSqlDatabase sqlDb = db_->getDatabase();
    
    // Check if ITEM
    {
        QSqlQuery q(sqlDb);
        q.prepare(R"(
            SELECT i.id, i.status, iba.box_id
            FROM items i
            LEFT JOIN item_box_assignments iba ON i.id = iba.item_id
            WHERE i.bar_code = :bc
        )");
        q.bindValue(":bc", barcode);
        
        if (q.exec() && q.next()) {
            qint64 itemId = q.value(0).toLongLong();
            int status = q.value(1).toInt();
            bool hasBox = !q.value(2).isNull();
            qint64 boxId = q.value(2).toLongLong();
            
            if (status != 1) {
                return Response::error("Item status must be 1", 
                    QJsonObject{{"status", status}});
            }
            
            if (hasBox) {
                // Item in box - need both permissions
                if (!session->hasPermission("postprod.remove_item")) {
                    return Response::error("Permission denied: postprod.remove_item");
                }
                if (!session->hasPermission("postprod.unseal_box")) {
                    return Response::error("Permission denied: postprod.unseal_box");
                }
                return processItemInBox(itemId, boxId, barcode, session->userId);
            } else {
                // Loose item (packaging mode)
                if (!session->hasPermission("postprod.remove_item")) {
                    return Response::error("Permission denied: postprod.remove_item");
                }
                return processLooseItem(itemId, barcode, session->userId);
            }
        }
    }
    
    // Check if BOX
    {
        QSqlQuery q(sqlDb);
        q.prepare("SELECT id, status FROM boxes WHERE bar_code = :bc");
        q.bindValue(":bc", barcode);
        
        if (q.exec() && q.next()) {
            qint64 boxId = q.value(0).toLongLong();
            int status = q.value(1).toInt();
            
            if (status != 1) {
                return Response::error("Box status must be 1 (sealed)",
                    QJsonObject{{"status", status}});
            }
            
            if (!session->hasPermission("postprod.unseal_box")) {
                return Response::error("Permission denied: postprod.unseal_box");
            }
            return processBox(boxId, barcode, session->userId);
        }
    }
    
    return Response::error("Barcode not found", QJsonObject{{"barcode", barcode}});
}

// ============================================================================
// Lookup (for validate_barcode)
// ============================================================================

Response RequestHandler::lookupBarcode(const QString& barcode) {
    QSqlDatabase sqlDb = db_->getDatabase();
    
    // Try item
    {
        QSqlQuery q(sqlDb);
        q.prepare(R"(
            SELECT i.id, i.status, iba.box_id, b.bar_code
            FROM items i
            LEFT JOIN item_box_assignments iba ON i.id = iba.item_id
            LEFT JOIN boxes b ON iba.box_id = b.id
            WHERE i.bar_code = :bc
        )");
        q.bindValue(":bc", barcode);
        
        if (q.exec() && q.next()) {
            QJsonObject data;
            data["barcode"] = barcode;
            data["entity_type"] = "item";
            data["entity_id"] = q.value(0).toLongLong();
            data["status"] = q.value(1).toInt();
            data["in_box"] = !q.value(2).isNull();
            if (!q.value(2).isNull()) {
                data["box_id"] = q.value(2).toLongLong();
                data["box_barcode"] = q.value(3).toString();
            }
            return Response::ok("Found", data);
        }
    }
    
    // Try box
    {
        QSqlQuery q(sqlDb);
        q.prepare(R"(
            SELECT b.id, b.status, COUNT(iba.id)
            FROM boxes b
            LEFT JOIN item_box_assignments iba ON b.id = iba.box_id
            WHERE b.bar_code = :bc
            GROUP BY b.id
        )");
        q.bindValue(":bc", barcode);
        
        if (q.exec() && q.next()) {
            QJsonObject data;
            data["barcode"] = barcode;
            data["entity_type"] = "box";
            data["entity_id"] = q.value(0).toLongLong();
            data["status"] = q.value(1).toInt();
            data["item_count"] = q.value(2).toInt();
            return Response::ok("Found", data);
        }
    }
    
    return Response::error("Barcode not found", QJsonObject{{"barcode", barcode}});
}

// ============================================================================
// Process Item In Box
// ============================================================================

Response RequestHandler::processItemInBox(qint64 itemId, qint64 boxId,
                                           const QString& itemBarcode, qint64 userId)
{
    Q_UNUSED(itemId)
    QSqlDatabase sqlDb = db_->getDatabase();
    
    // Get box barcode
    QString boxBarcode;
    {
        QSqlQuery q(sqlDb);
        q.prepare("SELECT bar_code FROM boxes WHERE id = :id");
        q.bindValue(":id", boxId);
        if (q.exec() && q.next()) boxBarcode = q.value(0).toString();
    }
    
    sqlDb.transaction();
    
    // Get all items in box
    QVector<qint64> itemIds;
    {
        QSqlQuery q(sqlDb);
        q.prepare("SELECT item_id FROM item_box_assignments WHERE box_id = :id");
        q.bindValue(":id", boxId);
        if (q.exec()) {
            while (q.next()) itemIds << q.value(0).toLongLong();
        }
    }
    
    // Remove assignments
    removeItemsFromBox(boxId);
    
    // Reset all items
    int resetCount = 0;
    for (qint64 id : itemIds) {
        if (resetItem(id)) resetCount++;
    }
    
    // Unseal box
    bool unsealed = unsealBox(boxId);
    
    // Audit
    writeAudit("box", boxBarcode, "unsealed", userId,
               QString("Via item %1, items reset: %2").arg(itemBarcode).arg(resetCount));
    
    sqlDb.commit();
    
    QJsonObject data;
    data["operation"] = "unseal_box";
    data["barcode"] = itemBarcode;
    data["entity_type"] = "item";
    data["items_reset"] = resetCount;
    data["box_barcode"] = boxBarcode;
    data["box_unsealed"] = unsealed;
    
    qInfo() << "[Handler] Unsealed box" << boxBarcode << "items:" << resetCount;
    return Response::ok("Box unsealed", data);
}

// ============================================================================
// Process Loose Item
// ============================================================================

Response RequestHandler::processLooseItem(qint64 itemId, const QString& barcode, 
                                           qint64 userId)
{
    QSqlDatabase sqlDb = db_->getDatabase();
    sqlDb.transaction();
    
    bool reset = resetItem(itemId);
    writeAudit("item", barcode, "removed", userId, "Packaging mode reset");
    
    sqlDb.commit();
    
    QJsonObject data;
    data["operation"] = "reset_loose_item";
    data["barcode"] = barcode;
    data["entity_type"] = "item";
    data["items_reset"] = reset ? 1 : 0;
    data["box_unsealed"] = false;
    
    qInfo() << "[Handler] Reset loose item" << barcode;
    return Response::ok("Item reset", data);
}

// ============================================================================
// Process Box
// ============================================================================

Response RequestHandler::processBox(qint64 boxId, const QString& barcode, qint64 userId)
{
    QSqlDatabase sqlDb = db_->getDatabase();
    sqlDb.transaction();
    
    // Get all items
    QVector<qint64> itemIds;
    {
        QSqlQuery q(sqlDb);
        q.prepare("SELECT item_id FROM item_box_assignments WHERE box_id = :id");
        q.bindValue(":id", boxId);
        if (q.exec()) {
            while (q.next()) itemIds << q.value(0).toLongLong();
        }
    }
    
    // Remove assignments
    removeItemsFromBox(boxId);
    
    // Reset items
    int resetCount = 0;
    for (qint64 id : itemIds) {
        if (resetItem(id)) resetCount++;
    }
    
    // Unseal
    bool unsealed = unsealBox(boxId);
    
    // Audit
    writeAudit("box", barcode, "unsealed", userId,
               QString("Direct scan, items reset: %1").arg(resetCount));
    
    sqlDb.commit();
    
    QJsonObject data;
    data["operation"] = "unseal_box";
    data["barcode"] = barcode;
    data["entity_type"] = "box";
    data["items_reset"] = resetCount;
    data["box_barcode"] = barcode;
    data["box_unsealed"] = unsealed;
    
    qInfo() << "[Handler] Unsealed box" << barcode << "items:" << resetCount;
    return Response::ok("Box unsealed", data);
}

// ============================================================================
// DB Helpers
// ============================================================================

bool RequestHandler::unsealBox(qint64 boxId) {
    QSqlQuery q(db_->getDatabase());
    q.prepare("UPDATE boxes SET status = 0 WHERE id = :id AND status = 1");
    q.bindValue(":id", boxId);
    return q.exec() && q.numRowsAffected() > 0;
}

int RequestHandler::removeItemsFromBox(qint64 boxId) {
    QSqlQuery q(db_->getDatabase());
    q.prepare("DELETE FROM item_box_assignments WHERE box_id = :id");
    q.bindValue(":id", boxId);
    if (q.exec()) return q.numRowsAffected();
    return 0;
}

bool RequestHandler::resetItem(qint64 itemId) {
    QSqlQuery q(db_->getDatabase());
    q.prepare("UPDATE items SET status = 0, scanned_at = NULL WHERE id = :id AND status = 1");
    q.bindValue(":id", itemId);
    return q.exec() && q.numRowsAffected() > 0;
}

void RequestHandler::writeAudit(const QString& entityType, const QString& barcode,
                                 const QString& action, qint64 userId, const QString& reason)
{
    QSqlQuery q(db_->getDatabase());
    q.prepare(R"(
        INSERT INTO audit_log (entity_type, entity_barcode, action, old_value, new_value, performed_by, reason)
        VALUES (:type, :barcode, :action, '1', '0', :user, :reason)
    )");
    q.bindValue(":type", entityType);
    q.bindValue(":barcode", barcode);
    q.bindValue(":action", action);
    q.bindValue(":user", userId);
    q.bindValue(":reason", reason);
    q.exec();
}

// ============================================================================
// Session Management
// ============================================================================

QString RequestHandler::createSession(qint64 userId, const QString& username,
                                        const QStringList& permissions)
{
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

Session* RequestHandler::getSession(const QString& token) {
    QMutexLocker lock(&mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return nullptr;
    if (!it->isValid()) {
        sessions_.erase(it);
        return nullptr;
    }
    return &(*it);
}

void RequestHandler::cleanExpiredSessions() {
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

} // namespace server
