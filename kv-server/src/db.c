#define _GNU_SOURCE
#include "db.h"
#include <libpq-fe.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    PGconn *conn;
    pthread_mutex_t mu; /* protect this connection during use */
} dbconn_t;

static dbconn_t *pool = NULL;
static int pool_size = 0;
static unsigned int rr_idx = 0; /* round-robin index */

int db_init(const char *conninfo, int pool_s) {
    if (pool) return 0;
    pool = calloc(pool_s, sizeof(dbconn_t));
    if (!pool) return -1;
    pool_size = pool_s;
    for (int i = 0; i < pool_size; ++i) {
        pool[i].conn = PQconnectdb(conninfo);
        if (PQstatus(pool[i].conn) != CONNECTION_OK) {
            fprintf(stderr, "DB connection %d failed: %s\n", i, PQerrorMessage(pool[i].conn));
            /* cleanup previous connections */
            for (int j = 0; j < i; ++j) {
                PQfinish(pool[j].conn);
                pthread_mutex_destroy(&pool[j].mu);
            }
            free(pool);
            pool = NULL;
            pool_size = 0;
            return -1;
        }
        pthread_mutex_init(&pool[i].mu, NULL);
        fprintf(stderr, "db_init: connection %d OK\n", i);
    }
    return 0;
}

void db_shutdown(void) {
    if (!pool) return;
    for (int i = 0; i < pool_size; ++i) {
        PQfinish(pool[i].conn);
        pthread_mutex_destroy(&pool[i].mu);
    }
    free(pool);
    pool = NULL;
    pool_size = 0;
}

/* Acquire a connection from the pool (locked) */
static dbconn_t *acquire_conn(void) {
    if (!pool) return NULL;
    unsigned int idx = __sync_fetch_and_add(&rr_idx, 1) % pool_size;
    pthread_mutex_lock(&pool[idx].mu);
    return &pool[idx];
}

static void release_conn(dbconn_t *c) {
    pthread_mutex_unlock(&c->mu);
}

/* db_get: returns 0 on success and sets *value_out (malloc'd) and *value_len, -1 on not found/error */
int db_get(const char *key, char **value_out, int *value_len) {
    if (!pool) {
        fprintf(stderr, "db_get: pool not initialized\n");
        return -1;
    }
    dbconn_t *c = acquire_conn();
    if (!c) {
        fprintf(stderr, "db_get: acquire_conn failed\n");
        return -1;
    }

    const char *paramValues[1] = { key };
    PGresult *res = PQexecParams(c->conn,
                                "SELECT value FROM kv_store WHERE key = $1",
                                1,       /* nParams */
                                NULL,    /* paramTypes (text) */
                                paramValues,
                                NULL,    /* paramLengths */
                                NULL,    /* paramFormats (text) */
                                0);      /* resultFormat: text */

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        /* Not necessarily fatal; print error for debugging */
        fprintf(stderr, "db_get query failed for key='%s': %s\n", key, PQerrorMessage(c->conn));
        PQclear(res);
        release_conn(c);
        return -1;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        release_conn(c);
        fprintf(stderr, "db_get: key='%s' not found\n", key);
        return -1; /* not found */
    }

    /* Get first row, first column */
    int len = PQgetlength(res, 0, 0);
    const char *data = PQgetvalue(res, 0, 0);
    /* allocate and copy (ensure null-terminated) */
    *value_out = malloc(len + 1);
    if (!*value_out) {
        PQclear(res);
        release_conn(c);
        fprintf(stderr, "db_get: malloc failed\n");
        return -1;
    }
    memcpy(*value_out, data, len);
    (*value_out)[len] = '\0';
    if (value_len) *value_len = len;

    PQclear(res);
    release_conn(c);
    fprintf(stderr, "db_get: OK key='%s' len=%d\n", key, len);
    return 0;
}

/* db_put: insert or update value. value_len is number of bytes. Returns 0 on success. */
int db_put(const char *key, const char *value, int value_len) {
    if (!pool) {
        fprintf(stderr, "db_put: pool not initialized\n");
        return -1;
    }
    dbconn_t *c = acquire_conn();
    if (!c) {
        fprintf(stderr, "db_put: acquire_conn failed\n");
        return -1;
    }

    const char *paramValues[2] = { key, value };

    PGresult *res = PQexecParams(c->conn,
                                 "INSERT INTO kv_store(key, value) VALUES($1, $2) ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
                                 2,
                                 NULL,  /* paramTypes */
                                 paramValues,
                                 NULL,  /* paramLengths */
                                 NULL,  /* paramFormats (text) */
                                 0);    /* resultFormat (text) */

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db_put failed for key='%s': %s\n", key, PQerrorMessage(c->conn));
        PQclear(res);
        release_conn(c);
        return -1;
    }

    PQclear(res);
    release_conn(c);
    fprintf(stderr, "db_put: OK key='%s'\n", key);
    return 0;
}

/* db_delete: delete a key */
int db_delete(const char *key) {
    if (!pool) {
        fprintf(stderr, "db_delete: pool not initialized\n");
        return -1;
    }
    dbconn_t *c = acquire_conn();
    if (!c) {
        fprintf(stderr, "db_delete: acquire_conn failed\n");
        return -1;
    }

    const char *paramValues[1] = { key };
    PGresult *res = PQexecParams(c->conn,
                                 "DELETE FROM kv_store WHERE key = $1",
                                 1,
                                 NULL,
                                 paramValues,
                                 NULL,
                                 NULL,
                                 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db_delete failed for key='%s': %s\n", key, PQerrorMessage(c->conn));
        PQclear(res);
        release_conn(c);
        return -1;
    }
    PQclear(res);
    release_conn(c);
    fprintf(stderr, "db_delete: OK key='%s'\n", key);
    return 0;
}
