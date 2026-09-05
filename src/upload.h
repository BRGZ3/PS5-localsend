#ifndef PS5LOCALSEND_UPLOAD_H
#define PS5LOCALSEND_UPLOAD_H

#include "config.h"
#include "safe_path.h"
#include "sha256.h"

#include <stddef.h>
#include <stdint.h>

#define UPLOAD_ID_HEX_BYTES 32U
#define UPLOAD_AUTH_BYTES 80U
#define UPLOAD_METADATA_BYTES 1024U
#define UPLOAD_LEASE_SECONDS 60U
#define UPLOAD_WRITE_BUFFER_BYTES (4U * 1024U * 1024U)
#define UPLOAD_WRITE_QUEUE_DEPTH 4U
#define UPLOAD_WRITE_ALIGNMENT_BYTES 4096U
#define UPLOAD_WRITE_BUFFER_ALLOCATION_BYTES \
    (UPLOAD_WRITE_BUFFER_BYTES * UPLOAD_WRITE_QUEUE_DEPTH + \
     UPLOAD_WRITE_ALIGNMENT_BYTES)
/* Large, independently transferable extents used by the experimental
 * multi-connection transport.  Keeping the extent a multiple of the write
 * alignment also leaves room for a future direct-I/O implementation without
 * changing the browser protocol. */
#define UPLOAD_CHUNK_BYTES (8U * 1024U * 1024U)
#define UPLOAD_CHUNK_PARALLELISM 6U
#define UPLOAD_CHUNK_BUFFER_SLOTS (UPLOAD_CHUNK_PARALLELISM * 2U)

typedef struct upload_manager upload_manager_t;

typedef enum upload_result {
    UPLOAD_OK = 0,
    UPLOAD_INVALID = 1,
    UPLOAD_NOT_FOUND = 2,
    UPLOAD_FORBIDDEN = 3,
    UPLOAD_BUSY = 4,
    UPLOAD_TOO_LARGE = 5,
    UPLOAD_STORAGE_FULL = 6,
    UPLOAD_IO_ERROR = 7,
    UPLOAD_SIZE_MISMATCH = 8,
    UPLOAD_HASH_MISMATCH = 9,
    UPLOAD_LIMIT_REACHED = 10,
    UPLOAD_BACKPRESSURE = 11
} upload_result_t;

typedef struct upload_prepared {
    char upload_id[UPLOAD_ID_HEX_BYTES + 1U];
    char final_name[SAFE_PATH_NAME_BYTES + 1U];
    uint64_t size;
} upload_prepared_t;

/* Server-side timings returned after a completed upload.  They are intended
 * for diagnosing target-specific throughput without changing the file
 * format or the browser upload protocol. */
typedef struct upload_metrics {
    uint64_t preparation_ms;
    uint64_t transfer_ms;
    uint64_t write_ms;
    int direct_io;
} upload_metrics_t;

/* A lock-protected, non-secret view used by the public receiver status page. */
typedef struct upload_snapshot {
    int active;
    int started;
    char name[SAFE_PATH_NAME_BYTES + 1U];
    uint64_t received;
    uint64_t expected;
    int has_last_metrics;
    uint64_t last_completed_size;
    upload_metrics_t last_metrics;
} upload_snapshot_t;

upload_manager_t *upload_manager_create(const app_config_t *config);
void upload_manager_destroy(upload_manager_t *manager);
upload_result_t upload_manager_set_destination(upload_manager_t *manager,
                                               const char *destination);
upload_result_t upload_prepare(upload_manager_t *manager, const char *client,
                               const char *authorization,
                               const unsigned char *json, size_t json_size,
                               upload_prepared_t *prepared);
upload_result_t upload_begin(upload_manager_t *manager, const char *upload_id,
                             const char *client, const char *authorization,
                             uint64_t content_length);
upload_result_t upload_write(upload_manager_t *manager, const char *upload_id,
                             const void *data, size_t size);
upload_result_t upload_enable_direct_stream(upload_manager_t *manager,
                                            const char *upload_id);
upload_result_t upload_write_direct(upload_manager_t *manager,
                                    const char *upload_id, const void *data,
                                    size_t size);
upload_result_t upload_finish(upload_manager_t *manager, const char *upload_id,
                              char digest[SHA256_HEX_BYTES + 1U]);
upload_result_t upload_finish_with_metrics(
    upload_manager_t *manager, const char *upload_id,
    char digest[SHA256_HEX_BYTES + 1U], upload_metrics_t *metrics);
upload_result_t upload_chunk_begin(upload_manager_t *manager,
                                   const char *upload_id, const char *client,
                                   const char *authorization, uint64_t total,
                                   uint64_t offset, uint64_t length);
upload_result_t upload_chunk_write(upload_manager_t *manager,
                                   const char *upload_id, uint64_t offset,
                                   const void *data, size_t size);
upload_result_t upload_chunk_finish(upload_manager_t *manager,
                                    const char *upload_id, uint64_t offset,
                                    uint64_t length);
void upload_chunk_abort(upload_manager_t *manager, const char *upload_id,
                        uint64_t offset, uint64_t length);
upload_result_t upload_finish_chunks_with_metrics(
    upload_manager_t *manager, const char *upload_id, const char *client,
    const char *authorization, char digest[SHA256_HEX_BYTES + 1U],
    upload_metrics_t *metrics);
upload_result_t upload_cancel(upload_manager_t *manager, const char *upload_id,
                              const char *client, const char *authorization);
void upload_abort(upload_manager_t *manager, const char *upload_id);
int upload_snapshot(upload_manager_t *manager, upload_snapshot_t *snapshot);
/* Deterministic lease hook for lifecycle tests. */
void upload_reap_at(upload_manager_t *manager, uint64_t now_seconds);

#endif
