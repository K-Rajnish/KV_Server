# KV-Server Performance Analysis
**Author:** Kumar Rajnish  
**ID:** 25M2123  
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

#### **Create a Key-Value Pair**
```bash
curl -i -X POST -H "Content-Type: application/json" \
-d '{"key":"key_name","value":"Value"}' \
http://127.0.0.1:8080/kv

