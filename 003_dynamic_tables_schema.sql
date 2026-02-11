-- ============================================================================
-- Migration: 003_dynamic_tables_schema.sql
-- Production Automation System - Dynamic Tables Schema
-- ============================================================================
-- Version: 1.0
-- Created: 2026-01-18
-- Description: This migration documents the new dynamic table architecture.
--              Instead of single 'items' and 'boxes' tables, tables are now
--              created dynamically per product/packaging GTIN:
--              - items_{product_gtin} - Items table for each product
--              - boxes_{packaging_gtin} - Boxes table for each packaging
--              - {product_gtin}_{packaging_gtin}_assignments - Item-box assignments
--
-- Note: Tables are created automatically by DbService when products/packaging
--       are created. This migration preserves the original tables for backward
--       compatibility and reference.
-- ============================================================================

BEGIN;

-- ============================================================================
-- TEMPLATE: ITEMS TABLE (created per product GTIN)
-- ============================================================================
-- Table name format: items_{product_gtin}
-- Example: items_12345678901234
--
-- CREATE TABLE IF NOT EXISTS items_{gtin} (
--     id BIGSERIAL PRIMARY KEY,
--     bar_code VARCHAR(255) NOT NULL,
--     production_line BIGINT NOT NULL,
--     status SMALLINT NOT NULL DEFAULT 0,
--     imported_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
--     scanned_at TIMESTAMP,
--     
--     CONSTRAINT uniq_items_{gtin}_barcode UNIQUE (bar_code),
--     CONSTRAINT fk_items_{gtin}_production_line 
--         FOREIGN KEY (production_line) REFERENCES production_lines(id)
--         ON DELETE RESTRICT ON UPDATE CASCADE,
--     CONSTRAINT chk_items_{gtin}_status CHECK (status IN (0, 1, 2)),
--     CONSTRAINT chk_items_{gtin}_barcode_not_empty CHECK (LENGTH(TRIM(bar_code)) > 0)
-- );
--
-- Indexes:
-- - idx_items_{gtin}_barcode ON items_{gtin}(bar_code)
-- - idx_items_{gtin}_status ON items_{gtin}(status)
-- - idx_items_{gtin}_production_status ON items_{gtin}(production_line, status)

-- ============================================================================
-- TEMPLATE: BOXES TABLE (created per packaging GTIN)
-- ============================================================================
-- Table name format: boxes_{packaging_gtin}
-- Example: boxes_98765432109876
--
-- CREATE TABLE IF NOT EXISTS boxes_{gtin} (
--     id BIGSERIAL PRIMARY KEY,
--     bar_code VARCHAR(255) NOT NULL,
--     production_line BIGINT NOT NULL,
--     status SMALLINT NOT NULL DEFAULT 0,
--     imported_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
--     sealed_at TIMESTAMP,
--     
--     CONSTRAINT uniq_boxes_{gtin}_barcode UNIQUE (bar_code),
--     CONSTRAINT fk_boxes_{gtin}_production_line 
--         FOREIGN KEY (production_line) REFERENCES production_lines(id)
--         ON DELETE RESTRICT ON UPDATE CASCADE,
--     CONSTRAINT chk_boxes_{gtin}_status CHECK (status IN (0, 1, 2)),
--     CONSTRAINT chk_boxes_{gtin}_barcode_not_empty CHECK (LENGTH(TRIM(bar_code)) > 0)
-- );
--
-- Indexes:
-- - idx_boxes_{gtin}_barcode ON boxes_{gtin}(bar_code)
-- - idx_boxes_{gtin}_status ON boxes_{gtin}(status)
-- - idx_boxes_{gtin}_production_status ON boxes_{gtin}(production_line, status)

-- ============================================================================
-- TEMPLATE: ASSIGNMENTS TABLE (created per product/packaging combination)
-- ============================================================================
-- Table name format: {product_gtin}_{packaging_gtin}_assignments
-- Example: 12345678901234_98765432109876_assignments
--
-- CREATE TABLE IF NOT EXISTS {product_gtin}_{packaging_gtin}_assignments (
--     id BIGSERIAL PRIMARY KEY,
--     item_id BIGINT NOT NULL,
--     box_id BIGINT NOT NULL,
--     assigned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
--     
--     CONSTRAINT uniq_{product_gtin}_{packaging_gtin}_assignments_item UNIQUE (item_id),
--     CONSTRAINT fk_{product_gtin}_{packaging_gtin}_assignments_item 
--         FOREIGN KEY (item_id) REFERENCES items_{product_gtin}(id)
--         ON DELETE CASCADE ON UPDATE CASCADE,
--     CONSTRAINT fk_{product_gtin}_{packaging_gtin}_assignments_box 
--         FOREIGN KEY (box_id) REFERENCES boxes_{packaging_gtin}(id)
--         ON DELETE RESTRICT ON UPDATE CASCADE
-- );
--
-- Indexes:
-- - idx_{product_gtin}_{packaging_gtin}_assignments_item ON {table}(item_id)
-- - idx_{product_gtin}_{packaging_gtin}_assignments_box ON {table}(box_id)

-- ============================================================================
-- NOTE: Original tables are kept for backward compatibility
-- ============================================================================
-- The following tables from 001_initial_schema.sql are kept:
-- - items (original single items table)
-- - boxes (original single boxes table)
-- - item_box_assignments (original assignments table)
-- - pallet_box_assignments (still used - pallets reference boxes by ID)
-- - pallets (unchanged - single pallets table)

COMMIT;

-- ============================================================================
-- SCHEMA INFO
-- ============================================================================
DO $$
BEGIN
    RAISE NOTICE '====================================================';
    RAISE NOTICE 'Migration 003 completed';
    RAISE NOTICE 'Dynamic tables are created by DbService when';
    RAISE NOTICE 'products and packaging are created.';
    RAISE NOTICE '====================================================';
END $$;
