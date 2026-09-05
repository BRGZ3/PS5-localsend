#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static config_load_result_t load_text(const char *text, app_config_t *config,
                                      char *error, size_t error_size) {
    char path[] = "/tmp/ps5localsend-config-XXXXXX";
    FILE *file;
    int descriptor = mkstemp(path);
    config_load_result_t result;
    assert(descriptor >= 0);
    file = fdopen(descriptor, "w");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
    result = config_load_file(path, config, error, error_size);
    assert(unlink(path) == 0);
    return result;
}

static void test_defaults(void) {
    app_config_t config;
    config_set_defaults(&config);
    assert(config.port == 53317U);
    assert(config.auth_mode == CONFIG_AUTH_PIN);
    assert(config.language == CONFIG_LANGUAGE_EN);
    assert(config.pin_ttl_seconds == 120U);
    assert(config.session_ttl_seconds == 900U);
    assert(strcmp(config.destination, "/data/ps5localsend/inbox") == 0);
    assert(strcmp(config.internal_destination,
                  "/data/ps5localsend/inbox") == 0);
    assert(strcmp(config.usb_destination,
                  "/mnt/usb0/ps5localsend/inbox") == 0);
    assert(config.storage_target_count == 2U);
    assert(strcmp(config.storage_targets[0].id, "internal") == 0);
    assert(config.max_file_bytes == UINT64_C(21474836480));
    assert(config.max_files_per_session == 100U);
}

static void test_valid_file(void) {
    static const char text[] =
        "# local-only receiver\n"
        "port = 54321\n"
        "auth_mode=pin\n"
        "language=ru\n"
        "pin_ttl_seconds=60\n"
        "session_ttl_seconds=1200\n"
        "destination=/data/ps5localsend/custom/inbox\n"
        "internal_destination=/data/custom/receiver\n"
        "usb_destination=/mnt/usb0/custom/receiver\n"
        "max_file_bytes=99\n"
        "max_files_per_session=7\n";
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text(text, &config, error, sizeof(error)) == CONFIG_LOAD_OK);
    assert(config.port == 54321U);
    assert(config.language == CONFIG_LANGUAGE_RU);
    assert(config.pin_ttl_seconds == 60U);
    assert(config.session_ttl_seconds == 1200U);
    assert(strcmp(config.destination, "/data/custom/receiver") == 0);
    assert(strcmp(config.internal_destination, "/data/custom/receiver") == 0);
    assert(strcmp(config.usb_destination, "/mnt/usb0/custom/receiver") == 0);
    assert(config.max_file_bytes == 99U);
    assert(config.max_files_per_session == 7U);
}

static void test_usb_destination(void) {
    static const char text[] =
        "destination=/mnt/usb0/ps5localsend/inbox\n";
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text(text, &config, error, sizeof(error)) == CONFIG_LOAD_OK);
    assert(strcmp(config.destination, "/mnt/usb0/ps5localsend/inbox") == 0);
    assert(load_text("destination=/mnt/usb0\n", &config, error,
                     sizeof(error)) == CONFIG_LOAD_OK);
    assert(strcmp(config.destination, "/mnt/usb0") == 0);
}

static void test_no_pin_mode(void) {
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text("auth_mode=none\n", &config, error,
                     sizeof(error)) == CONFIG_LOAD_OK);
    assert(config.auth_mode == CONFIG_AUTH_NONE);
}

static void test_custom_storage_paths(void) {
    static const char text[] =
        "auth_mode=none\n"
        "destination=/mnt/ext0/game-images\n"
        "storage_path=inbox|Загрузки|/data/ps5localsend/inbox\n"
        "storage_path=m2|M.2 SSD|/mnt/ext0/game-images\n"
        "storage_path=usb_games|USB Games|/mnt/usb0/Games\n";
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text(text, &config, error, sizeof(error)) == CONFIG_LOAD_OK);
    assert(config.storage_target_count == 3U);
    assert(strcmp(config.destination, "/mnt/ext0/game-images") == 0);
    assert(strcmp(config_storage_by_id(&config, "m2")->label, "M.2 SSD") == 0);
    assert(strcmp(config_storage_by_path(&config, "/mnt/usb0/Games")->id,
                  "usb_games") == 0);
}

