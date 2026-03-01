#include "response_builder.h"
#include <QJsonArray>

namespace server::pipeline {

server::ScanResponse ResponseBuilder::buildScanResponse(
    const core::EntityState& state,
    const QVector<core::AvailableAction>& actions) {

    server::ScanResponse resp;
    resp.entityType = core::entityTypeToString(state.type);
    resp.context = buildContext(state);
    resp.availableActions = buildActions(actions);
    return resp;
}

QJsonObject ResponseBuilder::buildContext(const core::EntityState& state) {
    QJsonObject ctx;
    ctx["entity_type"] = core::entityTypeToString(state.type);

    switch (state.type) {
    case core::EntityType::Pallet: {
        if (state.pallet) {
            ctx["pallet"] = entityToJson(*state.pallet);
            ctx["pallet_box_count"] = state.palletBoxCount;
            ctx["pallet_max_boxes"] = state.palletMaxBoxes;
            ctx["pallet_is_full"] = state.palletIsFull;
            ctx["pallet_is_complete"] = state.palletIsComplete;
        }
        // Include original scanned entity if it was a box/item that resolved upward
        if (state.box) ctx["scanned_box"] = entityToJson(*state.box);
        if (state.item) ctx["scanned_item"] = entityToJson(*state.item);
        break;
    }
    case core::EntityType::Box: {
        if (state.box) {
            ctx["box"] = entityToJson(*state.box);
            ctx["box_item_count"] = state.boxItemCount;
            ctx["box_is_free"] = state.boxIsFree;
            ctx["box_is_sealed"] = state.boxIsSealed;
        }
        if (state.item) ctx["scanned_item"] = entityToJson(*state.item);
        break;
    }
    case core::EntityType::Item: {
        if (state.item) {
            ctx["item"] = entityToJson(*state.item);
            ctx["item_has_box"] = state.itemHasBox;
        }
        break;
    }
    default:
        break;
    }
    return ctx;
}

QJsonArray ResponseBuilder::buildActions(
    const QVector<core::AvailableAction>& actions) {
    QJsonArray arr;
    for (const auto& a : actions) {
        QJsonObject obj;
        obj["action"] = a.capability;
        obj["label"] = a.label;
        obj["scope"] = a.scope;
        if (!a.confirm.isEmpty()) obj["confirm"] = a.confirm;
        if (!a.steps.isEmpty()) {
            QJsonArray stepsArr;
            for (const auto& s : a.steps) {
                QJsonObject stepObj;
                stepObj["name"] = s.name;
                stepObj["input"] = s.input;
                stepObj["prompt"] = s.prompt;
                stepsArr.append(stepObj);
            }
            obj["steps"] = stepsArr;
        }
        arr.append(obj);
    }
    return arr;
}

QJsonObject ResponseBuilder::entityToJson(const core::Item& item) {
    QJsonObject obj;
    obj["id"] = item.id;
    obj["bar_code"] = item.barcode;
    obj["status"] = static_cast<int>(item.status);
    obj["status_string"] = item.statusString();
    obj["production_line"] = item.productionLine;
    obj["imported_at"] = item.importedAt.toString(Qt::ISODate);
    if (item.scannedAt) {
        obj["scanned_at"] = item.scannedAt->toString(Qt::ISODate);
    }
    return obj;
}

QJsonObject ResponseBuilder::entityToJson(const core::Box& box) {
    QJsonObject obj;
    obj["id"] = box.id;
    obj["bar_code"] = box.barcode;
    obj["status"] = static_cast<int>(box.status);
    obj["status_string"] = box.statusString();
    obj["production_line"] = box.productionLine;
    obj["imported_at"] = box.importedAt.toString(Qt::ISODate);
    if (box.sealedAt) {
        obj["sealed_at"] = box.sealedAt->toString(Qt::ISODate);
    }
    return obj;
}

QJsonObject ResponseBuilder::entityToJson(const core::Pallet& pallet) {
    QJsonObject obj;
    obj["id"] = pallet.id;
    obj["bar_code"] = pallet.barcode;
    obj["status"] = static_cast<int>(pallet.status);
    obj["status_string"] = pallet.statusString();
    obj["production_line"] = pallet.productionLine;
    obj["created_at"] = pallet.createdAt.toString(Qt::ISODate);
    obj["max_boxes"] = pallet.maxBoxes;
    return obj;
}

} // namespace server::pipeline
