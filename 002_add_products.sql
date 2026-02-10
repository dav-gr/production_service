-- ============================================================================
-- Migration: 002_add_products.sql
-- Production Automation System - Products Extension
-- ============================================================================

BEGIN;

-- ============================================================================
-- PRODUCTS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS products (
    id BIGSERIAL PRIMARY KEY,
    gtin VARCHAR(14) NOT NULL,
    name VARCHAR(255) NOT NULL,
    description TEXT,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_product_gtin UNIQUE (gtin),
    CONSTRAINT chk_product_gtin_format CHECK (gtin ~ '^[0-9]{8,14}$'),
    CONSTRAINT chk_product_name_not_empty CHECK (LENGTH(TRIM(name)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_products_gtin ON products(gtin);
CREATE INDEX IF NOT EXISTS idx_products_name ON products(name);

COMMENT ON TABLE products IS 'Product master data with GTIN codes';

-- ============================================================================
-- PRODUCT PACKAGING TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS product_packaging (
    id BIGSERIAL PRIMARY KEY,
    product_id BIGINT NOT NULL,
    number_of_products INTEGER NOT NULL,
    gtin VARCHAR(14) NOT NULL,
    name VARCHAR(255) NOT NULL,
    description TEXT,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT fk_packaging_product 
        FOREIGN KEY (product_id) REFERENCES products(id)
        ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT uniq_packaging_gtin UNIQUE (gtin),
    CONSTRAINT chk_packaging_gtin_format CHECK (gtin ~ '^[0-9]{8,14}$'),
    CONSTRAINT chk_packaging_name_not_empty CHECK (LENGTH(TRIM(name)) > 0),
    CONSTRAINT chk_packaging_quantity CHECK (number_of_products > 0)
);

CREATE INDEX IF NOT EXISTS idx_packaging_product ON product_packaging(product_id);
CREATE INDEX IF NOT EXISTS idx_packaging_gtin ON product_packaging(gtin);

COMMENT ON TABLE product_packaging IS 'Product packaging configurations';

-- ============================================================================
-- ADD PRODUCTION DATA MANAGEMENT PERMISSION
-- ============================================================================

INSERT INTO permissions (permission_name, description, category) VALUES
    ('production.manage_products', 'Manage products and packaging', 'production')
ON CONFLICT (permission_name) DO NOTHING;

-- Give accountant access to production data management
INSERT INTO role_permissions (role_id, permission_id, granted)
SELECT r.id, p.id, TRUE
FROM roles r, permissions p
WHERE r.role_name = 'Accountant'
  AND p.permission_name IN ('production.manage_lines', 'production.manage_products')
ON CONFLICT (role_id, permission_id) DO NOTHING;

COMMIT;

-- Verification
DO $$
BEGIN
    RAISE NOTICE 'Migration 002 completed - products and product_packaging tables created';
END $$;
