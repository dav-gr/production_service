-- ============================================================================
-- Migration: 001_initial_schema.sql
-- Production Automation System
-- ============================================================================
-- Version: 1.0
-- Created: 2026-01-18
-- Description: Complete initial schema with tables, constraints, indexes,
--              triggers, and seed data.
-- ============================================================================

-- Run this with: psql -U postgres -d prod_auto_dev -f migrations/001_initial_schema.sql

BEGIN;

-- ============================================================================
-- 1. PRODUCTION TABLES
-- ============================================================================

-- 1.1 Production Lines Table
CREATE TABLE IF NOT EXISTS production_lines (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    description TEXT,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_production_line_name UNIQUE (name),
    CONSTRAINT chk_production_line_name_not_empty CHECK (LENGTH(TRIM(name)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_production_lines_active 
    ON production_lines(active) WHERE active = TRUE;


-- 1.2 Items Table
CREATE TABLE IF NOT EXISTS items (
    id BIGSERIAL PRIMARY KEY,
    bar_code VARCHAR(255) NOT NULL,
    production_line BIGINT NOT NULL,
    status SMALLINT NOT NULL DEFAULT 0,
    imported_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    scanned_at TIMESTAMP,
    
    CONSTRAINT uniq_item_barcode UNIQUE (bar_code),
    CONSTRAINT fk_item_production_line 
        FOREIGN KEY (production_line) REFERENCES production_lines(id)
        ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT chk_item_status CHECK (status IN (0, 1, 2)),
    CONSTRAINT chk_item_barcode_not_empty CHECK (LENGTH(TRIM(bar_code)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_items_barcode ON items(bar_code);
CREATE INDEX IF NOT EXISTS idx_items_status ON items(status);
CREATE INDEX IF NOT EXISTS idx_items_barcode_status ON items(bar_code, status);
CREATE INDEX IF NOT EXISTS idx_items_production_status ON items(production_line, status);
CREATE INDEX IF NOT EXISTS idx_items_status_imported ON items(status, imported_at);
CREATE INDEX IF NOT EXISTS idx_items_scanned ON items(scanned_at) WHERE scanned_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_items_available ON items(production_line, imported_at) WHERE status = 0;
CREATE INDEX IF NOT EXISTS idx_items_assigned ON items(production_line) WHERE status = 1;


-- 1.3 Boxes Table
CREATE TABLE IF NOT EXISTS boxes (
    id BIGSERIAL PRIMARY KEY,
    bar_code VARCHAR(255) NOT NULL,
    production_line BIGINT NOT NULL,
    status SMALLINT NOT NULL DEFAULT 0,
    imported_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    sealed_at TIMESTAMP,
    
    CONSTRAINT uniq_box_barcode UNIQUE (bar_code),
    CONSTRAINT fk_box_production_line 
        FOREIGN KEY (production_line) REFERENCES production_lines(id)
        ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT chk_box_status CHECK (status IN (0, 1, 2)),
    CONSTRAINT chk_box_barcode_not_empty CHECK (LENGTH(TRIM(bar_code)) > 0),
    CONSTRAINT chk_box_sealed_at_consistency 
        CHECK ((status = 0 AND sealed_at IS NULL) OR (status IN (1, 2)))
);

CREATE INDEX IF NOT EXISTS idx_boxes_barcode ON boxes(bar_code);
CREATE INDEX IF NOT EXISTS idx_boxes_status ON boxes(status);
CREATE INDEX IF NOT EXISTS idx_boxes_status_sealed ON boxes(status, sealed_at);
CREATE INDEX IF NOT EXISTS idx_boxes_production_status ON boxes(production_line, status);
CREATE INDEX IF NOT EXISTS idx_boxes_empty ON boxes(production_line) WHERE status = 0;
CREATE INDEX IF NOT EXISTS idx_boxes_sealed ON boxes(production_line, sealed_at) WHERE status = 1;


-- 1.4 Pallets Table
CREATE TABLE IF NOT EXISTS pallets (
    id BIGSERIAL PRIMARY KEY,
    bar_code VARCHAR(255) NOT NULL,
    production_line BIGINT NOT NULL,
    status SMALLINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_pallet_barcode UNIQUE (bar_code),
    CONSTRAINT fk_pallet_production_line 
        FOREIGN KEY (production_line) REFERENCES production_lines(id)
        ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT chk_pallet_status CHECK (status IN (0, 1, 2)),
    CONSTRAINT chk_pallet_barcode_not_empty CHECK (LENGTH(TRIM(bar_code)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_pallets_barcode ON pallets(bar_code);
CREATE INDEX IF NOT EXISTS idx_pallets_status ON pallets(status);
CREATE INDEX IF NOT EXISTS idx_pallets_status_created ON pallets(status, created_at);
CREATE INDEX IF NOT EXISTS idx_pallets_production_status ON pallets(production_line, status);
CREATE INDEX IF NOT EXISTS idx_pallets_new ON pallets(production_line) WHERE status = 0;
CREATE INDEX IF NOT EXISTS idx_pallets_complete ON pallets(production_line) WHERE status = 1;


-- 1.5 Item-Box Assignments Table
CREATE TABLE IF NOT EXISTS item_box_assignments (
    id BIGSERIAL PRIMARY KEY,
    item_id BIGINT NOT NULL,
    box_id BIGINT NOT NULL,
    assigned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_item_assignment UNIQUE (item_id),
    CONSTRAINT fk_assignment_item 
        FOREIGN KEY (item_id) REFERENCES items(id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_assignment_box 
        FOREIGN KEY (box_id) REFERENCES boxes(id)
        ON DELETE RESTRICT ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_item_assignments_item ON item_box_assignments(item_id);
CREATE INDEX IF NOT EXISTS idx_item_assignments_box ON item_box_assignments(box_id);
CREATE INDEX IF NOT EXISTS idx_item_assignments_box_assigned ON item_box_assignments(box_id, assigned_at);


-- 1.6 Pallet-Box Assignments Table
CREATE TABLE IF NOT EXISTS pallet_box_assignments (
    id BIGSERIAL PRIMARY KEY,
    box_id BIGINT NOT NULL,
    pallet_id BIGINT NOT NULL,
    assigned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_box_pallet_assignment UNIQUE (box_id),
    CONSTRAINT fk_pallet_assignment_box 
        FOREIGN KEY (box_id) REFERENCES boxes(id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_pallet_assignment_pallet 
        FOREIGN KEY (pallet_id) REFERENCES pallets(id)
        ON DELETE RESTRICT ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_pallet_assignments_box ON pallet_box_assignments(box_id);
CREATE INDEX IF NOT EXISTS idx_pallet_assignments_pallet ON pallet_box_assignments(pallet_id);
CREATE INDEX IF NOT EXISTS idx_pallet_assignments_pallet_assigned ON pallet_box_assignments(pallet_id, assigned_at);


-- ============================================================================
-- 2. USER MANAGEMENT TABLES
-- ============================================================================

-- 2.1 Users Table
CREATE TABLE IF NOT EXISTS users (
    id BIGSERIAL PRIMARY KEY,
    username VARCHAR(100) NOT NULL,
    pin_hash VARCHAR(255) NOT NULL,
    full_name VARCHAR(255),
    email VARCHAR(255),
    phone_number VARCHAR(50),
    active BOOLEAN NOT NULL DEFAULT TRUE,
    superuser BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login TIMESTAMP,
    
    CONSTRAINT uniq_user_username UNIQUE (username),
    CONSTRAINT chk_user_username_not_empty CHECK (LENGTH(TRIM(username)) > 0),
    CONSTRAINT chk_user_username_format CHECK (username ~ '^[a-zA-Z0-9_]+$'),
    CONSTRAINT chk_user_email_format CHECK (email IS NULL OR email ~ '^[^@]+@[^@]+\.[^@]+$')
);

-- Partial unique index for email (only non-null values)
CREATE UNIQUE INDEX IF NOT EXISTS idx_users_email_unique 
    ON users(email) WHERE email IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);
CREATE INDEX IF NOT EXISTS idx_users_active ON users(active) WHERE active = TRUE;
CREATE INDEX IF NOT EXISTS idx_users_pin_hash ON users(pin_hash);


-- 2.2 Roles Table
CREATE TABLE IF NOT EXISTS roles (
    id BIGSERIAL PRIMARY KEY,
    role_name VARCHAR(100) NOT NULL,
    description TEXT,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_role_name UNIQUE (role_name),
    CONSTRAINT chk_role_name_not_empty CHECK (LENGTH(TRIM(role_name)) > 0)
);

CREATE INDEX IF NOT EXISTS idx_roles_active ON roles(active) WHERE active = TRUE;


-- 2.3 Permissions Table
CREATE TABLE IF NOT EXISTS permissions (
    id BIGSERIAL PRIMARY KEY,
    permission_name VARCHAR(100) NOT NULL,
    description TEXT,
    category VARCHAR(50),
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_permission_name UNIQUE (permission_name),
    CONSTRAINT chk_permission_name_not_empty CHECK (LENGTH(TRIM(permission_name)) > 0),
    CONSTRAINT chk_permission_name_format CHECK (permission_name ~ '^[a-z_]+\.[a-z_]+$')
);

CREATE INDEX IF NOT EXISTS idx_permissions_category ON permissions(category);
CREATE INDEX IF NOT EXISTS idx_permissions_active ON permissions(active) WHERE active = TRUE;


-- 2.4 User-Roles Junction Table
CREATE TABLE IF NOT EXISTS user_roles (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL,
    role_id BIGINT NOT NULL,
    assigned_by BIGINT,
    assigned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_user_role UNIQUE (user_id, role_id),
    CONSTRAINT fk_user_roles_user 
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_user_roles_role 
        FOREIGN KEY (role_id) REFERENCES roles(id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_user_roles_assigned_by 
        FOREIGN KEY (assigned_by) REFERENCES users(id)
        ON DELETE SET NULL ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_user_roles_user ON user_roles(user_id);
CREATE INDEX IF NOT EXISTS idx_user_roles_role ON user_roles(role_id);


-- 2.5 Role-Permissions Junction Table
CREATE TABLE IF NOT EXISTS role_permissions (
    id BIGSERIAL PRIMARY KEY,
    role_id BIGINT NOT NULL,
    permission_id BIGINT NOT NULL,
    granted BOOLEAN NOT NULL DEFAULT TRUE,
    assigned_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT uniq_role_permission UNIQUE (role_id, permission_id),
    CONSTRAINT fk_role_permissions_role 
        FOREIGN KEY (role_id) REFERENCES roles(id)
        ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_role_permissions_permission 
        FOREIGN KEY (permission_id) REFERENCES permissions(id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_role_permissions_role ON role_permissions(role_id);
CREATE INDEX IF NOT EXISTS idx_role_permissions_permission ON role_permissions(permission_id);


-- ============================================================================
-- 3. EXPORT TABLES
-- ============================================================================

-- 3.1 Export Documents Table
CREATE TABLE IF NOT EXISTS export_documents (
    id BIGSERIAL PRIMARY KEY,
    export_mode SMALLINT NOT NULL,
    lp_tin VARCHAR(50) NOT NULL,
    xml_content TEXT,
    xml_hash VARCHAR(64),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT,
    
    CONSTRAINT chk_export_mode CHECK (export_mode IN (0, 1, 2)),
    CONSTRAINT chk_xml_hash_format CHECK (xml_hash IS NULL OR xml_hash ~ '^[a-f0-9]{64}$'),
    CONSTRAINT fk_export_created_by 
        FOREIGN KEY (created_by) REFERENCES users(id)
        ON DELETE SET NULL ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_export_documents_created ON export_documents(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_export_documents_mode ON export_documents(export_mode);
CREATE INDEX IF NOT EXISTS idx_export_documents_lp_tin ON export_documents(lp_tin);


-- 3.2 Export Items Snapshot Table
CREATE TABLE IF NOT EXISTS export_items (
    id BIGSERIAL PRIMARY KEY,
    document_id BIGINT NOT NULL,
    bar_code VARCHAR(255) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT fk_export_items_document 
        FOREIGN KEY (document_id) REFERENCES export_documents(id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_export_items_document ON export_items(document_id);
CREATE INDEX IF NOT EXISTS idx_export_items_barcode ON export_items(bar_code);


-- 3.3 Export Boxes Snapshot Table
CREATE TABLE IF NOT EXISTS export_boxes (
    id BIGSERIAL PRIMARY KEY,
    document_id BIGINT NOT NULL,
    bar_code VARCHAR(255) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT fk_export_boxes_document 
        FOREIGN KEY (document_id) REFERENCES export_documents(id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_export_boxes_document ON export_boxes(document_id);
CREATE INDEX IF NOT EXISTS idx_export_boxes_barcode ON export_boxes(bar_code);


-- 3.4 Export Pallets Snapshot Table
CREATE TABLE IF NOT EXISTS export_pallets (
    id BIGSERIAL PRIMARY KEY,
    document_id BIGINT NOT NULL,
    bar_code VARCHAR(255) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT fk_export_pallets_document 
        FOREIGN KEY (document_id) REFERENCES export_documents(id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_export_pallets_document ON export_pallets(document_id);
CREATE INDEX IF NOT EXISTS idx_export_pallets_barcode ON export_pallets(bar_code);


-- ============================================================================
-- 4. AUDIT & AUTHENTICATION TABLES
-- ============================================================================

-- 4.1 Audit Log Table
CREATE TABLE IF NOT EXISTS audit_log (
    id BIGSERIAL PRIMARY KEY,
    entity_type VARCHAR(50) NOT NULL,
    entity_barcode VARCHAR(255) NOT NULL,
    action VARCHAR(50) NOT NULL,
    old_value TEXT,
    new_value TEXT,
    performed_by BIGINT NOT NULL,
    performed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    reason TEXT,
    
    CONSTRAINT chk_audit_entity_type CHECK (entity_type IN ('item', 'box', 'pallet')),
    CONSTRAINT chk_audit_action CHECK (action IN ('damaged', 'removed', 'replaced', 'unsealed', 'uncompleted')),
    CONSTRAINT fk_audit_performed_by 
        FOREIGN KEY (performed_by) REFERENCES users(id)
        ON DELETE RESTRICT ON UPDATE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_audit_entity ON audit_log(entity_type, entity_barcode);
CREATE INDEX IF NOT EXISTS idx_audit_user ON audit_log(performed_by);
CREATE INDEX IF NOT EXISTS idx_audit_date ON audit_log(performed_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_action ON audit_log(action);
CREATE INDEX IF NOT EXISTS idx_audit_entity_date ON audit_log(entity_type, entity_barcode, performed_at DESC);


-- 4.2 Authentication Rate Limiting Table
CREATE TABLE IF NOT EXISTS auth_attempts (
    id BIGSERIAL PRIMARY KEY,
    pin_hash_prefix VARCHAR(16) NOT NULL,
    attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    success BOOLEAN NOT NULL,
    ip_address INET
);

CREATE INDEX IF NOT EXISTS idx_auth_attempts_prefix_time ON auth_attempts(pin_hash_prefix, attempt_at DESC);
CREATE INDEX IF NOT EXISTS idx_auth_attempts_ip_time ON auth_attempts(ip_address, attempt_at DESC) 
    WHERE ip_address IS NOT NULL;


-- ============================================================================
-- 5. STATE MACHINE TRIGGERS
-- ============================================================================

-- 5.1 Item State Transition Trigger
CREATE OR REPLACE FUNCTION enforce_item_state_transition()
RETURNS TRIGGER AS $$
BEGIN
    -- Only allow valid transitions: 0->1, 1->2, 1->0
    IF OLD.status = 0 AND NEW.status NOT IN (0, 1) THEN
        RAISE EXCEPTION 'Invalid item state transition: Available(0) can only go to Assigned(1)';
    END IF;
    
    IF OLD.status = 1 AND NEW.status NOT IN (1, 2, 0) THEN
        RAISE EXCEPTION 'Invalid item state transition: Assigned(1) can only go to Exported(2) or Available(0)';
    END IF;
    
    IF OLD.status = 2 THEN
        RAISE EXCEPTION 'Cannot modify exported item';
    END IF;
    
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_item_state_transition ON items;
CREATE TRIGGER trg_item_state_transition
    BEFORE UPDATE OF status ON items
    FOR EACH ROW
    EXECUTE FUNCTION enforce_item_state_transition();


-- 5.2 Box State Transition Trigger
CREATE OR REPLACE FUNCTION enforce_box_state_transition()
RETURNS TRIGGER AS $$
BEGIN
    -- Only allow valid transitions: 0->1, 1->2, 1->0 (unseal)
    IF OLD.status = 0 AND NEW.status NOT IN (0, 1) THEN
        RAISE EXCEPTION 'Invalid box state transition: Empty(0) can only go to Sealed(1)';
    END IF;
    
    IF OLD.status = 1 AND NEW.status NOT IN (1, 2, 0) THEN
        RAISE EXCEPTION 'Invalid box state transition: Sealed(1) can only go to Exported(2) or Empty(0)';
    END IF;
    
    IF OLD.status = 2 THEN
        RAISE EXCEPTION 'Cannot modify exported box';
    END IF;
    
    -- Enforce sealed_at consistency
    IF NEW.status = 1 AND NEW.sealed_at IS NULL THEN
        NEW.sealed_at := CURRENT_TIMESTAMP;
    END IF;
    
    IF NEW.status = 0 THEN
        NEW.sealed_at := NULL;
    END IF;
    
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_box_state_transition ON boxes;
CREATE TRIGGER trg_box_state_transition
    BEFORE UPDATE OF status ON boxes
    FOR EACH ROW
    EXECUTE FUNCTION enforce_box_state_transition();


-- 5.3 Pallet State Transition Trigger
CREATE OR REPLACE FUNCTION enforce_pallet_state_transition()
RETURNS TRIGGER AS $$
BEGIN
    -- Only allow valid transitions: 0->1, 1->2, 1->0 (uncomplete)
    IF OLD.status = 0 AND NEW.status NOT IN (0, 1) THEN
        RAISE EXCEPTION 'Invalid pallet state transition: New(0) can only go to Complete(1)';
    END IF;
    
    IF OLD.status = 1 AND NEW.status NOT IN (1, 2, 0) THEN
        RAISE EXCEPTION 'Invalid pallet state transition: Complete(1) can only go to Exported(2) or New(0)';
    END IF;
    
    IF OLD.status = 2 THEN
        RAISE EXCEPTION 'Cannot modify exported pallet';
    END IF;
    
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_pallet_state_transition ON pallets;
CREATE TRIGGER trg_pallet_state_transition
    BEFORE UPDATE OF status ON pallets
    FOR EACH ROW
    EXECUTE FUNCTION enforce_pallet_state_transition();


-- ============================================================================
-- 6. SEED DATA
-- ============================================================================

-- 6.1 Default Production Lines
INSERT INTO production_lines (name, description, active) VALUES
    ('Line-A', 'Primary production line', TRUE),
    ('Line-B', 'Secondary production line', TRUE),
    ('Line-C', 'Quality control line', TRUE)
ON CONFLICT (name) DO NOTHING;


-- 6.2 Default Permissions (by module)
-- Import Module
INSERT INTO permissions (permission_name, description, category) VALUES
    ('import.items', 'Import items from CSV', 'import'),
    ('import.boxes', 'Import boxes from CSV', 'import'),
    ('import.pallets', 'Import pallets from CSV', 'import'),
    ('import.validate', 'Validate CSV files', 'import')
ON CONFLICT (permission_name) DO NOTHING;

-- Production Module
INSERT INTO permissions (permission_name, description, category) VALUES
    ('production.scan_item', 'Scan items', 'production'),
    ('production.assign_item', 'Assign items to boxes', 'production'),
    ('production.seal_box', 'Seal boxes', 'production'),
    ('production.assign_box', 'Assign boxes to pallets', 'production'),
    ('production.complete_pallet', 'Complete pallets', 'production'),
    ('production.view_stats', 'View production statistics', 'production'),
    ('production.manage_lines', 'Manage production lines', 'production')
ON CONFLICT (permission_name) DO NOTHING;

-- Export Module
INSERT INTO permissions (permission_name, description, category) VALUES
    ('export.create_box', 'Create box exports', 'export'),
    ('export.create_pallet', 'Create pallet exports', 'export'),
    ('export.view_documents', 'View export documents', 'export'),
    ('export.download_xml', 'Download export XML', 'export')
ON CONFLICT (permission_name) DO NOTHING;

-- Post-Production Module
INSERT INTO permissions (permission_name, description, category) VALUES
    ('postprod.mark_damaged', 'Mark items as damaged', 'postprod'),
    ('postprod.remove_item', 'Remove items from boxes', 'postprod'),
    ('postprod.replace_item', 'Replace items in boxes', 'postprod'),
    ('postprod.unseal_box', 'Unseal boxes', 'postprod'),
    ('postprod.remove_box', 'Remove boxes from pallets', 'postprod'),
    ('postprod.replace_box', 'Replace boxes on pallets', 'postprod'),
    ('postprod.uncomplete_pallet', 'Uncomplete pallets', 'postprod'),
    ('postprod.view_audit', 'View audit logs', 'postprod')
ON CONFLICT (permission_name) DO NOTHING;

-- User Management Module
INSERT INTO permissions (permission_name, description, category) VALUES
    ('user.create', 'Create users', 'user'),
    ('user.update', 'Update users', 'user'),
    ('user.delete', 'Delete users', 'user'),
    ('user.view', 'View users', 'user'),
    ('user.manage_roles', 'Manage user roles', 'user'),
    ('role.create', 'Create roles', 'role'),
    ('role.update', 'Update roles', 'role'),
    ('role.delete', 'Delete roles', 'role'),
    ('role.manage_permissions', 'Manage role permissions', 'role')
ON CONFLICT (permission_name) DO NOTHING;


-- 6.3 Default Roles
INSERT INTO roles (role_name, description, active) VALUES
    ('Administrator', 'Full system access', TRUE),
    ('Operator', 'Production operations', TRUE),
    ('Viewer', 'Read-only access', TRUE),
    ('Supervisor', 'Production and post-production access', TRUE)
ON CONFLICT (role_name) DO NOTHING;


-- 6.4 Assign Permissions to Roles
-- Administrator gets all permissions
INSERT INTO role_permissions (role_id, permission_id, granted)
SELECT r.id, p.id, TRUE
FROM roles r, permissions p
WHERE r.role_name = 'Administrator'
ON CONFLICT (role_id, permission_id) DO NOTHING;

-- Operator gets production permissions
INSERT INTO role_permissions (role_id, permission_id, granted)
SELECT r.id, p.id, TRUE
FROM roles r, permissions p
WHERE r.role_name = 'Operator'
  AND p.category IN ('production', 'import')
ON CONFLICT (role_id, permission_id) DO NOTHING;

-- Viewer gets view permissions only
INSERT INTO role_permissions (role_id, permission_id, granted)
SELECT r.id, p.id, TRUE
FROM roles r, permissions p
WHERE r.role_name = 'Viewer'
  AND p.permission_name IN ('production.view_stats', 'export.view_documents', 'postprod.view_audit', 'user.view')
ON CONFLICT (role_id, permission_id) DO NOTHING;

-- Supervisor gets production + post-production + export
INSERT INTO role_permissions (role_id, permission_id, granted)
SELECT r.id, p.id, TRUE
FROM roles r, permissions p
WHERE r.role_name = 'Supervisor'
  AND p.category IN ('production', 'postprod', 'export', 'import')
ON CONFLICT (role_id, permission_id) DO NOTHING;


-- 6.5 Default Admin User
-- PIN hash is SHA256 of '0000' - in production use proper hashing!
-- SHA256('0000') = 9af15b336e6a9619928537df30b2e6a2376569fcf9d7e773eccede65606529a0
INSERT INTO users (username, pin_hash, full_name, email, active, superuser) VALUES
    ('admin', '9af15b336e6a9619928537df30b2e6a2376569fcf9d7e773eccede65606529a0', 
     'System Administrator', 'admin@system.local', TRUE, TRUE)
ON CONFLICT (username) DO NOTHING;

-- Assign Administrator role to admin user
INSERT INTO user_roles (user_id, role_id)
SELECT u.id, r.id
FROM users u, roles r
WHERE u.username = 'admin' AND r.role_name = 'Administrator'
ON CONFLICT (user_id, role_id) DO NOTHING;


COMMIT;

-- ============================================================================
-- SCHEMA INFO
-- ============================================================================
DO $$
DECLARE
    table_count INTEGER;
    index_count INTEGER;
    trigger_count INTEGER;
BEGIN
    SELECT COUNT(*) INTO table_count FROM information_schema.tables 
        WHERE table_schema = 'public' AND table_type = 'BASE TABLE';
    SELECT COUNT(*) INTO index_count FROM pg_indexes WHERE schemaname = 'public';
    SELECT COUNT(*) INTO trigger_count FROM information_schema.triggers 
        WHERE trigger_schema = 'public';
    
    RAISE NOTICE '====================================================';
    RAISE NOTICE 'Schema initialized successfully!';
    RAISE NOTICE 'Tables: %, Indexes: %, Triggers: %', table_count, index_count, trigger_count;
    RAISE NOTICE '====================================================';
END $$;

-- ============================================================================
-- DB Info and Creds 
-- Host: localhost
-- Port: 5432
-- Database: prod_auto_dev
-- Username: prod_auto_dev
-- Password: prod_auto_dev
-- ============================================================================

