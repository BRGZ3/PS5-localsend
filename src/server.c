#include "server.h"

#include "fast_upload_server.h"
#include "router.h"

#include <microhttpd.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define SERVER_CONNECTION_LIMIT 16U
#define SERVER_PER_IP_CONNECTION_LIMIT 8U
#define SERVER_CONNECTION_TIMEOUT_SECONDS 15U
/* Keep libmicrohttpd's receive pool small and predictable on the PS5.  The
 * pool remains bounded per connection; the upload path itself never
 * accumulates an unbounded request body. */
#define SERVER_CONNECTION_MEMORY_LIMIT (1024U * 1024U)
#define SERVER_CONNECTION_MEMORY_INCREMENT (256U * 1024U)
#define SERVER_SOCKET_BUFFER_BYTES (4U * 1024U * 1024U)
#define SERVER_LISTEN_BACKLOG 16U

typedef struct request_state {
    size_t body_size;
    size_t body_limit;
    int body_too_large;
    unsigned char body[ROUTER_MAX_METADATA_BODY_BYTES];
    int streaming;
    int stream_started;
    int stream_finished;
    int stream_failed;
    int stream_chunk;
    uint64_t chunk_offset;
    uint64_t chunk_size;
    uint64_t chunk_received;
    char upload_id[UPLOAD_ID_HEX_BYTES + 1U];
    router_response_t stream_error;
} request_state_t;

struct http_server {
    struct MHD_Daemon *daemon;
    fast_upload_server_t *fast_upload;
    router_t router;
    uint16_t bound_port;
};

static enum MHD_Result add_header(struct MHD_Response *response,
                                  const char *name, const char *value) {
    return MHD_add_response_header(response, name, value);
}

static enum MHD_Result queue_router_response(struct MHD_Connection *connection,
                                             router_response_t *routed) {
    struct MHD_Response *response;
    enum MHD_Result result;
    response = MHD_create_response_from_buffer(routed->body_size,
                                               (void *)(uintptr_t)routed->body,
                                               MHD_RESPMEM_MUST_COPY);
    if (response == NULL) {
        return MHD_NO;
    }
    if (add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, routed->content_type) == MHD_NO ||
        add_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, routed->cache_control) == MHD_NO ||
        add_header(response, "Content-Security-Policy",
                   "default-src 'self'; base-uri 'none'; form-action 'self'; connect-src 'self' http://*:*; img-src 'self'; style-src 'self'; script-src 'self'") == MHD_NO ||
        add_header(response, "X-Content-Type-Options", "nosniff") == MHD_NO ||
        add_header(response, "Referrer-Policy", "no-referrer") == MHD_NO ||
        add_header(response, "X-Frame-Options", "DENY") == MHD_NO ||
        add_header(response, "Permissions-Policy", "camera=(), microphone=(), geolocation=()") == MHD_NO ||
        (routed->allow != NULL &&
         add_header(response, MHD_HTTP_HEADER_ALLOW, routed->allow) == MHD_NO)) {
        MHD_destroy_response(response);
        return MHD_NO;
    }
    result = MHD_queue_response(connection, routed->status, response);
    MHD_destroy_response(response);
    return result;
}

static int parse_content_length(const char *value, uint64_t *length) {
    char *end = NULL;
    unsigned long long parsed;
    if (value == NULL) return -1;
    if (*value == '\0' || *value == '-' || *value == '+') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    *length = (uint64_t)parsed;
    return 0;
}

