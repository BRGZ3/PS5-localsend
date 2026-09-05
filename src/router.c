#include "router.h"

#include "assets.h"
#include "platform.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_TARGET_INTERNAL "internal"
#define STORAGE_TARGET_CAPACITY CONFIG_STORAGE_ID_CAPACITY

static void clear_secret(void *buffer, size_t size) {
    volatile unsigned char *cursor = buffer;
    while (size > 0U) {
        *cursor++ = 0U;
        --size;
    }
}

static void set_json_error(router_response_t *response, unsigned int status,
                           const char *code, const char *message) {
    int count;
    if (response == NULL) {
        return;
    }
    if (code == NULL) {
        code = "internal_error";
    }
    if (message == NULL) {
        message = "An internal error occurred";
    }
    response->status = status;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
                     code, message);
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        response->storage[0] = '\0';
        response->body_size = 0U;
    } else {
        response->body_size = (size_t)count;
    }
    response->body = (const unsigned char *)response->storage;
}

static int json_escape_filename(const char *input, char *output,
                                size_t output_size) {
    size_t input_index = 0U;
    size_t output_index = 0U;
    if (input == NULL || output == NULL || output_size == 0U) {
        return -1;
    }
    while (input[input_index] != '\0') {
        unsigned char character = (unsigned char)input[input_index++];
        if (character == '"' || character == '\\') {
            if (output_index + 1U >= output_size) {
                return -1;
            }
            output[output_index++] = '\\';
        }
        if (output_index + 1U >= output_size) {
            return -1;
        }
        output[output_index++] = (char)character;
    }
    output[output_index] = '\0';
    return 0;
}

