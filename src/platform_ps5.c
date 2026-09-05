#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct notify_request {
    char reserved[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t request_size, int blocking);

static volatile sig_atomic_t shutdown_requested;

static void handle_signal(int signal_number) {
    (void)signal_number;
    shutdown_requested = 1;
}

int platform_init(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    shutdown_requested = 0;
    return 0;
}

void platform_shutdown(void) {
}

int platform_notify(const char *message) {
    notify_request_t request;
    if (message == NULL) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    (void)snprintf(request.message, sizeof(request.message), "%s", message);
    return sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

int platform_secure_random(void *buffer, size_t size) {
    unsigned char *cursor = buffer;
    int descriptor;
    if (buffer == NULL && size != 0U) {
        return -1;
    }
    descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) {
        return -1;
    }
    while (size > 0U) {
        ssize_t count = read(descriptor, cursor, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(descriptor);
            return -1;
        }
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return close(descriptor) == 0 ? 0 : -1;
}

int platform_get_lan_address(char *buffer, size_t size) {
    struct ifaddrs *addresses = NULL;
    struct ifaddrs *current;
    int result = -1;
    if (buffer == NULL || size == 0U || getifaddrs(&addresses) != 0) {
        return -1;
    }
    for (current = addresses; current != NULL; current = current->ifa_next) {
        struct sockaddr_in *address;
        if (current->ifa_addr == NULL || current->ifa_addr->sa_family != AF_INET ||
            (current->ifa_flags & 0x8U) != 0U) {
            continue;
        }
        address = (struct sockaddr_in *)(void *)current->ifa_addr;
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, (socklen_t)size) != NULL) {
            result = 0;
            break;
        }
    }
    freeifaddrs(addresses);
    return result;
}

const char *platform_data_dir(void) {
    return "/data/ps5localsend";
}

int platform_should_shutdown(void) {
    return shutdown_requested != 0;
}

void platform_sleep_ms(unsigned int milliseconds) {
    struct timespec duration;
    duration.tv_sec = (time_t)(milliseconds / 1000U);
    duration.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}
