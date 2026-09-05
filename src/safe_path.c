#include "safe_path.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int valid_utf8(const unsigned char *text) {
    while (*text != 0U) {
        unsigned int remaining;
        unsigned int sequence_bytes;
        unsigned int code;
        if (*text < 0x80U) { ++text; continue; }
        if ((*text & 0xe0U) == 0xc0U) { remaining = 1U; code = *text & 0x1fU; if (code < 2U) return 0; }
        else if ((*text & 0xf0U) == 0xe0U) { remaining = 2U; code = *text & 0x0fU; }
        else if ((*text & 0xf8U) == 0xf0U) { remaining = 3U; code = *text & 0x07U; if (code > 4U) return 0; }
        else return 0;
        sequence_bytes = remaining + 1U;
        ++text;
        while (remaining-- > 0U) { if ((*text & 0xc0U) != 0x80U) return 0; code = (code << 6U) | (*text & 0x3fU); ++text; }
        if ((sequence_bytes == 2U && code < 0x80U) ||
            (sequence_bytes == 3U && code < 0x800U) ||
            (sequence_bytes == 4U && code < 0x10000U) ||
            (code >= 0xd800U && code <= 0xdfffU) || code > 0x10ffffU) return 0;
    }
    return 1;
}

int safe_path_validate_filename(const char *name) {
    const unsigned char *cursor = (const unsigned char *)name;
    size_t length;
    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0U || length > SAFE_PATH_NAME_BYTES || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !valid_utf8(cursor)) return 0;
    while (*cursor != 0U) {
        if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == '/' || *cursor == '\\' || *cursor == '%') return 0;
        ++cursor;
    }
    return 1;
}

int safe_path_join(const char *directory, const char *name, char *output, size_t output_size) {
    int count;
    if (directory == NULL || !safe_path_validate_filename(name)) return -1;
    count = snprintf(output, output_size, "%s/%s", directory, name);
    return count > 0 && (size_t)count < output_size ? 0 : -1;
}

int safe_path_unique_name(const char *directory, const char *requested, char output[SAFE_PATH_NAME_BYTES + 1U]) {
    char path[SAFE_PATH_FULL_BYTES];
    const char *dot;
    size_t stem_size, extension_size;
    unsigned int suffix;
    if (!safe_path_validate_filename(requested)) return -1;
    if (safe_path_join(directory, requested, path, sizeof(path)) != 0) return -1;
    if (access(path, F_OK) != 0 && errno == ENOENT) { memcpy(output, requested, strlen(requested) + 1U); return 0; }
    dot = strrchr(requested, '.');
    if (dot == requested) dot = NULL;
    stem_size = dot == NULL ? strlen(requested) : (size_t)(dot - requested);
    extension_size = dot == NULL ? 0U : strlen(dot);
    for (suffix = 1U; suffix <= 9999U; ++suffix) {
        char suffix_text[24];
        int suffix_count = snprintf(suffix_text, sizeof(suffix_text), " (%u)", suffix);
        size_t keep;
        if (suffix_count < 0) return -1;
        if ((size_t)suffix_count + extension_size >= SAFE_PATH_NAME_BYTES) return -1;
        keep = SAFE_PATH_NAME_BYTES - (size_t)suffix_count - extension_size;
        if (keep > stem_size) keep = stem_size;
        while (keep > 0U && keep < stem_size &&
               (((unsigned char)requested[keep] & 0xc0U) == 0x80U)) {
            --keep;
        }
        memcpy(output, requested, keep);
        memcpy(output + keep, suffix_text, (size_t)suffix_count);
        if (extension_size > 0U) memcpy(output + keep + (size_t)suffix_count, dot, extension_size);
        output[keep + (size_t)suffix_count + extension_size] = '\0';
        if (!safe_path_validate_filename(output)) return -1;
        if (safe_path_join(directory, output, path, sizeof(path)) != 0) return -1;
        if (access(path, F_OK) != 0 && errno == ENOENT) return 0;
    }
    return -1;
}

int safe_path_ensure_directory(const char *directory) {
    char path[SAFE_PATH_FULL_BYTES];
    char *cursor;
    size_t length;
    if (directory == NULL || directory[0] == '\0') return -1;
    length = strlen(directory);
    if (length == 0U || length >= sizeof(path)) return -1;
    memcpy(path, directory, length + 1U);
    for (cursor = path + (path[0] == '/' ? 1 : 0); ; ++cursor) {
        if (*cursor == '/' || *cursor == '\0') {
            char saved = *cursor;
            struct stat status;
            *cursor = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
            if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode)) return -1;
            *cursor = saved;
            if (saved == '\0') break;
        }
    }
    return 0;
}