static int valid_host(const char *host) {
    const unsigned char *cursor = (const unsigned char *)host;
    size_t length;
    if (host == NULL) {
        return 0;
    }
    length = strlen(host);
    if (length == 0U || length > 255U) {
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor <= 0x20U || *cursor == 0x7fU || *cursor == '/' ||
            *cursor == '\\' || *cursor == '@' || *cursor == '#') {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

static int same_origin(const router_request_t *request) {
    char expected[263];
    int count;
    if (!valid_host(request->host) || request->origin == NULL) {
        return 0;
    }
    count = snprintf(expected, sizeof(expected), "http://%s", request->host);
    return count > 0 && (size_t)count < sizeof(expected) &&
           strcmp(request->origin, expected) == 0;
}

static int is_json_content_type(const char *content_type) {
    static const char base[] = "application/json";
    const char *suffix;
    if (content_type == NULL || strncmp(content_type, base, sizeof(base) - 1U) != 0) {
        return 0;
    }
    suffix = content_type + sizeof(base) - 1U;
    if (*suffix == '\0') {
        return 1;
    }
    while (isspace((unsigned char)*suffix) != 0) {
        ++suffix;
    }
    return strcmp(suffix, "; charset=utf-8") == 0 ||
           strcmp(suffix, ";charset=utf-8") == 0;
}

static void skip_json_whitespace(const unsigned char **cursor,
                                 const unsigned char *end) {
    while (*cursor < end && isspace(**cursor) != 0) {
        ++*cursor;
    }
}

static int parse_json_string_token(const unsigned char **cursor,
                                   const unsigned char *end, char *output,
                                   size_t output_size) {
    size_t length = 0U;
    if (*cursor >= end || *(*cursor)++ != '"') {
        return -1;
    }
    while (*cursor < end && **cursor != '"') {
        unsigned char character = *(*cursor)++;
        if (character == '\\' || character < 0x20U ||
            length + 1U >= output_size) {
            return -1;
        }
        output[length++] = (char)character;
    }
    if (*cursor >= end || *(*cursor)++ != '"') {
        return -1;
    }
    output[length] = '\0';
    return 0;
}

static int parse_storage_target(const unsigned char *body, size_t body_size,
                                char target[STORAGE_TARGET_CAPACITY]) {
    const unsigned char *cursor = body;
    const unsigned char *end = body == NULL ? NULL : body + body_size;
    char key[16];
    if (body == NULL || body_size == 0U) {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (cursor >= end || *cursor++ != '{') {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (parse_json_string_token(&cursor, end, key, sizeof(key)) != 0 ||
        strcmp(key, "target") != 0) {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (cursor >= end || *cursor++ != ':') {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (parse_json_string_token(&cursor, end, target,
                                STORAGE_TARGET_CAPACITY) != 0) {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (cursor >= end || *cursor++ != '}') {
        return -1;
    }
    skip_json_whitespace(&cursor, end);
    if (cursor != end || target[0] == '\0') return -1;
    {
        const unsigned char *character = (const unsigned char *)target;
        while (*character != '\0') {
            if (!isalnum(*character) && *character != '_' && *character != '-')
                return -1;
            ++character;
        }
    }
    return 0;
}

static const char *storage_path_for_target(const app_config_t *config,
                                           const char *target) {
    const config_storage_target_t *entry = config_storage_by_id(config, target);
    return entry == NULL ? NULL : entry->path;
}

static const char *storage_target_for_path(const app_config_t *config,
                                           const char *path) {
    const config_storage_target_t *entry = config_storage_by_path(config, path);
    if (entry != NULL) return entry->id;
    return config->storage_target_count > 0U ? config->storage_targets[0].id
                                             : STORAGE_TARGET_INTERNAL;
}

static int authorized(const router_t *router, const router_request_t *request) {
    if (router->auth_mode == CONFIG_AUTH_NONE) {
        return 1;
    }
    return auth_validate_bearer(router->auth, request->client_address,
                                request->authorization) == AUTH_OK;
}

static const char *request_authorization(const router_t *router,
                                         const router_request_t *request) {
    return router->auth_mode == CONFIG_AUTH_NONE
               ? "NoAuth"
               : request->authorization;
}

static int pin_auth_enabled(router_t *router) {
    return router->auth_mode == CONFIG_AUTH_PIN;
}

static void set_upload_error(router_response_t *response, upload_result_t result,
                             int error_number) {
    char message[256];
    const char *description;
    int count;
    switch (result) {
        case UPLOAD_INVALID: set_json_error(response, 400U, "invalid_upload", "Upload request is malformed"); break;
        case UPLOAD_NOT_FOUND: set_json_error(response, 404U, "upload_not_found", "Upload does not exist"); break;
        case UPLOAD_FORBIDDEN: set_json_error(response, 403U, "upload_forbidden", "Upload belongs to another session"); break;
        case UPLOAD_BUSY: set_json_error(response, 409U, "upload_busy", "Another upload is active"); break;
        case UPLOAD_TOO_LARGE: set_json_error(response, 413U, "file_too_large", "File exceeds the configured limit"); break;
        case UPLOAD_STORAGE_FULL: set_json_error(response, 507U, "insufficient_storage", "Destination does not have enough space"); break;
        case UPLOAD_SIZE_MISMATCH: set_json_error(response, 400U, "size_mismatch", "Content-Length or received size does not match metadata"); break;
        case UPLOAD_HASH_MISMATCH: set_json_error(response, 422U, "hash_mismatch", "SHA-256 does not match metadata"); break;
        case UPLOAD_LIMIT_REACHED: set_json_error(response, 429U, "file_limit_reached", "Session file limit reached"); break;
        case UPLOAD_BACKPRESSURE: set_json_error(response, 429U, "upload_backpressure", "Upload staging window is full; retry this chunk"); break;
        case UPLOAD_IO_ERROR:
            description = error_number > 0 ? strerror(error_number) : NULL;
            if (description != NULL) {
                count = snprintf(message, sizeof(message),
                                 "Unable to write destination storage "
                                 "(errno %d: %s)", error_number, description);
                if (count > 0 && (size_t)count < sizeof(message)) {
                    set_json_error(response, 500U, "storage_error", message);
                    break;
                }
            }
            set_json_error(response, 500U, "storage_error",
                           "Unable to write destination storage");
            break;
        default: set_json_error(response, 500U, "internal_error", "Upload failed"); break;
    }
}

static void set_upload_completed_response(router_response_t *response,
                                          const char *digest,
                                          const upload_metrics_t *metrics) {
    int count;
    if (response == NULL || digest == NULL || metrics == NULL) {
        return;
    }
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"completed\":true,\"sha256\":\"%s\","
                     "\"timing\":{\"preparationMs\":%llu,"
                     "\"transferMs\":%llu,\"writeMs\":%llu,"
                     "\"directIO\":%s}}\n",
                     digest,
                     (unsigned long long)metrics->preparation_ms,
                     (unsigned long long)metrics->transfer_ms,
                     (unsigned long long)metrics->write_ms,
                     metrics->direct_io != 0 ? "true" : "false");
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return;
    }
    response->status = 201U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = (const unsigned char *)response->storage;
    response->body_size = (size_t)count;
}

static int json_escape_string(const char *input, char *output,
                              size_t output_size) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *cursor = (const unsigned char *)input;
    size_t used = 0U;
    if (input == NULL || output == NULL || output_size == 0U) return -1;
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\\') {
            if (used + 2U >= output_size) return -1;
            output[used++] = '\\'; output[used++] = (char)*cursor;
        } else if (*cursor < 0x20U) {
            if (used + 6U >= output_size) return -1;
            output[used++] = '\\'; output[used++] = 'u';
            output[used++] = '0'; output[used++] = '0';
            output[used++] = hex[*cursor >> 4U];
            output[used++] = hex[*cursor & 0x0fU];
        } else {
            if (used + 1U >= output_size) return -1;
            output[used++] = (char)*cursor;
        }
        ++cursor;
    }
    output[used] = '\0';
    return 0;
}

static int upload_id_from_path(const char *path,
                               char output[UPLOAD_ID_HEX_BYTES + 1U]) {
    static const char prefix[] = "/api/v1/uploads/";
    size_t index;
    if (strncmp(path, prefix, sizeof(prefix) - 1U) != 0 ||
        strlen(path + sizeof(prefix) - 1U) != UPLOAD_ID_HEX_BYTES) return 0;
    memcpy(output, path + sizeof(prefix) - 1U, UPLOAD_ID_HEX_BYTES + 1U);
    for (index = 0U; index < UPLOAD_ID_HEX_BYTES; ++index)
        if (!isdigit((unsigned char)output[index]) &&
            (output[index] < 'a' || output[index] > 'f')) return 0;
    return 1;
}

static int upload_complete_from_path(
    const char *path, char output[UPLOAD_ID_HEX_BYTES + 1U]) {
    static const char prefix[] = "/api/v1/uploads/";
    static const char suffix[] = "/complete";
    size_t prefix_size = sizeof(prefix) - 1U;
    size_t suffix_size = sizeof(suffix) - 1U;
    size_t path_size;
    size_t index;
    if (path == NULL || output == NULL) {
        return 0;
    }
    path_size = strlen(path);
    if (path_size != prefix_size + UPLOAD_ID_HEX_BYTES + suffix_size ||
        strncmp(path, prefix, prefix_size) != 0 ||
        strcmp(path + prefix_size + UPLOAD_ID_HEX_BYTES, suffix) != 0) {
        return 0;
    }
    memcpy(output, path + prefix_size, UPLOAD_ID_HEX_BYTES);
    output[UPLOAD_ID_HEX_BYTES] = '\0';
    for (index = 0U; index < UPLOAD_ID_HEX_BYTES; ++index) {
        if (!isdigit((unsigned char)output[index]) &&
            (output[index] < 'a' || output[index] > 'f')) {
            return 0;
        }
    }
    return 1;
}

static int parse_range_number(const char **cursor, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;
    if (cursor == NULL || *cursor == NULL || value == NULL ||
        !isdigit((unsigned char)**cursor)) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(*cursor, &end, 10);
    if (errno != 0 || end == *cursor) {
        return -1;
    }
    *cursor = end;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_content_range(const char *value, uint64_t *offset,
                               uint64_t *length, uint64_t *total) {
    const char *cursor;
    uint64_t end;
    uint64_t parsed_total;
    if (value == NULL || offset == NULL || length == NULL || total == NULL ||
        strncmp(value, "bytes ", sizeof("bytes ") - 1U) != 0) {
        return -1;
    }
    cursor = value + sizeof("bytes ") - 1U;
    if (parse_range_number(&cursor, offset) != 0 || *cursor++ != '-' ||
        parse_range_number(&cursor, &end) != 0 || *cursor++ != '/' ||
        parse_range_number(&cursor, &parsed_total) != 0 || *cursor != '\0' ||
        end < *offset || parsed_total == 0U || end >= parsed_total) {
        return -1;
    }
    *length = end - *offset + 1U;
    *total = parsed_total;
    return 0;
}

static void dispatch_upload_prepare(router_t *router,
                                    const router_request_t *request,
                                    router_response_t *response) {
    upload_prepared_t prepared;
    upload_result_t result;
    char escaped_name[sizeof(response->storage)];
    uint16_t fast_upload_port;
    int count;
    if (strcmp(request->method, "POST") != 0) {
        set_json_error(response, 405U, "method_not_allowed", "Only POST is allowed for this resource");
        response->allow = "POST"; return;
    }
    if (!same_origin(request)) { set_json_error(response, 403U, "origin_forbidden", "Origin and Host must match this server"); return; }
    if (!authorized(router, request)) { set_json_error(response, 401U, "unauthorized", "A valid bearer session is required"); return; }
    if (request->body_size > ROUTER_MAX_METADATA_BODY_BYTES) { set_json_error(response, 413U, "body_too_large", "Upload metadata is too large"); return; }
    if (!is_json_content_type(request->content_type)) { set_json_error(response, 415U, "unsupported_media_type", "Content-Type must be application/json"); return; }
    pthread_mutex_lock(&router->mutex);
    result = upload_prepare(router->uploads, request->client_address,
                            request_authorization(router, request), request->body,
                            request->body_size, &prepared);
    fast_upload_port = router->fast_upload_port;
    pthread_mutex_unlock(&router->mutex);
    if (result != UPLOAD_OK) { int error_number = errno; set_upload_error(response, result, error_number); return; }
    if (json_escape_string(prepared.final_name, escaped_name,
                           sizeof(escaped_name)) != 0) {
        upload_cancel(router->uploads, prepared.upload_id,
                      request->client_address,
                      request_authorization(router, request));
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return;
    }
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"uploadId\":\"%s\",\"name\":\"%s\","
                     "\"size\":%llu,\"chunkSize\":%u,"
                     "\"chunkParallelism\":%u,\"fastUploadPort\":%u,"
                     "\"fastUploadMode\":\"direct\"}\n",
                     prepared.upload_id, escaped_name,
                     (unsigned long long)prepared.size, UPLOAD_CHUNK_BYTES,
                     UPLOAD_CHUNK_PARALLELISM,
                     (unsigned int)fast_upload_port);
    if (count < 0 || (size_t)count >= sizeof(response->storage)) { upload_cancel(router->uploads, prepared.upload_id, request->client_address, request_authorization(router, request)); set_json_error(response, 500U, "internal_error", "Unable to create response"); return; }
    response->status = 201U; response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store"; response->body = (const unsigned char *)response->storage; response->body_size = (size_t)count;
}

static void dispatch_upload_delete(router_t *router,
                                   const router_request_t *request,
                                   router_response_t *response,
                                   const char *upload_id) {
    upload_result_t result;
    if (strcmp(request->method, "DELETE") != 0) {
        set_json_error(response, 405U, "method_not_allowed", "Only PUT and DELETE are allowed for this resource");
        response->allow = "PUT, DELETE"; return;
    }
    if (request->body_size != 0U) { set_json_error(response, 413U, "body_too_large", "This resource does not accept a request body"); return; }
    if (!same_origin(request)) { set_json_error(response, 403U, "origin_forbidden", "Origin and Host must match this server"); return; }
    if (!authorized(router, request)) { set_json_error(response, 401U, "unauthorized", "A valid bearer session is required"); return; }
    result = upload_cancel(router->uploads, upload_id, request->client_address,
                           request_authorization(router, request));
    if (result != UPLOAD_OK) { int error_number = errno; set_upload_error(response, result, error_number); return; }
    response->status = 200U; response->content_type = "application/json; charset=utf-8"; response->cache_control = "no-store";
    memcpy(response->storage, "{\"cancelled\":true}\n", 20U); response->body = (const unsigned char *)response->storage; response->body_size = 19U;
}

static void dispatch_upload_complete(router_t *router,
                                     const router_request_t *request,
                                     router_response_t *response,
                                     const char *upload_id) {
    char digest[SHA256_HEX_BYTES + 1U];
    upload_metrics_t metrics;
    upload_result_t result;
    if (strcmp(request->method, "POST") != 0) {
        set_json_error(response, 405U, "method_not_allowed",
                       "Only POST is allowed for this resource");
        response->allow = "POST";
        return;
    }
    if (request->body_size != 0U) {
        set_json_error(response, 413U, "body_too_large",
                       "This resource does not accept a request body");
        return;
    }
    if (!same_origin(request)) {
        set_json_error(response, 403U, "origin_forbidden",
                       "Origin and Host must match this server");
        return;
    }
    if (!authorized(router, request)) {
        set_json_error(response, 401U, "unauthorized",
                       "A valid bearer session is required");
        return;
    }
    result = upload_finish_chunks_with_metrics(
        router->uploads, upload_id, request->client_address,
        request_authorization(router, request), digest, &metrics);
    if (result != UPLOAD_OK) {
        int error_number = errno;
        set_upload_error(response, result, error_number);
        return;
    }
    set_upload_completed_response(response, digest, &metrics);
}

static void set_storage_error(router_response_t *response,
                              upload_result_t result) {
    if (result == UPLOAD_BUSY) {
        set_json_error(response, 409U, "storage_busy",
                       "Finish or cancel the active upload before changing storage");
    } else if (result == UPLOAD_STORAGE_FULL) {
        set_json_error(response, 507U, "storage_unavailable",
                       "The selected storage does not have enough space");
    } else {
        set_json_error(response, 503U, "storage_unavailable",
                       "The selected storage is not mounted or cannot be created");
    }
}

static void dispatch_storage(router_t *router,
                             const router_request_t *request,
                             router_response_t *response) {
    char target[STORAGE_TARGET_CAPACITY];
    char previous_destination[CONFIG_DESTINATION_CAPACITY];
    app_config_t next_config;
    upload_result_t result;
    const char *destination;
    int count;

    if (strcmp(request->method, "POST") != 0) {
        set_json_error(response, 405U, "method_not_allowed",
                       "Only POST is allowed for this resource");
        response->allow = "POST";
        return;
    }
    if (!same_origin(request)) {
        set_json_error(response, 403U, "origin_forbidden",
                       "Origin and Host must match this server");
        return;
    }
    if (!authorized(router, request)) {
        set_json_error(response, 401U, "unauthorized",
                       "A valid bearer session is required");
        return;
    }
    if (request->body_size > ROUTER_MAX_METADATA_BODY_BYTES) {
        set_json_error(response, 413U, "body_too_large",
                       "Storage settings are too large");
        return;
    }
    if (!is_json_content_type(request->content_type)) {
        set_json_error(response, 415U, "unsupported_media_type",
                       "Content-Type must be application/json");
        return;
    }
    if (parse_storage_target(request->body, request->body_size, target) != 0) {
        set_json_error(response, 400U, "invalid_storage",
                       "Expected a configured storage target ID");
        return;
    }
    pthread_mutex_lock(&router->mutex);
    destination = storage_path_for_target(&router->config, target);
    if (destination == NULL) {
        pthread_mutex_unlock(&router->mutex);
        set_json_error(response, 400U, "invalid_storage",
                       "Unknown configured storage target");
        return;
    }
    memcpy(previous_destination, router->config.destination,
           sizeof(previous_destination));
    result = upload_manager_set_destination(router->uploads, destination);
    if (result != UPLOAD_OK) {
        pthread_mutex_unlock(&router->mutex);
        set_storage_error(response, result);
        return;
    }
    next_config = router->config;
    memcpy(next_config.destination, destination, strlen(destination) + 1U);
    if (config_save_file(router->config_path, &next_config) != 0) {
        (void)upload_manager_set_destination(router->uploads,
                                              previous_destination);
        pthread_mutex_unlock(&router->mutex);
        set_json_error(response, 500U, "storage_config_error",
                       "Unable to persist the storage setting");
        return;
    }
    router->config = next_config;
    pthread_mutex_unlock(&router->mutex);

    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"target\":\"%s\",\"path\":\"%s\"}\n",
                     target, destination);
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return;
    }
    response->status = 200U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = (const unsigned char *)response->storage;
    response->body_size = (size_t)count;
}

