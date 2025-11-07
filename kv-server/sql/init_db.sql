-- sql/init_db.sql
-- This file contains the schema for the kv_store table only.
-- The scripts/init_db.sh will ensure it's created as kvuser.

CREATE SCHEMA IF NOT EXISTS public;

CREATE TABLE IF NOT EXISTS public.kv_store (
    key TEXT PRIMARY KEY,
    value BYTEA,
    created_at TIMESTAMP DEFAULT now()
);
