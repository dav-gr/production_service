#ifndef SERVER_PIPELINE_ENTITY_RESOLVER_H
#define SERVER_PIPELINE_ENTITY_RESOLVER_H

#include "core/db/db_service.h"
#include "core/db/types.h"
#include <optional>

namespace server::pipeline {

class EntityResolver {
public:
    explicit EntityResolver(core::DbService* db);

    // Resolve a barcode to an entity across all dynamic tables
    // Searches pallets -> boxes_{gtin} -> items_{gtin}
    // Returns std::nullopt if barcode not found anywhere
    std::optional<core::ResolvedEntity> resolve(const QString& barcode);

private:
    core::DbService* db_;
};

} // namespace server::pipeline

#endif // SERVER_PIPELINE_ENTITY_RESOLVER_H