static void dispatch_challenge(router_t *router,
                               const router_request_t *request,
                               router_response_t *response) {
    auth_challenge_result_t challenge;
    auth_result_t result;
    char notification[192];
    int count;
    if (!pin_auth_enabled(router)) {
        set_json_error(response, 409U, "auth_disabled",
                       "PIN authentication is disabled by configuration");
        return;
    }
    if (strcmp(request->method, "POST") != 0) {
        set_json_error(response, 405U, "method_not_allowed",
                       "Only POST is allowed for this resource");
        response->allow = "POST";
        return;
    }
    if (request->body_size != 0U) {
        set_json_error(response, 413U, "body_too_large",
                       "This resource does not accept a request body");
        return;
    }
    if (!same_origin(request)) {
        set_json_error(response, 403U, "origin_forbidden",
                       "Origin and Host must match this server");
        return;
    }
    result = auth_create_challenge(router->auth, request->client_address,
                                   &challenge);
    if (result == AUTH_CAPACITY) {
        set_json_error(response, 429U, "too_many_challenges",
                       "Too many authentication challenges are active");
        return;
    }
    if (result == AUTH_RATE_LIMITED) {
        set_json_error(response, 429U, "challenge_rate_limited",
                       "Wait before requesting another PIN");
        return;
    }
    if (result != AUTH_OK) {
        set_json_error(response, 500U, "auth_unavailable",
                       "Unable to create an authentication challenge");
        return;
    }
    count = snprintf(notification, sizeof(notification),
                     "PS5 LocalSend PIN %s for %s", challenge.pin,
                     request->client_address);
    if (count < 0 || (size_t)count >= sizeof(notification) ||
        platform_notify(notification) != 0) {
        clear_secret(&challenge, sizeof(challenge));
        clear_secret(notification, sizeof(notification));
        set_json_error(response, 500U, "notification_failed",
                       "Unable to show the PIN on PS5");
        return;
    }
    clear_secret(notification, sizeof(notification));
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"challengeId\":\"%s\",\"expiresIn\":%u}\n",
                     challenge.challenge_id, challenge.expires_in_seconds);
    clear_secret(&challenge, sizeof(challenge));
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return;
    }
    response->status = 201U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = (const unsigned char *)response->storage;
    response->body_size = (size_t)count;
}

