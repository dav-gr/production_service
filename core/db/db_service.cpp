#include "db_service.h"

#include <QSqlError>
#include <QSqlRecord>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QUuid>
#include <QtConcurrent>
#include <QDebug>
#include <QThread>
#include <QRandomGenerator>

namespace core {

// ============================================================================
// Construction / Destruction
// ============================================================================

DbService::DbService(QObject* parent)
    : QObject(parent)
    , connectionName_(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

DbService::~DbService() {
    disconnect();
}

// ============================================================================
// Connection Management
// ============================================================================

bool DbService::connect(const AppConfig& config) {
    return connect(config.host, config.port, config.database, 
                   config.user, config.password);
}

bool DbService::connect(const QString& host, int port, const QString& database,
                        const QString& user, const QString& password) {
    QMutexLocker locker(&mutex_);
    
    // Store config for reconnect
    config_.host = host;
    config_.port = port;
    config_.database = database;
    config_.user = user;
    config_.password = password;
    
    // Remove existing connection if any
    if (QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase::removeDatabase(connectionName_);
    }
    
    // Create new connection
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", connectionName_);
    db.setHostName(host);
    db.setPort(port);
    db.setDatabaseName(database);
    db.setUserName(user);
    db.setPassword(password);
    db.setConnectOptions("connect_timeout=10");
    
    if (!db.open()) {
        lastError_ = db.lastError().text();
        qWarning() << "DbService: Connection failed:" << lastError_;
        return false;
    }
    
    config_.validated = true;
    qDebug() << "DbService: Connected to" << host << ":" << port << "/" << database;
    return true;
}

void DbService::disconnect() {
    QMutexLocker locker(&mutex_);
    
    if (QSqlDatabase::contains(connectionName_)) {
        {
            QSqlDatabase db = QSqlDatabase::database(connectionName_);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connectionName_);
    }
    qDebug() << "DbService: Disconnected";
}

bool DbService::isConnected() const {
    QMutexLocker locker(&mutex_);
    
    if (!QSqlDatabase::contains(connectionName_)) {
        return false;
    }
    
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    return db.isOpen() && db.isValid();
}

bool DbService::reconnect() {
    qDebug() << "DbService: Attempting reconnect...";
    disconnect();
    return connect(config_);
}

QString DbService::lastError() const {
    QMutexLocker locker(&mutex_);
    return lastError_;
}

bool DbService::testConnection(const AppConfig& config, QString* errorOut) {
    QString testConnName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", testConnName);
    db.setHostName(config.host);
    db.setPort(config.port);
    db.setDatabaseName(config.database);
    db.setUserName(config.user);
    db.setPassword(config.password);
    db.setConnectOptions("connect_timeout=10");
    
    bool success = db.open();
    
    if (!success && errorOut) {
        *errorOut = db.lastError().text();
    }
    
    db.close();
    QSqlDatabase::removeDatabase(testConnName);
    
    return success;
}

bool DbService::ensureConnected() {
    if (isConnected()) {
        return true;
    }
    
    if (reconnect()) {
        emit connectionRestored();
        return true;
    }
    
    emit connectionLost();
    return false;
}

QSqlDatabase DbService::getDatabase() {
    return QSqlDatabase::database(connectionName_);
}

// ============================================================================
// Authentication
// ============================================================================

std::optional<AuthenticatedUser> DbService::authenticate(const QString& username, const QString& pinHash) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT id, username, pin_hash, full_name, email, phone_number, "
        "       active, superuser, created_at, last_login "
        "FROM users WHERE username = :username AND pin_hash = :hash AND active = true"
    );
    query.bindValue(":username", username);
    query.bindValue(":hash", pinHash);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        qWarning() << "DbService: Auth query failed:" << lastError_;
        return std::nullopt;
    }
    
    if (!query.next()) {
        QMutexLocker locker(&mutex_);
        lastError_ = "Invalid username or PIN";
        return std::nullopt;
    }
    
    User user = parseUser(query);
    
    // Update last login
    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE users SET last_login = NOW() WHERE id = :id");
    updateQuery.bindValue(":id", user.id);
    updateQuery.exec();
    
    // Build authenticated user
    AuthenticatedUser authUser;
    authUser.user = user;
    
    // Get roles
    QSqlQuery rolesQuery(db);
    rolesQuery.prepare(
        "SELECT r.id, r.role_name, r.description, r.active "
        "FROM roles r "
        "JOIN user_roles ur ON r.id = ur.role_id "
        "WHERE ur.user_id = :userId AND r.active = true"
    );
    rolesQuery.bindValue(":userId", user.id);
    
    if (rolesQuery.exec()) {
        while (rolesQuery.next()) {
            authUser.roles.append(parseRole(rolesQuery));
        }
    }
    
    // Get permissions
    QSqlQuery permsQuery(db);
    permsQuery.prepare(
        "SELECT DISTINCT p.id, p.permission_name, p.category, p.description "
        "FROM permissions p "
        "JOIN role_permissions rp ON p.id = rp.permission_id "
        "JOIN user_roles ur ON rp.role_id = ur.role_id "
        "WHERE ur.user_id = :userId AND rp.granted = true AND p.active = true"
    );
    permsQuery.bindValue(":userId", user.id);
    
    if (permsQuery.exec()) {
        while (permsQuery.next()) {
            authUser.permissions.append(parsePermission(permsQuery));
        }
    }
    
    qDebug() << "DbService: User" << user.username << "authenticated";
    return authUser;
}

std::optional<User> DbService::getUserByUsername(const QString& username) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT id, username, pin_hash, full_name, email, phone_number, "
        "       active, superuser, created_at, last_login "
        "FROM users WHERE username = :username"
    );
    query.bindValue(":username", username);
    
    if (query.exec() && query.next()) {
        return parseUser(query);
    }
    
    return std::nullopt;
}

std::optional<User> DbService::getUser(UserId userId) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT id, username, pin_hash, full_name, email, phone_number, "
        "       active, superuser, created_at, last_login "
        "FROM users WHERE id = :id"
    );
    query.bindValue(":id", userId);
    
    if (query.exec() && query.next()) {
        return parseUser(query);
    }
    
    return std::nullopt;
}

// ============================================================================
// Production Lines
// ============================================================================

QVector<ProductionLine> DbService::getProductionLines() {
    QVector<ProductionLine> lines;
    
    if (!ensureConnected()) return lines;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, name, created_at FROM production_lines ORDER BY name")) {
        while (query.next()) {
            ProductionLine line;
            line.id = query.value(0).toLongLong();
            line.name = query.value(1).toString();
            line.createdAt = query.value(2).toDateTime();
            lines.append(line);
        }
    }
    
    return lines;
}

std::optional<ProductionLine> DbService::getProductionLine(ProductionLineId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT id, name, created_at FROM production_lines WHERE id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        ProductionLine line;
        line.id = query.value(0).toLongLong();
        line.name = query.value(1).toString();
        line.createdAt = query.value(2).toDateTime();
        return line;
    }
    
    return std::nullopt;
}

// ============================================================================
// Import Operations
// ============================================================================

QFuture<ImportResult> DbService::importItemsAsync(const QString& filePath,
                                                   ProductionLineId lineId) {
    return QtConcurrent::run([this, filePath, lineId]() {
        return doImport(filePath, lineId, "items");
    });
}

QFuture<ImportResult> DbService::importBoxesAsync(const QString& filePath,
                                                   ProductionLineId lineId) {
    return QtConcurrent::run([this, filePath, lineId]() {
        return doImport(filePath, lineId, "boxes");
    });
}

QFuture<ImportResult> DbService::importPalletsAsync(const QString& filePath,
                                                     ProductionLineId lineId) {
    return QtConcurrent::run([this, filePath, lineId]() {
        return doImport(filePath, lineId, "pallets");
    });
}

