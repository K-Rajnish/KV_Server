#ifndef DB_H
#define DB_H

/* Initialize DB connection pool. Returns 0 on success. */
int db_init(const char *conninfo, int pool_size);
void db_shutdown(void);

/* DB operations:
   - db_get returns newly allocated value (caller frees). returns 0 on success, -1 not found or error.
   - db_put inserts or updates value; returns 0 on success, -1 otherwise.
   - db_delete deletes key; returns 0 on success, -1 if not found/error.
*/
int db_get(const char *key, char **value_out, int *value_len);
int db_put(const char *key, const char *value, int value_len);
int db_delete(const char *key);

#endif /* DB_H */