static void dispatch_verify(router_t *router, const router_request_t *request,
                            router_response_t *response) {
    char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
    char pin[AUTH_PIN_DIGITS + 1U];
    auth_session_result_t session;
    auth_result_t result;
    int count;
    if (!pin_auth_enabled(router)) {
        set_json_error(response, 409U, "auth_disabled",
                       "PIN authentication is disabled by configuration");
        return;
    }
    if (strcmp(request->method, "POST") != 0) {
        set_json_error(response, 405U, "method_not_allowed",
                       "Only POST is allowed for this resource");
        response->allow = "POST";
        return;
    }
    if (request->body_size > ROUTER_MAX_AUTH_BODY_BYTES) {
        set_json_error(response, 413U, "body_too_large",
                       "Authentication JSON is too large");
        return;
    }
    if (!same_origin(request)) {
        set_json_error(response, 403U, "origin_forbidden",
                       "Origin and Host must match this server");
        return;
    }
    if (!is_json_content_type(request->content_type)) {
        set_json_error(response, 415U, "unsupported_media_type",
                       "Content-Type must be application/json");
        return;
    }
    if (auth_parse_verify_json(request->body, request->body_size, challenge_id,
                               pin) != 0) {
        set_json_error(response, 400U, "invalid_json",
                       "Expected exactly challengeId and a six-digit pin");
        return;
    }
    result = auth_verify_challenge(router->auth, request->client_address,
                                   challenge_id, pin, &session);
    clear_secret(challenge_id, sizeof(challenge_id));
    clear_secret(pin, sizeof(pin));
    if (result == AUTH_RATE_LIMITED) {
        set_json_error(response, 429U, "too_many_attempts",
                       "This challenge has reached its attempt limit");
        return;
    }
    if (result == AUTH_EXPIRED) {
        set_json_error(response, 401U, "challenge_expired",
                       "This authentication challenge has expired");
        return;
    }
    if (result == AUTH_USED) {
        set_json_error(response, 401U, "challenge_used",
                       "This authentication challenge was already used");
        return;
    }
    if (result != AUTH_OK) {
        set_json_error(response, 401U, "invalid_credentials",
                       "The challenge or PIN is invalid");
        return;
    }
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"token\":\"%s\",\"tokenType\":\"Bearer\","
                     "\"expiresIn\":%u}\n",
                     session.token, session.expires_in_seconds);
    clear_secret(&session, sizeof(session));
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return;
    }
    response->status = 200U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = (const unsigned char *)response->storage;
    response->body_size = (size_t)count;
}

