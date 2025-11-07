/*
 * loadgen.c - closed-loop load generator for KV server
 *
 * Workloads:
 *   - putall     : DB-heavy alternating POST/DELETE on many keys
 *   - getall     : DB-heavy unique GETs (use --seed to populate DB)
 *   - mix        : mix of GET/POST/DELETE (use --mix-ratio or --read-pct/--write-pct/--delete-pct)
 *   - getpopular : small hot set repeatedly accessed by all clients (cache-hit heavy)
 *
 * Closed-loop: each thread sends request -> waits for response -> sends next.
 *
 * Build:
 *   gcc -O2 -g -Wall -Wextra -std=gnu11 -pthread -o loadgen src/loadgen.c -lcurl
 *
 * Example:
 *   ./loadgen --workload getpopular --hotset-size 10 --threads 8 --duration 30 --seed
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <curl/curl.h>
#include <errno.h>

#define DEFAULT_TARGET "http://127.0.0.1:8080"
#define MAX_URL_LEN 4096
#define MAX_KEY_LEN 256
#define MAX_VALUE_LEN 4096

typedef enum { WL_PUTALL = 0, WL_GETALL = 1, WL_MIX = 2, WL_GETPOPULAR = 3 } workload_t;

typedef struct {
    char target[MAX_URL_LEN];
    int duration;
    int threads;
    int keyspace;
    int value_size;
    int read_pct;
    int write_pct;
    int delete_pct;
    char csv_out[512];
    workload_t workload;
    int seed_db;
    int hotset_size; /* for getpopular */
} cfg_t;

static cfg_t cfg = {
    .target = DEFAULT_TARGET,
    .duration = 30,
    .threads = 4,
    .keyspace = 1000,
    .value_size = 100,
    .read_pct = 80,
    .write_pct = 15,
    .delete_pct = 5,
    .csv_out = "",
    .workload = WL_MIX,
    .seed_db = 0,
    .hotset_size = 10
};

/* global counters */
static atomic_ulong total_reqs = 0;
static atomic_ulong total_success = 0;
static atomic_ulong total_fail = 0;
static atomic_ulong total_get = 0;
static atomic_ulong total_get_ok = 0;
static atomic_ulong total_post = 0;
static atomic_ulong total_post_ok = 0;
static atomic_ulong total_delete = 0;
static atomic_ulong total_delete_ok = 0;

/* latency accumulators (ns) — total across run */
static atomic_ulong total_lat_sum_ns = 0;
static atomic_ulong total_lat_count = 0;

/* control */
static atomic_int stop_flag = 0;

/* counters for unique key generation */
static atomic_ulong global_put_counter = 0;
static atomic_ulong global_get_counter = 0;

/* helper: monotonic time */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* sleep helper */
static void sleep_ns(uint64_t ns) {
    struct timespec req, rem;
    req.tv_sec = ns / 1000000000ULL;
    req.tv_nsec = ns % 1000000000ULL;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
}

/* deterministic value payload */
static void build_value(char *buf, size_t bufsize, int thread_id, int seq, int bytes) {
    if (bytes <= 0) { buf[0] = '\0'; return; }
    int p = snprintf(buf, bufsize, "t%d_s%d:", thread_id, seq);
    while (p < bytes - 1) {
        int n = snprintf(buf + p, bufsize - p, "%02x", (unsigned)(p & 0xff));
        if (n <= 0) break;
        p += n;
    }
    buf[bytes] = '\0';
}

