#include "capability_engine.h"
#include <QFile>
#include <QJsonDocument>
#include <QDebug>

namespace server::pipeline {

CapabilityEngine::CapabilityEngine() {}

bool CapabilityEngine::loadRules(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[CapabilityEngine] Cannot open rules file:" << filePath;
        return false;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "[CapabilityEngine] JSON parse error:" << err.errorString();
        return false;
    }

    QJsonArray rulesArray = doc.object()["capability_rules"].toArray();
    rules_.clear();

    for (const auto& ruleVal : rulesArray) {
        QJsonObject ruleObj = ruleVal.toObject();
        Rule rule;
        rule.capability = ruleObj["capability"].toString();
        rule.scope = ruleObj["scope"].toString();
        rule.label = ruleObj["label"].toString();
        if (rule.label.isEmpty()) rule.label = rule.capability;
        rule.confirm = ruleObj["confirm"].toString();

        // Parse conditions
        QJsonArray whenArray = ruleObj["when"].toArray();
        for (const auto& condVal : whenArray) {
            QJsonObject condObj = condVal.toObject();
            Condition cond;
            cond.field = condObj["field"].toString();
            if (condObj.contains("eq")) {
                QJsonValue eqVal = condObj["eq"];
                if (eqVal.isBool()) cond.eq_bool = eqVal.toBool();
                else if (eqVal.isDouble()) cond.eq_int = eqVal.toInt();
            }
            if (condObj.contains("gt")) {
                cond.gt_int = condObj["gt"].toInt();
            }
            rule.conditions.append(cond);
        }

        // Parse steps
        QJsonArray stepsArray = ruleObj["steps"].toArray();
        for (const auto& stepVal : stepsArray) {
            QJsonObject stepObj = stepVal.toObject();
            core::ActionStep step;
            step.name = stepObj["name"].toString();
            step.input = stepObj["input"].toString();
            step.prompt = stepObj["prompt"].toString();
            rule.steps.append(step);
        }

        rules_.append(rule);
        qInfo() << "[CapabilityEngine] Loaded rule:" << rule.capability
                << "scope:" << rule.scope
                << "conditions:" << rule.conditions.size();
    }

    qInfo() << "[CapabilityEngine] Loaded" << rules_.size() << "capability rules";
    return true;
}

QVector<core::AvailableAction> CapabilityEngine::evaluate(
    const core::EntityState& state) const {

    QVector<core::AvailableAction> actions;
    QString scope = core::entityTypeToString(state.type);

    for (const auto& rule : rules_) {
        if (rule.scope != scope) continue;

        bool allMatch = true;
        for (const auto& cond : rule.conditions) {
            if (!evaluateCondition(cond, state)) {
                allMatch = false;
                break;
            }
        }

        if (allMatch) {
            core::AvailableAction action;
            action.capability = rule.capability;
            action.scope = rule.scope;
            action.label = rule.label;
            action.steps = rule.steps;
            action.confirm = interpolateConfirm(rule.confirm, state);
            actions.append(action);
        }
    }
    return actions;
}

QVariant CapabilityEngine::getFieldValue(const QString& field,
                                          const core::EntityState& state) const {
    // Pallet fields
    if (field == "pallet.is_full")      return state.palletIsFull;
    if (field == "pallet.is_complete")  return state.palletIsComplete;
    if (field == "pallet.box_count")    return state.palletBoxCount;
    if (field == "pallet.max_boxes")    return state.palletMaxBoxes;

    // Box fields
    if (field == "box.status")     return state.box ? static_cast<int>(state.box->status) : -1;
    if (field == "box.is_free")    return state.boxIsFree;
    if (field == "box.is_sealed")  return state.boxIsSealed;
    if (field == "box.item_count") return state.boxItemCount;

    // Item fields
    if (field == "item.status")  return state.item ? static_cast<int>(state.item->status) : -1;
    if (field == "item.has_box") return state.itemHasBox;

    qWarning() << "[CapabilityEngine] Unknown field:" << field;
    return QVariant();
}

bool CapabilityEngine::evaluateCondition(const Condition& cond,
                                          const core::EntityState& state) const {
    QVariant val = getFieldValue(cond.field, state);
    if (!val.isValid()) return false;

    if (cond.eq_bool.has_value())
        return val.toBool() == cond.eq_bool.value();
    if (cond.eq_int.has_value())
        return val.toInt() == cond.eq_int.value();
    if (cond.gt_int.has_value())
        return val.toInt() > cond.gt_int.value();

    return false;
}

QString CapabilityEngine::interpolateConfirm(const QString& tpl,
                                              const core::EntityState& state) const {
    if (tpl.isEmpty()) return tpl;

    QString result = tpl;
    result.replace("{item_count}", QString::number(state.boxItemCount));
    result.replace("{box_count}", QString::number(state.palletBoxCount));
    result.replace("{max_boxes}", QString::number(state.palletMaxBoxes));
    return result;
}

} // namespace server::pipeline
