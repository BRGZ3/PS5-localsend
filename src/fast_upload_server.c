#define _POSIX_C_SOURCE 200809L

#include "fast_upload_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define FAST_UPLOAD_CONNECTION_LIMIT 8U
#define FAST_UPLOAD_LISTEN_BACKLOG 16
#define FAST_UPLOAD_HEADER_BYTES (16U * 1024U)
#define FAST_UPLOAD_READ_BYTES (1024U * 1024U)
#define FAST_UPLOAD_SOCKET_BUFFER_BYTES (4U * 1024U * 1024U)
#define FAST_UPLOAD_TIMEOUT_SECONDS 30

typedef struct fast_request {
    char method[8];
    char path[ROUTER_MAX_PATH_BYTES + 1U];
    char host[264];
    char origin[264];
    char authorization[UPLOAD_AUTH_BYTES];
    char content_type[64];
    char content_range[128];
    uint64_t content_length;
    int has_content_length;
    size_t body_offset;
    size_t buffered_bytes;
} fast_request_t;

typedef struct fast_connection {
    struct fast_upload_server *server;
    int descriptor;
    unsigned int slot;
    struct sockaddr_storage address;
} fast_connection_t;

struct fast_upload_server {
    router_t *router;
    uint16_t ui_port;
    uint16_t bound_port;
    int listener;
    pthread_t accept_thread;
    int accept_thread_started;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int stopping;
    int client_fds[FAST_UPLOAD_CONNECTION_LIMIT];
    unsigned int active_workers;
};

static const char *status_text(unsigned int status) {
    switch (status) {
        case 200U: return "OK";
        case 201U: return "Created";
        case 204U: return "No Content";
        case 400U: return "Bad Request";
        case 401U: return "Unauthorized";
        case 403U: return "Forbidden";
        case 404U: return "Not Found";
        case 405U: return "Method Not Allowed";
        case 408U: return "Request Timeout";
        case 409U: return "Conflict";
        case 413U: return "Content Too Large";
        case 415U: return "Unsupported Media Type";
        case 422U: return "Unprocessable Content";
        case 429U: return "Too Many Requests";
        case 500U: return "Internal Server Error";
        case 507U: return "Insufficient Storage";
        default: return "Error";
    }
}

static int send_all(int descriptor, const void *data, size_t size) {
    const unsigned char *cursor = data;
    while (size > 0U) {
        ssize_t count = send(descriptor, cursor, size, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return 0;
}

static int send_response(int descriptor, const router_response_t *response,
                         const char *origin) {
    char header[2048];
    const char *content_type = response->content_type != NULL
                                   ? response->content_type
                                   : "application/json; charset=utf-8";
    const char *cache_control = response->cache_control != NULL
                                    ? response->cache_control
                                    : "no-store";
    int count = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %u %s\r\n"
        "Content-Length: %llu\r\n"
        "Content-Type: %s\r\n"
        "Cache-Control: %s\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "Vary: Origin\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n",
        response->status, status_text(response->status),
        (unsigned long long)response->body_size, content_type, cache_control,
        origin != NULL ? origin : "null");
    if (count < 0 || (size_t)count >= sizeof(header) ||
        send_all(descriptor, header, (size_t)count) != 0) {
        return -1;
    }
    return response->body_size == 0U ||
                   send_all(descriptor, response->body,
                            response->body_size) == 0
               ? 0
               : -1;
}

static int send_simple_error(int descriptor, unsigned int status,
                             const char *code, const char *message,
                             const char *origin) {
    router_response_t response;
    int count;
    memset(&response, 0, sizeof(response));
    count = snprintf(response.storage, sizeof(response.storage),
                     "{\"error\":{\"code\":\"%s\","
                     "\"message\":\"%s\"}}\n",
                     code, message);
    if (count < 0 || (size_t)count >= sizeof(response.storage)) {
        return -1;
    }
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.cache_control = "no-store";
    response.body = (const unsigned char *)response.storage;
    response.body_size = (size_t)count;
    return send_response(descriptor, &response, origin);
}

static char *trim(char *value) {
    char *end;
    while (*value != '\0' && isspace((unsigned char)*value) != 0) {
        ++value;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]) != 0) {
        *--end = '\0';
    }
    return value;
}

static int parse_u64(const char *value, uint64_t *output) {
    char *end = NULL;
    unsigned long long parsed;
    if (value == NULL || *value == '\0' || *value == '-' || *value == '+') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    *output = (uint64_t)parsed;
    return 0;
}

