#ifndef CORE_DB_SERVICE_H
#define CORE_DB_SERVICE_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QFuture>
#include <QMutex>
#include <optional>
#include "types.h"

namespace core {

/**
 * @brief Single database service for all database operations
 * 
 * Design:
 * - SYNC for fast queries (<100ms): auth, getItem, stats
 * - ASYNC for slow operations: imports, exports
 * - Uses Qt's QSqlDatabase with PostgreSQL driver (QPSQL)
 * - Thread-safe via QMutex
 * 
 * Usage:
 *   DbService db;
 *   if (db.connect(config)) {
 *       auto user = db.authenticate(pinHash);
 *       auto stats = db.getStats();
 *   }
 */
class DbService : public QObject {
    Q_OBJECT

public:
    explicit DbService(QObject* parent = nullptr);
    ~DbService() override;

    // Non-copyable
    DbService(const DbService&) = delete;
    DbService& operator=(const DbService&) = delete;

    // =========================================================================
    // Connection Management
    // =========================================================================
    
    bool connect(const AppConfig& config);
    bool connect(const QString& host, int port, const QString& database,
                 const QString& user, const QString& password);
    void disconnect();
    bool isConnected() const;
    bool reconnect();
    QString lastError() const;
    
    // Test connection without storing
    static bool testConnection(const AppConfig& config, QString* errorOut = nullptr);

    // =========================================================================
    // Authentication (SYNC)
    // =========================================================================
    
    std::optional<AuthenticatedUser> authenticate(const QString& username, const QString& pinHash);
    std::optional<User> getUserByUsername(const QString& username);
    std::optional<User> getUser(UserId userId);

    // =========================================================================
    // Production Lines (SYNC)
    // =========================================================================
    
    QVector<ProductionLine> getProductionLines();
    std::optional<ProductionLine> getProductionLine(ProductionLineId id);

    // =========================================================================
    // Products (SYNC)
    // =========================================================================

    QVector<Product> getProducts();
    std::optional<Product> getProduct(ProductId id);
    bool createProduct(const Product& product);
    bool updateProduct(const Product& product);
    bool deleteProduct(ProductId id);

    // =========================================================================
    // Product Packaging (SYNC)
    // =========================================================================

    QVector<ProductPackaging> getProductPackaging();
    std::optional<ProductPackaging> getPackaging(ProductPackagingId id);
    bool createPackaging(const ProductPackaging& packaging);
    bool updatePackaging(const ProductPackaging& packaging);
    bool deletePackaging(ProductPackagingId id);

    // =========================================================================
    // User Management (SYNC)
    // =========================================================================

    QVector<User> getUsers();
    bool createUser(const User& user);
    bool updateUser(const User& user);
    bool deleteUser(UserId userId);

    // =========================================================================
    // Role Management (SYNC)
    // =========================================================================

    QVector<Role> getRoles();
    std::optional<Role> getRole(RoleId roleId);
    bool createRole(const Role& role);
    bool updateRole(const Role& role);
    bool deleteRole(RoleId roleId);
    QVector<Role> getUserRoles(UserId userId);
    bool assignRoleToUser(UserId userId, RoleId roleId);
    bool removeRoleFromUser(UserId userId, RoleId roleId);

    // =========================================================================
    // Permission Management (SYNC)
    // =========================================================================

    QVector<Permission> getPermissions();
    QVector<Permission> getRolePermissions(RoleId roleId);
    bool assignPermissionToRole(RoleId roleId, qint32 permissionId);
    bool removePermissionFromRole(RoleId roleId, qint32 permissionId);

    // =========================================================================
    // Import Operations (ASYNC)
    // =========================================================================
    
    QFuture<ImportResult> importItemsAsync(const QString& filePath, 
                                            ProductionLineId lineId);
    QFuture<ImportResult> importBoxesAsync(const QString& filePath,
                                            ProductionLineId lineId);
    QFuture<ImportResult> importPalletsAsync(const QString& filePath,
                                              ProductionLineId lineId);

    // =========================================================================
    // Item Operations (SYNC)
    // =========================================================================
    