/* HTTP request helper. returns 0 on success (2xx), -1 on failure */
static int do_request(CURL *eh, const char *method, const char *base_target, const char *key,
                      const char *value, int value_len, double *lat_ms_out) {
    char url[MAX_URL_LEN];
    CURLcode rc;
    long http_code = 0;
    uint64_t t0, t1;

    if (strcmp(method, "POST") == 0) {
        snprintf(url, sizeof(url), "%s/kv", base_target);
        size_t json_len = (size_t)value_len + strlen(key) + 64;
        char *json = malloc(json_len);
        if (!json) return -1;
        snprintf(json, json_len, "{\"key\":\"%s\",\"value\":\"%s\"}", key, value ? value : "");
        curl_easy_reset(eh);
        curl_easy_setopt(eh, CURLOPT_URL, url);
        struct curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(eh, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDS, json);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
        curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, 5000L);
        t0 = now_ns();
        rc = curl_easy_perform(eh);
        t1 = now_ns();
        curl_slist_free_all(hdrs);
        free(json);
        if (lat_ms_out) *lat_ms_out = (double)(t1 - t0) / 1e6;
        if (rc != CURLE_OK) return -1;
        curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
        return (http_code >= 200 && http_code < 300 || http_code == 400) ? 0 : -1;
    } else if (strcmp(method, "GET") == 0) {
        snprintf(url, sizeof(url), "%s/kv/%s", base_target, key);
        curl_easy_reset(eh);
        curl_easy_setopt(eh, CURLOPT_URL, url);
        curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, 5000L);
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        t0 = now_ns();
        rc = curl_easy_perform(eh);
        t1 = now_ns();
        if (lat_ms_out) *lat_ms_out = (double)(t1 - t0) / 1e6;
        if (rc != CURLE_OK) return -1;
        curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
        return (http_code >= 200 && http_code < 300 || http_code ==404) ? 0 : -1;
    } else { /* DELETE */
        snprintf(url, sizeof(url), "%s/kv/%s", base_target, key);
        curl_easy_reset(eh);
        curl_easy_setopt(eh, CURLOPT_URL, url);
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, 5000L);
        t0 = now_ns();
        rc = curl_easy_perform(eh);
        t1 = now_ns();
        if (lat_ms_out) *lat_ms_out = (double)(t1 - t0) / 1e6;
        if (rc != CURLE_OK) return -1;
        curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
        return (http_code >= 200 && http_code < 300) ? 0 : -1;
    }
}

/* parse a ratio string G:P:D */
static int parse_ratio_str(const char *s, int *g, int *p, int *d) {
    if (!s) return -1;
    int a=0,b=0,c=0;
    char after=0;
    int rc = sscanf(s, "%d:%d:%d%c", &a, &b, &c, &after);
    if (rc < 3) return -1;
    if (a < 0 || b < 0 || c < 0) return -1;
    *g = a; *p = b; *d = c;
    return 0;
}

/* worker args */
typedef struct { int id; unsigned int rand_state; } worker_arg_t;

/* MIX op chooser */
static const char *pick_op_mix(unsigned int *st) {
    int r = rand_r(st) % 100;
    if (r < cfg.read_pct) return "GET";
    r -= cfg.read_pct;
    if (r < cfg.write_pct) return "POST";
    return "DELETE";
}

/* key generators */
static void gen_putall_key(char *out, size_t outsz) {
    unsigned long id = atomic_fetch_add(&global_put_counter, 1);
    snprintf(out, outsz, "p%lu", id % (unsigned long)cfg.keyspace);
}
static void gen_getall_key(char *out, size_t outsz) {
    unsigned long id = atomic_fetch_add(&global_get_counter, 1);
    snprintf(out, outsz, "g%lu", id % (unsigned long)cfg.keyspace);
}
static void gen_mix_key(char *out, size_t outsz, unsigned int *st) {
    unsigned long id = rand_r(st) % (unsigned long)cfg.keyspace;
    snprintf(out, outsz, "k%lu", id);
}
/* hot key generator for getpopular */
static void gen_hot_key(char *out, size_t outsz, unsigned int *st) {
    if (cfg.hotset_size <= 0) cfg.hotset_size = 1;
    unsigned long id = rand_r(st) % (unsigned long)cfg.hotset_size;
    snprintf(out, outsz, "hot%lu", id);
}