static int copy_header_value(char *destination, size_t capacity,
                             const char *value) {
    size_t length = strlen(value);
    if (length == 0U || length >= capacity) {
        return -1;
    }
    memcpy(destination, value, length + 1U);
    return 0;
}

static int parse_request(unsigned char *buffer, size_t size,
                         fast_request_t *request) {
    unsigned char *header_end = NULL;
    char *line;
    char *next;
    size_t index;
    memset(request, 0, sizeof(*request));
    for (index = 3U; index < size; ++index) {
        if (buffer[index - 3U] == '\r' && buffer[index - 2U] == '\n' &&
            buffer[index - 1U] == '\r' && buffer[index] == '\n') {
            header_end = buffer + index - 3U;
            request->body_offset = index + 1U;
            request->buffered_bytes = size - request->body_offset;
            break;
        }
    }
    if (header_end == NULL) {
        return 1;
    }
    *header_end = '\0';
    line = (char *)(void *)buffer;
    next = strstr(line, "\r\n");
    if (next == NULL) {
        return -1;
    }
    *next = '\0';
    if (sscanf(line, "%7s %2048s HTTP/1.1", request->method,
               request->path) != 2) {
        return -1;
    }
    line = next + 2;
    while (*line != '\0') {
        char *colon;
        next = strstr(line, "\r\n");
        if (next != NULL) {
            *next = '\0';
        }
        colon = strchr(line, ':');
        if (colon == NULL) {
            return -1;
        }
        *colon = '\0';
        {
            char *name = trim(line);
            char *value = trim(colon + 1);
            if (strcasecmp(name, "Host") == 0) {
                if (request->host[0] != '\0' ||
                    copy_header_value(request->host,
                                      sizeof(request->host), value) != 0) {
                    return -1;
                }
            } else if (strcasecmp(name, "Origin") == 0) {
                if (request->origin[0] != '\0' ||
                    copy_header_value(request->origin,
                                      sizeof(request->origin), value) != 0) {
                    return -1;
                }
            } else if (strcasecmp(name, "Authorization") == 0) {
                if (request->authorization[0] != '\0' ||
                    copy_header_value(request->authorization,
                                      sizeof(request->authorization), value) !=
                        0) {
                    return -1;
                }
            } else if (strcasecmp(name, "Content-Type") == 0) {
                if (request->content_type[0] != '\0' ||
                    copy_header_value(request->content_type,
                                      sizeof(request->content_type), value) !=
                        0) {
                    return -1;
                }
            } else if (strcasecmp(name, "Content-Range") == 0) {
                if (request->content_range[0] != '\0' ||
                    copy_header_value(request->content_range,
                                      sizeof(request->content_range), value) !=
                        0) {
                    return -1;
                }
            } else if (strcasecmp(name, "Content-Length") == 0) {
                if (request->has_content_length != 0 ||
                    parse_u64(value, &request->content_length) != 0) {
                    return -1;
                }
                request->has_content_length = 1;
            }
        }
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }
    return 0;
}

static int split_authority(const char *authority, char *host,
                           size_t host_capacity, uint16_t *port) {
    const char *port_text;
    size_t host_length;
    uint64_t parsed_port;
    if (authority == NULL || *authority == '\0') {
        return -1;
    }
    if (*authority == '[') {
        const char *closing = strchr(authority, ']');
        if (closing == NULL || closing[1] != ':') {
            return -1;
        }
        host_length = (size_t)(closing - authority + 1);
        port_text = closing + 2;
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon == NULL || strchr(authority, ':') != colon) {
            return -1;
        }
        host_length = (size_t)(colon - authority);
        port_text = colon + 1;
    }
    if (host_length == 0U || host_length >= host_capacity ||
        parse_u64(port_text, &parsed_port) != 0 || parsed_port > 65535U) {
        return -1;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    *port = (uint16_t)parsed_port;
    return 0;
}

static int validate_origin(const fast_upload_server_t *server,
                           const fast_request_t *request,
                           char ui_authority[264]) {
    static const char prefix[] = "http://";
    const char *origin_authority;
    char origin_host[256];
    char request_host[256];
    uint16_t origin_port;
    uint16_t request_port;
    size_t length;
    if (strncmp(request->origin, prefix, sizeof(prefix) - 1U) != 0) {
        return -1;
    }
    origin_authority = request->origin + sizeof(prefix) - 1U;
    if (strchr(origin_authority, '/') != NULL ||
        split_authority(origin_authority, origin_host, sizeof(origin_host),
                        &origin_port) != 0 ||
        split_authority(request->host, request_host, sizeof(request_host),
                        &request_port) != 0 ||
        origin_port != server->ui_port || request_port != server->bound_port ||
        strcasecmp(origin_host, request_host) != 0) {
        return -1;
    }
    length = strlen(origin_authority);
    if (length >= 264U) {
        return -1;
    }
    memcpy(ui_authority, origin_authority, length + 1U);
    return 0;
}

