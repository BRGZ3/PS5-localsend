#define _POSIX_C_SOURCE 200809L

#include "fast_upload_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t received_bytes;
static unsigned int abort_count;

int router_upload_stream_begin_ex(
    router_t *router, const router_request_t *request, int has_content_length,
    uint64_t content_length, char upload_id[UPLOAD_ID_HEX_BYTES + 1U],
    int *chunk_mode, uint64_t *chunk_offset, uint64_t *chunk_size,
    router_response_t *error_response) {
    (void)router;
    (void)error_response;
    assert(request != NULL);
    assert(strcmp(request->method, "PUT") == 0);
    assert(strcmp(request->path,
                  "/api/v1/uploads/0123456789abcdef0123456789abcdef") == 0);
    assert(strcmp(request->client_address, "127.0.0.1") == 0);
    assert(strcmp(request->host, "127.0.0.1:54321") == 0);
    assert(strcmp(request->origin, "http://127.0.0.1:54321") == 0);
    assert(strcmp(request->content_type, "application/octet-stream") == 0);
    assert(strcmp(request->authorization, "Bearer test") == 0);
    assert(has_content_length != 0 && content_length > 0U);
    memcpy(upload_id, "0123456789abcdef0123456789abcdef",
           UPLOAD_ID_HEX_BYTES + 1U);
    *chunk_mode = request->content_range != NULL;
    *chunk_offset = 0U;
    *chunk_size = content_length;
    return 0;
}

int router_upload_stream_write(router_t *router, const char *upload_id,
                               const void *data, size_t size,
                               router_response_t *error_response) {
    (void)router;
    (void)data;
    (void)error_response;
    assert(strcmp(upload_id, "0123456789abcdef0123456789abcdef") == 0);
    pthread_mutex_lock(&state_mutex);
    received_bytes += (uint64_t)size;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

int router_upload_stream_enable_direct(router_t *router,
                                       const char *upload_id,
                                       router_response_t *error_response) {
    (void)router;
    (void)error_response;
    assert(strcmp(upload_id, "0123456789abcdef0123456789abcdef") == 0);
    return 0;
}

int router_upload_stream_write_direct(router_t *router,
                                      const char *upload_id,
                                      const void *data, size_t size,
                                      router_response_t *error_response) {
    return router_upload_stream_write(router, upload_id, data, size,
                                      error_response);
}

int router_upload_stream_write_chunk(router_t *router, const char *upload_id,
                                     uint64_t offset, const void *data,
                                     size_t size,
                                     router_response_t *error_response) {
    (void)offset;
    return router_upload_stream_write(router, upload_id, data, size,
                                      error_response);
}

static void completed_response(router_response_t *response) {
    static const unsigned char body[] = "{\"completed\":true}\n";
    memset(response, 0, sizeof(*response));
    response->status = 201U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = body;
    response->body_size = sizeof(body) - 1U;
}

void router_upload_stream_finish(router_t *router, const char *upload_id,
                                 router_response_t *response) {
    (void)router;
    assert(strcmp(upload_id, "0123456789abcdef0123456789abcdef") == 0);
    completed_response(response);
}

int router_upload_stream_finish_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size,
                                      router_response_t *response) {
    (void)offset;
    (void)size;
    router_upload_stream_finish(router, upload_id, response);
    return 0;
}

void router_upload_stream_abort(router_t *router, const char *upload_id) {
    (void)router;
    (void)upload_id;
    pthread_mutex_lock(&state_mutex);
    ++abort_count;
    pthread_mutex_unlock(&state_mutex);
}

void router_upload_stream_abort_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size) {
    (void)offset;
    (void)size;
    router_upload_stream_abort(router, upload_id);
}

static int connect_to(uint16_t port) {
    struct sockaddr_in address;
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    assert(descriptor >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    address.sin_port = htons(port);
    assert(connect(descriptor, (struct sockaddr *)(void *)&address,
                   (socklen_t)sizeof(address)) == 0);
    return descriptor;
}

static void send_all_test(int descriptor, const void *data, size_t size) {
    const unsigned char *cursor = data;
    while (size > 0U) {
        ssize_t count = send(descriptor, cursor, size, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count > 0);
        cursor += (size_t)count;
        size -= (size_t)count;
    }
}

static size_t read_response(int descriptor, char *buffer, size_t capacity) {
    size_t used = 0U;
    while (used + 1U < capacity) {
        ssize_t count = recv(descriptor, buffer + used, capacity - used - 1U,
                             0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
    }
    buffer[used] = '\0';
    return used;
}

int main(void) {
    static const char upload_path[] =
        "/api/v1/uploads/0123456789abcdef0123456789abcdef";
    router_t router;
    fast_upload_server_t *server;
    uint16_t port;
    char header[1024];
    char response[4096];
    unsigned char *body;
    size_t body_size = 2U * 1024U * 1024U + 17U;
    int descriptor;
    int count;

    memset(&router, 0, sizeof(router));
    server = fast_upload_server_start(&router, 54321U, 0U);
    assert(server != NULL);
    port = fast_upload_server_port(server);
    assert(port != 0U);

    descriptor = connect_to(port);
    count = snprintf(header, sizeof(header),
                     "OPTIONS %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                     "Origin: http://127.0.0.1:54321\r\n\r\n",
                     upload_path, (unsigned int)port);
    assert(count > 0 && (size_t)count < sizeof(header));
    send_all_test(descriptor, header, (size_t)count);
    assert(read_response(descriptor, response, sizeof(response)) > 0U);
    assert(strstr(response, "HTTP/1.1 204 No Content") != NULL);
    assert(strstr(response, "Access-Control-Allow-Origin: "
                            "http://127.0.0.1:54321") != NULL);
    (void)close(descriptor);

    body = malloc(body_size);
    assert(body != NULL);
    memset(body, 0x5a, body_size);
    descriptor = connect_to(port);
    count = snprintf(
        header, sizeof(header),
        "PUT %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Origin: http://127.0.0.1:54321\r\n"
        "Authorization: Bearer test\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %llu\r\n\r\n",
        upload_path, (unsigned int)port, (unsigned long long)body_size);
    assert(count > 0 && (size_t)count < sizeof(header));
    send_all_test(descriptor, header, (size_t)count);
    send_all_test(descriptor, body, body_size);
    assert(read_response(descriptor, response, sizeof(response)) > 0U);
    assert(strstr(response, "HTTP/1.1 201 Created") != NULL);
    assert(strstr(response, "{\"completed\":true}") != NULL);
    (void)close(descriptor);
    pthread_mutex_lock(&state_mutex);
    assert(received_bytes == (uint64_t)body_size);
    pthread_mutex_unlock(&state_mutex);

    descriptor = connect_to(port);
    count = snprintf(
        header, sizeof(header),
        "PUT %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Origin: http://127.0.0.1:54321\r\n"
        "Authorization: Bearer test\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 10\r\n\r\nabc",
        upload_path, (unsigned int)port);
    assert(count > 0 && (size_t)count < sizeof(header));
    send_all_test(descriptor, header, (size_t)count);
    (void)shutdown(descriptor, SHUT_RDWR);
    (void)close(descriptor);
    for (;;) {
        unsigned int observed;
        pthread_mutex_lock(&state_mutex);
        observed = abort_count;
        pthread_mutex_unlock(&state_mutex);
        if (observed != 0U) {
            break;
        }
        {
            struct timespec delay = {0, 1000000L};
            (void)nanosleep(&delay, NULL);
        }
    }

    free(body);
    fast_upload_server_stop(server);
    puts("fast upload server tests passed");
    return 0;
}