// Helper: Create thread-local database connection
static QSqlDatabase createThreadLocalConnection(const QString& host, int port,
                                                 const QString& database,
                                                 const QString& user, 
                                                 const QString& password) {
    // Each thread gets its own unique connection name
    QString threadConnName = QString("thread_%1_%2")
        .arg(quintptr(QThread::currentThread()), 0, 16)
        .arg(QRandomGenerator::global()->generate(), 0, 16);
    
    // Create connection for this thread
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", threadConnName);
    db.setHostName(host);
    db.setPort(port);
    db.setDatabaseName(database);
    db.setUserName(user);
    db.setPassword(password);
    db.setConnectOptions("connect_timeout=10");
    
    if (!db.open()) {
        qWarning() << "Failed to open thread-local DB connection:" << db.lastError().text();
        return QSqlDatabase();
    }
    
    return db;
}

ImportResult DbService::doImport(const QString& filePath, ProductionLineId lineId,
                               const QString& tableName) {
ImportResult result;
    
// Read CSV file with explicit UTF-8 encoding to preserve GS1 control characters
QFile file(filePath);
if (!file.open(QIODevice::ReadOnly)) {  // Don't use QIODevice::Text
    result.errors.append("Cannot open file: " + filePath);
    return result;
}

// Read as raw bytes, then decode as UTF-8 explicitly
QByteArray rawData = file.readAll();
file.close();

QString content = QString::fromUtf8(rawData);

// Split into lines (handle both Unix \n and Windows \r\n)
QStringList lines = content.split(QRegularExpression("\\r?\\n"), Qt::SkipEmptyParts);

QStringList barcodes;
for (const QString& line : lines) {
    QString barcode = line.trimmed();
    if (!barcode.isEmpty()) {
        barcodes.append(barcode);
    }
}
    
result.totalRecords = barcodes.size();
    
if (barcodes.isEmpty()) {
    result.errors.append("No valid barcodes found");
    return result;
}
    
// Create thread-local database connection for async operation
QString dbHost, dbDatabase, dbUser, dbPassword;
int dbPort;
{
    QMutexLocker locker(&mutex_);
    dbHost = config_.host;
    dbPort = config_.port;
    dbDatabase = config_.database;
    dbUser = config_.user;
    dbPassword = config_.password;
}
    
QSqlDatabase db = createThreadLocalConnection(dbHost, dbPort, 
                                               dbDatabase, dbUser, dbPassword);
    
if (!db.isOpen()) {
    result.errors.append("Failed to create database connection: Driver not loaded or connection failed");
    return result;
}
    
    QString timestampCol = (tableName == "pallets") ? "created_at" : "imported_at";
    
    // Process in batches
    const int batchSize = 500;
    int processed = 0;
    
    db.transaction();
    
    for (int i = 0; i < barcodes.size(); i += batchSize) {
        QStringList batch = barcodes.mid(i, qMin(batchSize, barcodes.size() - i));
        
        // Build parameterized query with placeholders
        QStringList valuePlaceholders;
        for (int j = 0; j < batch.size(); ++j) {
            valuePlaceholders.append(QString("(:bc%1, :lineId, 0, NOW())").arg(j));
        }
        
        QString sql = QString(
            "INSERT INTO %1 (bar_code, production_line, status, %2) "
            "VALUES %3 ON CONFLICT (bar_code) DO NOTHING"
        ).arg(tableName, timestampCol, valuePlaceholders.join(", "));
        
        QSqlQuery query(db);
        query.prepare(sql);
        
        // Bind each barcode value safely
        for (int j = 0; j < batch.size(); ++j) {
            query.bindValue(QString(":bc%1").arg(j), batch[j]);
        }
        query.bindValue(":lineId", lineId);
        
        if (query.exec()) {
            int affected = query.numRowsAffected();
            result.importedCount += affected;
            result.skippedCount += batch.size() - affected;
        } else {
            result.errorCount += batch.size();
            result.errors.append(query.lastError().text());
        }
        
        processed += batch.size();
        emit importProgress(processed, result.totalRecords);
    }
    
if (result.errorCount == 0) {
    db.commit();
} else {
    db.rollback();
}
    
qDebug() << "DbService: Import complete -" << result.summary();
return result;
}

// ============================================================================
// Item Operations
// ============================================================================

std::optional<Item> DbService::getItem(ItemId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    query.prepare(
        "SELECT id, bar_code, status, production_line, imported_at, scanned_at "
        "FROM items WHERE id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parseItem(query);
    }
    
    return std::nullopt;
}

QVector<Item> DbService::getItemsByStatus(ItemStatus status, ProductionLineId lineId,
                                           int limit) {
    QVector<Item> items;
    
    if (!ensureConnected()) return items;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT id, bar_code, status, production_line, imported_at, scanned_at "
        "FROM items WHERE status = :status";
    
    if (lineId > 0) {
        sql += " AND production_line = :lineId";
    }
    sql += " ORDER BY imported_at LIMIT :limit";
    
    query.prepare(sql);
    query.bindValue(":status", static_cast<int>(status));
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    query.bindValue(":limit", limit);
    
    if (query.exec()) {
        while (query.next()) {
            items.append(parseItem(query));
        }
    }
    
    return items;
}

QVector<Item> DbService::getItemsInBox(BoxId boxId) {
    QVector<Item> items;
    
    if (!ensureConnected()) return items;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    query.prepare(
        "SELECT i.id, i.bar_code, i.status, i.production_line, i.imported_at, i.scanned_at "
        "FROM items i "
        "JOIN item_box_assignments iba ON i.id = iba.item_id "
        "JOIN boxes b ON iba.box_id = b.id "
        "WHERE b.id = :boxId ORDER BY iba.assigned_at"
    );
    query.bindValue(":boxId", boxId);
    
    if (query.exec()) {
        while (query.next()) {
            items.append(parseItem(query));
        }
    }
    
    return items;
}

QVector<Item> DbService::getScannedItemsNotInBox(ProductionLineId lineId, int limit) {
    QVector<Item> items;
    
    if (!ensureConnected()) return items;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT i.id, i.bar_code, i.status, i.production_line, i.imported_at, i.scanned_at "
        "FROM items i "
        "LEFT JOIN item_box_assignments iba ON i.id = iba.item_id "
        "WHERE i.status = 1 AND iba.item_id IS NULL";
    
    if (lineId > 0) {
        sql += " AND i.production_line = :lineId";
    }
    sql += " ORDER BY i.scanned_at LIMIT :limit";
    
    query.prepare(sql);
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    query.bindValue(":limit", limit);
    
    if (query.exec()) {
        while (query.next()) {
            items.append(parseItem(query));
        }
    }
    
    return items;
}

int DbService::countScannedItemsNotInBox(ProductionLineId lineId) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT COUNT(*) FROM items i "
        "LEFT JOIN item_box_assignments iba ON i.id = iba.item_id "
        "WHERE i.status = 1 AND iba.item_id IS NULL";
    
    if (lineId > 0) {
        sql += " AND i.production_line = :lineId";
    }
    
    query.prepare(sql);
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

bool DbService::assignItemToBox(ItemId itemId, BoxId boxId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    db.transaction();
    
    // Verify box exists and is empty (status = 0)
    QSqlQuery boxQuery(db);
    boxQuery.prepare("SELECT id, status FROM boxes WHERE id = :id");
    boxQuery.bindValue(":id", boxId);
    
    if (!boxQuery.exec() || !boxQuery.next()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Box not found";
        return false;
    }
    
    if (boxQuery.value(1).toInt() != 0) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Box must be Empty";
        return false;
    }
    
    // Verify item exists and is available (status = 0)
    QSqlQuery itemQuery(db);
    itemQuery.prepare("SELECT id, status FROM items WHERE id = :id");
    itemQuery.bindValue(":id", itemId);
    
    if (!itemQuery.exec() || !itemQuery.next()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Item not found";
        return false;
    }
    
    if (itemQuery.value(1).toInt() != 0) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Item must be Available";
        return false;
    }
    
    // Create assignment
    QSqlQuery assignQuery(db);
    assignQuery.prepare(
        "INSERT INTO item_box_assignments (item_id, box_id, assigned_at) "
        "VALUES (:itemId, :boxId, NOW())"
    );
    assignQuery.bindValue(":itemId", itemId);
    assignQuery.bindValue(":boxId", boxId);
    
    if (!assignQuery.exec()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = assignQuery.lastError().text();
        return false;
    }
    
    // Update item status
    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE items SET status = 1 WHERE id = :id");
    updateQuery.bindValue(":id", itemId);
    
    if (!updateQuery.exec()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = updateQuery.lastError().text();
        return false;
    }
    
    db.commit();
    return true;
}

