#ifndef PS5LOCALSEND_FAST_UPLOAD_SERVER_H
#define PS5LOCALSEND_FAST_UPLOAD_SERVER_H

#include "router.h"

#include <stdint.h>

typedef struct fast_upload_server fast_upload_server_t;

fast_upload_server_t *fast_upload_server_start(router_t *router,
                                               uint16_t ui_port,
                                               uint16_t requested_port);
uint16_t fast_upload_server_port(const fast_upload_server_t *server);
void fast_upload_server_stop(fast_upload_server_t *server);

#endif
