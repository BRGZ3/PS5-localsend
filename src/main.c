#include "config.h"
#include "platform.h"
#include "safe_path.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_configuration(const char *path, int allow_missing,
                              app_config_t *config) {
    char error[CONFIG_ERROR_CAPACITY];
    config_load_result_t result =
        config_load_file(path, config, error, sizeof(error));
    if (result == CONFIG_LOAD_OK) {
        return 0;
    }
    if (result == CONFIG_LOAD_NOT_FOUND && allow_missing != 0) {
        if (safe_path_ensure_directory(platform_data_dir()) == 0 &&
            config_save_file(path, config) == 0) {
            fprintf(stderr, "created default configuration: %s\n", path);
        } else {
            fprintf(stderr, "warning: %s: %s; defaults remain active\n", path,
                    error);
        }
        return 0;
    }
    fprintf(stderr, "error: %s: %s\n", path, error);
    {
        char notification[CONFIG_ERROR_CAPACITY + 40U];
        int count = snprintf(notification, sizeof(notification),
                             "PS5 LocalSend config error: %s", error);
        if (count > 0 && (size_t)count < sizeof(notification)) {
            (void)platform_notify(notification);
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    app_config_t config;
    http_server_t *server = NULL;
    char config_path[CONFIG_DESTINATION_CAPACITY];
    char address[64];
    char message[256];
    const char *requested_path = NULL;
    int run_once = 0;
    int allow_missing = 1;
    int check_only = 0;
    int port_override = -1;
    int argument_index;

    for (argument_index = 1; argument_index < argc; ++argument_index) {
        if (strcmp(argv[argument_index], "--once") == 0 && run_once == 0) {
            run_once = 1;
        } else if (strcmp(argv[argument_index], "--check-config") == 0 &&
                   requested_path == NULL && argument_index + 1 < argc) {
            requested_path = argv[++argument_index];
            allow_missing = 0;
            run_once = 1;
            check_only = 1;
        } else if (strcmp(argv[argument_index], "--port") == 0 &&
                   port_override < 0 && argument_index + 1 < argc) {
            char *end = NULL;
            unsigned long parsed;
            errno = 0;
            parsed = strtoul(argv[++argument_index], &end, 10);
            if (errno != 0 || end == argv[argument_index] || *end != '\0' ||
                parsed > 65535UL) {
                fprintf(stderr, "error: --port must be between 0 and 65535\n");
                return 2;
            }
            port_override = (int)parsed;
        } else {
            fprintf(stderr,
                    "usage: %s [--once] [--port PORT] | --check-config PATH\n",
                    argv[0]);
            return 2;
        }
    }
    if (check_only != 0 && port_override >= 0) {
        fprintf(stderr,
                "usage: %s [--once] [--port PORT] | --check-config PATH\n",
                argv[0]);
        return 2;
    }

    if (platform_init() != 0) {
        fprintf(stderr, "error: platform initialization failed\n");
        return 1;
    }
    if (requested_path == NULL) {
        int count = snprintf(config_path, sizeof(config_path), "%s/config.ini",
                             platform_data_dir());
        if (count < 0 || (size_t)count >= sizeof(config_path)) {
            fprintf(stderr, "error: configuration path is too long\n");
            platform_shutdown();
            return 1;
        }
        requested_path = config_path;
    }
    if (load_configuration(requested_path, allow_missing, &config) != 0) {
        platform_shutdown();
        return 1;
    }
    if (check_only != 0) {
        printf("config ok: auth_mode=%s language=%s port=%u destination=%s\n",
               config_auth_mode_name(config.auth_mode),
               config_language_name(config.language),
               (unsigned int)config.port, config.destination);
        platform_shutdown();
        return 0;
    }
    if (port_override >= 0) {
        config.port = (uint16_t)port_override;
    }
    server = server_start(&config);
    if (server == NULL) {
        fprintf(stderr, "error: HTTP server failed to start on port %u\n",
                (unsigned int)config.port);
        platform_shutdown();
        return 1;
    }
    config.port = server_port(server);

    if (platform_get_lan_address(address, sizeof(address)) == 0) {
        (void)snprintf(message, sizeof(message),
                       "PS5 LocalSend ready at http://%s:%u/", address,
                       (unsigned int)config.port);
    } else {
        (void)snprintf(message, sizeof(message),
                       "PS5 LocalSend ready on HTTP port %u",
                       (unsigned int)config.port);
    }
    if (platform_notify(message) != 0) {
        fprintf(stderr, "warning: platform notification failed\n");
    }
    printf("listening: http://127.0.0.1:%u/\n", (unsigned int)config.port);
    (void)fflush(stdout);

    while (run_once == 0 && platform_should_shutdown() == 0) {
        platform_sleep_ms(250U);
    }
    server_stop(server);
    platform_shutdown();
    return 0;
}