/* worker main (closed-loop) */
static void *worker_main(void *varg) {
    worker_arg_t *arg = (worker_arg_t *)varg;
    int id = arg->id;
    unsigned int st = arg->rand_state;
    CURL *eh = curl_easy_init();
    if (!eh) {
        fprintf(stderr, "worker %d: curl_easy_init failed\n", id);
        return NULL;
    }
    int seq = 0;

    while (!atomic_load(&stop_flag)) {
        const char *op = NULL;
        char key[MAX_KEY_LEN];
        char value[MAX_VALUE_LEN];

        if (cfg.workload == WL_PUTALL) {
            /* alternate POST and DELETE to stress DB */
            if ((seq & 1) == 0) op = "POST"; else op = "DELETE";
            gen_putall_key(key, sizeof(key));
            if (op[0] == 'P') build_value(value, sizeof(value), id, seq, cfg.value_size); else value[0]='\0';
        } else if (cfg.workload == WL_GETALL) {
            op = "GET";
            gen_getall_key(key, sizeof(key));
            value[0] = '\0';
        } else if (cfg.workload == WL_GETPOPULAR) {
            op = "GET";
            gen_hot_key(key, sizeof(key), &st);
            value[0] = '\0';
        } else { /* MIX */
            op = pick_op_mix(&st);
            gen_mix_key(key, sizeof(key), &st);
            if (op[0] == 'P') build_value(value, sizeof(value), id, seq, cfg.value_size); else value[0]='\0';
        }

        double lat_ms = 0.0;
        int rc = do_request(eh, op, cfg.target, key, value, (int)strlen(value), &lat_ms);

        atomic_fetch_add(&total_reqs, 1);
        if (rc == 0) atomic_fetch_add(&total_success, 1); else atomic_fetch_add(&total_fail, 1);

        if (strcmp(op,"GET")==0) {
            atomic_fetch_add(&total_get,1);
            if (rc==0) atomic_fetch_add(&total_get_ok,1);
        } else if (strcmp(op,"POST")==0) {
            atomic_fetch_add(&total_post,1);
            if (rc==0) atomic_fetch_add(&total_post_ok,1);
        } else {
            atomic_fetch_add(&total_delete,1);
            if (rc==0) atomic_fetch_add(&total_delete_ok,1);
        }

        if (rc==0 && lat_ms>0.0) {
            uint64_t add_ns = (uint64_t)(lat_ms * 1e6);
            atomic_fetch_add(&total_lat_sum_ns, add_ns);
            atomic_fetch_add(&total_lat_count, 1);
        }

        seq++;
        /* closed-loop: immediately next request */
    }

    curl_easy_cleanup(eh);
    return NULL;
}

/* seeding: posts keys appropriate for workload:
   - getall: seeds keys "g0..gN-1"
   - getpopular: seeds hot keys "hot0..hotM-1"
   - otherwise seeds generic "k0..kN-1" */
static int seed_database(void) {
    CURL *eh = curl_easy_init();
    if (!eh) {
        fprintf(stderr, "seed: curl_easy_init failed\n");
        return -1;
    }
    char key[MAX_KEY_LEN], value[MAX_VALUE_LEN];
    if (cfg.workload == WL_GETALL) {
        for (int i=0;i<cfg.keyspace;i++) {
            snprintf(key,sizeof(key),"g%d",i);
            build_value(value,sizeof(value),0,i,cfg.value_size);
            double lat; if (do_request(eh,"POST",cfg.target,key,value,(int)strlen(value),&lat)!=0) {
                fprintf(stderr,"seed: POST g%d failed\n",i);
                curl_easy_cleanup(eh); return -1;
            }
            if ((i & 127) == 0) fprintf(stderr,"seed: posted %d g-keys...\n",i);
        }
    } else if (cfg.workload == WL_GETPOPULAR) {
        for (int i=0;i<cfg.hotset_size;i++) {
            snprintf(key,sizeof(key),"hot%d",i);
            build_value(value,sizeof(value),0,i,cfg.value_size);
            double lat; if (do_request(eh,"POST",cfg.target,key,value,(int)strlen(value),&lat)!=0) {
                fprintf(stderr,"seed: POST hot%d failed\n",i);
                curl_easy_cleanup(eh); return -1;
            }
            if ((i & 127) == 0) fprintf(stderr,"seed: posted %d hot-keys...\n",i);
        }
    } else {
        for (int i=0;i<cfg.keyspace;i++) {
            snprintf(key,sizeof(key),"k%d",i);
            build_value(value,sizeof(value),0,i,cfg.value_size);
            double lat; if (do_request(eh,"POST",cfg.target,key,value,(int)strlen(value),&lat)!=0) {
                fprintf(stderr,"seed: POST k%d failed\n",i);
                curl_easy_cleanup(eh); return -1;
            }
            if ((i & 127) == 0) fprintf(stderr,"seed: posted %d keys...\n",i);
        }
    }
    curl_easy_cleanup(eh);
    fprintf(stderr,"seed: finished seeding\n");
    return 0;
}