static void socket_address_text(const struct sockaddr_storage *address,
                                char output[AUTH_CLIENT_ADDRESS_BYTES]) {
    (void)snprintf(output, AUTH_CLIENT_ADDRESS_BYTES, "unknown");
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)(const void *)address;
        (void)inet_ntop(AF_INET, &ipv4->sin_addr, output,
                        AUTH_CLIENT_ADDRESS_BYTES);
    } else if (address->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)(const void *)address;
        (void)inet_ntop(AF_INET6, &ipv6->sin6_addr, output,
                        AUTH_CLIENT_ADDRESS_BYTES);
    }
}

static int send_preflight(int descriptor, const char *origin) {
    char response[1024];
    int count = snprintf(
        response, sizeof(response),
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: PUT, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, Content-Range\r\n"
        "Access-Control-Max-Age: 600\r\n"
        "Vary: Origin\r\n"
        "Connection: close\r\n\r\n",
        origin);
    return count > 0 && (size_t)count < sizeof(response)
               ? send_all(descriptor, response, (size_t)count)
               : -1;
}

static int receive_headers(int descriptor, unsigned char *buffer,
                           size_t *received, fast_request_t *request) {
    *received = 0U;
    while (*received < FAST_UPLOAD_HEADER_BYTES) {
        ssize_t count = recv(descriptor, buffer + *received,
                             FAST_UPLOAD_HEADER_BYTES - *received, 0);
        int parse_result;
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        *received += (size_t)count;
        parse_result = parse_request(buffer, *received, request);
        if (parse_result <= 0) {
            return parse_result;
        }
    }
    return -1;
}

static void unregister_worker(fast_connection_t *connection) {
    fast_upload_server_t *server = connection->server;
    pthread_mutex_lock(&server->mutex);
    server->client_fds[connection->slot] = -1;
    if (server->active_workers > 0U) {
        --server->active_workers;
    }
    (void)pthread_cond_broadcast(&server->condition);
    pthread_mutex_unlock(&server->mutex);
}