int DbService::assignItemsToBox(const QVector<ItemId>& itemIds, BoxId boxId) {
    int count = 0;
    for (ItemId itemId : itemIds) {
        if (assignItemToBox(itemId, boxId)) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Box Operations
// ============================================================================

std::optional<Box> DbService::getBox(BoxId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    query.prepare(
        "SELECT id, bar_code, status, production_line, imported_at, sealed_at "
        "FROM boxes WHERE id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parseBox(query);
    }
    
    return std::nullopt;
}

QVector<Box> DbService::getBoxesByStatus(BoxStatus status, ProductionLineId lineId,
                                          int limit) {
    QVector<Box> boxes;
    
    if (!ensureConnected()) return boxes;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT id, bar_code, status, production_line, imported_at, sealed_at "
        "FROM boxes WHERE status = :status";
    
    if (lineId > 0) {
        sql += " AND production_line = :lineId";
    }
    sql += " ORDER BY imported_at LIMIT :limit";
    
    query.prepare(sql);
    query.bindValue(":status", static_cast<int>(status));
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    query.bindValue(":limit", limit);
    
    if (query.exec()) {
        while (query.next()) {
            boxes.append(parseBox(query));
        }
    }
    
    return boxes;
}

QVector<Box> DbService::getSealedBoxesNotOnPallet(ProductionLineId lineId, int limit) {
    QVector<Box> boxes;
    
    if (!ensureConnected()) return boxes;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT b.id, b.bar_code, b.status, b.production_line, b.imported_at, b.sealed_at "
        "FROM boxes b "
        "LEFT JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "WHERE b.status = 1 AND pba.box_id IS NULL";
    
    if (lineId > 0) {
        sql += " AND b.production_line = :lineId";
    }
    sql += " ORDER BY b.imported_at LIMIT :limit";
    
    query.prepare(sql);
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    query.bindValue(":limit", limit);
    
    if (query.exec()) {
        while (query.next()) {
            boxes.append(parseBox(query));
        }
    }
    
    return boxes;
}

int DbService::countSealedBoxesNotOnPallet(ProductionLineId lineId) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT COUNT(*) FROM boxes b "
        "LEFT JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "WHERE b.status = 1 AND pba.box_id IS NULL";
    
    if (lineId > 0) {
        sql += " AND b.production_line = :lineId";
    }
    
    query.prepare(sql);
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

QVector<Box> DbService::getBoxesOnPallet(PalletId palletId) {
    QVector<Box> boxes;
    
    if (!ensureConnected()) return boxes;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT b.id, b.bar_code, b.status, b.production_line, b.imported_at, b.sealed_at "
        "FROM boxes b "
        "JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "JOIN pallets p ON pba.pallet_id = p.id "
        "WHERE p.id = :palletId ORDER BY pba.assigned_at"
    );
    query.bindValue(":palletId", palletId);
    
    if (query.exec()) {
        while (query.next()) {
            boxes.append(parseBox(query));
        }
    }
    
    return boxes;
}

bool DbService::sealBox(BoxId id) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    
    // Check box has items
    QSqlQuery countQuery(db);
    countQuery.prepare(
        "SELECT COUNT(*) FROM item_box_assignments iba "
        "JOIN boxes b ON iba.box_id = b.id WHERE b.id = :id"
    );
    countQuery.bindValue(":id", id);
    
    if (!countQuery.exec() || !countQuery.next() || countQuery.value(0).toInt() == 0) {
        QMutexLocker locker(&mutex_);
        lastError_ = "Box is empty - cannot seal";
        return false;
    }
    
    QSqlQuery query(db);
    query.prepare(
        "UPDATE boxes SET status = 1, sealed_at = NOW() "
        "WHERE id = :id AND status = 0"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.numRowsAffected() > 0) {
        return true;
    }
    
    QMutexLocker locker(&mutex_);
    lastError_ = "Box not found or not Empty";
    return false;
}

bool DbService::assignBoxToPallet(BoxId boxId, PalletId palletId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    db.transaction();
    
    // Verify pallet exists and is new (status = 0)
    QSqlQuery palletQuery(db);
    palletQuery.prepare("SELECT id, status FROM pallets WHERE id = :id");
    palletQuery.bindValue(":id", palletId);
    
    if (!palletQuery.exec() || !palletQuery.next()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Pallet not found";
        return false;
    }
    
    if (palletQuery.value(1).toInt() != 0) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Pallet must be New";
        return false;
    }
    
    // Verify box exists and is sealed (status = 1)
    QSqlQuery boxQuery(db);
    boxQuery.prepare("SELECT id, status FROM boxes WHERE id = :id");
    boxQuery.bindValue(":id", boxId);
    
    if (!boxQuery.exec() || !boxQuery.next()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Box not found";
        return false;
    }
    
    if (boxQuery.value(1).toInt() != 1) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = "Box must be Sealed";
        return false;
    }
    
    // Create assignment
    QSqlQuery assignQuery(db);
    assignQuery.prepare(
        "INSERT INTO pallet_box_assignments (box_id, pallet_id, assigned_at) "
        "VALUES (:boxId, :palletId, NOW())"
    );
    assignQuery.bindValue(":boxId", boxId);
    assignQuery.bindValue(":palletId", palletId);
    
    if (!assignQuery.exec()) {
        db.rollback();
        QMutexLocker locker(&mutex_);
        lastError_ = assignQuery.lastError().text();
        return false;
    }
    
    db.commit();
    return true;
}

int DbService::getBoxItemCount(BoxId id) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT COUNT(*) FROM item_box_assignments iba "
        "JOIN boxes b ON iba.box_id = b.id WHERE b.id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

// ============================================================================
// Pallet Operations
// ============================================================================

std::optional<Pallet> DbService::getPallet(PalletId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    query.prepare(
        "SELECT id, bar_code, status, production_line, created_at "
        "FROM pallets WHERE id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parsePallet(query);
    }
    
    return std::nullopt;
}

QVector<Pallet> DbService::getPalletsByStatus(PalletStatus status, ProductionLineId lineId,
                                               int limit) {
    QVector<Pallet> pallets;
    
    if (!ensureConnected()) return pallets;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    QString sql = 
        "SELECT id, bar_code, status, production_line, created_at "
        "FROM pallets WHERE status = :status";
    
    if (lineId > 0) {
        sql += " AND production_line = :lineId";
    }
    sql += " ORDER BY created_at LIMIT :limit";
    
    query.prepare(sql);
    query.bindValue(":status", static_cast<int>(status));
    if (lineId > 0) {
        query.bindValue(":lineId", lineId);
    }
    query.bindValue(":limit", limit);
    
    if (query.exec()) {
        while (query.next()) {
            pallets.append(parsePallet(query));
        }
    }
    
    return pallets;
}

bool DbService::completePallet(PalletId id) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    
    QSqlQuery countQuery(db);
    countQuery.prepare(
        "SELECT COUNT(*) FROM pallet_box_assignments pba "
        "JOIN pallets p ON pba.pallet_id = p.id WHERE p.id = :id"
    );
    countQuery.bindValue(":id", id);
    
    if (!countQuery.exec() || !countQuery.next() || countQuery.value(0).toInt() == 0) {
        QMutexLocker locker(&mutex_);
        lastError_ = "Pallet has no boxes";
        return false;
    }
    
    QSqlQuery query(db);
    query.prepare(
        "UPDATE pallets SET status = 1 "
        "WHERE id = :id AND status = 0"
    );
    query.bindValue(":id", id);
    
    return query.exec() && query.numRowsAffected() > 0;
}

