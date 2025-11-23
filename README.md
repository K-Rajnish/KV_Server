# KV-Server Performance Analysis
**Course:** DECS (CS744)

---

## Project Overview
This project implements a high-performance **HTTP-based Key-Value (KV) Server** in **C**, featuring:
- **In-memory caching** for fast access (LRU replacement)
- **Persistent storage** using PostgreSQL
- **RESTful API** supporting **GET, POST, DELETE**
- **Configurable multi-threaded load generator**
- **Pthreads-based server concurrency**
- **CPU core pinning (taskset)** for client-server resource isolation

---

## System Architecture Overview

### **Server**
- Uses **CivetWeb HTTP server library**
- **In-line memory cache** with dynamic size & **LRU eviction**
- **PostgreSQL database** for persistent durability
- **Multithreaded request handling**
- Exposes **HTTP port 8080**
- Supports the following REST endpoints:

### **Load Generator**
-Implemented in C using libcurl library for HTTP communication
- Supports multiple workloads types:
Put All: Only create/delete operations → Disk-bound (DB intensive)
Get All: Unique GETs → Disk-bound (no cache reuse)
Get Popular: Repeated GETs for a small set of keys → Cache-bound (CPU/memory intensive)
Mixed Workload: Combination of GET, POST, DELETE with configurable ratios

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

#### **Create a Key-Value Pair**
```bash
curl -i -X POST -H "Content-Type: application/json" \
-d '{"key":"key_name","value":"Value"}' \
http://127.0.0.1:8080/kv

