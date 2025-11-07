#define _GNU_SOURCE
#include "http.h"
#include "cache.h"
#include "db.h"
#include <civetweb.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/* build listen string "addr:port" */
static void build_listening_ports(char *buf, size_t buflen, const char *addr, int port) {
    snprintf(buf, buflen, "%s:%d", addr, port);
}

/* simple url-decode */
static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && (src[1] && src[2]) && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* parse application/x-www-form-urlencoded body into key/value. Returns 0 on success. */
static int parse_form_kv(const char *body, char **key_out, char **value_out) {
    if (!body) return -1;
    const char *eq = strchr(body, '=');
    if (!eq) return -1;
    size_t klen = eq - body;
    char *k = malloc(klen + 1);
    if (!k) return -1;
    strncpy(k, body, klen);
    k[klen] = '\0';
    char *v = strdup(eq + 1);
    if (!v) { free(k); return -1; }
    char *kd = malloc(klen + 1);
    if (!kd) { free(k); free(v); return -1; }
    url_decode(kd, k);
    char *vd = malloc(strlen(v) + 1);
    if (!vd) { free(k); free(v); free(kd); return -1; }
    url_decode(vd, v);
    free(k);
    free(v);
    *key_out = kd;
    *value_out = vd;
    return 0;
}

/* POST /kv  - accept form or small JSON {"key":"k","value":"v"} */
static int handle_post_kv(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    /* only handle POST here; return 0 to allow other handlers (e.g. /kv/) to run */
    if (strcmp(ri->request_method, "POST") != 0) {
        return 0; /* not handled here */
    }

    int content_len = (int)ri->content_length;
    if (content_len <= 0 || content_len > 10 * 1024 * 1024) {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nBad content length\n");
        return 1;
    }

    char *body = malloc(content_len + 1);
    if (!body) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nMemory error\n");
        return 1;
    }
    int read = mg_read(conn, body, content_len);
    if (read <= 0) {
        free(body);
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nFailed read body\n");
        return 1;
    }
    body[read] = '\0';

    char *key = NULL, *value = NULL;

    const char *ct = mg_get_header(conn, "Content-Type");
    if (!ct) ct = "";

    if (strstr(ct, "application/x-www-form-urlencoded")) {
        if (parse_form_kv(body, &key, &value) != 0) {
            free(body);
            mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nInvalid form\n");
            return 1;
        }
    } else {
        /* naive JSON parse */
        char *kpos = strstr(body, "\"key\"");
        char *vpos = strstr(body, "\"value\"");
        if (kpos) {
            char *colon = strchr(kpos, ':');
            if (colon) {
                char *start = strchr(colon, '\"');
                if (start) {
                    start++;
                    char *end = strchr(start, '\"');
                    if (end) {
                        size_t len = end - start;
                        key = malloc(len + 1);
                        strncpy(key, start, len); key[len] = '\0';
                    }
                }
            }
        }
        if (vpos) {
            char *colon = strchr(vpos, ':');
            if (colon) {
                char *start = strchr(colon, '\"');
                if (start) {
                    start++;
                    char *end = strchr(start, '\"');
                    if (end) {
                        size_t len = end - start;
                        value = malloc(len + 1);
                        strncpy(value, start, len); value[len] = '\0';
                    }
                }
            }
        }
    }

    if (!key || !value) {
        free(body);
        if (key) free(key);
        if (value) free(value);
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing key/value\n");
        return 1;
    }

    /* persist to DB first */
    if (db_put(key, value, (int)strlen(value)) != 0) {
        fprintf(stderr, "handle_post_kv: db_put failed for key='%s'\n", key);
        free(body);
        free(key);
        free(value);
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nDB error\n");
        return 1;
    }
    fprintf(stderr, "handle_post_kv: db_put OK for key='%s'\n", key);

    cache_put(key, value);

    mg_printf(conn, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n{\"status\":\"ok\"}\n");

    free(body);
    free(key);
    free(value);
    return 1;
}

static int handle_get_kv(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;
    const char *prefix = "/kv/";
    char *key = NULL;

    if (strncmp(uri, prefix, strlen(prefix)) == 0) {
        const char *kstart = uri + strlen(prefix);
        /* decode percent-encoding in the path component to get the exact key */
        char tmp[4096];
        const char *qstart = kstart;
        char *qmark = strchr(qstart, '?');
        size_t take_len = qmark ? (size_t)(qmark - qstart) : strlen(qstart);
        if (take_len >= sizeof(tmp)) take_len = sizeof(tmp) - 1;
        memcpy(tmp, qstart, take_len);
        tmp[take_len] = '\0';
        /* decode percent-encoding into heap-allocated string */
        char *decoded = malloc(take_len + 1);
        if (!decoded) {
            mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nMemory error\n");
            return 1;
        }
        url_decode(decoded, tmp);
        key = decoded;
    } else if (ri->query_string) {
        const char *q = strstr(ri->query_string, "key=");
        if (q) {
            q += 4;
            /* handle possible additional params after key=... */
            const char *amp = strchr(q, '&');
            size_t len = amp ? (size_t)(amp - q) : strlen(q);
            char *tmpq = malloc(len + 1);
            if (!tmpq) {
                mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nMemory error\n");
                return 1;
            }
            memcpy(tmpq, q, len);
            tmpq[len] = '\0';
            char *decoded = malloc(len + 1);
            if (!decoded) { free(tmpq); mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nMemory error\n"); return 1; }
            url_decode(decoded, tmpq);
            free(tmpq);
            key = decoded;
        }
    }

    if (!key) {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing key\n");
        return 1;
    }

    fprintf(stderr, "handle_get_kv: looking up key='%s'\n", key);

    char *val = cache_get(key);
    if (val) {
        fprintf(stderr, "handle_get_kv: cache HIT for key='%s'\n", key);
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"key\":\"%s\",\"value(Cache)\":\"%s\"}\n", key, val);
        free(val);
        free(key);
        return 1;
    } else {
        fprintf(stderr, "handle_get_kv: cache MISS for key='%s'\n", key);
    }

    char *dbval = NULL;
    int vlen = 0;
    if (db_get(key, &dbval, &vlen) == 0) {
        fprintf(stderr, "handle_get_kv: db_get OK for key='%s' len=%d\n", key, vlen);
        cache_put(key, dbval);
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"key\":\"%s\",\"value(DB)\":\"%s\"}\n", key, dbval);
        free(dbval);
        free(key);
        return 1;
    } else {
        fprintf(stderr, "handle_get_kv: db_get NOTFOUND/ERROR for key='%s'\n", key);
        mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nError 404: Not Found\nNot Found\n");
        free(key);
        return 1;
    }
}