int DbService::getPalletBoxCount(PalletId id) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT COUNT(*) FROM pallet_box_assignments pba "
        "JOIN pallets p ON pba.pallet_id = p.id WHERE p.id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

// ============================================================================
// Export Operations
// ============================================================================

QFuture<ExportResult> DbService::exportItemsAsync(const QVector<ItemId>& itemIds,
                                                   const QString& lpTin) {
    return QtConcurrent::run([this, itemIds, lpTin]() {
        return doExportItems(itemIds, lpTin);
    });
}

QFuture<ExportResult> DbService::exportBoxesAsync(const QVector<BoxId>& boxIds,
                                                   const QString& lpTin) {
    return QtConcurrent::run([this, boxIds, lpTin]() {
        return doExportBoxes(boxIds, lpTin);
    });
}

QFuture<ExportResult> DbService::exportPalletsAsync(const QVector<PalletId>& palletIds,
                                                     const QString& lpTin) {
    return QtConcurrent::run([this, palletIds, lpTin]() {
        return doExportPallets(palletIds, lpTin);
    });
}

ExportResult DbService::doExportItems(const QVector<ItemId>& itemIds, const QString& lpTin) {
    ExportResult result;
    
    if (itemIds.isEmpty()) {
        result.error = "No items to export";
        return result;
    }
    
    // Create thread-local database connection for async operation
    QString dbHost, dbDatabase, dbUser, dbPassword;
    int dbPort;
    {
        QMutexLocker locker(&mutex_);
        dbHost = config_.host;
        dbPort = config_.port;
        dbDatabase = config_.database;
        dbUser = config_.user;
        dbPassword = config_.password;
    }
    
    QSqlDatabase db = createThreadLocalConnection(dbHost, dbPort,
                                                   dbDatabase, dbUser, dbPassword);
    
    if (!db.isOpen()) {
        result.error = "Failed to create database connection";
        return result;
    }
    
    db.transaction();
    
    // Build ID list for IN clause
    QStringList idStrings;
    for (ItemId id : itemIds) {
        idStrings.append(QString::number(id));
    }
    QString idList = idStrings.join(",");
    
    // Verify items have status = 1 (Assigned/Scanned) and are not in any box
    QSqlQuery verifyQuery(db);
    QString verifySql = QString(
        "SELECT COUNT(*) FROM items i "
        "LEFT JOIN item_box_assignments iba ON i.id = iba.item_id "
        "WHERE i.id IN (%1) AND i.status = 1 AND iba.item_id IS NULL"
    ).arg(idList);
    
    if (!verifyQuery.exec(verifySql) || !verifyQuery.next()) {
        db.rollback();
        result.error = "Verify query failed";
        return result;
    }
    
    if (verifyQuery.value(0).toInt() != itemIds.size()) {
        db.rollback();
        result.error = "Some items not found, not scanned, or already assigned to a box";
        return result;
    }
    
    // Create document with export_mode = 2 (ItemExport)
    QSqlQuery createDoc(db);
    createDoc.prepare(
        "INSERT INTO export_documents (export_mode, lp_tin, created_at) "
        "VALUES (2, :lpTin, NOW()) RETURNING id"
    );
    createDoc.bindValue(":lpTin", lpTin);
    
    if (!createDoc.exec() || !createDoc.next()) {
        db.rollback();
        result.error = "Failed to create document";
        return result;
    }
    
    result.documentId = createDoc.value(0).toLongLong();
    
    // Snapshot items (store barcodes in snapshot)
    QString snapshotSql = QString(
        "INSERT INTO export_items (document_id, bar_code, created_at) "
        "SELECT %1, bar_code, NOW() FROM items WHERE id IN (%2)"
    ).arg(result.documentId).arg(idList);
    
    QSqlQuery snapshotQuery(db);
    if (!snapshotQuery.exec(snapshotSql)) {
        db.rollback();
        result.error = "Failed to snapshot items";
        return result;
    }
    result.itemsExported = snapshotQuery.numRowsAffected();
    
    // Update item statuses to Exported
    QString updateSql = QString(
        "UPDATE items SET status = 2 WHERE id IN (%1)"
    ).arg(idList);
    
    QSqlQuery updateQuery(db);
    if (!updateQuery.exec(updateSql)) {
        db.rollback();
        result.error = "Failed to update item statuses";
        return result;
    }
    
    db.commit();
    result.success = true;
    
    qDebug() << "DbService: Item export complete - Doc:" << result.documentId;
    
    // Generate XML content
    QString xmlContent = generateItemExportXml(result.documentId, lpTin, db);
    
    // Update document with XML content
    QSqlQuery updateXmlQuery(db);
    updateXmlQuery.prepare(
        "UPDATE export_documents SET xml_content = :xml WHERE id = :id"
    );
    updateXmlQuery.bindValue(":xml", xmlContent.toUtf8());
    updateXmlQuery.bindValue(":id", result.documentId);
    
    if (!updateXmlQuery.exec()) {
        qDebug() << "Warning: Failed to update XML content:" << updateXmlQuery.lastError().text();
    }
    
    return result;
}

ExportResult DbService::doExportBoxes(const QVector<BoxId>& boxIds, const QString& lpTin) {
ExportResult result;
    
if (boxIds.isEmpty()) {
    result.error = "No boxes to export";
    return result;
}
    
// Create thread-local database connection for async operation
QString dbHost, dbDatabase, dbUser, dbPassword;
int dbPort;
{
    QMutexLocker locker(&mutex_);
    dbHost = config_.host;
    dbPort = config_.port;
    dbDatabase = config_.database;
    dbUser = config_.user;
    dbPassword = config_.password;
}
    
QSqlDatabase db = createThreadLocalConnection(dbHost, dbPort,
                                               dbDatabase, dbUser, dbPassword);
    
if (!db.isOpen()) {
    result.error = "Failed to create database connection";
    return result;
}
    
db.transaction();
    
// Build ID list for IN clause
QStringList idStrings;
for (BoxId id : boxIds) {
    idStrings.append(QString::number(id));
}
QString idList = idStrings.join(",");
    
// Verify boxes are sealed
QSqlQuery verifyQuery(db);
QString verifySql = QString(
    "SELECT COUNT(*) FROM boxes WHERE id IN (%1) AND status = 1"
).arg(idList);
    
if (!verifyQuery.exec(verifySql) || !verifyQuery.next()) {
    db.rollback();
    result.error = "Verify query failed";
    return result;
}
    
if (verifyQuery.value(0).toInt() != boxIds.size()) {
    db.rollback();
    result.error = "Some boxes not found or not sealed";
    return result;
}
    
// Create document
QSqlQuery createDoc(db);
createDoc.prepare(
    "INSERT INTO export_documents (export_mode, lp_tin, created_at) "
    "VALUES (0, :lpTin, NOW()) RETURNING id"
);
createDoc.bindValue(":lpTin", lpTin);
    
if (!createDoc.exec() || !createDoc.next()) {
    db.rollback();
    result.error = "Failed to create document";
    return result;
}
    
result.documentId = createDoc.value(0).toLongLong();
    
// Snapshot boxes (store barcodes in snapshot)
QString snapshotSql = QString(
    "INSERT INTO export_boxes (document_id, bar_code, created_at) "
    "SELECT %1, bar_code, NOW() FROM boxes WHERE id IN (%2)"
).arg(result.documentId).arg(idList);
    
QSqlQuery snapshotQuery(db);
if (!snapshotQuery.exec(snapshotSql)) {
    db.rollback();
    result.error = "Failed to snapshot boxes";
    return result;
}
result.boxesExported = snapshotQuery.numRowsAffected();
    
// Get items in these boxes and snapshot them
QString itemSnapshotSql = QString(
    "INSERT INTO export_items (document_id, bar_code, created_at) "
    "SELECT %1, i.bar_code, NOW() "
    "FROM items i "
    "JOIN item_box_assignments iba ON i.id = iba.item_id "
    "WHERE iba.box_id IN (%2)"
).arg(result.documentId).arg(idList);
    
QSqlQuery itemSnapshotQuery(db);
if (!itemSnapshotQuery.exec(itemSnapshotSql)) {
    db.rollback();
    result.error = "Failed to snapshot items";
    return result;
}
result.itemsExported = itemSnapshotQuery.numRowsAffected();
    
// Update box statuses to Exported
QString updateSql = QString(
    "UPDATE boxes SET status = 2 WHERE id IN (%1)"
).arg(idList);
    
QSqlQuery updateQuery(db);
if (!updateQuery.exec(updateSql)) {
    db.rollback();
    result.error = "Failed to update box statuses";
    return result;
}
    
// Update item statuses to Exported
QString updateItemsSql = QString(
    "UPDATE items SET status = 2 WHERE id IN ("
    "  SELECT item_id FROM item_box_assignments WHERE box_id IN (%1))"
).arg(idList);
    
QSqlQuery updateItemsQuery(db);
if (!updateItemsQuery.exec(updateItemsSql)) {
    db.rollback();
    result.error = "Failed to update item statuses";
    return result;
}
    
    
db.commit();
result.success = true;
    
qDebug() << "DbService: Export complete - Doc:" << result.documentId;

// Generate XML content
QString xmlContent = generateBoxExportXml(result.documentId, lpTin, db);

// Update document with XML content
QSqlQuery updateXmlQuery(db);
updateXmlQuery.prepare(
    "UPDATE export_documents SET xml_content = :xml WHERE id = :id"
);
updateXmlQuery.bindValue(":xml", xmlContent.toUtf8());
updateXmlQuery.bindValue(":id", result.documentId);

if (!updateXmlQuery.exec()) {
    qDebug() << "Warning: Failed to update XML content:" << updateXmlQuery.lastError().text();
}

return result;
}

