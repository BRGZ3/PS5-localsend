#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_LINE_CAPACITY 2048U
#define CONFIG_KEY_COUNT 11U

static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text) != 0) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]) != 0) {
        --end;
    }
    *end = '\0';
    return text;
}

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum,
                     uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (*text == '\0' || *text == '-' || *text == '+') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int destination_syntax_is_safe(const char *path) {
    const char *segment;
    const char *cursor;
    static const char *const blocked_roots[] = {"/dev", "/proc", "/sys"};
    size_t index;
    if (path == NULL || path[0] != '/' || path[1] == '\0') {
        return 0;
    }
    for (index = 0U; index < sizeof(blocked_roots) / sizeof(blocked_roots[0]);
         ++index) {
        size_t size = strlen(blocked_roots[index]);
        if (strncmp(path, blocked_roots[index], size) == 0 &&
            (path[size] == '\0' || path[size] == '/')) {
            return 0;
        }
    }
    for (cursor = path; *cursor != '\0'; ++cursor) {
        unsigned char character = (unsigned char)*cursor;
        if (character < 0x20U || character == 0x7fU || *cursor == '\\' ||
            *cursor == '"') {
            return 0;
        }
    }
    segment = path + 1;
    for (cursor = segment;; ++cursor) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - segment);
            if (length == 0U || (length == 1U && segment[0] == '.') ||
                (length == 2U && segment[0] == '.' && segment[1] == '.')) {
                return 0;
            }
            if (*cursor == '\0') {
                break;
            }
            segment = cursor + 1;
        }
    }
    return 1;
}

static int destination_is_safe_for_root(const char *path, const char *root) {
    size_t root_size = strlen(root);
    return destination_syntax_is_safe(path) &&
           strncmp(path, root, root_size) == 0 && path[root_size] == '/' &&
           path[root_size + 1U] != '\0';
}

static int destination_is_safe(const char *path) {
    return destination_syntax_is_safe(path);
}

const char *config_auth_mode_name(config_auth_mode_t mode) {
    return mode == CONFIG_AUTH_NONE ? "none" : "pin";
}

const char *config_language_name(config_language_t language) {
    return language == CONFIG_LANGUAGE_RU ? "ru" : "en";
}

const config_storage_target_t *config_storage_by_id(const app_config_t *config,
                                                     const char *id) {
    size_t index;
    if (config == NULL || id == NULL) return NULL;
    for (index = 0U; index < config->storage_target_count; ++index)
        if (strcmp(config->storage_targets[index].id, id) == 0)
            return &config->storage_targets[index];
    return NULL;
}

const config_storage_target_t *config_storage_by_path(const app_config_t *config,
                                                       const char *path) {
    size_t index;
    if (config == NULL || path == NULL) return NULL;
    for (index = 0U; index < config->storage_target_count; ++index)
        if (strcmp(config->storage_targets[index].path, path) == 0)
            return &config->storage_targets[index];
    return NULL;
}

static int storage_id_is_safe(const char *id) {
    const unsigned char *cursor = (const unsigned char *)id;
    size_t length = strlen(id);
    if (length == 0U || length >= CONFIG_STORAGE_ID_CAPACITY) return 0;
    while (*cursor != '\0') {
        if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-') return 0;
        ++cursor;
    }
    return 1;
}

static int storage_label_is_safe(const char *label) {
    const unsigned char *cursor = (const unsigned char *)label;
    size_t length = strlen(label);
    if (length == 0U || length >= CONFIG_STORAGE_LABEL_CAPACITY) return 0;
    while (*cursor != '\0') {
        if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == '"' ||
            *cursor == '\\' || *cursor == '|') return 0;
        ++cursor;
    }
    return 1;
}

