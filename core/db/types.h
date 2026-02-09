#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <QString>
#include <QDateTime>
#include <QVector>
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
    QDateTime importedAt;
    std::optional<QDateTime> scannedAt;
    
    QString statusString() const { return itemStatusToString(status); }
};

struct Box {
    BoxId id = 0;
    QString barcode;
    BoxStatus status = BoxStatus::Empty;
    ProductionLineId productionLine = 0;
    QDateTime importedAt;
    std::optional<QDateTime> sealedAt;
    
    QString statusString() const { return boxStatusToString(status); }
};

struct Pallet {
    PalletId id = 0;
    QString barcode;
    PalletStatus status = PalletStatus::New;
    ProductionLineId productionLine = 0;
    QDateTime createdAt;
    // Note: Schema does not have completed_at column
    
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

} // namespace core

#endif // CORE_TYPES_H