ExportResult DbService::doExportPallets(const QVector<PalletId>& palletIds, const QString& lpTin) {
    ExportResult result;
    
    if (palletIds.isEmpty()) {
        result.error = "No pallets to export";
        return result;
    }
    
    // Create thread-local database connection for async operation
    QString dbHost, dbDatabase, dbUser, dbPassword;
    int dbPort;
    {
        QMutexLocker locker(&mutex_);
        dbHost = config_.host;
        dbPort = config_.port;
        dbDatabase = config_.database;
        dbUser = config_.user;
        dbPassword = config_.password;
    }
    
    QSqlDatabase db = createThreadLocalConnection(dbHost, dbPort,
                                                   dbDatabase, dbUser, dbPassword);
    
    if (!db.isOpen()) {
        result.error = "Failed to create database connection";
        return result;
    }
    
    db.transaction();
    
    // Build ID list for IN clause
    QStringList idStrings;
    for (PalletId id : palletIds) {
        idStrings.append(QString::number(id));
    }
    QString idList = idStrings.join(",");
    
    // Verify pallets are complete
    QSqlQuery verifyQuery(db);
    QString verifySql = QString(
        "SELECT COUNT(*) FROM pallets WHERE id IN (%1) AND status = 1"
    ).arg(idList);
    
    if (!verifyQuery.exec(verifySql) || !verifyQuery.next()) {
        db.rollback();
        result.error = "Verify query failed";
        return result;
    }
    
    if (verifyQuery.value(0).toInt() != palletIds.size()) {
        db.rollback();
        result.error = "Some pallets not found or not complete";
        return result;
    }
    
    // Create document
    QSqlQuery createDoc(db);
    createDoc.prepare(
        "INSERT INTO export_documents (export_mode, lp_tin, created_at) "
        "VALUES (1, :lpTin, NOW()) RETURNING id"
    );
    createDoc.bindValue(":lpTin", lpTin);
    
    if (!createDoc.exec() || !createDoc.next()) {
        db.rollback();
        result.error = "Failed to create document";
        return result;
    }
    
    result.documentId = createDoc.value(0).toLongLong();
    
    // Snapshot pallets
    QString snapshotSql = QString(
        "INSERT INTO export_pallets (document_id, bar_code, created_at) "
        "SELECT %1, bar_code, NOW() FROM pallets WHERE id IN (%2)"
    ).arg(result.documentId).arg(idList);
    
    QSqlQuery snapshotQuery(db);
    if (!snapshotQuery.exec(snapshotSql)) {
        db.rollback();
        result.error = "Failed to snapshot pallets";
        return result;
    }
    result.palletsExported = snapshotQuery.numRowsAffected();
    
    // Snapshot boxes on these pallets
    QString boxSnapshotSql = QString(
        "INSERT INTO export_boxes (document_id, bar_code, created_at) "
        "SELECT %1, b.bar_code, NOW() "
        "FROM boxes b "
        "JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "WHERE pba.pallet_id IN (%2)"
    ).arg(result.documentId).arg(idList);
    
    QSqlQuery boxSnapshotQuery(db);
    if (!boxSnapshotQuery.exec(boxSnapshotSql)) {
        db.rollback();
        result.error = "Failed to snapshot boxes";
        return result;
    }
    result.boxesExported = boxSnapshotQuery.numRowsAffected();
    
    // Snapshot items in these boxes
    QString itemSnapshotSql = QString(
        "INSERT INTO export_items (document_id, bar_code, created_at) "
        "SELECT %1, i.bar_code, NOW() "
        "FROM items i "
        "JOIN item_box_assignments iba ON i.id = iba.item_id "
        "JOIN boxes b ON iba.box_id = b.id "
        "JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "WHERE pba.pallet_id IN (%2)"
    ).arg(result.documentId).arg(idList);
    
    QSqlQuery itemSnapshotQuery(db);
    if (!itemSnapshotQuery.exec(itemSnapshotSql)) {
        db.rollback();
        result.error = "Failed to snapshot items";
        return result;
    }
    result.itemsExported = itemSnapshotQuery.numRowsAffected();
    
    // Update pallet statuses to Exported
    QString updatePalletSql = QString(
        "UPDATE pallets SET status = 2 WHERE id IN (%1)"
    ).arg(idList);
    
    QSqlQuery updatePalletQuery(db);
    if (!updatePalletQuery.exec(updatePalletSql)) {
        db.rollback();
        result.error = "Failed to update pallet statuses";
        return result;
    }
    
    // Update box statuses to Exported
    QString updateBoxesSql = QString(
        "UPDATE boxes SET status = 2 WHERE id IN ("
        "  SELECT box_id FROM pallet_box_assignments WHERE pallet_id IN (%1))"
    ).arg(idList);
    
    QSqlQuery updateBoxesQuery(db);
    if (!updateBoxesQuery.exec(updateBoxesSql)) {
        db.rollback();
        result.error = "Failed to update box statuses";
        return result;
    }
    
    // Update item statuses to Exported
    QString updateItemsSql = QString(
        "UPDATE items SET status = 2 WHERE id IN ("
        "  SELECT i.id FROM items i "
        "  JOIN item_box_assignments iba ON i.id = iba.item_id "
        "  JOIN boxes b ON iba.box_id = b.id "
        "  JOIN pallet_box_assignments pba ON b.id = pba.box_id "
        "  WHERE pba.pallet_id IN (%1))"
    ).arg(idList);
    
    QSqlQuery updateItemsQuery(db);
    if (!updateItemsQuery.exec(updateItemsSql)) {
        db.rollback();
        result.error = "Failed to update item statuses";
        return result;
    }
    
    
    db.commit();
    result.success = true;
    
    qDebug() << "DbService: Pallet export complete - Doc:" << result.documentId;
    
    // Generate XML content
    QString xmlContent = generatePalletExportXml(result.documentId, lpTin, db);
    
    // Update document with XML content
    QSqlQuery updateXmlQuery(db);
    updateXmlQuery.prepare(
        "UPDATE export_documents SET xml_content = :xml WHERE id = :id"
    );
    updateXmlQuery.bindValue(":xml", xmlContent.toUtf8());
    updateXmlQuery.bindValue(":id", result.documentId);
    
    if (!updateXmlQuery.exec()) {
        qDebug() << "Warning: Failed to update XML content:" << updateXmlQuery.lastError().text();
    }
    
    return result;
}

