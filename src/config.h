#ifndef PS5LOCALSEND_CONFIG_H
#define PS5LOCALSEND_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define CONFIG_DESTINATION_CAPACITY 1024U
#define CONFIG_ERROR_CAPACITY 256U
#define CONFIG_STORAGE_TARGETS_MAX 8U
#define CONFIG_STORAGE_ID_CAPACITY 32U
#define CONFIG_STORAGE_LABEL_CAPACITY 96U

typedef struct config_storage_target {
    char id[CONFIG_STORAGE_ID_CAPACITY];
    char label[CONFIG_STORAGE_LABEL_CAPACITY];
    char path[CONFIG_DESTINATION_CAPACITY];
} config_storage_target_t;

typedef enum config_auth_mode {
    CONFIG_AUTH_PIN = 0,
    CONFIG_AUTH_NONE = 1
} config_auth_mode_t;

typedef enum config_language {
    CONFIG_LANGUAGE_EN = 0,
    CONFIG_LANGUAGE_RU = 1
} config_language_t;

typedef struct app_config {
    uint16_t port;
    config_auth_mode_t auth_mode;
    config_language_t language;
    unsigned int pin_ttl_seconds;
    unsigned int session_ttl_seconds;
    char destination[CONFIG_DESTINATION_CAPACITY];
    char internal_destination[CONFIG_DESTINATION_CAPACITY];
    char usb_destination[CONFIG_DESTINATION_CAPACITY];
    config_storage_target_t storage_targets[CONFIG_STORAGE_TARGETS_MAX];
    size_t storage_target_count;
    uint64_t max_file_bytes;
    unsigned int max_files_per_session;
} app_config_t;

typedef enum config_load_result {
    CONFIG_LOAD_OK = 0,
    CONFIG_LOAD_NOT_FOUND = 1,
    CONFIG_LOAD_INVALID = 2,
    CONFIG_LOAD_IO_ERROR = 3
} config_load_result_t;

const char *config_auth_mode_name(config_auth_mode_t mode);
const char *config_language_name(config_language_t language);
const config_storage_target_t *config_storage_by_id(const app_config_t *config,
                                                     const char *id);
const config_storage_target_t *config_storage_by_path(const app_config_t *config,
                                                       const char *path);

void config_set_defaults(app_config_t *config);
config_load_result_t config_load_file(const char *path, app_config_t *config,
                                      char *error, size_t error_size);
int config_save_file(const char *path, const app_config_t *config);

#endif
