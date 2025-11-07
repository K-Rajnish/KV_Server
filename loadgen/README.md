###########################################
Build the loadgen.c:
 gcc -O2 -g -Wall -Wextra -std=gnu11 -pthread -o loadgen src/loadgen.c -lcurl
 
To run:
taskset -c 4-7 ./loadgen --target http://127.0.0.1:8080 --duration 20 --threads 4 --rate 200 --keyspace 1000 --value-size 64 --csv-out out.csv


Workloads:

PUTALL (DB bound):
./loadgen --workload putall --threads 8 --duration 30 --keyspace 20000 --value-size 128

GETALL(DB bound, seed db first:->ensures DB has keys to read)
./loadgen --workload getall --seed --threads 8 --duration 30 --keyspace 20000

GETPOPULAR (cache-hit heavy):
./loadgen --workload getpopular --hotset-size 10 --seed --threads 16 --duration 30

MIX with explicit ratio:
./loadgen --workload mix --mix-ratio 80:15:5 --threads 8 --duration 30 --keyspace 10000