/* print final summary */
static void print_summary(int duration) {
    unsigned long treqs = (unsigned long)atomic_load(&total_reqs);
    unsigned long tsucc = (unsigned long)atomic_load(&total_success);
    unsigned long tfail = (unsigned long)atomic_load(&total_fail);
    unsigned long lat_count = (unsigned long)atomic_load(&total_lat_count);
    unsigned long lat_sum_ns = (unsigned long)atomic_load(&total_lat_sum_ns);

    double avg_thr = (double)tsucc / (double)duration;
    double avg_ms = 0.0;
    if (lat_count > 0) avg_ms = ((double)lat_sum_ns / (double)lat_count) / 1e6;

    printf("\n=== Summary ===\n");
    printf("Duration(s): %d\n", duration);
    printf("Total requests: %lu  Success: %lu  Fail: %lu\n", treqs, tsucc, tfail);
    printf("Avg throughput (successful req/s): %.3f\n", avg_thr);
    printf("Avg response time (ms): %.6f\n", avg_ms);
    printf("GET total=%lu OK=%lu\n", (unsigned long)atomic_load(&total_get), (unsigned long)atomic_load(&total_get_ok));
    printf("POST total=%lu OK=%lu\n", (unsigned long)atomic_load(&total_post), (unsigned long)atomic_load(&total_post_ok));
    printf("DELETE total=%lu OK=%lu\n", (unsigned long)atomic_load(&total_delete), (unsigned long)atomic_load(&total_delete_ok));
}

/* usage */
static void print_usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --target URL           target base URL (default %s)\n"
        "  --duration S           test duration seconds (default %d)\n"
        "  --threads N            number of clients (threads) (default %d)\n"
        "  --keyspace N           number of keys for generic workloads (default %d)\n"
        "  --value-size N         bytes for write value (default %d)\n"
        "  --workload TYPE        putall|getall|mix|getpopular (default mix)\n"
        "  --hotset-size N        hot set size for getpopular (default %d)\n"
        "  --read-pct P           read percent for mix (default %d)\n"
        "  --write-pct P          write percent for mix (default %d)\n"
        "  --delete-pct P         delete percent for mix (default %d)\n"
        "  --mix-ratio G:P:D      compact ratio for mix (GET:POST:DELETE)\n"
        "  --seed                 pre-seed DB before test (useful for getall/getpopular)\n"
        "  --help\n",
        p, DEFAULT_TARGET, cfg.duration, cfg.threads, cfg.keyspace, cfg.value_size,
        cfg.hotset_size, cfg.read_pct, cfg.write_pct, cfg.delete_pct
    );
}

/* parse workload string */
static workload_t parse_workload(const char *s) {
    if (!s) return WL_MIX;
    if (strcmp(s,"putall")==0) return WL_PUTALL;
    if (strcmp(s,"getall")==0) return WL_GETALL;
    if (strcmp(s,"getpopular")==0) return WL_GETPOPULAR;
    return WL_MIX;
}