static int add_storage_target(app_config_t *config, const char *value,
                              char *error, size_t error_size) {
    char copy[CONFIG_LINE_CAPACITY];
    char *id;
    char *label;
    char *path;
    char *separator;
    config_storage_target_t *target;
    if (strlen(value) >= sizeof(copy)) {
        set_error(error, error_size, "storage_path is too long"); return -1;
    }
    (void)snprintf(copy, sizeof(copy), "%s", value);
    id = trim(copy);
    separator = strchr(id, '|');
    if (separator == NULL) {
        set_error(error, error_size, "storage_path must be id|label|path"); return -1;
    }
    *separator = '\0'; label = trim(separator + 1);
    separator = strchr(label, '|');
    if (separator == NULL) {
        set_error(error, error_size, "storage_path must be id|label|path"); return -1;
    }
    *separator = '\0'; path = trim(separator + 1); id = trim(id); label = trim(label);
    if (strchr(path, '|') != NULL || !storage_id_is_safe(id) ||
        !storage_label_is_safe(label) || strlen(path) >= CONFIG_DESTINATION_CAPACITY ||
        !destination_is_safe(path)) {
        set_error(error, error_size, "invalid storage_path entry"); return -1;
    }
    if (config->storage_target_count >= CONFIG_STORAGE_TARGETS_MAX) {
        set_error(error, error_size, "too many storage_path entries"); return -1;
    }
    if (config_storage_by_id(config, id) != NULL) {
        set_error(error, error_size, "duplicate storage_path id"); return -1;
    }
    target = &config->storage_targets[config->storage_target_count++];
    (void)snprintf(target->id, sizeof(target->id), "%s", id);
    (void)snprintf(target->label, sizeof(target->label), "%s", label);
    (void)snprintf(target->path, sizeof(target->path), "%s", path);
    return 0;
}

void config_set_defaults(app_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = 53317U;
    config->auth_mode = CONFIG_AUTH_PIN;
    config->language = CONFIG_LANGUAGE_EN;
    config->pin_ttl_seconds = 120U;
    config->session_ttl_seconds = 900U;
    (void)snprintf(config->destination, sizeof(config->destination),
                   "/data/ps5localsend/inbox");
    (void)snprintf(config->internal_destination,
                   sizeof(config->internal_destination),
                   "/data/ps5localsend/inbox");
    (void)snprintf(config->usb_destination, sizeof(config->usb_destination),
                   "/mnt/usb0/ps5localsend/inbox");
    config->storage_target_count = 2U;
    (void)snprintf(config->storage_targets[0].id,
                   sizeof(config->storage_targets[0].id), "internal");
    (void)snprintf(config->storage_targets[0].label,
                   sizeof(config->storage_targets[0].label), "Internal storage");
    (void)snprintf(config->storage_targets[0].path,
                   sizeof(config->storage_targets[0].path), "%s",
                   config->internal_destination);
    (void)snprintf(config->storage_targets[1].id,
                   sizeof(config->storage_targets[1].id), "usb");
    (void)snprintf(config->storage_targets[1].label,
                   sizeof(config->storage_targets[1].label), "USB drive");
    (void)snprintf(config->storage_targets[1].path,
                   sizeof(config->storage_targets[1].path), "%s",
                   config->usb_destination);
    config->max_file_bytes = UINT64_C(21474836480);
    config->max_files_per_session = 100U;
}

