#include "action_executor.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

namespace server::pipeline {

ActionExecutor::ActionExecutor(core::DbService* db) : db_(db) {}

server::ActionResponse ActionExecutor::execute(const core::EntityState& state,
                                                const QString& action,
                                                const QJsonObject& params,
                                                qint64 userId) {
    qInfo() << "[ActionExecutor] Executing" << action
            << "on" << core::entityTypeToString(state.type)
            << "userId:" << userId;

    if (action == "COMPLETE") return doComplete(state, userId);
    if (action == "REMOVE")  return doRemove(state, params, userId);
    if (action == "REPLACE") return doReplace(state, params, userId);
    if (action == "PRINT")   return doPrint(state, userId);
    if (action == "UNSEAL")  return doUnseal(state, userId);
    if (action == "DESTROY") return doDestroy(state, userId);

    return server::ActionResponse::error("Unknown action: " + action);
}

server::ActionResponse ActionExecutor::doComplete(const core::EntityState& state,
                                                    qint64 userId) {
    if (!state.pallet)
        return server::ActionResponse::error("No pallet in context");
    if (!state.palletIsFull)
        return server::ActionResponse::error("Pallet is not full");
    if (state.palletIsComplete)
        return server::ActionResponse::error("Pallet already complete");

    QSqlDatabase sqlDb = db_->getDatabase();
    QSqlQuery q(sqlDb);
    q.prepare("UPDATE pallets SET status = 1 WHERE id = :id AND status = 0");
    q.bindValue(":id", state.pallet->id);

    if (!q.exec() || q.numRowsAffected() == 0)
        return server::ActionResponse::error("Failed to complete pallet");

    writeAudit("pallet", state.pallet->barcode, "complete", userId, "Pallet completed");

    QJsonObject data;
    data["pallet_id"] = state.pallet->id;
    data["pallet_barcode"] = state.pallet->barcode;
    return server::ActionResponse::ok("Pallet completed", data);
}

server::ActionResponse ActionExecutor::doRemove(const core::EntityState& state,
                                                  const QJsonObject& params,
                                                  qint64 userId) {
    if (!state.pallet)
        return server::ActionResponse::error("No pallet in context");

    QString boxBarcode = params["box_barcode"].toString();
    if (boxBarcode.isEmpty())
        return server::ActionResponse::error("box_barcode required");

    // Find the box in global boxes table
    auto boxEntity = db_->findEntityByBarcode(boxBarcode);
    if (!boxEntity || boxEntity->type != core::EntityType::Box)
        return server::ActionResponse::error("Box not found: " + boxBarcode);

    QSqlDatabase sqlDb = db_->getDatabase();
    sqlDb.transaction();

    // Delete from pallet_box_assignments
    QSqlQuery delQ(sqlDb);
    delQ.prepare("DELETE FROM pallet_box_assignments "
                  "WHERE box_id = :boxId AND pallet_id = :palletId");
    delQ.bindValue(":boxId", boxEntity->box->id);
    delQ.bindValue(":palletId", state.pallet->id);

    if (!delQ.exec() || delQ.numRowsAffected() == 0) {
        sqlDb.rollback();
        return server::ActionResponse::error("Box not on this pallet");
    }

    // If pallet was complete, uncomplete it
    if (state.palletIsComplete) {
        QSqlQuery uncQ(sqlDb);
        uncQ.prepare("UPDATE pallets SET status = 0 WHERE id = :id AND status = 1");
        uncQ.bindValue(":id", state.pallet->id);
        uncQ.exec();
    }

    sqlDb.commit();

    writeAudit("pallet", state.pallet->barcode, "remove_box", userId,
               "Removed box " + boxBarcode);

    QJsonObject data;
    data["box_barcode"] = boxBarcode;
    data["pallet_barcode"] = state.pallet->barcode;
    return server::ActionResponse::ok("Box removed from pallet", data);
}

server::ActionResponse ActionExecutor::doReplace(const core::EntityState& state,
                                                   const QJsonObject& params,
                                                   qint64 userId) {
    if (!state.pallet)
        return server::ActionResponse::error("No pallet in context");

    QString oldBoxBarcode = params["old_box"].toString();
    QString newBoxBarcode = params["new_box"].toString();
    if (oldBoxBarcode.isEmpty() || newBoxBarcode.isEmpty())
        return server::ActionResponse::error("old_box and new_box barcodes required");

    // Resolve both boxes
    auto oldEntity = db_->findEntityByBarcode(oldBoxBarcode);
    auto newEntity = db_->findEntityByBarcode(newBoxBarcode);

    if (!oldEntity || oldEntity->type != core::EntityType::Box)
        return server::ActionResponse::error("Old box not found: " + oldBoxBarcode);
    if (!newEntity || newEntity->type != core::EntityType::Box)
        return server::ActionResponse::error("New box not found: " + newBoxBarcode);

    // Validate new box: must be sealed and free
    if (newEntity->box->status != core::BoxStatus::Sealed)
        return server::ActionResponse::error("New box must be sealed");
    if (!db_->isBoxFree(newEntity->box->id))
        return server::ActionResponse::error("New box is already on a pallet");

    QSqlDatabase sqlDb = db_->getDatabase();
    sqlDb.transaction();

    // Delete old assignment
    QSqlQuery delQ(sqlDb);
    delQ.prepare("DELETE FROM pallet_box_assignments "
                  "WHERE box_id = :boxId AND pallet_id = :palletId");
    delQ.bindValue(":boxId", oldEntity->box->id);
    delQ.bindValue(":palletId", state.pallet->id);
    if (!delQ.exec() || delQ.numRowsAffected() == 0) {
        sqlDb.rollback();
        return server::ActionResponse::error("Old box not on this pallet");
    }

    // Insert new assignment
    QSqlQuery insQ(sqlDb);
    insQ.prepare("INSERT INTO pallet_box_assignments (box_id, pallet_id, assigned_at) "
                  "VALUES (:boxId, :palletId, NOW())");
    insQ.bindValue(":boxId", newEntity->box->id);
    insQ.bindValue(":palletId", state.pallet->id);
    if (!insQ.exec()) {
        sqlDb.rollback();
        return server::ActionResponse::error("Failed to assign new box: " + insQ.lastError().text());
    }

    sqlDb.commit();

    writeAudit("pallet", state.pallet->barcode, "replace_box", userId,
               "Replaced " + oldBoxBarcode + " with " + newBoxBarcode);

    QJsonObject data;
    data["old_box"] = oldBoxBarcode;
    data["new_box"] = newBoxBarcode;
    data["pallet_barcode"] = state.pallet->barcode;
    return server::ActionResponse::ok("Box replaced", data);
}

server::ActionResponse ActionExecutor::doPrint(const core::EntityState& state,
                                                 qint64 userId) {
    if (!state.pallet)
        return server::ActionResponse::error("No pallet in context");

    // STUB: log only — actual printing will be implemented later
    qInfo() << "[ActionExecutor] PRINT stub for pallet" << state.pallet->barcode;

    writeAudit("pallet", state.pallet->barcode, "print", userId, "Print label (stub)");

    QJsonObject data;
    data["pallet_barcode"] = state.pallet->barcode;
    return server::ActionResponse::ok("Print requested (stub)", data);
}

server::ActionResponse ActionExecutor::doUnseal(const core::EntityState& state,
                                                  qint64 userId) {
    if (!state.box)
        return server::ActionResponse::error("No box in context");

    core::ActionResult dbResult = db_->unsealBoxAction(state.box->id);

    if (!dbResult.success)
        return server::ActionResponse::error(dbResult.message);

    writeAudit("box", state.box->barcode, "unseal_box", userId,
               "Box unsealed: " + dbResult.message);

    return server::ActionResponse::ok(dbResult.message, dbResult.data);
}

server::ActionResponse ActionExecutor::doDestroy(const core::EntityState& state,
                                                   qint64 userId) {
    if (!state.item)
        return server::ActionResponse::error("No item in context");

    core::ActionResult dbResult = db_->destroyItemAction(state.item->id);

    if (!dbResult.success)
        return server::ActionResponse::error(dbResult.message);

    writeAudit("item", state.item->barcode, "destroy_item", userId,
               "Item destroyed: " + dbResult.message);

    return server::ActionResponse::ok(dbResult.message, dbResult.data);
}

void ActionExecutor::writeAudit(const QString& entityType, const QString& barcode,
                                 const QString& action, qint64 userId,
                                 const QString& reason) {
    QSqlDatabase sqlDb = db_->getDatabase();
    QSqlQuery q(sqlDb);
    q.prepare("INSERT INTO audit_log (entity_type, barcode, action, user_id, reason, created_at) "
              "VALUES (:entityType, :barcode, :action, :userId, :reason, NOW())");
    q.bindValue(":entityType", entityType);
    q.bindValue(":barcode", barcode);
    q.bindValue(":action", action);
    q.bindValue(":userId", userId);
    q.bindValue(":reason", reason);

    if (!q.exec()) {
        qWarning() << "[ActionExecutor] Failed to write audit log:"
                    << q.lastError().text();
    }
}

} // namespace server::pipeline