std::optional<ExportDocument> DbService::getExportDocument(ExportDocumentId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT id, export_mode, lp_tin, created_at, xml_content, xml_hash "
        "FROM export_documents WHERE id = :id"
    );
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parseExportDocument(query);
    }
    
    return std::nullopt;
}

QVector<ExportDocument> DbService::getExportDocuments(int limit, int offset) {
    QVector<ExportDocument> docs;
    
    if (!ensureConnected()) return docs;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare(
        "SELECT id, export_mode, lp_tin, created_at, xml_content, xml_hash "
        "FROM export_documents ORDER BY created_at DESC LIMIT :limit OFFSET :offset"
    );
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);
    
    if (query.exec()) {
        while (query.next()) {
            docs.append(parseExportDocument(query));
        }
    }
    
    return docs;
}

int DbService::getExportDocumentItemCount(ExportDocumentId id) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT COUNT(*) FROM export_items WHERE document_id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

int DbService::getExportDocumentBoxCount(ExportDocumentId id) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT COUNT(*) FROM export_boxes WHERE document_id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

int DbService::getExportDocumentPalletCount(ExportDocumentId id) {
    if (!ensureConnected()) return 0;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT COUNT(*) FROM export_pallets WHERE document_id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

// ============================================================================
// Statistics
// ============================================================================

ProductionStats DbService::getStats(std::optional<ProductionLineId> lineId) {
    ProductionStats stats;
    
    if (!ensureConnected()) return stats;
    
    QSqlDatabase db = getDatabase();
    QString where = lineId ? QString(" WHERE production_line = %1").arg(*lineId) : "";
    
    // Items
    QSqlQuery itemsQuery(db);
    if (itemsQuery.exec(QString("SELECT status, COUNT(*) FROM items%1 GROUP BY status").arg(where))) {
        while (itemsQuery.next()) {
            int status = itemsQuery.value(0).toInt();
            int count = itemsQuery.value(1).toInt();
            stats.totalItems += count;
            switch (status) {
                case 0: stats.availableItems = count; break;
                case 1: stats.assignedItems = count; break;
                case 2: stats.exportedItems = count; break;
            }
        }
    }
    
    // Boxes
    QSqlQuery boxesQuery(db);
    if (boxesQuery.exec(QString("SELECT status, COUNT(*) FROM boxes%1 GROUP BY status").arg(where))) {
        while (boxesQuery.next()) {
            int status = boxesQuery.value(0).toInt();
            int count = boxesQuery.value(1).toInt();
            stats.totalBoxes += count;
            switch (status) {
                case 0: stats.emptyBoxes = count; break;
                case 1: stats.sealedBoxes = count; break;
                case 2: stats.exportedBoxes = count; break;
            }
        }
    }
    
    // Pallets
    QSqlQuery palletsQuery(db);
    if (palletsQuery.exec(QString("SELECT status, COUNT(*) FROM pallets%1 GROUP BY status").arg(where))) {
        while (palletsQuery.next()) {
            int status = palletsQuery.value(0).toInt();
            int count = palletsQuery.value(1).toInt();
            stats.totalPallets += count;
            switch (status) {
                case 0: stats.newPallets = count; break;
                case 1: stats.completePallets = count; break;
                case 2: stats.exportedPallets = count; break;
            }
        }
    }
    
    return stats;
}

// ============================================================================
// Products
// ============================================================================

QVector<Product> DbService::getProducts() {
    QVector<Product> products;
    if (!ensureConnected()) return products;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, gtin, name, description, created_at FROM products ORDER BY name")) {
        while (query.next()) {
            products.append(parseProduct(query));
        }
    }
    return products;
}

std::optional<Product> DbService::getProduct(ProductId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT id, gtin, name, description, created_at FROM products WHERE id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parseProduct(query);
    }
    return std::nullopt;
}

bool DbService::createProduct(const Product& product) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO products (gtin, name, description) VALUES (:gtin, :name, :desc)");
    query.bindValue(":gtin", product.gtin);
    query.bindValue(":name", product.name);
    query.bindValue(":desc", product.description);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool DbService::updateProduct(const Product& product) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("UPDATE products SET gtin = :gtin, name = :name, description = :desc WHERE id = :id");
    query.bindValue(":gtin", product.gtin);
    query.bindValue(":name", product.name);
    query.bindValue(":desc", product.description);
    query.bindValue(":id", product.id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool DbService::deleteProduct(ProductId id) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM products WHERE id = :id");
    query.bindValue(":id", id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

// ============================================================================
// Product Packaging
// ============================================================================

QVector<ProductPackaging> DbService::getProductPackaging() {
    QVector<ProductPackaging> packaging;
    if (!ensureConnected()) return packaging;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, product_id, number_of_products, gtin, name, description, created_at "
                   "FROM product_packaging ORDER BY name")) {
        while (query.next()) {
            packaging.append(parseProductPackaging(query));
        }
    }
    return packaging;
}

std::optional<ProductPackaging> DbService::getPackaging(ProductPackagingId id) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT id, product_id, number_of_products, gtin, name, description, created_at "
                  "FROM product_packaging WHERE id = :id");
    query.bindValue(":id", id);
    
    if (query.exec() && query.next()) {
        return parseProductPackaging(query);
    }
    return std::nullopt;
}

bool DbService::createPackaging(const ProductPackaging& pkg) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO product_packaging (product_id, number_of_products, gtin, name, description) "
                  "VALUES (:productId, :num, :gtin, :name, :desc)");
    query.bindValue(":productId", pkg.productId);
    query.bindValue(":num", pkg.numberOfProducts);
    query.bindValue(":gtin", pkg.gtin);
    query.bindValue(":name", pkg.name);
    query.bindValue(":desc", pkg.description);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool DbService::updatePackaging(const ProductPackaging& pkg) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("UPDATE product_packaging SET product_id = :productId, number_of_products = :num, "
                  "gtin = :gtin, name = :name, description = :desc WHERE id = :id");
    query.bindValue(":productId", pkg.productId);
    query.bindValue(":num", pkg.numberOfProducts);
    query.bindValue(":gtin", pkg.gtin);
    query.bindValue(":name", pkg.name);
    query.bindValue(":desc", pkg.description);
    query.bindValue(":id", pkg.id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool DbService::deletePackaging(ProductPackagingId id) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM product_packaging WHERE id = :id");
    query.bindValue(":id", id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

// ============================================================================
// User Management
// ============================================================================

QVector<User> DbService::getUsers() {
    QVector<User> users;
    if (!ensureConnected()) return users;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, username, pin_hash, full_name, email, phone_number, "
                   "active, superuser, created_at, last_login FROM users ORDER BY username")) {
        while (query.next()) {
            users.append(parseUser(query));
        }
    }
    return users;
}