static int apply_value(app_config_t *config, const char *key, const char *value,
                       unsigned int *seen, char *error, size_t error_size) {
    static const char *const keys[CONFIG_KEY_COUNT] = {
        "port", "auth_mode", "pin_ttl_seconds", "session_ttl_seconds",
        "destination", "internal_destination", "usb_destination",
        "storage_path", "max_file_bytes", "max_files_per_session",
        "language"};
    size_t index;
    uint64_t number;

    for (index = 0U; index < CONFIG_KEY_COUNT; ++index) {
        if (strcmp(key, keys[index]) == 0) {
            break;
        }
    }
    if (index == CONFIG_KEY_COUNT) {
        set_error(error, error_size, "unknown configuration key");
        return -1;
    }
    if (index != 7U && (*seen & (1U << index)) != 0U) {
        set_error(error, error_size, "duplicate configuration key");
        return -1;
    }
    if (index == 7U && (*seen & (1U << index)) == 0U) {
        memset(config->storage_targets, 0, sizeof(config->storage_targets));
        config->storage_target_count = 0U;
    }
    *seen |= 1U << index;

    if (index == 0U) {
        if (parse_u64(value, 1024U, 65535U, &number) != 0) {
            set_error(error, error_size, "port must be between 1024 and 65535");
            return -1;
        }
        config->port = (uint16_t)number;
    } else if (index == 1U) {
        if (strcmp(value, "pin") == 0) {
            config->auth_mode = CONFIG_AUTH_PIN;
        } else if (strcmp(value, "none") == 0) {
            config->auth_mode = CONFIG_AUTH_NONE;
        } else {
            set_error(error, error_size, "auth_mode must be pin or none");
            return -1;
        }
    } else if (index == 2U) {
        if (parse_u64(value, 30U, 600U, &number) != 0) {
            set_error(error, error_size, "pin_ttl_seconds must be between 30 and 600");
            return -1;
        }
        config->pin_ttl_seconds = (unsigned int)number;
    } else if (index == 3U) {
        if (parse_u64(value, 60U, 86400U, &number) != 0) {
            set_error(error, error_size,
                      "session_ttl_seconds must be between 60 and 86400");
            return -1;
        }
        config->session_ttl_seconds = (unsigned int)number;
    } else if (index == 4U) {
        if (strlen(value) >= sizeof(config->destination) ||
            destination_is_safe(value) == 0) {
            set_error(error, error_size,
                      "destination must be a safe absolute directory");
            return -1;
        }
        (void)snprintf(config->destination, sizeof(config->destination), "%s", value);
    } else if (index == 5U) {
        if (strlen(value) >= sizeof(config->internal_destination) ||
            destination_is_safe_for_root(value, "/data") == 0) {
            set_error(error, error_size,
                      "internal_destination must be a safe child of /data");
            return -1;
        }
        (void)snprintf(config->internal_destination,
                       sizeof(config->internal_destination), "%s", value);
    } else if (index == 6U) {
        if (strlen(value) >= sizeof(config->usb_destination) ||
            destination_is_safe_for_root(value, "/mnt/usb0") == 0) {
            set_error(error, error_size,
                      "usb_destination must be a safe child of /mnt/usb0");
            return -1;
        }
        (void)snprintf(config->usb_destination,
                       sizeof(config->usb_destination), "%s", value);
    } else if (index == 7U) {
        return add_storage_target(config, value, error, error_size);
    } else if (index == 8U) {
        if (parse_u64(value, 1U, UINT64_C(1099511627776), &number) != 0) {
            set_error(error, error_size,
                      "max_file_bytes must be between 1 and 1099511627776");
            return -1;
        }
        config->max_file_bytes = number;
    } else if (index == 9U) {
        if (parse_u64(value, 1U, 1000U, &number) != 0) {
            set_error(error, error_size,
                      "max_files_per_session must be between 1 and 1000");
            return -1;
        }
        config->max_files_per_session = (unsigned int)number;
    } else {
        if (strcmp(value, "en") == 0) {
            config->language = CONFIG_LANGUAGE_EN;
        } else if (strcmp(value, "ru") == 0) {
            config->language = CONFIG_LANGUAGE_RU;
        } else {
            set_error(error, error_size, "language must be en or ru");
            return -1;
        }
    }
    return 0;
}

