BEGIN;

-- Add max_boxes column to pallets table for pallet.is_full computation
ALTER TABLE pallets ADD COLUMN IF NOT EXISTS max_boxes INTEGER NOT NULL DEFAULT 0;

COMMIT;