int router_init(router_t *router, const app_config_t *config) {
    int count;
    if (router == NULL || config == NULL) {
        return -1;
    }
    memset(router, 0, sizeof(*router));
    if (pthread_mutex_init(&router->mutex, NULL) != 0) {
        return -1;
    }
    router->config = *config;
    router->auth_mode = config->auth_mode;
    count = snprintf(router->config_path, sizeof(router->config_path),
                     "%s/config.ini", platform_data_dir());
    if (count < 0 || (size_t)count >= sizeof(router->config_path)) {
        pthread_mutex_destroy(&router->mutex);
        memset(router, 0, sizeof(*router));
        return -1;
    }
    router->auth = auth_manager_create(config->pin_ttl_seconds,
                                       config->session_ttl_seconds);
    router->uploads = upload_manager_create(config);
    if (router->auth == NULL || router->uploads == NULL) {
        auth_manager_destroy(router->auth);
        upload_manager_destroy(router->uploads);
        pthread_mutex_destroy(&router->mutex);
        memset(router, 0, sizeof(*router));
        return -1;
    }
    return 0;
}

void router_destroy(router_t *router) {
    if (router == NULL) {
        return;
    }
    auth_manager_destroy(router->auth);
    upload_manager_destroy(router->uploads);
    pthread_mutex_destroy(&router->mutex);
    memset(router, 0, sizeof(*router));
}

void router_set_port(router_t *router, uint16_t port) {
    if (router == NULL) {
        return;
    }
    pthread_mutex_lock(&router->mutex);
    router->config.port = port;
    pthread_mutex_unlock(&router->mutex);
}

void router_set_fast_upload_port(router_t *router, uint16_t port) {
    if (router == NULL) {
        return;
    }
    pthread_mutex_lock(&router->mutex);
    router->fast_upload_port = port;
    pthread_mutex_unlock(&router->mutex);
}