static size_t body_limit_for_request(const char *method, const char *url) {
    if (strcmp(method, "POST") == 0 &&
        strcmp(url, "/api/v1/auth/verify") == 0) {
        return ROUTER_MAX_AUTH_BODY_BYTES;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/uploads") == 0)
        return ROUTER_MAX_METADATA_BODY_BYTES;
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/storage") == 0)
        return ROUTER_MAX_METADATA_BODY_BYTES;
    return 0U;
}

static void client_address(struct MHD_Connection *connection, char *output,
                           size_t output_size) {
    const union MHD_ConnectionInfo *info = MHD_get_connection_info(
        connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    (void)snprintf(output, output_size, "unknown");
    if (info == NULL || info->client_addr == NULL) {
        return;
    }
    if (info->client_addr->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)(const void *)info->client_addr;
        (void)inet_ntop(AF_INET, &ipv4->sin_addr, output,
                        (socklen_t)output_size);
    } else if (info->client_addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)(const void *)info->client_addr;
        (void)inet_ntop(AF_INET6, &ipv6->sin6_addr, output,
                        (socklen_t)output_size);
    }
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls,
                              enum MHD_RequestTerminationCode toe) {
    request_state_t *state = *con_cls;
    (void)cls;
    (void)connection;
    (void)toe;
    if (state != NULL) {
        if (state->stream_started != 0 && state->stream_finished == 0) {
            if (state->stream_chunk != 0) {
                router_upload_stream_abort_chunk(&((http_server_t *)cls)->router,
                                                 state->upload_id,
                                                 state->chunk_offset,
                                                 state->chunk_size);
            } else {
                router_upload_stream_abort(&((http_server_t *)cls)->router,
                                           state->upload_id);
            }
        }
        memset(state, 0, sizeof(*state));
        free(state);
        *con_cls = NULL;
    }
}

static void tune_connection(void *cls, struct MHD_Connection *connection,
                            void **socket_context,
                            enum MHD_ConnectionNotificationCode toe) {
    const union MHD_ConnectionInfo *info;
    int buffer_size = (int)SERVER_SOCKET_BUFFER_BYTES;
    int one = 1;
    (void)cls;
    (void)socket_context;
    if (toe != MHD_CONNECTION_NOTIFY_STARTED || connection == NULL) {
        return;
    }
    info = MHD_get_connection_info(
        connection, MHD_CONNECTION_INFO_CONNECTION_FD);
    if (info == NULL || info->connect_fd == MHD_INVALID_SOCKET) {
        return;
    }
    /* A larger kernel window prevents a phone's TCP sender from stalling on
     * a high-latency Wi-Fi hop while the application drains libmicrohttpd's
     * bounded user-space buffer.  Failure is harmless on platforms that do
     * not permit changing the accepted socket buffers. */
    (void)setsockopt(info->connect_fd, SOL_SOCKET, SO_RCVBUF,
                     &buffer_size, (socklen_t)sizeof(buffer_size));
    (void)setsockopt(info->connect_fd, SOL_SOCKET, SO_SNDBUF,
                     &buffer_size, (socklen_t)sizeof(buffer_size));
#ifdef TCP_NODELAY
    /* Uploads are already buffered by libmicrohttpd; avoid adding another
     * delayed-ACK/Nagle wait on the control and streaming connection. */
    (void)setsockopt(info->connect_fd, IPPROTO_TCP, TCP_NODELAY,
                     &one, (socklen_t)sizeof(one));
#endif
#ifdef SO_NOSIGPIPE
    /* A disconnected browser must not terminate the payload while a write is
     * being drained from the temporary-file buffer. */
    (void)setsockopt(info->connect_fd, SOL_SOCKET, SO_NOSIGPIPE,
                     &one, (socklen_t)sizeof(one));
#endif
}

static enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls) {
    http_server_t *server = cls;
    request_state_t *state = *con_cls;
    router_request_t request;
    router_response_t routed;
    char address[AUTH_CLIENT_ADDRESS_BYTES];
    const char *content_length;
    uint64_t declared_length = 0U;
    (void)version;

    if (state == NULL) {
        state = calloc(1U, sizeof(*state));
        if (state == NULL) {
            return MHD_NO;
        }
        state->body_limit = body_limit_for_request(method, url);
        content_length = MHD_lookup_connection_value(
            connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH);
        if (strcmp(method, "PUT") == 0 &&
            strncmp(url, "/api/v1/uploads/", 16U) == 0) {
            memset(&request, 0, sizeof(request));
            client_address(connection, address, sizeof(address));
            request.method = method; request.path = url; request.client_address = address;
            request.host = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_HOST);
            request.origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
            request.content_type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
            request.authorization = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_AUTHORIZATION);
            request.content_range = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Range");
            state->streaming = 1;
            if (router_upload_stream_begin_ex(&server->router, &request,
                    content_length != NULL && parse_content_length(content_length, &declared_length) == 0,
                    declared_length, state->upload_id, &state->stream_chunk,
                    &state->chunk_offset, &state->chunk_size,
                    &state->stream_error) != 0)
                state->stream_failed = 1;
            else state->stream_started = 1;
        } else if (content_length != NULL &&
                   (parse_content_length(content_length, &declared_length) != 0 ||
                    declared_length > state->body_limit)) {
            state->body_too_large = 1;
        }
        *con_cls = state;
        if (state->streaming != 0 && state->stream_failed != 0)
            return queue_router_response(connection, &state->stream_error);
        return MHD_YES;
    }
    if (upload_data_size != NULL && *upload_data_size > 0U) {
        if (state->streaming != 0) {
            if (state->stream_failed == 0) {
                int write_result;
                if (state->stream_chunk != 0) {
                    write_result = router_upload_stream_write_chunk(
                        &server->router, state->upload_id,
                        state->chunk_offset + state->chunk_received,
                        upload_data, *upload_data_size, &state->stream_error);
                    if (write_result == 0) {
                        state->chunk_received += (uint64_t)*upload_data_size;
                    }
                } else {
                    write_result = router_upload_stream_write(
                        &server->router, state->upload_id, upload_data,
                        *upload_data_size, &state->stream_error);
                }
                if (write_result != 0) {
                    state->stream_failed = 1;
                    /* A mid-stream storage failure cannot retain a structured
                     * response without draining an attacker-controlled body. */
                    return MHD_NO;
                }
            }
        } else if (state->body_too_large == 0 &&
            *upload_data_size <= state->body_limit - state->body_size) {
            (void)memcpy(state->body + state->body_size, upload_data,
                         *upload_data_size);
            state->body_size += *upload_data_size;
        } else {
            state->body_too_large = 1;
        }
        *upload_data_size = 0U;
        return MHD_YES;
    }

    if (state->streaming != 0) {
        if (state->stream_failed != 0) return queue_router_response(connection, &state->stream_error);
        if (state->stream_chunk != 0) {
            if (router_upload_stream_finish_chunk(
                    &server->router, state->upload_id, state->chunk_offset,
                    state->chunk_size, &routed) != 0) {
                state->stream_failed = 1;
            }
        } else {
            router_upload_stream_finish(&server->router, state->upload_id,
                                        &routed);
        }
        state->stream_finished = 1;
        return queue_router_response(connection, &routed);
    }

    memset(&request, 0, sizeof(request));
    client_address(connection, address, sizeof(address));
    request.method = method;
    request.path = url;
    request.body = state->body;
    request.body_size = state->body_too_large != 0
                            ? state->body_limit + 1U
                            : state->body_size;
    request.client_address = address;
    request.host = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                               MHD_HTTP_HEADER_HOST);
    request.origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                 "Origin");
    request.content_type = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
    request.authorization = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_AUTHORIZATION);
    router_dispatch(&server->router, &request, &routed);
    return queue_router_response(connection, &routed);
}

