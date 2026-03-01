#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <QString>
#include <QDateTime>
#include <QVector>
#include <QJsonObject>
#include <optional>
#include <cstdint>

namespace core {

// ============================================================================
// ID Types
// ============================================================================
using UserId = qint64;
using RoleId = qint32;
using ItemId = qint64;
using BoxId = qint64;
using PalletId = qint64;
using ProductionLineId = qint64;
using ExportDocumentId = qint64;
using ProductId = qint64;
using ProductPackagingId = qint64;

// ============================================================================
// Enums
// ============================================================================
enum class ItemStatus : qint16 { 
    Available = 0, 
    Assigned = 1, 
    Exported = 2 
};

enum class BoxStatus : qint16 { 
    Empty = 0, 
    Sealed = 1, 
    Exported = 2 
};

enum class PalletStatus : qint16 { 
    New = 0, 
    Complete = 1, 
    Exported = 2 
};

enum class ExportMode : qint16 { 
    BoxExport = 0, 
    PalletExport = 1,
    ItemExport = 2
};

// Status to string helpers
inline QString itemStatusToString(ItemStatus status) {
    switch (status) {
        case ItemStatus::Available: return "Available";
        case ItemStatus::Assigned: return "Assigned";
        case ItemStatus::Exported: return "Exported";
        default: return "Unknown";
    }
}

inline QString boxStatusToString(BoxStatus status) {
    switch (status) {
        case BoxStatus::Empty: return "Empty";
        case BoxStatus::Sealed: return "Sealed";
        case BoxStatus::Exported: return "Exported";
        default: return "Unknown";
    }
}

inline QString palletStatusToString(PalletStatus status) {
    switch (status) {
        case PalletStatus::New: return "New";
        case PalletStatus::Complete: return "Complete";
        case PalletStatus::Exported: return "Exported";
        default: return "Unknown";
    }
}

// ============================================================================
// Entity Structs
// ============================================================================

struct User {
    UserId id = 0;
    QString username;
    QString pinHash;
    QString fullName;
    QString email;
    QString phoneNumber;
    bool active = true;
    bool superuser = false;
    QDateTime createdAt;
    std::optional<QDateTime> lastLogin;
};

struct Role {
    RoleId id = 0;
    QString name;
    QString description;
    bool active = true;
};

struct Permission {
    qint32 id = 0;
    QString name;
    QString category;
    QString description;
};

struct AuthenticatedUser {
    User user;
    QVector<Role> roles;
    QVector<Permission> permissions;
    
    bool hasPermission(const QString& permName) const {
        if (user.superuser) return true;
        for (const auto& p : permissions) {
            if (p.name == permName) return true;
        }
        return false;
    }
    
    bool hasRole(const QString& roleName) const {
        for (const auto& r : roles) {
            if (r.name == roleName) return true;
        }
        return false;
    }
};

struct ProductionLine {
    ProductionLineId id = 0;
    QString name;
    QDateTime createdAt;
};

struct Product {
    ProductId id = 0;
    QString gtin;
    QString name;
    QString description;
    QDateTime createdAt;
};

struct ProductPackaging {
    ProductPackagingId id = 0;
    ProductId productId = 0;
    int numberOfProducts = 0;
    QString gtin;
    QString name;
    QString description;
    QDateTime createdAt;
};

struct Item {
    ItemId id = 0;
    QString barcode;
    ItemStatus status = ItemStatus::Available;
    ProductionLineId productionLine = 0;
    ProductId productId = 0;  // References the product (determines table)
    QDateTime importedAt;
    std::optional<QDateTime> scannedAt;
    bool isDeleted = false;

    QString statusString() const { return itemStatusToString(status); }
};

struct Box {
    BoxId id = 0;
    QString barcode;
    BoxStatus status = BoxStatus::Empty;
    ProductionLineId productionLine = 0;
    ProductPackagingId packagingId = 0;  // References the packaging (determines table)
    QDateTime importedAt;
    std::optional<QDateTime> sealedAt;
    bool isDeleted = false;

    QString statusString() const { return boxStatusToString(status); }
};

struct Pallet {
    PalletId id = 0;
    QString barcode;
    PalletStatus status = PalletStatus::New;
    ProductionLineId productionLine = 0;
    QDateTime createdAt;
    int maxBoxes = 0;  // max boxes per pallet (0 = unlimited)

    QString statusString() const { return palletStatusToString(status); }
};

struct ExportDocument {
    ExportDocumentId id = 0;
    ExportMode mode = ExportMode::BoxExport;
    QString lpTin;
    QDateTime createdAt;
    QByteArray xmlContent;
    QString xmlHash;
    int itemCount = 0;
    int boxCount = 0;
    int palletCount = 0;
};

// ============================================================================
// Result Structs
// ============================================================================

struct ImportResult {
    int totalRecords = 0;
    int importedCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    QStringList errors;
    
    bool success() const { 
        return errorCount == 0 && errors.isEmpty(); 
    }
    
