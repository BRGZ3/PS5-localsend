#include "config.h"
#include "router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static router_request_t request(const char *method, const char *path,
                                size_t body_size) {
    router_request_t value;
    memset(&value, 0, sizeof(value));
    value.method = method;
    value.path = path;
    value.body = (const unsigned char *)"x";
    value.body_size = body_size;
    value.client_address = "127.0.0.1";
    value.host = "127.0.0.1:53317";
    value.origin = "http://127.0.0.1:53317";
    return value;
}

int main(void) {
    app_config_t config;
    router_t router;
    router_request_t input;
    router_response_t response;
    char upload_id[UPLOAD_ID_HEX_BYTES + 1U];

    config_set_defaults(&config);
    assert(router_init(&router, &config) == 0);
    input = request("GET", "/api/v1/status", 0U);
    router_dispatch(&router, &input, NULL);
    router_dispatch(NULL, &input, &response);
    assert(response.status == 400U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 200U);
    assert(strstr((const char *)response.body, "\"authMode\":\"pin\"") != NULL);
    assert(strstr((const char *)response.body, "21474836480") != NULL);
    assert(strstr((const char *)response.body, "\"state\":\"idle\"") != NULL);
    assert(strstr((const char *)response.body,
                  "\"storage\":{\"target\":\"internal\"") != NULL);
    assert(strstr((const char *)response.body,
                  "\"targets\":[{\"id\":\"internal\"") != NULL);

    input = request("POST", "/api/v1/status", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 405U);
    assert(strcmp(response.allow, "GET") == 0);

    input = request("GET", "/missing", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 404U);
    assert(strstr((const char *)response.body, "not_found") != NULL);

    input = request("GET", "/", 1U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 413U);

    input = request("GET", "/assets/app.js", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 200U);
    assert(response.body_size > 100U);
    assert(strcmp(response.content_type, "text/javascript; charset=utf-8") == 0);

    input = request("POST", "/api/v1/auth/challenge", 0U);
    input.origin = NULL;
    router_dispatch(&router, &input, &response);
    assert(response.status == 403U);

    input = request("POST", "/api/v1/auth/challenge", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 201U);
    assert(strstr((const char *)response.body, "challengeId") != NULL);
    assert(strstr((const char *)response.body, "pin") == NULL);

    assert(router_upload_stream_begin(NULL, &input, 1, 0U, upload_id,
                                      &response) == -1);
    assert(router_upload_stream_begin(&router, NULL, 1, 0U, upload_id,
                                      &response) == -1);
    assert(router_upload_stream_begin(&router, &input, 1, 0U, NULL,
                                      &response) == -1);
    assert(router_upload_stream_begin(&router, &input, 1, 0U, upload_id,
                                      NULL) == -1);
    assert(router_upload_stream_write(NULL, "id", "x", 1U, &response) == -1);
    assert(router_upload_stream_write(&router, NULL, "x", 1U, &response) == -1);
    assert(router_upload_stream_write(&router, "id", NULL, 1U, &response) == -1);
    assert(router_upload_stream_write(&router, "id", "x", 1U, NULL) == -1);
    router_upload_stream_finish(NULL, "id", &response);
    assert(response.status == 400U);
    router_upload_stream_finish(&router, NULL, &response);
    assert(response.status == 400U);
    router_upload_stream_finish(&router, "id", NULL);

    router_destroy(&router);

    config_set_defaults(&config);
    config.auth_mode = CONFIG_AUTH_NONE;
    assert(router_init(&router, &config) == 0);
    input = request("GET", "/api/v1/status", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 200U);
    assert(strstr((const char *)response.body, "\"authMode\":\"none\"") != NULL);
    input = request("POST", "/api/v1/auth/challenge", 0U);
    router_dispatch(&router, &input, &response);
    assert(response.status == 409U);
    assert(strstr((const char *)response.body, "auth_disabled") != NULL);
    router_destroy(&router);
    puts("router tests passed");
    return 0;
}
