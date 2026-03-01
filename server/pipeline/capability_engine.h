#ifndef SERVER_PIPELINE_CAPABILITY_ENGINE_H
#define SERVER_PIPELINE_CAPABILITY_ENGINE_H

#include "core/db/types.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <optional>

namespace server::pipeline {

class CapabilityEngine {
public:
    CapabilityEngine();

    // Load rules from JSON file
    bool loadRules(const QString& filePath);

    // Evaluate rules against the computed state
    // Returns list of available actions for the given state
    QVector<core::AvailableAction> evaluate(const core::EntityState& state) const;

private:
    struct Condition {
        QString field;
        // Exactly one of these comparison operators is set
        std::optional<bool> eq_bool;
        std::optional<int>  eq_int;
        std::optional<int>  gt_int;
    };

    struct Rule {
        QString capability;
        QString scope;       // "pallet", "box", "item"
        QString label;
        QVector<Condition> conditions;
        QVector<core::ActionStep> steps;
        QString confirm;
    };

    // Evaluate a single condition against the state
    bool evaluateCondition(const Condition& cond, const core::EntityState& state) const;

    // Get field value from state by dotted path (e.g., "pallet.is_full")
    QVariant getFieldValue(const QString& field, const core::EntityState& state) const;

    // Interpolate confirm message with state values
    QString interpolateConfirm(const QString& tpl, const core::EntityState& state) const;

    QVector<Rule> rules_;
};

} // namespace server::pipeline

#endif // SERVER_PIPELINE_CAPABILITY_ENGINE_H