config_load_result_t config_load_file(const char *path, app_config_t *config,
                                      char *error, size_t error_size) {
    FILE *file;
    char line[CONFIG_LINE_CAPACITY];
    unsigned long line_number = 0UL;
    unsigned int seen = 0U;

    if (path == NULL || config == NULL) {
        set_error(error, error_size, "invalid configuration arguments");
        return CONFIG_LOAD_INVALID;
    }
    config_set_defaults(config);
    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            set_error(error, error_size, "configuration file not found; using defaults");
            return CONFIG_LOAD_NOT_FOUND;
        }
        set_error(error, error_size, "unable to open configuration file");
        return CONFIG_LOAD_IO_ERROR;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *key;
        char *value;
        char *separator;
        ++line_number;
        if (line_number == 1UL &&
            (((unsigned char)line[0] == 0xffU &&
              (unsigned char)line[1] == 0xfeU) ||
             ((unsigned char)line[0] == 0xfeU &&
              (unsigned char)line[1] == 0xffU))) {
            set_error(error, error_size,
                      "UTF-16 config is unsupported; save config.ini as UTF-8");
            (void)fclose(file);
            return CONFIG_LOAD_INVALID;
        }
        if (line_number == 1UL && strlen(line) >= 3U &&
            (unsigned char)line[0] == 0xefU &&
            (unsigned char)line[1] == 0xbbU &&
            (unsigned char)line[2] == 0xbfU) {
            memmove(line, line + 3, strlen(line + 3) + 1U);
        }
        if (strchr(line, '\n') == NULL && feof(file) == 0) {
            set_error(error, error_size, "configuration line is too long");
            (void)fclose(file);
            return CONFIG_LOAD_INVALID;
        }
        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }
        separator = strchr(key, '=');
        if (separator == NULL) {
            set_error(error, error_size, "configuration line must contain '='");
            (void)fclose(file);
            return CONFIG_LOAD_INVALID;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);
        if (*key == '\0' || *value == '\0' ||
            apply_value(config, key, value, &seen, error, error_size) != 0) {
            (void)line_number;
            (void)fclose(file);
            return CONFIG_LOAD_INVALID;
        }
    }
    if (ferror(file) != 0) {
        set_error(error, error_size, "error while reading configuration file");
        (void)fclose(file);
        return CONFIG_LOAD_IO_ERROR;
    }
    (void)fclose(file);
    if ((seen & (1U << 7U)) == 0U && (seen & (1U << 4U)) != 0U) {
        if ((destination_is_safe_for_root(config->destination, "/mnt/usb0") ||
             strcmp(config->destination, "/mnt/usb0") == 0) &&
            (seen & (1U << 6U)) == 0U) {
            (void)snprintf(config->usb_destination,
                           sizeof(config->usb_destination), "%s",
                           config->destination);
        } else if (destination_is_safe_for_root(config->destination, "/data") &&
                   (seen & (1U << 5U)) == 0U) {
            (void)snprintf(config->internal_destination,
                           sizeof(config->internal_destination), "%s",
                           config->destination);
        }
    }
    if ((seen & (1U << 7U)) == 0U) {
        (void)snprintf(config->storage_targets[0].path,
                       sizeof(config->storage_targets[0].path), "%s",
                       config->internal_destination);
        (void)snprintf(config->storage_targets[1].path,
                       sizeof(config->storage_targets[1].path), "%s",
                       config->usb_destination);
    }
    if (config->storage_target_count == 0U) {
        set_error(error, error_size, "at least one storage_path is required");
        return CONFIG_LOAD_INVALID;
    }
    if (config_storage_by_path(config, config->destination) == NULL) {
        (void)snprintf(config->destination, sizeof(config->destination), "%s",
                       config->storage_targets[0].path);
    }
    set_error(error, error_size, "ok");
    return CONFIG_LOAD_OK;
}

int config_save_file(const char *path, const app_config_t *config) {
    char temporary[CONFIG_DESTINATION_CAPACITY + 8U];
    FILE *file;
    int count;
    int failed = 0;
    int saved_errno = 0;
    size_t storage_index;

    if (path == NULL || config == NULL) {
        errno = EINVAL;
        return -1;
    }
    count = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count < 0 || (size_t)count >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    file = fopen(temporary, "w");
    if (file == NULL) {
        return -1;
    }
    if (fprintf(file,
                "# PS5 LocalSend configuration\n"
                "port=%u\n"
                "auth_mode=%s\n"
                "language=%s\n"
                "pin_ttl_seconds=%u\n"
                "session_ttl_seconds=%u\n"
                "destination=%s\n",
                (unsigned int)config->port,
                config_auth_mode_name(config->auth_mode),
                config_language_name(config->language),
                config->pin_ttl_seconds,
                config->session_ttl_seconds,
                config->destination) < 0) {
        failed = 1;
        saved_errno = errno;
    }
    for (storage_index = 0U;
         failed == 0 && storage_index < config->storage_target_count;
         ++storage_index) {
        const config_storage_target_t *target =
            &config->storage_targets[storage_index];
        if (fprintf(file, "storage_path=%s|%s|%s\n", target->id,
                    target->label, target->path) < 0) {
            failed = 1; saved_errno = errno;
        }
    }
    if (failed == 0 &&
        fprintf(file, "max_file_bytes=%llu\nmax_files_per_session=%u\n",
                (unsigned long long)config->max_file_bytes,
                config->max_files_per_session) < 0) {
        failed = 1; saved_errno = errno;
    }
    if (failed == 0 && fflush(file) != 0) {
        failed = 1; saved_errno = errno;
    }
    if (fclose(file) != 0 && failed == 0) {
        failed = 1;
        saved_errno = errno;
    }
    if (failed != 0 || rename(temporary, path) != 0) {
        if (failed == 0) {
            saved_errno = errno;
        }
        (void)unlink(temporary);
        errno = saved_errno != 0 ? saved_errno : EIO;
        return -1;
    }
    return 0;
}
