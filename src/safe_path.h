#ifndef PS5LOCALSEND_SAFE_PATH_H
#define PS5LOCALSEND_SAFE_PATH_H

#include <stddef.h>

#define SAFE_PATH_NAME_BYTES 255U
#define SAFE_PATH_FULL_BYTES 1400U

int safe_path_validate_filename(const char *name);
int safe_path_join(const char *directory, const char *name, char *output,
                   size_t output_size);
int safe_path_unique_name(const char *directory, const char *requested,
                          char output[SAFE_PATH_NAME_BYTES + 1U]);
int safe_path_ensure_directory(const char *directory);

#endif