static int handle_delete_kv(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;
    const char *prefix = "/kv/";
    char *key = NULL;
    if (strncmp(uri, prefix, strlen(prefix)) == 0) {
        key = strdup(uri + strlen(prefix));
        char *q = strchr(key, '?'); if (q) *q = '\0';
    } else {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing key in URI\n");
        return 1;
    }
    if (!key) {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing key\n");
        return 1;
    }

    int rc_db = db_delete(key);
    (void)cache_delete(key);

    if (rc_db == 0) {
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"deleted\"}\n");
    } else {
        mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n\r\n{\"error\":\"not found\"}\n");
    }
    free(key);
    return 1;
}

/* GET /metrics returns simple JSON stats */
static int handle_metrics(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    unsigned long hits=0, misses=0, items=0;
    cache_stats(&hits, &misses, &items);
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"cache_hits\":%lu,\"cache_misses\":%lu,\"cache_items\":%lu}\n",
              hits, misses, items);
    return 1;
}

/* unified dispatcher for /kv and /kv/ prefixes */
static int kv_dispatch(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *method = ri->request_method;
    const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;

    /* POST to /kv or /kv/ -> create/update */
    if (strcmp(method, "POST") == 0) {
        /* accept POST on exactly /kv or /kv/ */
        if (strcmp(uri, "/kv") == 0 || strcmp(uri, "/kv/") == 0) {
            return handle_post_kv(conn, cbdata);
        }
        /* otherwise POST to /kv/<key> is not allowed */
        mg_printf(conn, "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\n\r\nPOST not allowed on this path\n");
        return 1;
    }

    /* GET /kv/<key> */
    if (strcmp(method, "GET") == 0) {
        /* let handle_get_kv expect /kv/<key> */
        return handle_get_kv(conn, cbdata);
    }

    /* DELETE /kv/<key> */
    if (strcmp(method, "DELETE") == 0) {
        return handle_delete_kv(conn, cbdata);
    }

    mg_printf(conn, "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\n\r\nMethod not allowed\n");
    return 1;
}


/* global context for civetweb */
static struct mg_context *ctx = NULL;

int start_http_server(const char *bind_addr, int port, int num_threads, int cache_capacity,
                      const char *db_conninfo, int db_pool_size)
{
    (void)num_threads;
    char ports[64];
    build_listening_ports(ports, sizeof(ports), bind_addr, port);

    /* expose civetweb error log and useful options */
    char thread_str[16];
    snprintf(thread_str, sizeof(thread_str), "%d", num_threads);

    const char *options[] = {
        "listening_ports", ports,
        "num_threads", thread_str,
        "error_log_file", "/tmp/civet_error.log",
        NULL
    };


    printf("[server] civet options:\n");
    for (int i = 0; options[i]; i += 2) {
        printf("  %s = %s\n", options[i], options[i+1]);
    }

    /* initialize cache first */
    if (cache_init((size_t)cache_capacity) != 0) {
        fprintf(stderr, "cache_init failed\n");
        return -1;
    }

    /* Start civetweb FIRST so we can isolate Civet errors from DB errors */
    static struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    ctx = mg_start(&callbacks, NULL, options);
    if (!ctx) {
        int err = errno;
        fprintf(stderr, "mg_start failed: errno=%d (%s). Check /tmp/civet_error.log for details\n", err, strerror(err));
        cache_free();
        return -1;
    }

    printf("HTTP server started on %s\n", ports);

    /* Now initialize DB; if it fails, log error but keep server running */
    if (db_init(db_conninfo, db_pool_size) != 0) {
        fprintf(stderr, "Warning: db_init failed — server is running but DB unavailable. Check DB settings/logs.\n");
    } else {
        printf("DB pool initialized (size=%d)\n", db_pool_size);
    }

    mg_set_request_handler(ctx, "/kv", kv_dispatch, NULL);
    mg_set_request_handler(ctx, "/kv/", kv_dispatch, NULL);
    mg_set_request_handler(ctx, "/metrics", handle_metrics, NULL);

    return 0;
}

void stop_http_server(void) {
    if (ctx) {
        mg_stop(ctx);
        ctx = NULL;
    }
    db_shutdown();
    cache_free();
}