int main(int argc, char **argv) {
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--target")==0 && i+1<argc) strncpy(cfg.target, argv[++i], sizeof(cfg.target)-1);
        else if (strcmp(argv[i],"--duration")==0 && i+1<argc) cfg.duration = atoi(argv[++i]);
        else if (strcmp(argv[i],"--threads")==0 && i+1<argc) cfg.threads = atoi(argv[++i]);
        else if (strcmp(argv[i],"--keyspace")==0 && i+1<argc) cfg.keyspace = atoi(argv[++i]);
        else if (strcmp(argv[i],"--value-size")==0 && i+1<argc) cfg.value_size = atoi(argv[++i]);
        else if (strcmp(argv[i],"--workload")==0 && i+1<argc) cfg.workload = parse_workload(argv[++i]);
        else if (strcmp(argv[i],"--hotset-size")==0 && i+1<argc) cfg.hotset_size = atoi(argv[++i]);
        else if (strcmp(argv[i],"--read-pct")==0 && i+1<argc) cfg.read_pct = atoi(argv[++i]);
        else if (strcmp(argv[i],"--write-pct")==0 && i+1<argc) cfg.write_pct = atoi(argv[++i]);
        else if (strcmp(argv[i],"--delete-pct")==0 && i+1<argc) cfg.delete_pct = atoi(argv[++i]);
        else if ((strcmp(argv[i],"--mix-ratio")==0 || strcmp(argv[i],"--ratio")==0) && i+1<argc) {
            int g=0,p=0,d=0;
            if (parse_ratio_str(argv[++i], &g, &p, &d) != 0) {
                fprintf(stderr,"Invalid ratio format. Use G:P:D\n"); return 1;
            }
            cfg.read_pct = g; cfg.write_pct = p; cfg.delete_pct = d;
        }
        else if (strcmp(argv[i],"--seed")==0) cfg.seed_db = 1;
        else if (strcmp(argv[i],"--help")==0) { print_usage(argv[0]); return 0; }
        else { fprintf(stderr,"Unknown arg: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

    /* normalize mix percentages */
    if (cfg.workload == WL_MIX) {
        int total = cfg.read_pct + cfg.write_pct + cfg.delete_pct;
        if (total == 0) { cfg.read_pct = 100; cfg.write_pct = cfg.delete_pct = 0; }
        else if (total != 100) {
            cfg.read_pct = (cfg.read_pct * 100) / total;
            cfg.write_pct = (cfg.write_pct * 100) / total;
            cfg.delete_pct = 100 - cfg.read_pct - cfg.write_pct;
        }
    }

    printf("Loadgen config: target=%s dur=%d threads=%d keyspace=%d valsz=%d workload=%d hotset=%d seed=%d mix=%d/%d/%d\n",
        cfg.target, cfg.duration, cfg.threads, cfg.keyspace, cfg.value_size, (int)cfg.workload, cfg.hotset_size, cfg.seed_db,
        cfg.read_pct, cfg.write_pct, cfg.delete_pct);

    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        fprintf(stderr,"curl_global_init failed\n"); return 1;
    }

    /* optional seeding */
    if (cfg.seed_db) {
        fprintf(stderr,"Seeding DB...\n");
        if (seed_database() != 0) {
            fprintf(stderr,"Seeding failed\n"); return 1;
        }
    }

    /* start workers */
    pthread_t *workers = calloc((size_t)cfg.threads, sizeof(pthread_t));
    worker_arg_t *wargs = calloc((size_t)cfg.threads, sizeof(worker_arg_t));
    atomic_store(&stop_flag, 0);
    atomic_store(&total_reqs, 0);
    atomic_store(&total_success, 0);
    atomic_store(&total_fail, 0);
    atomic_store(&total_lat_sum_ns, 0);
    atomic_store(&total_lat_count, 0);

    for (int i=0;i<cfg.threads;i++) {
        wargs[i].id = i;
        wargs[i].rand_state = (unsigned int)time(NULL) ^ (unsigned int)(i * 1103515245);
        if (pthread_create(&workers[i], NULL, worker_main, &wargs[i]) != 0) {
            fprintf(stderr,"failed to create worker %d\n", i);
            atomic_store(&stop_flag, 1);
        }
    }

    /* run for duration */
    sleep(cfg.duration);
    atomic_store(&stop_flag, 1);

    /* join */
    for (int i=0;i<cfg.threads;i++) pthread_join(workers[i], NULL);

    /* summary */
    print_summary(cfg.duration);

    curl_global_cleanup();
    free(workers);
    free(wargs);
    return 0;
}