bool DbService::createUser(const User& user) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO users (username, pin_hash, full_name, email, phone_number, active, superuser) "
                  "VALUES (:username, :pinHash, :fullName, :email, :phone, :active, :superuser)");
    query.bindValue(":username", user.username);
    query.bindValue(":pinHash", user.pinHash);
    query.bindValue(":fullName", user.fullName);
    query.bindValue(":email", user.email.isEmpty() ? QVariant() : user.email);
    query.bindValue(":phone", user.phoneNumber.isEmpty() ? QVariant() : user.phoneNumber);
    query.bindValue(":active", user.active);
    query.bindValue(":superuser", user.superuser);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool DbService::updateUser(const User& user) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("UPDATE users SET username = :username, pin_hash = :pinHash, full_name = :fullName, "
                  "email = :email, phone_number = :phone, active = :active, superuser = :superuser "
                  "WHERE id = :id");
    query.bindValue(":username", user.username);
    query.bindValue(":pinHash", user.pinHash);
    query.bindValue(":fullName", user.fullName);
    query.bindValue(":email", user.email.isEmpty() ? QVariant() : user.email);
    query.bindValue(":phone", user.phoneNumber.isEmpty() ? QVariant() : user.phoneNumber);
    query.bindValue(":active", user.active);
    query.bindValue(":superuser", user.superuser);
    query.bindValue(":id", user.id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool DbService::deleteUser(UserId userId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM users WHERE id = :id");
    query.bindValue(":id", userId);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

// ============================================================================
// Role Management
// ============================================================================

QVector<Role> DbService::getRoles() {
    QVector<Role> roles;
    if (!ensureConnected()) return roles;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, role_name, description, active FROM roles ORDER BY role_name")) {
        while (query.next()) {
            roles.append(parseRole(query));
        }
    }
    return roles;
}

std::optional<Role> DbService::getRole(RoleId roleId) {
    if (!ensureConnected()) return std::nullopt;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT id, role_name, description, active FROM roles WHERE id = :id");
    query.bindValue(":id", roleId);
    
    if (query.exec() && query.next()) {
        return parseRole(query);
    }
    return std::nullopt;
}

bool DbService::createRole(const Role& role) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO roles (role_name, description, active) VALUES (:name, :desc, :active)");
    query.bindValue(":name", role.name);
    query.bindValue(":desc", role.description);
    query.bindValue(":active", role.active);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool DbService::updateRole(const Role& role) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("UPDATE roles SET role_name = :name, description = :desc, active = :active WHERE id = :id");
    query.bindValue(":name", role.name);
    query.bindValue(":desc", role.description);
    query.bindValue(":active", role.active);
    query.bindValue(":id", role.id);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool DbService::deleteRole(RoleId roleId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM roles WHERE id = :id");
    query.bindValue(":id", roleId);
    
    if (!query.exec()) {
        QMutexLocker locker(&mutex_);
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

QVector<Role> DbService::getUserRoles(UserId userId) {
    QVector<Role> roles;
    if (!ensureConnected()) return roles;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT r.id, r.role_name, r.description, r.active FROM roles r "
                  "JOIN user_roles ur ON r.id = ur.role_id WHERE ur.user_id = :userId");
    query.bindValue(":userId", userId);
    
    if (query.exec()) {
        while (query.next()) {
            roles.append(parseRole(query));
        }
    }
    return roles;
}

bool DbService::assignRoleToUser(UserId userId, RoleId roleId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO user_roles (user_id, role_id) VALUES (:userId, :roleId) "
                  "ON CONFLICT (user_id, role_id) DO NOTHING");
    query.bindValue(":userId", userId);
    query.bindValue(":roleId", roleId);
    
    return query.exec();
}

bool DbService::removeRoleFromUser(UserId userId, RoleId roleId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM user_roles WHERE user_id = :userId AND role_id = :roleId");
    query.bindValue(":userId", userId);
    query.bindValue(":roleId", roleId);
    
    return query.exec() && query.numRowsAffected() > 0;
}

// ============================================================================
// Permission Management
// ============================================================================

QVector<Permission> DbService::getPermissions() {
    QVector<Permission> perms;
    if (!ensureConnected()) return perms;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    if (query.exec("SELECT id, permission_name, category, description FROM permissions "
                   "WHERE active = true ORDER BY category, permission_name")) {
        while (query.next()) {
            perms.append(parsePermission(query));
        }
    }
    return perms;
}

QVector<Permission> DbService::getRolePermissions(RoleId roleId) {
    QVector<Permission> perms;
    if (!ensureConnected()) return perms;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("SELECT p.id, p.permission_name, p.category, p.description FROM permissions p "
                  "JOIN role_permissions rp ON p.id = rp.permission_id "
                  "WHERE rp.role_id = :roleId AND rp.granted = true AND p.active = true");
    query.bindValue(":roleId", roleId);
    
    if (query.exec()) {
        while (query.next()) {
            perms.append(parsePermission(query));
        }
    }
    return perms;
}

bool DbService::assignPermissionToRole(RoleId roleId, qint32 permissionId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("INSERT INTO role_permissions (role_id, permission_id, granted) "
                  "VALUES (:roleId, :permId, true) "
                  "ON CONFLICT (role_id, permission_id) DO UPDATE SET granted = true");
    query.bindValue(":roleId", roleId);
    query.bindValue(":permId", permissionId);
    
    return query.exec();
}

bool DbService::removePermissionFromRole(RoleId roleId, qint32 permissionId) {
    if (!ensureConnected()) return false;
    
    QSqlDatabase db = getDatabase();
    QSqlQuery query(db);
    
    query.prepare("DELETE FROM role_permissions WHERE role_id = :roleId AND permission_id = :permId");
    query.bindValue(":roleId", roleId);
    query.bindValue(":permId", permissionId);
    
    return query.exec() && query.numRowsAffected() > 0;
}

// ============================================================================
// Parse Helpers
// ============================================================================

User DbService::parseUser(const QSqlQuery& query) {
    User user;
    user.id = query.value("id").toLongLong();
    user.username = query.value("username").toString();
    user.pinHash = query.value("pin_hash").toString();
    user.fullName = query.value("full_name").toString();
    user.email = query.value("email").toString();
    user.phoneNumber = query.value("phone_number").toString();
    user.active = query.value("active").toBool();
    user.superuser = query.value("superuser").toBool();
    user.createdAt = query.value("created_at").toDateTime();
    if (!query.value("last_login").isNull()) {
        user.lastLogin = query.value("last_login").toDateTime();
    }
    return user;
}

Role DbService::parseRole(const QSqlQuery& query) {
    Role role;
    role.id = query.value(0).toInt();
    role.name = query.value(1).toString();
    role.description = query.value(2).toString();
    role.active = query.value(3).toBool();
    return role;
}

Permission DbService::parsePermission(const QSqlQuery& query) {
    Permission perm;
    perm.id = query.value(0).toInt();
    perm.name = query.value(1).toString();
    perm.category = query.value(2).toString();
    perm.description = query.value(3).toString();
    return perm;
}

Item DbService::parseItem(const QSqlQuery& query) {
    Item item;
    item.id = query.value(0).toLongLong();
    item.barcode = query.value(1).toString();
    item.status = static_cast<ItemStatus>(query.value(2).toInt());
    item.productionLine = query.value(3).toLongLong();
    item.importedAt = query.value(4).toDateTime();
    if (!query.value(5).isNull()) {
        item.scannedAt = query.value(5).toDateTime();
    }
    return item;
}

Box DbService::parseBox(const QSqlQuery& query) {
    Box box;
    box.id = query.value(0).toLongLong();
    box.barcode = query.value(1).toString();
    box.status = static_cast<BoxStatus>(query.value(2).toInt());
    box.productionLine = query.value(3).toLongLong();
    box.importedAt = query.value(4).toDateTime();
    if (!query.value(5).isNull()) {
        box.sealedAt = query.value(5).toDateTime();
    }
    return box;
}

Pallet DbService::parsePallet(const QSqlQuery& query) {
    Pallet pallet;
    pallet.id = query.value(0).toLongLong();
    pallet.barcode = query.value(1).toString();
    pallet.status = static_cast<PalletStatus>(query.value(2).toInt());
    pallet.productionLine = query.value(3).toLongLong();
    pallet.createdAt = query.value(4).toDateTime();
    // Note: Schema does not have completed_at column
    return pallet;
}

ExportDocument DbService::parseExportDocument(const QSqlQuery& query) {
    ExportDocument doc;
    doc.id = query.value(0).toLongLong();
    doc.mode = static_cast<ExportMode>(query.value(1).toInt());
    doc.lpTin = query.value(2).toString();
    doc.createdAt = query.value(3).toDateTime();
    doc.xmlContent = query.value(4).toByteArray();
    doc.xmlHash = query.value(5).toString();
    return doc;
}

Product DbService::parseProduct(const QSqlQuery& query) {
    Product p;
    p.id = query.value(0).toLongLong();
    p.gtin = query.value(1).toString();
    p.name = query.value(2).toString();
    p.description = query.value(3).toString();
    p.createdAt = query.value(4).toDateTime();
    return p;
}

ProductPackaging DbService::parseProductPackaging(const QSqlQuery& query) {
    ProductPackaging pp;
    pp.id = query.value(0).toLongLong();
    pp.productId = query.value(1).toLongLong();
    pp.numberOfProducts = query.value(2).toInt();
    pp.gtin = query.value(3).toString();
    pp.name = query.value(4).toString();
    pp.description = query.value(5).toString();
    pp.createdAt = query.value(6).toDateTime();
    return pp;
}

QString DbService::buildPlaceholders(const QStringList& values) {
    QStringList escaped;
    for (const QString& v : values) {
        escaped.append(QString("'%1'").arg(QString(v).replace("'", "''")));
    }
    return escaped.join(", ");
}

QString DbService::cleanBarcodeForExport(const QString& barcode) {
    if (barcode.isEmpty()) {
        return barcode;
    }
    
    // GS1 Group Separator character (0x1D = decimal 29)
    const QChar gs1Separator(0x1D);
    
    // Find the GS1 separator
    int separatorPos = barcode.indexOf(gs1Separator);
    
    if (separatorPos != -1) {
        // Found GS separator, truncate at this position
        return barcode.left(separatorPos);
    }
    
    // Fallback: Look for "93" pattern which might indicate AI 93
    // This handles cases where the separator might not be preserved
    int ai93Pos = barcode.indexOf("93");
    if (ai93Pos > 0) {
        // Make sure this looks like an AI position (not part of the main barcode)
        // GS1 barcodes typically have the main data first, so AI 93 should be near the end
        // Check if there's enough data before "93" (at least 20 chars for a typical GTIN+serial)
        if (ai93Pos >= 20) {
            // Check if the character before "93" might be a separator we can't see
            // or if "93" is at a logical break point
            return barcode.left(ai93Pos);
        }
    }
    
    // No separator found, return original barcode
    return barcode;
}

QString DbService::generateItemExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db) {
    QString xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<item_export>\n";
    xml += "  <Document>\n";
    xml += "    <organisation>\n";
    xml += "      <id_info>\n";
    xml += QString("        <LP_info LP_TIN=\"%1\" />\n").arg(lpTin);
    xml += "      </id_info>\n";
    xml += "    </organisation>\n";
    
    // Get all items for this export
    QSqlQuery itemQuery(db);
    itemQuery.prepare("SELECT bar_code FROM export_items WHERE document_id = :docId ORDER BY created_at");
    itemQuery.bindValue(":docId", docId);
    
    if (!itemQuery.exec()) {
        qDebug() << "Failed to query export items:" << itemQuery.lastError().text();
        return xml;
    }
    
    while (itemQuery.next()) {
        QString itemBarcode = cleanBarcodeForExport(itemQuery.value(0).toString());
        xml += QString("    <cis><![CDATA[%1]]></cis>\n").arg(itemBarcode);
    }
    
    xml += "  </Document>\n";
    xml += "</item_export>\n";
    
    return xml;
}

