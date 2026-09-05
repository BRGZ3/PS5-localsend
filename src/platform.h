#ifndef PS5LOCALSEND_PLATFORM_H
#define PS5LOCALSEND_PLATFORM_H

#include <stddef.h>

int platform_init(void);
void platform_shutdown(void);
int platform_notify(const char *message);
int platform_secure_random(void *buffer, size_t size);
int platform_get_lan_address(char *buffer, size_t size);
const char *platform_data_dir(void);
int platform_should_shutdown(void);
void platform_sleep_ms(unsigned int milliseconds);

#endif
