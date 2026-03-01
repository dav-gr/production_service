#include "state_resolver.h"
#include <QDebug>

namespace server::pipeline {

StateResolver::StateResolver(core::DbService* db) : db_(db) {}

core::EntityState StateResolver::computeState(const core::ResolvedEntity& entity) {
    core::EntityState state;
    state.type = entity.type;
    state.productId = entity.productId;
    state.packagingId = entity.packagingId;
    state.productGtin = entity.productGtin;
    state.packagingGtin = entity.packagingGtin;

    switch (entity.type) {
    case core::EntityType::Item:
        state.item = entity.item;
        resolveItemState(state, *entity.item);
        break;
    case core::EntityType::Box:
        state.box = entity.box;
        resolveBoxState(state, *entity.box);
        break;
    case core::EntityType::Pallet:
        state.pallet = entity.pallet;
        resolvePalletState(state, *entity.pallet);
        break;
    default:
        break;
    }
    return state;
}

void StateResolver::resolveItemState(core::EntityState& state,
                                      const core::Item& item) {
    // Check if item is assigned to a box (deprecated: uses global item_box_assignments)
    auto boxInfo = db_->findBoxForItem(item.id);
    if (boxInfo) {
        state.itemHasBox = true;

        // Resolve parent box (deprecated: uses global boxes table)
        auto parentBox = db_->getBox(boxInfo->boxId);
        if (parentBox) {
            state.parentBox = *parentBox;

            // Check if box is on a pallet -> upward resolve to pallet context
            auto palletId = db_->findPalletForBox(boxInfo->boxId);
            if (palletId) {
                auto parentPallet = db_->getPallet(*palletId);
                if (parentPallet) {
                    state.parentPallet = *parentPallet;
                    // UPWARD RESOLUTION: item -> box -> pallet
                    state.type = core::EntityType::Pallet;
                    state.pallet = *parentPallet;
                    resolvePalletState(state, *parentPallet);
                    return;
                }
            }

            // Box is sealed + free -> resolve to box context
            if (parentBox->status == core::BoxStatus::Sealed) {
                auto isFree = db_->isBoxFree(boxInfo->boxId);
                if (isFree) {
                    state.type = core::EntityType::Box;
                    state.box = *parentBox;
                    state.boxIsSealed = true;
                    state.boxIsFree = true;
                    state.boxItemCount = db_->getBoxItemCount(boxInfo->boxId);
                    return;
                }
            }
        }
    }
    // Item with no box, or box not sealed/not free -> show item context
    // state.type remains EntityType::Item
}

void StateResolver::resolveBoxState(core::EntityState& state,
                                     const core::Box& box) {
    state.boxIsSealed = (box.status == core::BoxStatus::Sealed);
    state.boxItemCount = db_->getBoxItemCount(box.id);

    // Check if box is on a pallet
    auto palletId = db_->findPalletForBox(box.id);
    if (palletId) {
        state.boxIsFree = false;
        auto parentPallet = db_->getPallet(*palletId);
        if (parentPallet) {
            state.parentPallet = *parentPallet;
            // UPWARD RESOLUTION: box -> pallet
            state.type = core::EntityType::Pallet;
            state.pallet = *parentPallet;
            resolvePalletState(state, *parentPallet);
            return;
        }
    } else {
        state.boxIsFree = true;
        // Box is sealed + free -> show box context (state.type remains Box)
    }
}

void StateResolver::resolvePalletState(core::EntityState& state,
                                        const core::Pallet& pallet) {
    state.palletBoxCount = db_->countBoxesOnPallet(pallet.id);
    state.palletMaxBoxes = pallet.maxBoxes;
    state.palletIsFull = (pallet.maxBoxes > 0 && state.palletBoxCount >= pallet.maxBoxes);
    state.palletIsComplete = (pallet.status == core::PalletStatus::Complete);
}

} // namespace server::pipeline