void router_dispatch(router_t *router, const router_request_t *request,
                     router_response_t *response) {
    const embedded_asset_t *asset;
    upload_snapshot_t snapshot;
    char escaped_name[SAFE_PATH_NAME_BYTES * 2U + 1U];
    char last_upload[256];
    const char *transfer_state;
    int count;
    if (response == NULL) {
        return;
    }
    memset(response, 0, sizeof(*response));
    if (router == NULL || router->auth == NULL || router->uploads == NULL ||
        request == NULL ||
        request->method == NULL || request->path == NULL ||
        strlen(request->method) > ROUTER_MAX_METHOD_BYTES ||
        strlen(request->path) > ROUTER_MAX_PATH_BYTES) {
        set_json_error(response, 400U, "bad_request", "Malformed request");
        return;
    }
    if (strcmp(request->path, "/api/v1/auth/challenge") == 0) {
        dispatch_challenge(router, request, response);
        return;
    }
    if (strcmp(request->path, "/api/v1/auth/verify") == 0) {
        dispatch_verify(router, request, response);
        return;
    }
    if (strcmp(request->path, "/api/v1/uploads") == 0) {
        dispatch_upload_prepare(router, request, response); return;
    }
    if (strcmp(request->path, "/api/v1/storage") == 0) {
        dispatch_storage(router, request, response); return;
    }
    {
        char upload_id[UPLOAD_ID_HEX_BYTES + 1U];
        if (upload_complete_from_path(request->path, upload_id)) {
            dispatch_upload_complete(router, request, response, upload_id);
            return;
        }
        if (upload_id_from_path(request->path, upload_id)) {
            if (strcmp(request->method, "PUT") == 0) {
                set_json_error(response, 400U, "stream_required", "PUT must use the streaming transport");
                return;
            }
            dispatch_upload_delete(router, request, response, upload_id); return;
        }
    }
    asset = assets_find(request->path);
    if (asset != NULL || strcmp(request->path, "/api/v1/status") == 0) {
        if (strcmp(request->method, "GET") != 0) {
            set_json_error(response, 405U, "method_not_allowed",
                           "Only GET is allowed for this resource");
            response->allow = "GET";
            return;
        }
        if (request->body_size > 0U) {
            set_json_error(response, 413U, "body_too_large",
                           "This resource does not accept a request body");
            return;
        }
    }
    if (asset != NULL) {
        response->status = 200U;
        response->content_type = asset->content_type;
        response->cache_control = strcmp(request->path, "/") == 0
                                      ? "no-cache"
                                      : ((strcmp(request->path, "/assets/app.css") == 0 ||
                                          strcmp(request->path, "/assets/app.js") == 0)
                                             ? "no-store"
                                             : "public, max-age=3600");
        response->body = asset->data;
        response->body_size = asset->size;
        return;
    }
    if (strcmp(request->path, "/api/v1/status") == 0) {
        app_config_t config_snapshot;
        uint16_t fast_upload_port;
        char storage_targets[12288];
        size_t storage_targets_used = 0U;
        size_t storage_index;
        pthread_mutex_lock(&router->mutex);
        config_snapshot = router->config;
        fast_upload_port = router->fast_upload_port;
        pthread_mutex_unlock(&router->mutex);
        storage_targets[storage_targets_used++] = '[';
        storage_targets[storage_targets_used] = '\0';
        for (storage_index = 0U;
             storage_index < config_snapshot.storage_target_count;
             ++storage_index) {
            const config_storage_target_t *target =
                &config_snapshot.storage_targets[storage_index];
            int target_count = snprintf(
                storage_targets + storage_targets_used,
                sizeof(storage_targets) - storage_targets_used,
                "%s{\"id\":\"%s\",\"label\":\"%s\",\"path\":\"%s\"}",
                storage_index == 0U ? "" : ",", target->id, target->label,
                target->path);
            if (target_count < 0 ||
                (size_t)target_count >=
                    sizeof(storage_targets) - storage_targets_used) {
                set_json_error(response, 500U, "internal_error",
                               "Configured storage list is too large");
                return;
            }
            storage_targets_used += (size_t)target_count;
        }
        if (storage_targets_used + 2U > sizeof(storage_targets)) {
            set_json_error(response, 500U, "internal_error",
                           "Configured storage list is too large");
            return;
        }
        storage_targets[storage_targets_used++] = ']';
        storage_targets[storage_targets_used] = '\0';
        if (upload_snapshot(router->uploads, &snapshot) != 0 ||
            json_escape_filename(snapshot.name, escaped_name,
                                 sizeof(escaped_name)) != 0) {
            set_json_error(response, 500U, "internal_error",
                           "Unable to read transfer status");
            return;
        }
        transfer_state = snapshot.active == 0
                             ? "idle"
                             : (snapshot.started == 0 ? "preparing" : "receiving");
        if (snapshot.has_last_metrics) {
            int timing_count = snprintf(
                last_upload, sizeof(last_upload),
                "{\"bytes\":%llu,\"timing\":{\"preparationMs\":%llu,"
                "\"transferMs\":%llu,\"writeMs\":%llu,\"directIO\":%s}}",
                (unsigned long long)snapshot.last_completed_size,
                (unsigned long long)snapshot.last_metrics.preparation_ms,
                (unsigned long long)snapshot.last_metrics.transfer_ms,
                (unsigned long long)snapshot.last_metrics.write_ms,
                snapshot.last_metrics.direct_io != 0 ? "true" : "false");
            if (timing_count < 0 || (size_t)timing_count >= sizeof(last_upload)) {
                set_json_error(response, 500U, "internal_error",
                               "Unable to create transfer diagnostics");
                return;
            }
        } else {
            memcpy(last_upload, "null", sizeof("null"));
        }
        count = snprintf(response->storage, sizeof(response->storage),
                         "{\"version\":\"" PS5LOCALSEND_VERSION
                         "\",\"authMode\":\"%s\",\"language\":\"%s\","
                         "\"ready\":true,\"limits\":{\"maxFileBytes\":%llu,"
                         "\"maxFilesPerSession\":%u},\"transfer\":{"
                         "\"state\":\"%s\",\"active\":%s,\"name\":\"%s\","
                         "\"receivedBytes\":%llu,\"expectedBytes\":%llu},"
                         "\"capabilities\":{\"chunkUpload\":true,"
                         "\"chunkSize\":%u,\"chunkParallelism\":%u,"
                         "\"fastUploadPort\":%u,"
                         "\"fastUploadMode\":\"direct\"},"
                         "\"storage\":{\"target\":\"%s\","
                         "\"path\":\"%s\",\"targets\":%s},"
                         "\"lastUpload\":%s}\n",
                         config_auth_mode_name(config_snapshot.auth_mode),
                         config_language_name(config_snapshot.language),
                         (unsigned long long)config_snapshot.max_file_bytes,
                         config_snapshot.max_files_per_session,
                         transfer_state, snapshot.active == 0 ? "false" : "true",
                         escaped_name,
                         (unsigned long long)snapshot.received,
                         (unsigned long long)snapshot.expected,
                         UPLOAD_CHUNK_BYTES, UPLOAD_CHUNK_PARALLELISM,
                         (unsigned int)fast_upload_port,
                         storage_target_for_path(&config_snapshot,
                                                 config_snapshot.destination),
                         config_snapshot.destination,
                         storage_targets,
                         last_upload);
        if (count < 0 || (size_t)count >= sizeof(response->storage)) {
            set_json_error(response, 500U, "internal_error",
                           "Unable to create response");
            return;
        }
        response->status = 200U;
        response->content_type = "application/json; charset=utf-8";
        response->cache_control = "no-store";
        response->body = (const unsigned char *)response->storage;
        response->body_size = (size_t)count;
        return;
    }
    set_json_error(response, 404U, "not_found", "Resource not found");
}

