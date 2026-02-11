# DbService Refactoring Specification v2.0

## Overview

This document provides step-by-step instructions for refactoring projects that depend on `core::DbService`, `core::Item`, `core::Box`, and related types after the migration to **dynamic tables per product/packaging GTIN**.

### Key Architectural Change

Instead of single `items` and `boxes` tables, the system now uses:
- `items_{product_gtin}` - Separate items table per product
- `boxes_{packaging_gtin}` - Separate boxes table per packaging  
- `{product_gtin}_{packaging_gtin}_assignments` - Item-box assignments per product/packaging combination

---

## Table of Contents

1. [Struct Changes](#1-struct-changes)
2. [Production Line Operations](#2-production-line-operations)
3. [Import Operations](#3-import-operations)
4. [Item Operations](#4-item-operations)
5. [Box Operations](#5-box-operations)
6. [Export Operations](#6-export-operations)
7. [Statistics](#7-statistics)
8. [Migration Checklist](#8-migration-checklist)
9. [Code Examples](#9-code-examples)

---

## 1. Struct Changes

### 1.1 Item Struct

**Before:**
```cpp
struct Item {
    ItemId id = 0;
    QString barcode;
    ItemStatus status = ItemStatus::Available;
    ProductionLineId productionLine = 0;
    QDateTime importedAt;
    std::optional<QDateTime> scannedAt;
    bool isDeleted = false;
};
```

**After:**
```cpp
struct Item {
    ItemId id = 0;
    QString barcode;
    ItemStatus status = ItemStatus::Available;
    ProductionLineId productionLine = 0;
    ProductId productId = 0;  // NEW: References the product (determines table)
    QDateTime importedAt;
    std::optional<QDateTime> scannedAt;
    bool isDeleted = false;
};
```

**Action Required:**
- If you access `Item` fields, be aware of the new `productId` field
- When serializing/deserializing `Item`, include `productId`
- Update any JSON/protocol mappings to include `product_id`

### 1.2 Box Struct

**Before:**
```cpp
struct Box {
    BoxId id = 0;
    QString barcode;
    BoxStatus status = BoxStatus::Empty;
    ProductionLineId productionLine = 0;
    QDateTime importedAt;
    std::optional<QDateTime> sealedAt;
    bool isDeleted = false;
};
```

**After:**
```cpp
struct Box {
    BoxId id = 0;
    QString barcode;
    BoxStatus status = BoxStatus::Empty;
    ProductionLineId productionLine = 0;
    ProductPackagingId packagingId = 0;  // NEW: References the packaging (determines table)
    QDateTime importedAt;
    std::optional<QDateTime> sealedAt;
    bool isDeleted = false;
};
```

**Action Required:**
- If you access `Box` fields, be aware of the new `packagingId` field
- When serializing/deserializing `Box`, include `packagingId`
- Update any JSON/protocol mappings to include `packaging_id`

---

## 2. Production Line Operations

### 2.1 New Methods Added

The following **new methods** are now available:

```cpp
bool createProductionLine(const ProductionLine& line);
bool updateProductionLine(const ProductionLine& line);
bool deleteProductionLine(ProductionLineId id);
```

**Action Required:**
- If your project manages production lines, you can now use these CRUD methods
- No changes required if you only read production lines

---

## 3. Import Operations

### 3.1 importItemsAsync

**Before:**
```cpp
QFuture<ImportResult> importItemsAsync(const QString& filePath, 
                                        ProductionLineId lineId);
```

**After:**
```cpp
QFuture<ImportResult> importItemsAsync(const QString& filePath, 
                                        ProductionLineId lineId,
                                        ProductId productId);  // NEW PARAMETER
```

**Action Required:**
```cpp
// OLD
auto future = db->importItemsAsync(filePath, lineId);

// NEW - Must provide ProductId
auto future = db->importItemsAsync(filePath, lineId, productId);
```

### 3.2 importBoxesAsync

**Before:**
```cpp
QFuture<ImportResult> importBoxesAsync(const QString& filePath,
                                        ProductionLineId lineId);
```

**After:**
```cpp
QFuture<ImportResult> importBoxesAsync(const QString& filePath,
                                        ProductionLineId lineId,
                                        ProductPackagingId packagingId);  // NEW PARAMETER
```

**Action Required:**
```cpp
// OLD
auto future = db->importBoxesAsync(filePath, lineId);

// NEW - Must provide PackagingId
auto future = db->importBoxesAsync(filePath, lineId, packagingId);
```

### 3.3 importPalletsAsync (UNCHANGED)

```cpp
QFuture<ImportResult> importPalletsAsync(const QString& filePath,
                                          ProductionLineId lineId);
```

No changes required for pallet imports.

---

## 4. Item Operations

### 4.1 getItem

**Before:**
```cpp
std::optional<Item> getItem(ItemId id);
```

**After:**
```cpp
std::optional<Item> getItem(ProductId productId, ItemId id);  // NEW PARAMETER
```

**Action Required:**
```cpp
// OLD
auto item = db->getItem(itemId);

// NEW - Must provide ProductId first
auto item = db->getItem(productId, itemId);
```

### 4.2 getItemsByStatus

**Before:**
```cpp
QVector<Item> getItemsByStatus(ItemStatus status, 
                                ProductionLineId lineId = 0, 
                                int limit = 100);
```

**After:**
```cpp
QVector<Item> getItemsByStatus(ProductId productId,  // NEW PARAMETER (first)
                                ItemStatus status, 
                                ProductionLineId lineId = 0, 
                                int limit = 100);
```

**Action Required:**
```cpp
// OLD
auto items = db->getItemsByStatus(ItemStatus::Available, lineId, 100);

// NEW - Must provide ProductId first
auto items = db->getItemsByStatus(productId, ItemStatus::Available, lineId, 100);
```

### 4.3 getItemsInBox

**Before:**
```cpp
QVector<Item> getItemsInBox(BoxId boxId);
```

**After:**
```cpp
QVector<Item> getItemsInBox(ProductId productId,           // NEW
                            ProductPackagingId packagingId, // NEW
                            BoxId boxId);
```

**Action Required:**
```cpp
// OLD
auto items = db->getItemsInBox(boxId);

// NEW - Must provide ProductId and PackagingId
auto items = db->getItemsInBox(productId, packagingId, boxId);
```

### 4.4 getScannedItemsNotInBox

**Before:**
```cpp
QVector<Item> getScannedItemsNotInBox(ProductionLineId lineId = 0, int limit = 200);
```

**After:**
```cpp
QVector<Item> getScannedItemsNotInBox(ProductId productId,           // NEW
                                       ProductPackagingId packagingId, // NEW
                                       ProductionLineId lineId = 0, 
                                       int limit = 200);
```

### 4.5 countScannedItemsNotInBox

**Before:**
```cpp
int countScannedItemsNotInBox(ProductionLineId lineId = 0);
```

**After:**
```cpp
int countScannedItemsNotInBox(ProductId productId,           // NEW
                               ProductPackagingId packagingId, // NEW
                               ProductionLineId lineId = 0);
```

### 4.6 assignItemToBox

**Before:**
```cpp
bool assignItemToBox(ItemId itemId, BoxId boxId);
```

**After:**
```cpp
bool assignItemToBox(ProductId productId,           // NEW
                     ProductPackagingId packagingId, // NEW
                     ItemId itemId, 
                     BoxId boxId);
```

**Action Required:**
```cpp
// OLD
bool success = db->assignItemToBox(itemId, boxId);

// NEW - Must provide ProductId and PackagingId first
bool success = db->assignItemToBox(productId, packagingId, itemId, boxId);
```

### 4.7 assignItemsToBox

**Before:**
```cpp
int assignItemsToBox(const QVector<ItemId>& itemIds, BoxId boxId);
```

**After:**
```cpp
int assignItemsToBox(ProductId productId,           // NEW
                     ProductPackagingId packagingId, // NEW
                     const QVector<ItemId>& itemIds, 
                     BoxId boxId);
```

---

## 5. Box Operations

### 5.1 getBox

**Before:**
```cpp
std::optional<Box> getBox(BoxId id);
```

**After:**
```cpp
std::optional<Box> getBox(ProductPackagingId packagingId, BoxId id);  // NEW PARAMETER
```

**Action Required:**
```cpp
// OLD
auto box = db->getBox(boxId);

// NEW - Must provide PackagingId first
auto box = db->getBox(packagingId, boxId);
```

### 5.2 getBoxesByStatus

**Before:**
```cpp
QVector<Box> getBoxesByStatus(BoxStatus status, 
                               ProductionLineId lineId = 0,
                               int limit = 100);
```

**After:**
```cpp
QVector<Box> getBoxesByStatus(ProductPackagingId packagingId,  // NEW PARAMETER (first)
                               BoxStatus status, 
                               ProductionLineId lineId = 0,
                               int limit = 100);
```

### 5.3 getSealedBoxesNotOnPallet

**Before:**
```cpp
QVector<Box> getSealedBoxesNotOnPallet(ProductionLineId lineId = 0, int limit = 200);
```

**After:**
```cpp
QVector<Box> getSealedBoxesNotOnPallet(ProductPackagingId packagingId,  // NEW
                                        ProductionLineId lineId = 0, 
                                        int limit = 200);
```

### 5.4 countSealedBoxesNotOnPallet

**Before:**
```cpp
int countSealedBoxesNotOnPallet(ProductionLineId lineId = 0);
```

**After:**
```cpp
int countSealedBoxesNotOnPallet(ProductPackagingId packagingId,  // NEW
                                 ProductionLineId lineId = 0);
```

### 5.5 getBoxesOnPallet

**Before:**
```cpp
QVector<Box> getBoxesOnPallet(PalletId palletId);
```

**After:**
```cpp
QVector<Box> getBoxesOnPallet(ProductPackagingId packagingId, PalletId palletId);  // NEW
```

### 5.6 sealBox

**Before:**
```cpp
bool sealBox(BoxId id);
```

**After:**
```cpp
bool sealBox(ProductPackagingId packagingId, BoxId id);  // NEW PARAMETER
```

**Action Required:**
```cpp
// OLD
bool success = db->sealBox(boxId);

// NEW - Must provide PackagingId first
bool success = db->sealBox(packagingId, boxId);
```

### 5.7 assignBoxToPallet

**Before:**
```cpp
bool assignBoxToPallet(BoxId boxId, PalletId palletId);
```

**After:**
```cpp
bool assignBoxToPallet(ProductPackagingId packagingId,  // NEW
                       BoxId boxId, 
                       PalletId palletId);
```

### 5.8 getBoxItemCount

**Before:**
```cpp
int getBoxItemCount(BoxId id);
```

**After:**
```cpp
int getBoxItemCount(ProductId productId,            // NEW
                    ProductPackagingId packagingId,  // NEW
                    BoxId id);
```

---

## 6. Export Operations

### 6.1 exportItemsAsync

**Before:**
```cpp
QFuture<ExportResult> exportItemsAsync(const QVector<ItemId>& itemIds,
                                        const QString& lpTin);
```

**After:**
```cpp
QFuture<ExportResult> exportItemsAsync(ProductId productId,  // NEW PARAMETER
                                        const QVector<ItemId>& itemIds,
                                        const QString& lpTin);
```

**Action Required:**
```cpp
// OLD
auto future = db->exportItemsAsync(itemIds, lpTin);

// NEW - Must provide ProductId first
auto future = db->exportItemsAsync(productId, itemIds, lpTin);
```

### 6.2 exportBoxesAsync

**Before:**
```cpp
QFuture<ExportResult> exportBoxesAsync(const QVector<BoxId>& boxIds, 
                                        const QString& lpTin);
```

**After:**
```cpp
QFuture<ExportResult> exportBoxesAsync(ProductId productId,            // NEW
                                        ProductPackagingId packagingId, // NEW
                                        const QVector<BoxId>& boxIds, 
                                        const QString& lpTin);
```

**Action Required:**
```cpp
// OLD
auto future = db->exportBoxesAsync(boxIds, lpTin);

// NEW - Must provide ProductId and PackagingId first
auto future = db->exportBoxesAsync(productId, packagingId, boxIds, lpTin);
```

### 6.3 exportPalletsAsync

**Before:**
```cpp
QFuture<ExportResult> exportPalletsAsync(const QVector<PalletId>& palletIds,
                                          const QString& lpTin);
```

**After:**
```cpp
QFuture<ExportResult> exportPalletsAsync(ProductId productId,            // NEW
                                          ProductPackagingId packagingId, // NEW
                                          const QVector<PalletId>& palletIds,
                                          const QString& lpTin);
```

---

## 7. Statistics

### 7.1 getStats

**Before:**
```cpp
ProductionStats getStats(std::optional<ProductionLineId> lineId = std::nullopt);
```

**After:**
```cpp
ProductionStats getStats(ProductId productId,            // NEW (required)
                         ProductPackagingId packagingId,  // NEW (required)
                         std::optional<ProductionLineId> lineId = std::nullopt);
```

**Action Required:**
```cpp
// OLD
auto stats = db->getStats();
auto stats = db->getStats(lineId);

// NEW - Must provide ProductId and PackagingId
auto stats = db->getStats(productId, packagingId);
auto stats = db->getStats(productId, packagingId, lineId);
```

---

## 8. Migration Checklist

Use this checklist to ensure complete migration:

### Step 1: Update Dependencies
- [ ] Update `core` library/submodule to latest version
- [ ] Rebuild to identify compilation errors

### Step 2: Data Model Updates
- [ ] Update `Item` serialization to include `productId`
- [ ] Update `Box` serialization to include `packagingId`
- [ ] Update JSON/Protocol mappings for `Item` and `Box`
- [ ] Update database queries if directly accessing structs

### Step 3: Context Management
- [ ] Ensure your application tracks current `ProductId` context
- [ ] Ensure your application tracks current `ProductPackagingId` context
- [ ] Consider adding product/packaging selection UI if not present

### Step 4: Import Operations
- [ ] Update all `importItemsAsync` calls to include `ProductId`
- [ ] Update all `importBoxesAsync` calls to include `ProductPackagingId`

### Step 5: Item Operations
- [ ] Update `getItem` calls
- [ ] Update `getItemsByStatus` calls
- [ ] Update `getItemsInBox` calls
- [ ] Update `getScannedItemsNotInBox` calls
- [ ] Update `countScannedItemsNotInBox` calls
- [ ] Update `assignItemToBox` calls
- [ ] Update `assignItemsToBox` calls

### Step 6: Box Operations
- [ ] Update `getBox` calls
- [ ] Update `getBoxesByStatus` calls
- [ ] Update `getSealedBoxesNotOnPallet` calls
- [ ] Update `countSealedBoxesNotOnPallet` calls
- [ ] Update `getBoxesOnPallet` calls
- [ ] Update `sealBox` calls
- [ ] Update `assignBoxToPallet` calls
- [ ] Update `getBoxItemCount` calls

### Step 7: Export Operations
- [ ] Update `exportItemsAsync` calls
- [ ] Update `exportBoxesAsync` calls
- [ ] Update `exportPalletsAsync` calls

### Step 8: Statistics
- [ ] Update `getStats` calls

### Step 9: Testing
- [ ] Test with existing data (if using legacy tables)
- [ ] Test with new product/packaging creation
- [ ] Test full workflow: import → scan → assign → seal → export

---

## 9. Code Examples

### 9.1 Full Workflow Example (Before)

```cpp
// OLD CODE
void processProduction(DbService* db, ProductionLineId lineId) {
    // Import
    db->importItemsAsync("items.csv", lineId);
    db->importBoxesAsync("boxes.csv", lineId);
    
    // Get items and boxes
    auto items = db->getItemsByStatus(ItemStatus::Available, lineId);
    auto boxes = db->getBoxesByStatus(BoxStatus::Empty, lineId);
    
    // Assign items to box
    if (!items.isEmpty() && !boxes.isEmpty()) {
        db->assignItemToBox(items[0].id, boxes[0].id);
    }
    
    // Seal and export
    db->sealBox(boxes[0].id);
    db->exportBoxesAsync({boxes[0].id}, "123456789");
    
    // Stats
    auto stats = db->getStats(lineId);
}
```

### 9.2 Full Workflow Example (After)

```cpp
// NEW CODE
void processProduction(DbService* db, ProductionLineId lineId,
                       ProductId productId, ProductPackagingId packagingId) {
    // Import - now requires product/packaging context
    db->importItemsAsync("items.csv", lineId, productId);
    db->importBoxesAsync("boxes.csv", lineId, packagingId);
    
    // Get items and boxes - now requires product/packaging context
    auto items = db->getItemsByStatus(productId, ItemStatus::Available, lineId);
    auto boxes = db->getBoxesByStatus(packagingId, BoxStatus::Empty, lineId);
    
    // Assign items to box - now requires both contexts
    if (!items.isEmpty() && !boxes.isEmpty()) {
        db->assignItemToBox(productId, packagingId, items[0].id, boxes[0].id);
    }
    
    // Seal and export - now requires packaging/product context
    db->sealBox(packagingId, boxes[0].id);
    db->exportBoxesAsync(productId, packagingId, {boxes[0].id}, "123456789");
    
    // Stats - now requires product/packaging context
    auto stats = db->getStats(productId, packagingId, lineId);
}
```

### 9.3 Getting Product/Packaging Context

```cpp
// Get available products
QVector<Product> products = db->getProducts();

// Get packaging for a product
QVector<ProductPackaging> allPackaging = db->getProductPackaging();
QVector<ProductPackaging> productPackaging;
for (const auto& pkg : allPackaging) {
    if (pkg.productId == selectedProduct.id) {
        productPackaging.append(pkg);
    }
}

// Now use product.id and packaging.id in operations
ProductId productId = selectedProduct.id;
ProductPackagingId packagingId = selectedPackaging.id;
```

### 9.4 Creating New Product and Packaging

```cpp
// Create product (this also creates items_{gtin} table)
Product product;
product.gtin = "12345678901234";
product.name = "My Product";
product.description = "Description";
db->createProduct(product);

// Create packaging (this also creates boxes_{gtin} and assignments table)
ProductPackaging packaging;
packaging.productId = productId;  // Get ID after creation or query
packaging.gtin = "98765432109876";
packaging.name = "Box of 10";
packaging.numberOfProducts = 10;
db->createPackaging(packaging);
```

---

## Appendix A: Method Signature Quick Reference

| Operation | Old Signature | New Signature |
|-----------|---------------|---------------|
| `getItem` | `(ItemId)` | `(ProductId, ItemId)` |
| `getItemsByStatus` | `(status, lineId, limit)` | `(ProductId, status, lineId, limit)` |
| `getItemsInBox` | `(BoxId)` | `(ProductId, PackagingId, BoxId)` |
| `assignItemToBox` | `(ItemId, BoxId)` | `(ProductId, PackagingId, ItemId, BoxId)` |
| `getBox` | `(BoxId)` | `(PackagingId, BoxId)` |
| `getBoxesByStatus` | `(status, lineId, limit)` | `(PackagingId, status, lineId, limit)` |
| `sealBox` | `(BoxId)` | `(PackagingId, BoxId)` |
| `assignBoxToPallet` | `(BoxId, PalletId)` | `(PackagingId, BoxId, PalletId)` |
| `getBoxItemCount` | `(BoxId)` | `(ProductId, PackagingId, BoxId)` |
| `importItemsAsync` | `(path, lineId)` | `(path, lineId, ProductId)` |
| `importBoxesAsync` | `(path, lineId)` | `(path, lineId, PackagingId)` |
| `exportItemsAsync` | `(itemIds, lpTin)` | `(ProductId, itemIds, lpTin)` |
| `exportBoxesAsync` | `(boxIds, lpTin)` | `(ProductId, PackagingId, boxIds, lpTin)` |
| `exportPalletsAsync` | `(palletIds, lpTin)` | `(ProductId, PackagingId, palletIds, lpTin)` |
| `getStats` | `(lineId?)` | `(ProductId, PackagingId, lineId?)` |

---

## Appendix B: Database Table Naming

| Table Type | Naming Convention | Example |
|------------|-------------------|---------|
| Items | `items_{product_gtin}` | `items_12345678901234` |
| Boxes | `boxes_{packaging_gtin}` | `boxes_98765432109876` |
| Assignments | `{product_gtin}_{packaging_gtin}_assignments` | `12345678901234_98765432109876_assignments` |
| Pallets | `pallets` (unchanged) | `pallets` |

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2026-01-18 | Dynamic tables per product/packaging GTIN |
| 1.0 | - | Original single-table architecture |
