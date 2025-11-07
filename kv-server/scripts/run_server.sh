#!/usr/bin/env bash
# Usage: ./run_server.sh [cpu-list]
# Example: ./run_server.sh 0-3

CPU=${1:-0-3}
echo "Running server pinned to CPU(s): $CPU"

# Example DB connection string:
DB_CONN="host=127.0.0.1 port=5432 user=kvuser password=kvpass dbname=kvdb"

taskset -c $CPU ./kv_server --port 8080 --threads 8 --cache_capacity 10000 --db_conn "$DB_CONN" --db_pool 4
