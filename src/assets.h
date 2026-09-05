#ifndef PS5LOCALSEND_ASSETS_H
#define PS5LOCALSEND_ASSETS_H

#include <stddef.h>

typedef struct embedded_asset {
    const char *path;
    const char *content_type;
    const unsigned char *data;
    size_t size;
} embedded_asset_t;

const embedded_asset_t *assets_find(const char *path);

#endif