static void *connection_main(void *argument) {
    fast_connection_t *connection = argument;
    fast_upload_server_t *server = connection->server;
    unsigned char *buffer = NULL;
    fast_request_t parsed;
    router_request_t request;
    router_response_t response;
    char address[AUTH_CLIENT_ADDRESS_BYTES];
    char ui_authority[264];
    char upload_id[UPLOAD_ID_HEX_BYTES + 1U];
    size_t received_headers = 0U;
    uint64_t received_body = 0U;
    uint64_t chunk_offset = 0U;
    uint64_t chunk_size = 0U;
    int chunk_mode = 0;
    int stream_started = 0;
    int parse_result;

    buffer = malloc(FAST_UPLOAD_READ_BYTES + FAST_UPLOAD_HEADER_BYTES);
    if (buffer == NULL) {
        (void)send_simple_error(connection->descriptor, 500U,
                                "internal_error", "Unable to allocate buffer",
                                NULL);
        goto done;
    }
    parse_result = receive_headers(connection->descriptor, buffer,
                                   &received_headers, &parsed);
    if (parse_result != 0) {
        (void)send_simple_error(connection->descriptor, 400U, "bad_request",
                                "Malformed HTTP request", NULL);
        goto done;
    }
    if (validate_origin(server, &parsed, ui_authority) != 0) {
        (void)send_simple_error(connection->descriptor, 403U,
                                "origin_forbidden",
                                "Origin does not match the PS5 receiver",
                                NULL);
        goto done;
    }
    if (strcmp(parsed.method, "OPTIONS") == 0) {
        (void)send_preflight(connection->descriptor, parsed.origin);
        goto done;
    }
    if (strcmp(parsed.method, "PUT") != 0) {
        (void)send_simple_error(connection->descriptor, 405U,
                                "method_not_allowed", "Only PUT is allowed",
                                parsed.origin);
        goto done;
    }

    memset(&request, 0, sizeof(request));
    socket_address_text(&connection->address, address);
    request.method = parsed.method;
    request.path = parsed.path;
    request.client_address = address;
    request.host = ui_authority;
    request.origin = parsed.origin;
    request.content_type = parsed.content_type;
    request.authorization = parsed.authorization;
    request.content_range = parsed.content_range[0] != '\0'
                                ? parsed.content_range
                                : NULL;
    if (router_upload_stream_begin_ex(
            server->router, &request, parsed.has_content_length,
            parsed.content_length, upload_id, &chunk_mode, &chunk_offset,
            &chunk_size, &response) != 0) {
        (void)send_response(connection->descriptor, &response, parsed.origin);
        goto done;
    }
    stream_started = 1;
    if (chunk_mode == 0 &&
        router_upload_stream_enable_direct(server->router, upload_id,
                                           &response) != 0) {
        (void)send_response(connection->descriptor, &response, parsed.origin);
        goto done;
    }

    if ((uint64_t)parsed.buffered_bytes > parsed.content_length) {
        goto stream_error;
    }
    if (parsed.buffered_bytes > 0U) {
        int write_result = chunk_mode != 0
                               ? router_upload_stream_write_chunk(
                                     server->router, upload_id, chunk_offset,
                                     buffer + parsed.body_offset,
                                     parsed.buffered_bytes, &response)
                               : router_upload_stream_write_direct(
                                     server->router, upload_id,
                                     buffer + parsed.body_offset,
                                     parsed.buffered_bytes, &response);
        if (write_result != 0) {
            goto stream_error;
        }
        received_body = (uint64_t)parsed.buffered_bytes;
    }
    while (received_body < parsed.content_length) {
        uint64_t remaining = parsed.content_length - received_body;
        size_t requested = remaining < (uint64_t)FAST_UPLOAD_READ_BYTES
                               ? (size_t)remaining
                               : FAST_UPLOAD_READ_BYTES;
        ssize_t count = recv(connection->descriptor, buffer, requested, 0);
        int write_result;
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            goto stream_error;
        }
        write_result = chunk_mode != 0
                           ? router_upload_stream_write_chunk(
                                 server->router, upload_id,
                                 chunk_offset + received_body, buffer,
                                 (size_t)count, &response)
                           : router_upload_stream_write_direct(
                                 server->router, upload_id, buffer,
                                 (size_t)count, &response);
        if (write_result != 0) {
            goto stream_error;
        }
        received_body += (uint64_t)count;
    }
    if (chunk_mode != 0) {
        if (router_upload_stream_finish_chunk(server->router, upload_id,
                                              chunk_offset, chunk_size,
                                              &response) != 0) {
            router_upload_stream_abort_chunk(server->router, upload_id,
                                             chunk_offset, chunk_size);
            (void)send_response(connection->descriptor, &response,
                                parsed.origin);
            goto done;
        }
    } else {
        router_upload_stream_finish(server->router, upload_id, &response);
    }
    (void)send_response(connection->descriptor, &response, parsed.origin);
    goto done;

stream_error:
    if (stream_started != 0) {
        if (chunk_mode != 0) {
            router_upload_stream_abort_chunk(server->router, upload_id,
                                             chunk_offset, chunk_size);
        } else {
            router_upload_stream_abort(server->router, upload_id);
        }
    }
done:
    if (buffer != NULL) {
        free(buffer);
    }
    (void)shutdown(connection->descriptor, SHUT_RDWR);
    (void)close(connection->descriptor);
    unregister_worker(connection);
    free(connection);
    return NULL;
}

static int reserve_slot(fast_upload_server_t *server, int descriptor,
                        unsigned int *slot) {
    unsigned int index;
    pthread_mutex_lock(&server->mutex);
    if (server->stopping != 0) {
        pthread_mutex_unlock(&server->mutex);
        return -1;
    }
    for (index = 0U; index < FAST_UPLOAD_CONNECTION_LIMIT; ++index) {
        if (server->client_fds[index] < 0) {
            server->client_fds[index] = descriptor;
            ++server->active_workers;
            *slot = index;
            pthread_mutex_unlock(&server->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&server->mutex);
    return -1;
}

static void release_reserved_slot(fast_upload_server_t *server,
                                  unsigned int slot) {
    pthread_mutex_lock(&server->mutex);
    server->client_fds[slot] = -1;
    if (server->active_workers > 0U) {
        --server->active_workers;
    }
    (void)pthread_cond_broadcast(&server->condition);
    pthread_mutex_unlock(&server->mutex);
}

static void tune_socket(int descriptor) {
    int buffer_size = (int)FAST_UPLOAD_SOCKET_BUFFER_BYTES;
    int one = 1;
    struct timeval timeout;
    timeout.tv_sec = FAST_UPLOAD_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &buffer_size,
                     (socklen_t)sizeof(buffer_size));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &buffer_size,
                     (socklen_t)sizeof(buffer_size));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     (socklen_t)sizeof(timeout));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     (socklen_t)sizeof(timeout));
