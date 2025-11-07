#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [--bind 0.0.0.0] [--port 8080] [--threads 8] [--cache_capacity 10000] [--db_conn \"...\" ] [--db_pool 4]\n",
        p);
}

int main(int argc, char **argv) {
    const char *bind_addr = "0.0.0.0";
    int port = 8080;
    int threads = 8;
    int cache_capacity = 10000;
    const char *db_conninfo = "host=127.0.0.1 port=5432 user=kvuser password=kvpass dbname=kvdb";
    int db_pool = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) { bind_addr = argv[++i]; }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) { port = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) { threads = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--cache_capacity") == 0 && i + 1 < argc) { cache_capacity = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--db_conn") == 0 && i + 1 < argc) { db_conninfo = argv[++i]; }
        else if (strcmp(argv[i], "--db_pool") == 0 && i + 1 < argc) { db_pool = atoi(argv[++i]); }
        else { usage(argv[0]); return 1; }
    }

    /*printf("Starting KV server on %s:%d (threads=%d, cache=%d, db_pool=%d)\n",
           bind_addr, port, threads, cache_capacity, db_pool);
    
   

    if (start_http_server(bind_addr, port, threads, cache_capacity, db_conninfo, db_pool) != 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    // The civetweb server runs until killed. This process just waits here.
    printf("Press Enter to stop server...\n");
    getchar();
    stop_http_server();
    return 0;
    */

    fprintf(stderr, "Starting KV server on %s:%d (threads=%d, cache=%d, db_pool=%d)\n",
           bind_addr, port, threads, cache_capacity, db_pool);

    if (start_http_server(bind_addr, port, threads, cache_capacity, db_conninfo, db_pool) != 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    /* The civetweb server runs until killed. This process just waits here. */
    fprintf(stderr, "Press Enter to stop server...\n");
    getchar();
    stop_http_server();
    return 0;

}
