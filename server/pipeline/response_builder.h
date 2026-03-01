#ifndef SERVER_PIPELINE_RESPONSE_BUILDER_H
#define SERVER_PIPELINE_RESPONSE_BUILDER_H

#include "core/db/types.h"
#include "server/protocol.h"
#include <QVector>

namespace server::pipeline {

class ResponseBuilder {
public:
    // Build ScanResponse from computed state and available actions
    static server::ScanResponse buildScanResponse(
        const core::EntityState& state,
        const QVector<core::AvailableAction>& actions);

private:
    static QJsonObject buildContext(const core::EntityState& state);
    static QJsonArray buildActions(const QVector<core::AvailableAction>& actions);
    static QJsonObject entityToJson(const core::Item& item);
    static QJsonObject entityToJson(const core::Box& box);
    static QJsonObject entityToJson(const core::Pallet& pallet);
};

} // namespace server::pipeline

#endif // SERVER_PIPELINE_RESPONSE_BUILDER_H