int router_upload_stream_begin_ex(
    router_t *router, const router_request_t *request, int has_content_length,
    uint64_t content_length, char upload_id[UPLOAD_ID_HEX_BYTES + 1U],
    int *chunk_mode, uint64_t *chunk_offset, uint64_t *chunk_size,
    router_response_t *error_response) {
    upload_result_t result;
    uint64_t range_offset = 0U;
    uint64_t range_length = 0U;
    uint64_t range_total = 0U;
    int is_chunk = 0;
    if (error_response == NULL || upload_id == NULL) return -1;
    memset(error_response, 0, sizeof(*error_response));
    if (chunk_mode != NULL) *chunk_mode = 0;
    if (chunk_offset != NULL) *chunk_offset = 0U;
    if (chunk_size != NULL) *chunk_size = 0U;
    if (router == NULL || router->auth == NULL || router->uploads == NULL ||
        request == NULL || request->method == NULL || request->path == NULL ||
        request->client_address == NULL || strcmp(request->method, "PUT") != 0 ||
        !upload_id_from_path(request->path, upload_id)) {
        set_json_error(error_response, 404U, "upload_not_found",
                       "Upload does not exist");
        return -1;
    }
    if (!same_origin(request)) {
        set_json_error(error_response, 403U, "origin_forbidden",
                       "Origin and Host must match this server");
        return -1;
    }
    if (!authorized(router, request)) {
        set_json_error(error_response, 401U, "unauthorized",
                       "A valid bearer session is required");
        return -1;
    }
    if (request->content_type == NULL ||
        strcmp(request->content_type, "application/octet-stream") != 0) {
        set_json_error(error_response, 415U, "unsupported_media_type",
                       "Content-Type must be application/octet-stream");
        return -1;
    }
    is_chunk = request->content_range != NULL &&
               request->content_range[0] != '\0';
    if (is_chunk && parse_content_range(request->content_range, &range_offset,
                                        &range_length, &range_total) != 0) {
        set_json_error(error_response, 400U, "invalid_range",
                       "Content-Range must be bytes start-end/total");
        return -1;
    }
    if (!has_content_length) {
        if (!is_chunk) {
            (void)upload_cancel(router->uploads, upload_id,
                                request->client_address,
                                request_authorization(router, request));
            set_json_error(error_response, 411U, "length_required",
                           "Content-Length is required");
            return -1;
        }
        /* Chunked transfer coding is safe here because Content-Range gives us
         * the exact extent and upload_chunk_finish still requires every byte
         * of that extent.  The legacy whole-file PUT keeps its strict
         * Content-Length requirement for compatibility and early rejection. */
        content_length = range_length;
    }
    if (is_chunk) {
        if (content_length != range_length) {
            set_json_error(error_response, 400U, "size_mismatch",
                           "Content-Length does not match Content-Range");
            return -1;
        }
        result = upload_chunk_begin(router->uploads, upload_id,
                                    request->client_address,
                                    request_authorization(router, request), range_total,
                                    range_offset, range_length);
        if (result == UPLOAD_OK) {
            if (chunk_mode != NULL) *chunk_mode = 1;
            if (chunk_offset != NULL) *chunk_offset = range_offset;
            if (chunk_size != NULL) *chunk_size = range_length;
            return 0;
        }
    } else {
        result = upload_begin(router->uploads, upload_id,
                              request->client_address,
                              request_authorization(router, request), content_length);
    }
    if (result != UPLOAD_OK) {
        if (!is_chunk && result == UPLOAD_SIZE_MISMATCH) {
            (void)upload_cancel(router->uploads, upload_id,
                                request->client_address,
                                request_authorization(router, request));
        }
        {
            int error_number = errno;
            set_upload_error(error_response, result, error_number);
        }
        return -1;
    }
    return 0;
}