static void test_utf8_bom(void) {
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text("\xef\xbb\xbf" "auth_mode=none\r\n", &config, error,
                     sizeof(error)) == CONFIG_LOAD_OK);
    assert(config.auth_mode == CONFIG_AUTH_NONE);
    assert(load_text("\xff\xfe" "a\0", &config, error,
                     sizeof(error)) == CONFIG_LOAD_INVALID);
    assert(strstr(error, "UTF-16") != NULL);
}

static void test_save_file(void) {
    char path[] = "/tmp/ps5localsend-config-save-XXXXXX";
    char error[CONFIG_ERROR_CAPACITY];
    app_config_t config;
    app_config_t loaded;
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);
    config_set_defaults(&config);
    config.port = 54321U;
    config.auth_mode = CONFIG_AUTH_NONE;
    config.language = CONFIG_LANGUAGE_RU;
    (void)snprintf(config.internal_destination,
                   sizeof(config.internal_destination), "/data/custom/inbox");
    (void)snprintf(config.destination, sizeof(config.destination),
                   "/data/custom/inbox");
    (void)snprintf(config.storage_targets[0].path,
                   sizeof(config.storage_targets[0].path),
                   "/data/custom/inbox");
    assert(config_save_file(path, &config) == 0);
    assert(config_load_file(path, &loaded, error, sizeof(error)) ==
           CONFIG_LOAD_OK);
    assert(loaded.port == 54321U);
    assert(loaded.auth_mode == CONFIG_AUTH_NONE);
    assert(loaded.language == CONFIG_LANGUAGE_RU);
    assert(strcmp(loaded.destination, config.destination) == 0);
    assert(strcmp(loaded.storage_targets[0].path,
                  config.storage_targets[0].path) == 0);
    assert(unlink(path) == 0);
}

static void expect_invalid(const char *text, const char *message_part) {
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    assert(load_text(text, &config, error, sizeof(error)) == CONFIG_LOAD_INVALID);
    if (strstr(error, message_part) == NULL)
        fprintf(stderr, "unexpected config error for %s: %s\n", text, error);
    assert(strstr(error, message_part) != NULL);
}

int main(void) {
    app_config_t config;
    char error[CONFIG_ERROR_CAPACITY];
    test_defaults();
    test_valid_file();
    test_usb_destination();
    test_no_pin_mode();
    test_custom_storage_paths();
    test_utf8_bom();
    test_save_file();
    expect_invalid("auth_mode=off\n", "auth_mode must be pin or none");
    expect_invalid("language=de\n", "language must be en or ru");
    expect_invalid("destination=/dev/inbox\n", "safe absolute");
    expect_invalid("destination=/data/ps5localsend/../escape\n", "safe absolute");
    expect_invalid("internal_destination=/mnt/usb0/inbox\n",
                   "internal_destination");
    expect_invalid("usb_destination=/data/inbox\n", "usb_destination");
    expect_invalid("storage_path=bad entry\n", "id|label|path");
    expect_invalid("storage_path=dup|One|/data/one\n"
                   "storage_path=dup|Two|/data/two\n", "duplicate");
    expect_invalid("port=80\n", "port must be");
    expect_invalid("port=53317\nport=53318\n", "duplicate");
    expect_invalid("surprise=value\n", "unknown");
    assert(config_load_file("/tmp/ps5localsend-does-not-exist", &config, error,
                            sizeof(error)) == CONFIG_LOAD_NOT_FOUND);
    assert(config.port == 53317U);
    puts("config tests passed");
    return 0;
}