#ifdef TCP_NODELAY
    (void)setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one,
                     (socklen_t)sizeof(one));
#endif
#ifdef SO_NOSIGPIPE
    (void)setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &one,
                     (socklen_t)sizeof(one));
#endif
}

static void *accept_main(void *argument) {
    fast_upload_server_t *server = argument;
    for (;;) {
        struct sockaddr_storage address;
        socklen_t address_size = (socklen_t)sizeof(address);
        fast_connection_t *connection;
        pthread_t thread;
        unsigned int slot;
        int descriptor = accept(server->listener,
                                (struct sockaddr *)(void *)&address,
                                &address_size);
        if (descriptor < 0) {
            if (errno == EINTR) {
                continue;
            }
            pthread_mutex_lock(&server->mutex);
            if (server->stopping != 0) {
                pthread_mutex_unlock(&server->mutex);
                break;
            }
            pthread_mutex_unlock(&server->mutex);
            continue;
        }
        tune_socket(descriptor);
        if (reserve_slot(server, descriptor, &slot) != 0) {
            (void)close(descriptor);
            continue;
        }
        connection = calloc(1U, sizeof(*connection));
        if (connection == NULL) {
            release_reserved_slot(server, slot);
            (void)close(descriptor);
            continue;
        }
        connection->server = server;
        connection->descriptor = descriptor;
        connection->slot = slot;
        connection->address = address;
        if (pthread_create(&thread, NULL, connection_main, connection) != 0) {
            release_reserved_slot(server, slot);
            (void)close(descriptor);
            free(connection);
            continue;
        }
        (void)pthread_detach(thread);
    }
    return NULL;
}

fast_upload_server_t *fast_upload_server_start(router_t *router,
                                               uint16_t ui_port,
                                               uint16_t requested_port) {
    fast_upload_server_t *server;
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int one = 1;
    unsigned int index;
    if (router == NULL || ui_port == 0U) {
        return NULL;
    }
    server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        return NULL;
    }
    server->listener = -1;
    for (index = 0U; index < FAST_UPLOAD_CONNECTION_LIMIT; ++index) {
        server->client_fds[index] = -1;
    }
    if (pthread_mutex_init(&server->mutex, NULL) != 0) {
        free(server);
        return NULL;
    }
    if (pthread_cond_init(&server->condition, NULL) != 0) {
        pthread_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
    server->router = router;
    server->ui_port = ui_port;
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0) {
        goto fail;
    }
    (void)setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &one,
                     (socklen_t)sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(requested_port);
    if (bind(server->listener, (struct sockaddr *)(void *)&address,
             (socklen_t)sizeof(address)) != 0 ||
        listen(server->listener, FAST_UPLOAD_LISTEN_BACKLOG) != 0 ||
        getsockname(server->listener, (struct sockaddr *)(void *)&address,
                    &address_size) != 0) {
        goto fail;
    }
    server->bound_port = ntohs(address.sin_port);
    if (server->bound_port == 0U ||
        pthread_create(&server->accept_thread, NULL, accept_main, server) != 0) {
        goto fail;
    }
    server->accept_thread_started = 1;
    return server;

fail:
    if (server->listener >= 0) {
        (void)close(server->listener);
    }
    pthread_cond_destroy(&server->condition);
    pthread_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
}

uint16_t fast_upload_server_port(const fast_upload_server_t *server) {
    return server == NULL ? 0U : server->bound_port;
}

void fast_upload_server_stop(fast_upload_server_t *server) {
    unsigned int index;
    if (server == NULL) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->stopping = 1;
    for (index = 0U; index < FAST_UPLOAD_CONNECTION_LIMIT; ++index) {
        if (server->client_fds[index] >= 0) {
            (void)shutdown(server->client_fds[index], SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&server->mutex);
    if (server->listener >= 0) {
        (void)shutdown(server->listener, SHUT_RDWR);
        (void)close(server->listener);
        server->listener = -1;
    }
    if (server->accept_thread_started != 0) {
        (void)pthread_join(server->accept_thread, NULL);
    }
    pthread_mutex_lock(&server->mutex);
    while (server->active_workers != 0U) {
        (void)pthread_cond_wait(&server->condition, &server->mutex);
    }
    pthread_mutex_unlock(&server->mutex);
    pthread_cond_destroy(&server->condition);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}
