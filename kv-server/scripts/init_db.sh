#!/usr/bin/env bash
set -euo pipefail

# init_db.sh
# Creates role kvuser (if needed), creates database kvdb owned by kvuser (if needed),
# and creates the kv_store table as kvuser (so kvuser is the owner).
#
# Run as a normal user; this script uses `sudo -u postgres` to perform privileged DB ops.

ROLE="kvuser"
PW="kvpass"
DB="kvdb"
SCHEMA_SQL="sql/init_db.sql"

echo "=== Initializing PostgreSQL KV DB ==="

# 1) create role if not exists
echo -n "Creating role ${ROLE} (if not exists)... "
sudo -u postgres psql -v ON_ERROR_STOP=1 -tAc "SELECT 1 FROM pg_roles WHERE rolname='${ROLE}'" | grep -q 1 || sudo -u postgres psql -v ON_ERROR_STOP=1 -c "CREATE ROLE ${ROLE} WITH LOGIN PASSWORD '${PW}';"
echo "done."

# 2) create database if not exists, owned by kvuser
echo -n "Creating database ${DB} owned by ${ROLE} (if not exists)... "
sudo -u postgres psql -v ON_ERROR_STOP=1 -tAc "SELECT 1 FROM pg_database WHERE datname='${DB}'" | grep -q 1 || sudo -u postgres psql -v ON_ERROR_STOP=1 -c "CREATE DATABASE ${DB} OWNER ${ROLE};"
echo "done."

# 3) create table as kvuser (so owner is kvuser).
#    We do this by connecting as postgres but doing SET ROLE to kvuser before running DDL.
echo -n "Creating table kv_store as ${ROLE} (if not exists)... "
if [ ! -f "${SCHEMA_SQL}" ]; then
  echo
  echo "ERROR: schema file not found: ${SCHEMA_SQL}"
  exit 1
fi

# Use psql to run the DDL while set to role kvuser; this makes kvuser the owner of objects created.
sudo -u postgres psql -v ON_ERROR_STOP=1 -d "${DB}" -c "SET ROLE ${ROLE}; $(sed -e ':a;N;$!ba;s/[\n\r]/ /g' "${SCHEMA_SQL}")"
echo "done."

# 4) ensure kvuser has privileges just in case
echo -n "Granting privileges on table kv_store to ${ROLE}... "
sudo -u postgres psql -v ON_ERROR_STOP=1 -d "${DB}" -c "GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE public.kv_store TO ${ROLE};"
echo "done."

echo "=== Initialization complete ==="
echo "Role: ${ROLE}"
echo "DB:   ${DB}"
echo "Table: public.kv_store (owned by ${ROLE})"
