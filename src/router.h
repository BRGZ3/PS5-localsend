#ifndef PS5LOCALSEND_ROUTER_H
#define PS5LOCALSEND_ROUTER_H

#include "auth.h"
#include "config.h"
#include "upload.h"

#include <stddef.h>
#include <pthread.h>

#define ROUTER_MAX_METHOD_BYTES 16U
#define ROUTER_MAX_PATH_BYTES 2048U
#define ROUTER_MAX_AUTH_BODY_BYTES 512U
#define ROUTER_MAX_METADATA_BODY_BYTES UPLOAD_METADATA_BYTES

typedef struct router {
    pthread_mutex_t mutex;
    app_config_t config;
    config_auth_mode_t auth_mode;
    uint16_t fast_upload_port;
    char config_path[CONFIG_DESTINATION_CAPACITY];
    auth_manager_t *auth;
    upload_manager_t *uploads;
} router_t;

typedef struct router_request {
    const char *method;
    const char *path;
    const unsigned char *body;
    size_t body_size;
    const char *client_address;
    const char *host;
    const char *origin;
    const char *content_type;
    const char *authorization;
    const char *content_range;
} router_request_t;

typedef struct router_response {
    unsigned int status;
    const char *content_type;
    const char *cache_control;
    const char *allow;
    const unsigned char *body;
    size_t body_size;
    char storage[16384];
} router_response_t;

int router_init(router_t *router, const app_config_t *config);
void router_destroy(router_t *router);
void router_set_port(router_t *router, uint16_t port);
void router_set_fast_upload_port(router_t *router, uint16_t port);
void router_dispatch(router_t *router, const router_request_t *request,
                     router_response_t *response);
int router_upload_stream_begin(router_t *router, const router_request_t *request,
                               int has_content_length, uint64_t content_length,
                               char upload_id[UPLOAD_ID_HEX_BYTES + 1U],
                               router_response_t *error_response);
int router_upload_stream_begin_ex(
    router_t *router, const router_request_t *request, int has_content_length,
    uint64_t content_length, char upload_id[UPLOAD_ID_HEX_BYTES + 1U],
    int *chunk_mode, uint64_t *chunk_offset, uint64_t *chunk_size,
    router_response_t *error_response);
int router_upload_stream_write(router_t *router, const char *upload_id,
                               const void *data, size_t size,
                               router_response_t *error_response);
int router_upload_stream_enable_direct(router_t *router,
                                       const char *upload_id,
                                       router_response_t *error_response);
int router_upload_stream_write_direct(router_t *router,
                                      const char *upload_id,
                                      const void *data, size_t size,
                                      router_response_t *error_response);
void router_upload_stream_finish(router_t *router, const char *upload_id,
                                 router_response_t *response);
int router_upload_stream_write_chunk(router_t *router, const char *upload_id,
                                     uint64_t offset, const void *data,
                                     size_t size,
                                     router_response_t *error_response);
int router_upload_stream_finish_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size,
                                      router_response_t *response);
void router_upload_stream_abort_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size);
void router_upload_stream_abort(router_t *router, const char *upload_id);

#endif
