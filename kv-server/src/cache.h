#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

int cache_init(size_t capacity);
void cache_free(void);

/* Return newly allocated value (caller frees) or NULL if not found */
char *cache_get(const char *key);

/* Put or update — makes internal copies of key/value */
int cache_put(const char *key, const char *value);

/* Remove key from cache */
int cache_delete(const char *key);

/* stats */
void cache_stats(unsigned long *hits, unsigned long *misses, unsigned long *items);

#endif
