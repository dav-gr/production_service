#include "entity_resolver.h"

namespace server::pipeline {

EntityResolver::EntityResolver(core::DbService* db) : db_(db) {}

std::optional<core::ResolvedEntity> EntityResolver::resolve(const QString& barcode) {
    return db_->findEntityByBarcode(barcode);
}

} // namespace server::pipeline