http_server_t *server_start(const app_config_t *config) {
    http_server_t *server;
    const union MHD_DaemonInfo *info;
    if (config == NULL) {
        return NULL;
    }
    server = calloc(1U, sizeof(*server));
    if (server == NULL || router_init(&server->router, config) != 0) {
        free(server);
        return NULL;
    }
    server->daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION |
            MHD_USE_ERROR_LOG | MHD_USE_TURBO,
        config->port, NULL,
        NULL, handle_request, server, MHD_OPTION_CONNECTION_MEMORY_LIMIT,
        (size_t)SERVER_CONNECTION_MEMORY_LIMIT,
        MHD_OPTION_CONNECTION_MEMORY_INCREMENT,
        (size_t)SERVER_CONNECTION_MEMORY_INCREMENT,
        MHD_OPTION_CONNECTION_LIMIT,
        (unsigned int)SERVER_CONNECTION_LIMIT, MHD_OPTION_PER_IP_CONNECTION_LIMIT,
        (unsigned int)SERVER_PER_IP_CONNECTION_LIMIT, MHD_OPTION_CONNECTION_TIMEOUT,
        (unsigned int)SERVER_CONNECTION_TIMEOUT_SECONDS,
        MHD_OPTION_LISTEN_BACKLOG_SIZE, (unsigned int)SERVER_LISTEN_BACKLOG,
        MHD_OPTION_NOTIFY_CONNECTION, &tune_connection, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, server,
        MHD_OPTION_STRICT_FOR_CLIENT, 1, MHD_OPTION_END);
    if (server->daemon == NULL) {
        router_destroy(&server->router);
        free(server);
        return NULL;
    }
    info = MHD_get_daemon_info(server->daemon, MHD_DAEMON_INFO_BIND_PORT);
    if (info == NULL) {
        MHD_stop_daemon(server->daemon);
        router_destroy(&server->router);
        free(server);
        return NULL;
    }
    server->bound_port = info->port;
    if (server->bound_port == 0U) {
        struct sockaddr_storage address;
        socklen_t address_size = (socklen_t)sizeof(address);
        info = MHD_get_daemon_info(server->daemon, MHD_DAEMON_INFO_LISTEN_FD);
        memset(&address, 0, sizeof(address));
        if (info == NULL ||
            getsockname(info->listen_fd, (struct sockaddr *)(void *)&address,
                        &address_size) != 0) {
            MHD_stop_daemon(server->daemon);
            router_destroy(&server->router);
            free(server);
            return NULL;
        }
        if (address.ss_family == AF_INET) {
            const struct sockaddr_in *ipv4 =
                (const struct sockaddr_in *)(const void *)&address;
            server->bound_port = ntohs(ipv4->sin_port);
        } else if (address.ss_family == AF_INET6) {
            const struct sockaddr_in6 *ipv6 =
                (const struct sockaddr_in6 *)(const void *)&address;
            server->bound_port = ntohs(ipv6->sin6_port);
        } else {
            MHD_stop_daemon(server->daemon);
            router_destroy(&server->router);
            free(server);
            return NULL;
        }
    }
    router_set_port(&server->router, server->bound_port);
    {
        uint16_t requested_fast_port =
            config->port != 0U && server->bound_port < UINT16_MAX
                ? (uint16_t)(server->bound_port + 1U)
                : 0U;
        server->fast_upload = fast_upload_server_start(
            &server->router, server->bound_port, requested_fast_port);
        if (server->fast_upload == NULL && requested_fast_port != 0U) {
            server->fast_upload = fast_upload_server_start(
                &server->router, server->bound_port, 0U);
        }
    }
    if (server->fast_upload == NULL) {
        MHD_stop_daemon(server->daemon);
        router_destroy(&server->router);
        free(server);
        return NULL;
    }
    router_set_fast_upload_port(
        &server->router, fast_upload_server_port(server->fast_upload));
    return server;
}

uint16_t server_port(const http_server_t *server) {
    return server == NULL ? 0U : server->bound_port;
}

void server_stop(http_server_t *server) {
    if (server == NULL) {
        return;
    }
    fast_upload_server_stop(server->fast_upload);
    MHD_stop_daemon(server->daemon);
    router_destroy(&server->router);
    free(server);
}
