######################################################
Build:
make clean && make

to run:
taskset -c 0-3 ./kv_server --port 8080 --threads 8 --cache_capacity 10000   --db_conn "host=127.0.0.1 port=5432 user=kvuser password=kvpass dbname=kvdb" --db_pool 4   2>&1 | tee server_run.log


To Post the key value pair to the database
curl -i -X POST -H "Content-Type: application/json" -d '{"key":"foo","value":"bar"}' http://127.0.0.1:8080/kv

To Access/Get a key value pair from the database
curl -i http://127.0.0.1:8080/kv/foo

To Delete a key value pair from the database
curl -i -X DELETE http://127.0.0.1:8080/kv/foo

with time stats:
curl -w " <-- time: %{time_total}s\n" "curl -i http://127.0.0.1:8080/kv/foo"

