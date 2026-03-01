#ifndef SERVER_PIPELINE_STATE_RESOLVER_H
#define SERVER_PIPELINE_STATE_RESOLVER_H

#include "core/db/db_service.h"
#include "core/db/types.h"

namespace server::pipeline {

class StateResolver {
public:
    explicit StateResolver(core::DbService* db);

    // Compute derived state and resolve upward (item->box->pallet)
    // Takes a resolved entity and returns full state with parent context
    core::EntityState computeState(const core::ResolvedEntity& entity);

private:
    // Resolve item -> box -> pallet chain
    void resolveItemState(core::EntityState& state, const core::Item& item);

    // Resolve box -> pallet chain
    void resolveBoxState(core::EntityState& state, const core::Box& box);

    // Resolve pallet state
    void resolvePalletState(core::EntityState& state, const core::Pallet& pallet);

    core::DbService* db_;
};

} // namespace server::pipeline

#endif // SERVER_PIPELINE_STATE_RESOLVER_H