QString DbService::generateBoxExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db) {
    QString xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<unit_pack>\n";
    xml += "  <Document>\n";
    xml += "    <organisation>\n";
    xml += "      <id_info>\n";
    xml += QString("        <LP_info LP_TIN=\"%1\" />\n").arg(lpTin);
    xml += "      </id_info>\n";
    xml += "    </organisation>\n";
    
    // Get all boxes for this export
    QSqlQuery boxQuery(db);
    boxQuery.prepare("SELECT bar_code FROM export_boxes WHERE document_id = :docId ORDER BY created_at");
    boxQuery.bindValue(":docId", docId);
    
    if (!boxQuery.exec()) {
        qDebug() << "Failed to query export boxes:" << boxQuery.lastError().text();
        return xml;
    }
    
    while (boxQuery.next()) {
        QString boxBarcode = cleanBarcodeForExport(boxQuery.value(0).toString());
        xml += "    <pack_content>\n";
        xml += QString("      <pack_code><![CDATA[%1]]></pack_code>\n").arg(boxBarcode);
        
        // Get items for this box
        QSqlQuery itemQuery(db);
        itemQuery.prepare(
            "SELECT ei.bar_code FROM export_items ei "
            "JOIN item_box_assignments iba ON ei.bar_code = (SELECT bar_code FROM items WHERE id = iba.item_id) "
            "JOIN boxes b ON iba.box_id = b.id "
            "WHERE b.bar_code = :boxBarcode AND ei.document_id = :docId "
            "ORDER BY ei.created_at"
        );
        itemQuery.bindValue(":boxBarcode", boxQuery.value(0).toString()); // Use original barcode for query
        itemQuery.bindValue(":docId", docId);
        
        if (itemQuery.exec()) {
            while (itemQuery.next()) {
                QString itemBarcode = cleanBarcodeForExport(itemQuery.value(0).toString());
                xml += QString("      <cis><![CDATA[%1]]></cis>\n").arg(itemBarcode);
            }
        }
        
        xml += "    </pack_content>\n";
    }
    
    xml += "  </Document>\n";
    xml += "</unit_pack>\n";
    
    return xml;
}

QString DbService::generatePalletExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db) {
    QString xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<aggregation_document>\n";
    xml += "  <Document>\n";
    xml += "    <organisation>\n";
    xml += "      <id_info>\n";
    xml += QString("        <LP_info LP_TIN=\"%1\" />\n").arg(lpTin);
    xml += "      </id_info>\n";
    xml += "    </organisation>\n";
    
    // Get all pallets for this export
    QSqlQuery palletQuery(db);
    palletQuery.prepare("SELECT bar_code FROM export_pallets WHERE document_id = :docId ORDER BY created_at");
    palletQuery.bindValue(":docId", docId);
    
    if (!palletQuery.exec()) {
        qDebug() << "Failed to query export pallets:" << palletQuery.lastError().text();
        return xml;
    }
    
    while (palletQuery.next()) {
        QString palletBarcode = cleanBarcodeForExport(palletQuery.value(0).toString());
        xml += "    <aggregation_unit>\n";
        xml += QString("      <sscc><![CDATA[%1]]></sscc>\n").arg(palletBarcode);
        
        // Get boxes on this pallet
        QSqlQuery boxQuery(db);
        boxQuery.prepare(
            "SELECT eb.bar_code FROM export_boxes eb "
            "JOIN pallet_box_assignments pba ON eb.bar_code = (SELECT bar_code FROM boxes WHERE id = pba.box_id) "
            "JOIN pallets p ON pba.pallet_id = p.id "
            "WHERE p.bar_code = :palletBarcode AND eb.document_id = :docId "
            "ORDER BY eb.created_at"
        );
        boxQuery.bindValue(":palletBarcode", palletQuery.value(0).toString()); // Use original barcode for query
        boxQuery.bindValue(":docId", docId);
        
        if (boxQuery.exec()) {
            while (boxQuery.next()) {
                QString boxBarcode = cleanBarcodeForExport(boxQuery.value(0).toString());
                xml += QString("      <unit_pack><![CDATA[%1]]></unit_pack>\n").arg(boxBarcode);
            }
        }
        
        xml += "    </aggregation_unit>\n";
    }
    
    xml += "  </Document>\n";
    xml += "</aggregation_document>\n";
    
    return xml;
}

} // namespace core
