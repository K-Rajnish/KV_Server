#define _GNU_SOURCE
#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

/*
 Simple LRU cache:
  - single global mutex for simplicity
  - hash table with chaining; each entry is also in a doubly-linked list for LRU
*/

typedef struct entry {
    char *key;
    char *value;
    size_t klen, vlen;
    struct entry *hnext; /* next in hash bucket */
    struct entry *prev, *next; /* LRU list */
} entry_t;

typedef struct {
    entry_t **buckets;
    size_t nbuckets;
    entry_t *lru_head, *lru_tail;
    size_t capacity;
    size_t size;
    pthread_mutex_t mu;
    unsigned long hits, misses;
} cache_t;

static cache_t *cache = NULL;

static size_t default_nbuckets(size_t cap) {
    return (cap * 2) + 3;
}

static unsigned long hash_fn(const char *s) {
    unsigned long h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)(*s++);
    return h;
}

int cache_init(size_t capacity) {
    if (cache) return 0;
    cache = calloc(1, sizeof(cache_t));
    if (!cache) return -1;
    cache->capacity = capacity;
    cache->nbuckets = default_nbuckets(capacity);
    cache->buckets = calloc(cache->nbuckets, sizeof(entry_t *));
    if (!cache->buckets) { free(cache); cache = NULL; return -1; }
    pthread_mutex_init(&cache->mu, NULL);
    cache->lru_head = cache->lru_tail = NULL;
    cache->size = 0;
    cache->hits = cache->misses = 0;
    return 0;
}

static void detach_lru(entry_t *e) {
    if (!e) return;
    if (e->prev) e->prev->next = e->next;
    else cache->lru_head = e->next;
    if (e->next) e->next->prev = e->prev;
    else cache->lru_tail = e->prev;
    e->prev = e->next = NULL;
}

static void insert_head(entry_t *e) {
    e->prev = NULL;
    e->next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail) cache->lru_tail = e;
}

static void evict_if_needed() {
    while (cache->size > cache->capacity && cache->lru_tail) {
        entry_t *e = cache->lru_tail;
        /* remove from hash */
        unsigned long h = hash_fn(e->key) % cache->nbuckets;
        entry_t **pp = &cache->buckets[h];
        while (*pp && *pp != e) pp = &(*pp)->hnext;
        if (*pp == e) *pp = e->hnext;
        /* remove from lru */
        detach_lru(e);
        /* free */
        free(e->key);
        free(e->value);
        free(e);
        cache->size--;
    }
}

char *cache_get(const char *key) {
    if (!cache) return NULL;
    pthread_mutex_lock(&cache->mu);
    unsigned long h = hash_fn(key) % cache->nbuckets;
    entry_t *cur = cache->buckets[h];
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            /* hit: move to head */
            detach_lru(cur);
            insert_head(cur);
            cache->hits++;
            char *val = strdup(cur->value);
            pthread_mutex_unlock(&cache->mu);
            return val;
        }
        cur = cur->hnext;
    }
    cache->misses++;
    pthread_mutex_unlock(&cache->mu);
    return NULL;
}

int cache_put(const char *key, const char *value) {
    if (!cache) return -1;
    pthread_mutex_lock(&cache->mu);
    unsigned long h = hash_fn(key) % cache->nbuckets;
    entry_t *cur = cache->buckets[h];
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            /* update existing */
            free(cur->value);
            cur->value = strdup(value);
            cur->vlen = strlen(value);
            detach_lru(cur);
            insert_head(cur);
            pthread_mutex_unlock(&cache->mu);
            return 0;
        }
        cur = cur->hnext;
    }
    /* create new entry */
    entry_t *e = calloc(1, sizeof(entry_t));
    if (!e) { pthread_mutex_unlock(&cache->mu); return -1; }
    e->key = strdup(key);
    e->value = strdup(value);
    e->klen = strlen(e->key);
    e->vlen = strlen(e->value);
    /* insert into hash bucket */
    e->hnext = cache->buckets[h];
    cache->buckets[h] = e;
    /* insert at head */
    insert_head(e);
    cache->size++;
    /* evict if necessary */
    evict_if_needed();
    pthread_mutex_unlock(&cache->mu);
    return 0;
}

int cache_delete(const char *key) {
    if (!cache) return -1;
    pthread_mutex_lock(&cache->mu);
    unsigned long h = hash_fn(key) % cache->nbuckets;
    entry_t **pp = &cache->buckets[h];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            entry_t *found = *pp;
            *pp = found->hnext;
            detach_lru(found);
            free(found->key);
            free(found->value);
            free(found);
            cache->size--;
            pthread_mutex_unlock(&cache->mu);
            return 0;
        }
        pp = &(*pp)->hnext;
    }
    pthread_mutex_unlock(&cache->mu);
    return -1;
}

void cache_stats(unsigned long *hits, unsigned long *misses, unsigned long *items) {
    if (!cache) { if (hits) *hits = 0; if (misses) *misses = 0; if (items) *items = 0; return; }
    pthread_mutex_lock(&cache->mu);
    if (hits) *hits = cache->hits;
    if (misses) *misses = cache->misses;
    if (items) *items = cache->size;
    pthread_mutex_unlock(&cache->mu);
}

void cache_free(void) {
    if (!cache) return;
    pthread_mutex_lock(&cache->mu);
    for (size_t i = 0; i < cache->nbuckets; ++i) {
        entry_t *cur = cache->buckets[i];
        while (cur) {
            entry_t *n = cur->hnext;
            free(cur->key);
            free(cur->value);
            free(cur);
            cur = n;
        }
    }
    free(cache->buckets);
    pthread_mutex_unlock(&cache->mu);
    pthread_mutex_destroy(&cache->mu);
    free(cache);
    cache = NULL;
}
