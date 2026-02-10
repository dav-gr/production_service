-- ============================================================================
-- Migration: 003_add_event_subscription.sql
-- Production Automation System - Pub/Sub Event Subscription
-- ============================================================================
-- Version: 1.0
-- Created: 2026-02-10
-- Description: Adds event_log, subscribers tables, soft-delete columns,
--              and triggers for real-time event notification.
-- ============================================================================

-- Run this with: psql -U postgres -d prod_auto_dev -f migrations/003_add_event_subscription.sql

BEGIN;

-- ============================================================================
-- 1. SOFT-DELETE COLUMNS
-- ============================================================================

-- Add is_deleted column to items
ALTER TABLE items ADD COLUMN IF NOT EXISTS is_deleted BOOLEAN NOT NULL DEFAULT FALSE;

-- Add is_deleted column to boxes
ALTER TABLE boxes ADD COLUMN IF NOT EXISTS is_deleted BOOLEAN NOT NULL DEFAULT FALSE;

-- Partial indexes for non-deleted records (improves query performance)
CREATE INDEX IF NOT EXISTS idx_items_not_deleted 
    ON items(production_line, status) WHERE NOT is_deleted;

CREATE INDEX IF NOT EXISTS idx_boxes_not_deleted 
    ON boxes(production_line, status) WHERE NOT is_deleted;

-- ============================================================================
-- 2. EVENT LOG TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS event_log (
    id          BIGSERIAL PRIMARY KEY,
    table_name  TEXT NOT NULL,
    event_type  TEXT NOT NULL,
    row_id      BIGINT,
    payload     JSONB DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Index for efficient resume queries (fetch events > last_event_id)
CREATE INDEX IF NOT EXISTS idx_event_log_id ON event_log(id);

-- Index for filtering by table (optional, for debugging/monitoring)
CREATE INDEX IF NOT EXISTS idx_event_log_table ON event_log(table_name);

-- Index for cleanup queries
CREATE INDEX IF NOT EXISTS idx_event_log_created ON event_log(created_at);

-- ============================================================================
-- 3. SUBSCRIBERS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS subscribers (
    id                SERIAL PRIMARY KEY,
    client_id         TEXT UNIQUE NOT NULL,
    connection_mode   TEXT NOT NULL,          -- 'permanent' | 'one_shot'
    subscription_mode TEXT NOT NULL,          -- 'full' | 'notify_only'
    callback_host     TEXT,                   -- ip:port for one_shot mode
    last_event_id     BIGINT DEFAULT 0 NOT NULL,
    updated_at        TIMESTAMPTZ DEFAULT now(),
    
    CONSTRAINT chk_connection_mode CHECK (connection_mode IN ('permanent', 'one_shot')),
    CONSTRAINT chk_subscription_mode CHECK (subscription_mode IN ('full', 'notify_only')),
    CONSTRAINT chk_callback_host_required CHECK (
        connection_mode != 'one_shot' OR callback_host IS NOT NULL
    )
);

CREATE INDEX IF NOT EXISTS idx_subscribers_client ON subscribers(client_id);

-- ============================================================================
-- 4. TRIGGER FUNCTION
-- ============================================================================
-- Logs INSERT, DELETE, UPDATE (soft-delete and status rollback) to event_log.
-- Skipped during bulk imports when app.bulk_import = 'on'.

CREATE OR REPLACE FUNCTION log_items_boxes_events()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    bulk text;
    p    jsonb;
BEGIN
    -- Check if bulk import is active (session-scoped setting)
    bulk := COALESCE(current_setting('app.bulk_import', true), 'off');
    IF bulk = 'on' THEN
        RETURN COALESCE(NEW, OLD);
    END IF;

    IF TG_OP = 'INSERT' THEN
        p := jsonb_build_object(
            'id', NEW.id,
            'bar_code', NEW.bar_code,
            'status', NEW.status,
            'production_line', NEW.production_line,
            'imported_at', NEW.imported_at
        );
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'insert', NEW.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'DELETE' THEN
        p := jsonb_build_object(
            'id', OLD.id,
            'bar_code', OLD.bar_code
        );
        INSERT INTO event_log(table_name, event_type, row_id, payload)
            VALUES (TG_TABLE_NAME, 'delete', OLD.id, p);
        PERFORM pg_notify('db_events', '');

    ELSIF TG_OP = 'UPDATE' THEN
        -- Priority 1: Soft-delete (is_deleted toggled FALSE ? TRUE)
        IF NOT OLD.is_deleted AND NEW.is_deleted THEN
            p := jsonb_build_object(
                'id', NEW.id,
                'bar_code', NEW.bar_code,
                'status', NEW.status,
                'production_line', NEW.production_line
            );
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'marked_as_deleted', NEW.id, p);
            PERFORM pg_notify('db_events', '');

        -- Priority 2: Status rollback (1?0 or 2?0)
        ELSIF OLD.status IN (1, 2) AND NEW.status = 0 THEN
            p := jsonb_build_object(
                'id', NEW.id,
                'bar_code', NEW.bar_code,
                'old_status', OLD.status,
                'new_status', NEW.status,
                'production_line', NEW.production_line
            );
            INSERT INTO event_log(table_name, event_type, row_id, payload)
                VALUES (TG_TABLE_NAME, 'status_to_0', NEW.id, p);
            PERFORM pg_notify('db_events', '');
        END IF;
    END IF;

    RETURN COALESCE(NEW, OLD);
END;
$$;

-- ============================================================================
-- 5. ATTACH TRIGGERS TO ITEMS AND BOXES
-- ============================================================================

-- Drop existing triggers if they exist (idempotent migration)
DROP TRIGGER IF EXISTS trg_items_event ON items;
DROP TRIGGER IF EXISTS trg_boxes_event ON boxes;

-- Create triggers
CREATE TRIGGER trg_items_event
    AFTER INSERT OR UPDATE OR DELETE ON items
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();

CREATE TRIGGER trg_boxes_event
    AFTER INSERT OR UPDATE OR DELETE ON boxes
    FOR EACH ROW EXECUTE FUNCTION log_items_boxes_events();

-- ============================================================================
-- 6. HELPER FUNCTION FOR CLEANUP
-- ============================================================================

CREATE OR REPLACE FUNCTION cleanup_old_events(retention_days INT DEFAULT 7)
RETURNS INT LANGUAGE plpgsql AS $$
DECLARE
    deleted_count INT;
BEGIN
    DELETE FROM event_log
    WHERE id < (SELECT COALESCE(MIN(last_event_id), 0) FROM subscribers)
      AND created_at < now() - (retention_days || ' days')::interval;
    
    GET DIAGNOSTICS deleted_count = ROW_COUNT;
    RETURN deleted_count;
END;
$$;

COMMIT;

-- ============================================================================
-- VERIFICATION QUERIES (run manually after migration)
-- ============================================================================
-- SELECT column_name, data_type FROM information_schema.columns 
--     WHERE table_name = 'items' AND column_name = 'is_deleted';
-- SELECT column_name, data_type FROM information_schema.columns 
--     WHERE table_name = 'boxes' AND column_name = 'is_deleted';
-- SELECT * FROM information_schema.tables WHERE table_name IN ('event_log', 'subscribers');
-- SELECT trigger_name, event_object_table FROM information_schema.triggers 
--     WHERE trigger_name LIKE 'trg_%_event';
