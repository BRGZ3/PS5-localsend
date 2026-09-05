#ifndef PS5LOCALSEND_SERVER_H
#define PS5LOCALSEND_SERVER_H

#include "config.h"

#include <stdint.h>

typedef struct http_server http_server_t;

http_server_t *server_start(const app_config_t *config);
uint16_t server_port(const http_server_t *server);
void server_stop(http_server_t *server);

#endif