int router_upload_stream_begin(router_t *router, const router_request_t *request,
                               int has_content_length, uint64_t content_length,
                               char upload_id[UPLOAD_ID_HEX_BYTES + 1U],
                               router_response_t *error_response) {
    return router_upload_stream_begin_ex(
        router, request, has_content_length, content_length, upload_id, NULL,
        NULL, NULL, error_response);
}

int router_upload_stream_write(router_t *router, const char *upload_id,
                               const void *data, size_t size,
                               router_response_t *error_response) {
    upload_result_t result;
    if (error_response == NULL) return -1;
    memset(error_response, 0, sizeof(*error_response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL ||
        (data == NULL && size != 0U)) {
        set_json_error(error_response, 400U, "bad_request", "Malformed upload stream");
        return -1;
    }
    result = upload_write(router->uploads, upload_id, data, size);
    if (result == UPLOAD_OK) return 0;
    { int error_number = errno; set_upload_error(error_response, result, error_number); } upload_abort(router->uploads, upload_id); return -1;
}

int router_upload_stream_enable_direct(router_t *router,
                                       const char *upload_id,
                                       router_response_t *error_response) {
    upload_result_t result;
    if (error_response == NULL) {
        return -1;
    }
    memset(error_response, 0, sizeof(*error_response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL) {
        set_json_error(error_response, 400U, "bad_request",
                       "Malformed upload stream");
        return -1;
    }
    result = upload_enable_direct_stream(router->uploads, upload_id);
    if (result == UPLOAD_OK) {
        return 0;
    }
    {
        int error_number = errno;
        set_upload_error(error_response, result, error_number);
    }
    upload_abort(router->uploads, upload_id);
    return -1;
}

int router_upload_stream_write_direct(router_t *router,
                                      const char *upload_id,
                                      const void *data, size_t size,
                                      router_response_t *error_response) {
    upload_result_t result;
    if (error_response == NULL) {
        return -1;
    }
    memset(error_response, 0, sizeof(*error_response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL ||
        (data == NULL && size != 0U)) {
        set_json_error(error_response, 400U, "bad_request",
                       "Malformed upload stream");
        return -1;
    }
    result = upload_write_direct(router->uploads, upload_id, data, size);
    if (result == UPLOAD_OK) {
        return 0;
    }
    {
        int error_number = errno;
        set_upload_error(error_response, result, error_number);
    }
    upload_abort(router->uploads, upload_id);
    return -1;
}

void router_upload_stream_finish(router_t *router, const char *upload_id,
                                 router_response_t *response) {
    char digest[SHA256_HEX_BYTES + 1U];
    upload_metrics_t metrics;
    upload_result_t result;
    if (response == NULL) return;
    memset(response, 0, sizeof(*response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL) {
        set_json_error(response, 400U, "bad_request", "Malformed upload stream");
        return;
    }
    result = upload_finish_with_metrics(router->uploads, upload_id, digest,
                                        &metrics);
    if (result != UPLOAD_OK) { int error_number = errno; set_upload_error(response, result, error_number); return; }
    set_upload_completed_response(response, digest, &metrics);
}

int router_upload_stream_write_chunk(router_t *router, const char *upload_id,
                                     uint64_t offset, const void *data,
                                     size_t size,
                                     router_response_t *error_response) {
    upload_result_t result;
    if (error_response == NULL) {
        return -1;
    }
    memset(error_response, 0, sizeof(*error_response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL ||
        (data == NULL && size != 0U)) {
        set_json_error(error_response, 400U, "bad_request",
                       "Malformed upload stream");
        return -1;
    }
    result = upload_chunk_write(router->uploads, upload_id, offset, data, size);
    if (result == UPLOAD_OK) {
        return 0;
    }
    {
        int error_number = errno;
        set_upload_error(error_response, result, error_number);
    }
    upload_abort(router->uploads, upload_id);
    return -1;
}

int router_upload_stream_finish_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size,
                                      router_response_t *response) {
    upload_result_t result;
    int count;
    if (response == NULL) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (router == NULL || router->uploads == NULL || upload_id == NULL) {
        set_json_error(response, 400U, "bad_request",
                       "Malformed upload stream");
        return -1;
    }
    result = upload_chunk_finish(router->uploads, upload_id, offset, size);
    if (result != UPLOAD_OK) {
        int error_number = errno;
        set_upload_error(response, result, error_number);
        return -1;
    }
    count = snprintf(response->storage, sizeof(response->storage),
                     "{\"chunk\":true,\"offset\":%llu,"
                     "\"size\":%llu}\n",
                     (unsigned long long)offset, (unsigned long long)size);
    if (count < 0 || (size_t)count >= sizeof(response->storage)) {
        set_json_error(response, 500U, "internal_error",
                       "Unable to create response");
        return -1;
    }
    response->status = 201U;
    response->content_type = "application/json; charset=utf-8";
    response->cache_control = "no-store";
    response->body = (const unsigned char *)response->storage;
    response->body_size = (size_t)count;
    return 0;
}

void router_upload_stream_abort_chunk(router_t *router, const char *upload_id,
                                      uint64_t offset, uint64_t size) {
    if (router != NULL) {
        upload_chunk_abort(router->uploads, upload_id, offset, size);
    }
}

void router_upload_stream_abort(router_t *router, const char *upload_id) {
    if (router != NULL) upload_abort(router->uploads, upload_id);
}
