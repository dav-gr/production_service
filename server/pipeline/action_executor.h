#ifndef SERVER_PIPELINE_ACTION_EXECUTOR_H
#define SERVER_PIPELINE_ACTION_EXECUTOR_H

#include "core/db/db_service.h"
#include "core/db/types.h"
#include "server/protocol.h"

namespace server::pipeline {

class ActionExecutor {
public:
    explicit ActionExecutor(core::DbService* db);

    // Execute an action request within the given entity context
    // All operations are atomic (single transaction)
    server::ActionResponse execute(const core::EntityState& state,
                                    const QString& action,
                                    const QJsonObject& params,
                                    qint64 userId);

private:
    server::ActionResponse doComplete(const core::EntityState& state, qint64 userId);
    server::ActionResponse doRemove(const core::EntityState& state,
                                     const QJsonObject& params, qint64 userId);
    server::ActionResponse doReplace(const core::EntityState& state,
                                      const QJsonObject& params, qint64 userId);
    server::ActionResponse doPrint(const core::EntityState& state, qint64 userId);
    server::ActionResponse doUnseal(const core::EntityState& state, qint64 userId);
    server::ActionResponse doDestroy(const core::EntityState& state, qint64 userId);

    void writeAudit(const QString& entityType, const QString& barcode,
                    const QString& action, qint64 userId, const QString& reason);

    core::DbService* db_;
};

} // namespace server::pipeline

#endif // SERVER_PIPELINE_ACTION_EXECUTOR_H