    QString summary() const {
        return QString("Total: %1, Imported: %2, Skipped: %3, Errors: %4")
            .arg(totalRecords)
            .arg(importedCount)
            .arg(skippedCount)
            .arg(errorCount);
    }
};

struct ExportResult {
    bool success = false;
    ExportDocumentId documentId = 0;
    QString error;
    int itemsExported = 0;
    int boxesExported = 0;
    int palletsExported = 0;
    
    QString summary() const {
        if (!success) return "Failed: " + error;
        return QString("Document #%1 - Items: %2, Boxes: %3, Pallets: %4")
            .arg(documentId)
            .arg(itemsExported)
            .arg(boxesExported)
            .arg(palletsExported);
    }
};

struct ProductionStats {
    // Items
    int totalItems = 0;
    int availableItems = 0;
    int assignedItems = 0;
    int exportedItems = 0;
    
    // Boxes
    int totalBoxes = 0;
    int emptyBoxes = 0;
    int sealedBoxes = 0;
    int exportedBoxes = 0;
    
    // Pallets
    int totalPallets = 0;
    int newPallets = 0;
    int completePallets = 0;
    int exportedPallets = 0;
};

// ============================================================================
// Configuration
// ============================================================================

struct AppConfig {
    QString host = "localhost";
    int port = 5432;
    QString database = "prod_auto_dev";
    QString user = "prod_auto_dev";
    QString password;
    bool validated = false;
    
    bool isValid() const {
        return !host.isEmpty() && 
               !database.isEmpty() && 
               !user.isEmpty() && 
               port > 0 && port < 65536;
    }
    
    QString displayString() const {
        return QString("%1@%2:%3/%4")
            .arg(user, host)
            .arg(port)
            .arg(database);
    }
};

// ============================================================================
// Pub/Sub Event Types
// ============================================================================

struct Event {
    qint64      id = 0;
    QString     table;      // "items" | "boxes"
    QString     type;       // "insert" | "delete" | "marked_as_deleted" | "status_to_0" | "bulk_import_finished"
    qint64      rowId = 0;
    QJsonObject payload;
    QDateTime   createdAt;
};

struct SubscriberInfo {
    QString  clientId;
    QString  connectionMode;    // "permanent" | "one_shot"
    QString  subscriptionMode;  // "full" | "notify_only"
    QString  callbackHost;      // "ip:port" � used only for one_shot
    qint64   lastEventId = 0;
};

// ============================================================================
// Pipeline Types (Entity Resolver → State Resolver → Capability Engine)
// ============================================================================

enum class EntityType { Item, Box, Pallet, Unknown };

inline QString entityTypeToString(EntityType t) {
    switch (t) {
        case EntityType::Item:   return "item";
        case EntityType::Box:    return "box";
        case EntityType::Pallet: return "pallet";
        default:                 return "unknown";
    }
}

inline EntityType entityTypeFromString(const QString& s) {
    if (s == "item")   return EntityType::Item;
    if (s == "box")    return EntityType::Box;
    if (s == "pallet") return EntityType::Pallet;
    return EntityType::Unknown;
}

// Resolved entity — result of Entity Resolver
// Holds the entity plus the GTIN metadata needed to address dynamic tables
struct ResolvedEntity {
    EntityType type = EntityType::Unknown;

    // Exactly one of these is populated
    std::optional<Item>   item;
    std::optional<Box>    box;
    std::optional<Pallet> pallet;

    // Table-addressing metadata (populated for item/box)
    ProductId           productId = 0;
    ProductPackagingId  packagingId = 0;
    QString             productGtin;
    QString             packagingGtin;
};

// Computed state — result of State Resolver
// Contains derived state and upward-resolved parent context
struct EntityState {
    EntityType type = EntityType::Unknown;

    // Pallet derived state
    int  palletBoxCount   = 0;
    int  palletMaxBoxes   = 0;
    bool palletIsFull     = false;
    bool palletIsComplete = false;

    // Box derived state
    int  boxItemCount = 0;
    bool boxIsFree    = true;
    bool boxIsSealed  = false;

    // Item derived state
    bool itemHasBox = false;

    // Upward-resolved parent context
    std::optional<Box>    parentBox;
    std::optional<Pallet> parentPallet;

    // Table-addressing metadata (copied from ResolvedEntity)
    ProductId           productId = 0;
    ProductPackagingId  packagingId = 0;
    QString             productGtin;
    QString             packagingGtin;

    // The original entity data
    std::optional<Item>   item;
    std::optional<Box>    box;
    std::optional<Pallet> pallet;
};

// Action step definition (from capability rules JSON)
struct ActionStep {
    QString name;       // e.g. "box_barcode", "old_box", "new_box"
    QString input;      // "scan" (only type for now)
    QString prompt;     // "Scan box to remove"
};

// Available action — built by Capability Engine
struct AvailableAction {
    QString             capability;  // "COMPLETE", "REMOVE", etc.
    QString             scope;       // "pallet", "box", "item"
    QString             label;       // Human-readable label for UI
    QVector<ActionStep> steps;       // Multi-step parameters
    QString             confirm;     // Confirmation message (may be empty)
};

// Action result — returned by Action Executor
struct ActionResult {
    bool    success = false;
    QString message;
    QJsonObject data;
};

} // namespace core

Q_DECLARE_METATYPE(core::Event)

#endif // CORE_TYPES_H
