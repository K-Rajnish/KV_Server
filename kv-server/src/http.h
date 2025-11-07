#ifndef HTTP_H
#define HTTP_H

int start_http_server(const char *bind_addr, int port, int num_threads, int cache_capacity,
                      const char *db_conninfo, int db_pool_size);
void stop_http_server(void);

#endif