    std::optional<Item> getItem(ItemId id);
    QVector<Item> getItemsByStatus(ItemStatus status, 
                                    ProductionLineId lineId = 0, 
                                    int limit = 100);
    QVector<Item> getItemsInBox(BoxId boxId);
    QVector<Item> getScannedItemsNotInBox(ProductionLineId lineId = 0, int limit = 200);
    int countScannedItemsNotInBox(ProductionLineId lineId = 0);
    bool assignItemToBox(ItemId itemId, BoxId boxId);
    int assignItemsToBox(const QVector<ItemId>& itemIds, BoxId boxId);

    // =========================================================================
    // Box Operations (SYNC)
    // =========================================================================
    
    std::optional<Box> getBox(BoxId id);
    QVector<Box> getBoxesByStatus(BoxStatus status, 
                                   ProductionLineId lineId = 0,
                                   int limit = 100);
    QVector<Box> getSealedBoxesNotOnPallet(ProductionLineId lineId = 0, int limit = 200);
    int countSealedBoxesNotOnPallet(ProductionLineId lineId = 0);
    QVector<Box> getBoxesOnPallet(PalletId palletId);
    bool sealBox(BoxId id);
    bool assignBoxToPallet(BoxId boxId, PalletId palletId);
    int getBoxItemCount(BoxId id);

    // =========================================================================
    // Pallet Operations (SYNC)
    // =========================================================================
    
    std::optional<Pallet> getPallet(PalletId id);
    QVector<Pallet> getPalletsByStatus(PalletStatus status, 
                                        ProductionLineId lineId = 0,
                                        int limit = 100);
    bool completePallet(PalletId id);
    int getPalletBoxCount(PalletId id);

    // =========================================================================
    // Export Operations (ASYNC)
    // =========================================================================
    
    QFuture<ExportResult> exportItemsAsync(const QVector<ItemId>& itemIds,
                                            const QString& lpTin);
    QFuture<ExportResult> exportBoxesAsync(const QVector<BoxId>& boxIds, 
                                            const QString& lpTin);
    QFuture<ExportResult> exportPalletsAsync(const QVector<PalletId>& palletIds,
                                              const QString& lpTin);
    
    std::optional<ExportDocument> getExportDocument(ExportDocumentId id);
    QVector<ExportDocument> getExportDocuments(int limit = 50, int offset = 0);
    int getExportDocumentItemCount(ExportDocumentId id);
    int getExportDocumentBoxCount(ExportDocumentId id);
    int getExportDocumentPalletCount(ExportDocumentId id);

    // =========================================================================
    // Statistics (SYNC)
    // =========================================================================
    
    ProductionStats getStats(std::optional<ProductionLineId> lineId = std::nullopt);

    // =========================================================================
    // Database Access
    // =========================================================================
    
    QSqlDatabase getDatabase();

signals:
    void connectionLost();
    void connectionRestored();
    void importProgress(int current, int total);

private:
bool ensureConnected();
    
// Import helpers
ImportResult doImport(const QString& filePath, ProductionLineId lineId,
                      const QString& tableName);
    
// Export helpers
ExportResult doExportItems(const QVector<ItemId>& itemIds, const QString& lpTin);
ExportResult doExportBoxes(const QVector<BoxId>& boxIds, const QString& lpTin);
ExportResult doExportPallets(const QVector<PalletId>& palletIds, const QString& lpTin);
QString generateItemExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db);
QString generateBoxExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db);
QString generatePalletExportXml(ExportDocumentId docId, const QString& lpTin, QSqlDatabase& db);
QString cleanBarcodeForExport(const QString& barcode);
    
// Parse helpers
    User parseUser(const QSqlQuery& query);
    Role parseRole(const QSqlQuery& query);
    Permission parsePermission(const QSqlQuery& query);
    Item parseItem(const QSqlQuery& query);
    Box parseBox(const QSqlQuery& query);
    Pallet parsePallet(const QSqlQuery& query);
    ExportDocument parseExportDocument(const QSqlQuery& query);
    Product parseProduct(const QSqlQuery& query);
    ProductPackaging parseProductPackaging(const QSqlQuery& query);
    
    QString buildPlaceholders(const QStringList& values);

    QString connectionName_;
    mutable QMutex mutex_;
    QString lastError_;
    AppConfig config_;
};

} // namespace core

#endif // CORE_DB_SERVICE_H
