#include "upload.h"

#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct upload_manager {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t writer_thread;
    int writer_thread_started;
    int writer_shutdown;
    int writer_discard;
    int writer_active;
    int writer_error;
    unsigned int write_head;
    unsigned int write_tail;
    unsigned int write_queued;
    size_t write_sizes[UPLOAD_WRITE_QUEUE_DEPTH];
    char destination[CONFIG_DESTINATION_CAPACITY];
    uint64_t max_file_bytes;
    unsigned int max_files;
    unsigned int lease_seconds;
    unsigned int completed_files;
    char completed_authorization[UPLOAD_AUTH_BYTES];
    int occupied;
    int started;
    int fd;
    uint64_t expected_size;
    uint64_t received;
    uint64_t last_activity;
    char upload_id[UPLOAD_ID_HEX_BYTES + 1U];
    char client[64];
    char authorization[UPLOAD_AUTH_BYTES];
    char final_name[SAFE_PATH_NAME_BYTES + 1U];
    char final_path[SAFE_PATH_FULL_BYTES];
    char part_path[SAFE_PATH_FULL_BYTES];
    char expected_hash[SHA256_HEX_BYTES + 1U];
    sha256_context_t sha256;
    unsigned char *write_buffer;
    size_t buffered;
    int fast_direct_stream;
    int direct_io;
    size_t chunk_count;
    size_t chunks_completed;
    unsigned char *chunk_states;
    uint64_t *chunk_progress;
    unsigned char **chunk_buffers;
    size_t next_chunk_to_write;
    unsigned int active_chunk_buffers;
    int chunk_transport;
    int finalizing_chunks;
    unsigned int active_chunk_writes;
    uint64_t preparation_start_ns;
    uint64_t preparation_elapsed_ns;
    uint64_t transfer_start_ns;
    uint64_t write_elapsed_ns;
    int has_last_metrics;
    uint64_t last_completed_size;
    upload_metrics_t last_metrics;
};

static void *upload_writer_main(void *argument);
static uint64_t chunk_length_for(const upload_manager_t *manager,
                                 size_t index);

static unsigned char *write_queue_buffer(upload_manager_t *manager,
                                         unsigned int index) {
    return manager->write_buffer +
           (size_t)index * (size_t)UPLOAD_WRITE_BUFFER_BYTES;
}

static int managed_part_name(const char *name) {
    static const char marker[] = ".ps5localsend-";
    static const char suffix[] = ".part";
    const size_t tail_size = sizeof(marker) - 1U + UPLOAD_ID_HEX_BYTES +
                             sizeof(suffix) - 1U;
    size_t length;
    size_t index;
    if (name == NULL) {
        return 0;
    }
    length = strlen(name);
    if (length != tail_size) {
        return 0;
    }
    if (memcmp(name, marker, sizeof(marker) - 1U) != 0) {
        return 0;
    }
    for (index = 0U; index < UPLOAD_ID_HEX_BYTES; ++index) {
        const char character = name[sizeof(marker) - 1U + index];
        if (!isdigit((unsigned char)character) &&
            (character < 'a' || character > 'f')) {
            return 0;
        }
    }
    return strcmp(name + sizeof(marker) - 1U + UPLOAD_ID_HEX_BYTES,
                  suffix) == 0;
}

/*
 * A same-directory hard link gives us atomic no-replace publication on the
 * host and on filesystems that implement link(2).  The PS5 data mount is not
 * required to implement hard links, so the PS5 build uses the same guarded
 * rename path as its compatibility fallback from the start.  The destination
 * name is selected while the upload slot is locked and checked immediately
 * before rename; the single-slot manager prevents an in-process race.
 *
 * Return 0 for a hard-link publication, 1 for a rename publication, and -1
 * for failure.  errno is preserved for the caller on failure.
 */
#ifdef PS5LOCALSEND_HOST
static int link_unsupported_errno(int error) {
    if (error == EPERM || error == EXDEV || error == ENOSYS) {
        return 1;
    }
#ifdef EOPNOTSUPP
    if (error == EOPNOTSUPP) {
        return 1;
    }
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
    if (error == ENOTSUP) {
        return 1;
    }
#endif
    return 0;
}
#endif

static int publish_part(const char *part_path, const char *final_path) {
    struct stat final_status;

#ifdef PS5LOCALSEND_HOST
    int link_error;
    if (link(part_path, final_path) == 0) {
        return 0;
    }
    link_error = errno;
    if (link_error == EEXIST || !link_unsupported_errno(link_error)) {
        errno = link_error;
        return -1;
    }

    if (lstat(final_path, &final_status) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
#else
    if (lstat(final_path, &final_status) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
#endif
    if (rename(part_path, final_path) != 0) {
        return -1;
    }
#ifdef PS5LOCALSEND_HOST
    fprintf(stderr,
            "warning: hard-link publication is unavailable; used rename fallback\n");
#endif
    return 1;
}

/*
 * The PS5 data filesystem may expose regular files without a reliable fsync(2)
 * implementation.  The target build therefore relies on close(2) followed by
 * same-directory rename(2) for publication.  The host build still calls
 * fsync, tolerating only errors that explicitly mean the operation is absent;
 * real I/O and capacity errors remain fatal there.
 */
#ifdef PS5LOCALSEND_HOST
static int sync_unsupported_errno(int error) {
    if (error == EINVAL || error == EPERM || error == ENOSYS) {
        return 1;
    }
#ifdef EOPNOTSUPP
    if (error == EOPNOTSUPP) {
        return 1;
    }
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
    if (error == ENOTSUP) {
        return 1;
    }
#endif
    return 0;
}
#endif

static int sync_for_publication(int descriptor) {
#ifdef PS5LOCALSEND_HOST
    static int warned_unavailable;
    int sync_error;
    for (;;) {
        if (fsync(descriptor) == 0) {
            return 0;
        }
        sync_error = errno;
        if (sync_error == EINTR) {
            continue;
        }
        if (!sync_unsupported_errno(sync_error)) {
            errno = sync_error;
            return -1;
        }
        if (warned_unavailable == 0) {
            fprintf(stderr,
                    "warning: fsync is unavailable on destination filesystem; "
                    "continuing with close and publication\n");
            warned_unavailable = 1;
        }
        return 0;
    }
#else
    static int warned_unavailable;
    (void)descriptor;
    if (warned_unavailable == 0) {
        fprintf(stderr,
                "warning: skipping fsync on PS5 destination; using close and "
                "rename for publication\n");
        warned_unavailable = 1;
    }
    return 0;
#endif
}

static int open_part_file(const char *path, int *direct_io) {
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    if (direct_io == NULL) {
        errno = EINVAL;
        return -1;
    }
    *direct_io = 0;
#ifdef PS5LOCALSEND_HOST
#ifdef O_NOFOLLOW
    {
        static int warned_no_follow;
        int descriptor = open(path, flags | O_NOFOLLOW, 0600);
        int open_error;
        if (descriptor >= 0) {
            return descriptor;
        }
        open_error = errno;
        if (open_error != EINVAL && open_error != EPERM && open_error != ENOSYS
#ifdef EOPNOTSUPP
            && open_error != EOPNOTSUPP
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
            && open_error != ENOTSUP
#endif
        ) {
            errno = open_error;
            return -1;
        }
        descriptor = open(path, flags, 0600);
        if (descriptor >= 0 && warned_no_follow == 0) {
            fprintf(stderr,
                    "warning: O_NOFOLLOW is unavailable; using O_EXCL for "
                    "temporary upload creation\n");
            warned_no_follow = 1;
        }
        return descriptor;
    }
#else
    return open(path, flags, 0600);
#endif
#else
    /* Match the PS5 FTP write path: sequential buffered I/O lets the kernel
     * merge and schedule writes while the receiver continues draining TCP.
     * O_DIRECT previously coupled every network callback to storage latency. */
    {
        int descriptor;
        descriptor = open(path, flags, 0600);
        if (descriptor >= 0 || errno == EEXIST) {
            return descriptor;
        }
        if (errno != EINVAL && errno != EPERM && errno != ENOSYS
#ifdef EOPNOTSUPP
            && errno != EOPNOTSUPP
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
            && errno != ENOTSUP
#endif
        ) {
            return -1;
        }
        /* A few PS5 data mounts implement create/truncate but reject O_EXCL.
         * The temporary basename is generated from secure random bytes and
         * only one upload is active at a time, so retrying with O_TRUNC is
         * safe here. */
        descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (descriptor >= 0) {
            fprintf(stderr,
                    "warning: O_EXCL is unavailable on PS5 destination; "
                    "using a random temporary name with O_TRUNC\n");
        }
        return descriptor;
    }
#endif
}

/*
 * Reserve the complete temporary-file extent before the first byte arrives.
 * On the PS5 data mounts this avoids making the filesystem repeatedly extend
 * and allocate the file while libmicrohttpd is trying to keep the TCP window
 * full. Some mounts do not implement POSIX fallocate; those are allowed to
 * use the previous incremental-allocation behavior. posix_fallocate returns
 * an error number directly rather than setting errno.
 */
static int reserve_upload_space(int descriptor, uint64_t size) {
#ifdef PS5LOCALSEND_HOST
    (void)descriptor;
    (void)size;
    return 0;
#else
    static int warned_unavailable;
    int result;

    if (size > (uint64_t)INT64_MAX) {
        errno = EFBIG;
        return -1;
    }
    result = posix_fallocate(descriptor, (off_t)0, (off_t)size);
    if (result == 0) {
        return 0;
    }
    if (result == EINVAL || result == ENOSYS
#ifdef EOPNOTSUPP
        || result == EOPNOTSUPP
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
        || result == ENOTSUP
#endif
    ) {
        if (warned_unavailable == 0) {
            fprintf(stderr,
                    "warning: destination does not support file preallocation; "
                    "using incremental writes\n");
            warned_unavailable = 1;
        }
        return 0;
    }
    errno = result;
    return -1;
#endif
}

static uint64_t current_seconds(void) {
    time_t now = time(NULL);
    return now == (time_t)-1 ? 0U : (uint64_t)now;
}

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static uint64_t elapsed_nanoseconds(uint64_t start, uint64_t end) {
    return end >= start && start != 0U ? end - start : 0U;
}

static uint64_t nanoseconds_to_milliseconds(uint64_t nanoseconds) {
    return (nanoseconds + UINT64_C(999999)) / UINT64_C(1000000);
}

static int ensure_destination_directory(const char *path) {
    char anchor[CONFIG_DESTINATION_CAPACITY];
    const char *end;
    size_t anchor_size;
    struct stat anchor_status;
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (path[0] != '/') {
        return safe_path_ensure_directory(path);
    }
    end = strchr(path + 1, '/');
    if (end == NULL) end = path + strlen(path);
    if (strncmp(path, "/mnt/", sizeof("/mnt/") - 1U) == 0) {
        const char *mount_end = strchr(path + sizeof("/mnt/") - 1U, '/');
        end = mount_end == NULL ? path + strlen(path) : mount_end;
    }
    anchor_size = (size_t)(end - path);
    if (anchor_size < 2U || anchor_size >= sizeof(anchor)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(anchor, path, anchor_size);
    anchor[anchor_size] = '\0';
    if (lstat(anchor, &anchor_status) != 0 ||
        !S_ISDIR(anchor_status.st_mode)) {
        errno = ENOENT;
        return -1;
    }
    return safe_path_ensure_directory(path);
}

static void cleanup_stale_parts(const char *directory) {
    DIR *stream = opendir(directory);
    struct dirent *entry;
    time_t now = time(NULL);
    if (stream == NULL || now == (time_t)-1) return;
    while ((entry = readdir(stream)) != NULL) {
        char path[SAFE_PATH_FULL_BYTES];
        struct stat status;
        if (!managed_part_name(entry->d_name) ||
            safe_path_join(directory, entry->d_name, path, sizeof(path)) != 0 ||
            lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_mtime > now - 86400) continue;
        if (unlink(path) == 0) fprintf(stderr, "removed stale upload part: %s\n", entry->d_name);
    }
    closedir(stream);
}

static int clear_slot(upload_manager_t *manager, int remove_part) {
    int failed = 0;
    size_t chunk_index;
    manager->writer_discard = 1;
    (void)pthread_cond_broadcast(&manager->condition);
    while (manager->writer_active != 0) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    if (manager->write_buffer != NULL && manager->buffered > 0U) {
        memset(write_queue_buffer(manager, manager->write_tail), 0,
               manager->buffered);
    }
    memset(manager->write_sizes, 0, sizeof(manager->write_sizes));
    manager->write_head = 0U;
    manager->write_tail = 0U;
    manager->write_queued = 0U;
    manager->buffered = 0U;
    manager->writer_error = 0;
    if (manager->fd >= 0) {
        if (close(manager->fd) != 0) failed = 1;
        manager->fd = -1;
    }
    if (remove_part && manager->part_path[0] != '\0' &&
        unlink(manager->part_path) != 0 && errno != ENOENT) {
        failed = 1;
    }
    manager->occupied = 0; manager->started = 0; manager->expected_size = 0U; manager->received = 0U;
    manager->last_activity = 0U;
    memset(manager->upload_id, 0, sizeof(manager->upload_id));
    memset(manager->client, 0, sizeof(manager->client));
    memset(manager->authorization, 0, sizeof(manager->authorization));
    memset(manager->final_name, 0, sizeof(manager->final_name));
    memset(manager->final_path, 0, sizeof(manager->final_path));
    memset(manager->part_path, 0, sizeof(manager->part_path));
    memset(manager->expected_hash, 0, sizeof(manager->expected_hash));
    memset(&manager->sha256, 0, sizeof(manager->sha256));
    if (manager->chunk_buffers != NULL) {
        for (chunk_index = 0U; chunk_index < manager->chunk_count;
             ++chunk_index) {
            free(manager->chunk_buffers[chunk_index]);
        }
    }
    free(manager->chunk_states);
    free(manager->chunk_progress);
    free(manager->chunk_buffers);
    manager->chunk_states = NULL;
    manager->chunk_progress = NULL;
    manager->chunk_buffers = NULL;
    manager->chunk_count = 0U;
    manager->chunks_completed = 0U;
    manager->next_chunk_to_write = 0U;
    manager->active_chunk_buffers = 0U;
    manager->chunk_transport = 0;
    manager->finalizing_chunks = 0;
    manager->active_chunk_writes = 0U;
    manager->fast_direct_stream = 0;
    manager->direct_io = 0;
    manager->preparation_start_ns = 0U;
    manager->preparation_elapsed_ns = 0U;
    manager->transfer_start_ns = 0U;
    manager->write_elapsed_ns = 0U;
    manager->writer_discard = 0;
    (void)pthread_cond_broadcast(&manager->condition);
    return failed ? -1 : 0;
}

static int write_all(upload_manager_t *manager, const unsigned char *cursor,
                     size_t remaining) {
    while (remaining > 0U) {
        ssize_t count = write(manager->fd, cursor, remaining);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                return EIO;
            }
            return errno;
        }
        cursor += (size_t)count;
        remaining -= (size_t)count;
    }
    return 0;
}

static int enqueue_write_buffer_locked(upload_manager_t *manager) {
    if (manager->buffered == 0U) {
        return 0;
    }
    while (manager->write_queued == UPLOAD_WRITE_QUEUE_DEPTH &&
           manager->writer_error == 0 && manager->writer_discard == 0) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    if (manager->writer_error != 0) {
        return manager->writer_error;
    }
    if (manager->writer_discard != 0 || manager->writer_shutdown != 0) {
        return ECANCELED;
    }
    manager->write_sizes[manager->write_tail] = manager->buffered;
    manager->write_tail =
        (manager->write_tail + 1U) % UPLOAD_WRITE_QUEUE_DEPTH;
    ++manager->write_queued;
    manager->buffered = 0U;
    (void)pthread_cond_broadcast(&manager->condition);
    return 0;
}

static int wait_for_write_queue_locked(upload_manager_t *manager) {
    while (manager->writer_error == 0 &&
           (manager->write_queued != 0U || manager->writer_active != 0)) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    return manager->writer_error;
}

static int chunk_ready_for_writer_locked(const upload_manager_t *manager) {
    return manager->chunk_transport != 0 &&
           manager->next_chunk_to_write < manager->chunk_count &&
           manager->chunk_states != NULL && manager->chunk_buffers != NULL &&
           manager->chunk_states[manager->next_chunk_to_write] == 2U &&
           manager->chunk_buffers[manager->next_chunk_to_write] != NULL;
}

/* A single sequential writer mirrors the fast path used by FTP receivers.
 * libmicrohttpd only copies into the bounded producer queue; disk I/O happens
 * concurrently and never interleaves offsets from multiple HTTP requests. */
static void *upload_writer_main(void *argument) {
    upload_manager_t *manager = argument;
    pthread_mutex_lock(&manager->mutex);
    for (;;) {
        unsigned int queue_index = 0U;
        size_t chunk_index = 0U;
        unsigned char *buffer;
        size_t size;
        int chunk_job;
        uint64_t write_start_ns;
        uint64_t write_elapsed_ns;
        int write_error;
        while (manager->writer_shutdown == 0 &&
               (manager->writer_discard != 0 ||
                manager->writer_error != 0 ||
                (manager->write_queued == 0U &&
                 !chunk_ready_for_writer_locked(manager)))) {
            (void)pthread_cond_wait(&manager->condition, &manager->mutex);
        }
        if (manager->writer_shutdown != 0) {
            break;
        }
        chunk_job = manager->write_queued == 0U;
        if (chunk_job != 0) {
            chunk_index = manager->next_chunk_to_write;
            buffer = manager->chunk_buffers[chunk_index];
            size = (size_t)chunk_length_for(manager, chunk_index);
        } else {
            queue_index = manager->write_head;
            buffer = write_queue_buffer(manager, queue_index);
            size = manager->write_sizes[queue_index];
        }
        manager->writer_active = 1;
        pthread_mutex_unlock(&manager->mutex);

        write_start_ns = monotonic_nanoseconds();
        write_error = write_all(manager, buffer, size);
        write_elapsed_ns = elapsed_nanoseconds(
            write_start_ns, monotonic_nanoseconds());

        pthread_mutex_lock(&manager->mutex);
        manager->write_elapsed_ns += write_elapsed_ns;
        if (chunk_job != 0) {
            if (write_error == 0 && manager->chunk_buffers != NULL &&
                manager->chunk_states != NULL &&
                chunk_index == manager->next_chunk_to_write &&
                manager->chunk_buffers[chunk_index] == buffer) {
                free(manager->chunk_buffers[chunk_index]);
                manager->chunk_buffers[chunk_index] = NULL;
                manager->chunk_states[chunk_index] = 3U;
                ++manager->next_chunk_to_write;
                if (manager->active_chunk_buffers > 0U) {
                    --manager->active_chunk_buffers;
                }
            }
        } else {
            manager->write_sizes[queue_index] = 0U;
            manager->write_head =
                (manager->write_head + 1U) % UPLOAD_WRITE_QUEUE_DEPTH;
            if (manager->write_queued > 0U) {
                --manager->write_queued;
            }
        }
        manager->writer_active = 0;
        if (write_error != 0 && manager->writer_error == 0) {
            manager->writer_error = write_error;
        }
        (void)pthread_cond_broadcast(&manager->condition);
    }
    manager->writer_active = 0;
    (void)pthread_cond_broadcast(&manager->condition);
    pthread_mutex_unlock(&manager->mutex);
    return NULL;
}

static uint64_t chunk_length_for(const upload_manager_t *manager,
                                 size_t index) {
    uint64_t offset = (uint64_t)index * (uint64_t)UPLOAD_CHUNK_BYTES;
    uint64_t remaining;
    if (manager == NULL || offset >= manager->expected_size) {
        return 0U;
    }
    remaining = manager->expected_size - offset;
    return remaining < (uint64_t)UPLOAD_CHUNK_BYTES
               ? remaining
               : (uint64_t)UPLOAD_CHUNK_BYTES;
}

static int hash_part_file(const char *path,
                          char digest[SHA256_HEX_BYTES + 1U]) {
    int descriptor;
    unsigned char *buffer;
    sha256_context_t context;
    int saved_error = 0;
    if (path == NULL || digest == NULL) {
        errno = EINVAL;
        return -1;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return -1;
    }
    buffer = malloc(1024U * 1024U);
    if (buffer == NULL) {
        saved_error = ENOMEM;
    } else {
        sha256_init(&context);
        for (;;) {
            ssize_t count = read(descriptor, buffer, 1024U * 1024U);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                saved_error = errno;
                break;
            }
            if (count == 0) {
                unsigned char raw[SHA256_DIGEST_BYTES];
                sha256_final(&context, raw);
                sha256_hex(raw, digest);
                memset(raw, 0, sizeof(raw));
                break;
            }
            sha256_update(&context, buffer, (size_t)count);
        }
    }
    if (buffer != NULL) {
        memset(buffer, 0, 1024U * 1024U);
        free(buffer);
    }
    if (close(descriptor) != 0 && saved_error == 0) {
        saved_error = errno;
    }
    if (saved_error != 0) {
        errno = saved_error;
        return -1;
    }
    return 0;
}

static void reap_locked(upload_manager_t *manager, uint64_t now) {
    if (manager->finalizing_chunks == 0 && manager->active_chunk_writes == 0U &&
        manager->occupied &&
        manager->last_activity != 0U &&
        now >= manager->last_activity &&
        now - manager->last_activity >= manager->lease_seconds)
        if (clear_slot(manager, 1) != 0)
            fprintf(stderr, "warning: unable to clean up expired upload\n");
}

void upload_reap_at(upload_manager_t *manager, uint64_t now_seconds) {
    if (manager == NULL) return;
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, now_seconds);
    pthread_mutex_unlock(&manager->mutex);
}

static int binding_matches(const upload_manager_t *manager, const char *client,
                           const char *authorization) {
    return client != NULL && authorization != NULL &&
           strcmp(manager->client, client) == 0 &&
           strcmp(manager->authorization, authorization) == 0;
}

static int random_id(char output[UPLOAD_ID_HEX_BYTES + 1U]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[UPLOAD_ID_HEX_BYTES / 2U];
    size_t index;
    if (platform_secure_random(bytes, sizeof(bytes)) != 0) return -1;
    for (index = 0U; index < sizeof(bytes); ++index) {
        output[index * 2U] = hex[bytes[index] >> 4U];
        output[index * 2U + 1U] = hex[bytes[index] & 15U];
    }
    output[UPLOAD_ID_HEX_BYTES] = '\0';
    memset(bytes, 0, sizeof(bytes));
    return 0;
}

static void skip_ws(const unsigned char **cursor, const unsigned char *end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t' ||
                             **cursor == '\n' || **cursor == '\r')) {
        ++*cursor;
    }
}

static int json_string(const unsigned char **cursor, const unsigned char *end,
                       char *output, size_t capacity) {
    size_t length = 0U;
    if (*cursor >= end || *(*cursor)++ != '"') return -1;
    while (*cursor < end && **cursor != '"') {
        unsigned char c = *(*cursor)++;
        if (c == '\\') {
            if (*cursor >= end) return -1;
            c = *(*cursor)++;
            if (c != '"' && c != '\\' && c != '/') return -1;
        }
        if (c < 0x20U || length + 1U >= capacity) return -1;
        output[length++] = (char)c;
    }
    if (*cursor >= end || *(*cursor)++ != '"') return -1;
    output[length] = '\0';
    return 0;
}

static int json_u64(const unsigned char **cursor, const unsigned char *end,
                    uint64_t *value) {
    uint64_t number = 0U;
    const unsigned char *start = *cursor;
    while (*cursor < end && isdigit(**cursor)) {
        unsigned int digit = (unsigned int)(**cursor - '0');
        if (number > (UINT64_MAX - digit) / 10U) return -1;
        number = number * 10U + digit; ++*cursor;
    }
    if (*cursor == start) return -1;
    if ((size_t)(*cursor - start) > 1U && *start == '0') return -1;
    *value = number;
    return 0;
}

static int parse_metadata(const unsigned char *json, size_t size, char *name,
                          size_t name_size, uint64_t *file_size,
                          char expected_hash[SHA256_HEX_BYTES + 1U]) {
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned int seen = 0U;
    char type[128];
    if (json == NULL || size == 0U) return -1;
    cursor = json;
    end = json + size;
    name[0] = '\0'; type[0] = '\0'; expected_hash[0] = '\0';
    skip_ws(&cursor, end);
    if (cursor >= end || *cursor++ != '{') return -1;
    for (;;) {
        char key[16];
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == '}') { ++cursor; break; }
        if (json_string(&cursor, end, key, sizeof(key)) != 0) return -1;
        skip_ws(&cursor, end); if (cursor >= end || *cursor++ != ':') return -1; skip_ws(&cursor, end);
        if (strcmp(key, "name") == 0 && !(seen & 1U)) {
            if (json_string(&cursor, end, name, name_size) != 0) return -1; seen |= 1U;
        } else if (strcmp(key, "size") == 0 && !(seen & 2U)) {
            if (json_u64(&cursor, end, file_size) != 0) return -1; seen |= 2U;
        } else if (strcmp(key, "type") == 0 && !(seen & 4U)) {
            if (json_string(&cursor, end, type, sizeof(type)) != 0) return -1; seen |= 4U;
        } else if (strcmp(key, "sha256") == 0 && !(seen & 8U)) {
            size_t i;
            if (json_string(&cursor, end, expected_hash, SHA256_HEX_BYTES + 1U) != 0 || strlen(expected_hash) != SHA256_HEX_BYTES) return -1;
            for (i = 0U; i < SHA256_HEX_BYTES; ++i) if (!isdigit((unsigned char)expected_hash[i]) && (expected_hash[i] < 'a' || expected_hash[i] > 'f')) return -1;
            seen |= 8U;
        } else return -1;
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor; skip_ws(&cursor, end);
            if (cursor >= end || *cursor == '}') return -1;
            continue;
        }
        if (cursor < end && *cursor == '}') { ++cursor; break; }
        return -1;
    }
    skip_ws(&cursor, end);
    return cursor == end && (seen & 7U) == 7U &&
                   safe_path_validate_filename(name) &&
                   !managed_part_name(name) && type[0] != '\0'
               ? 0
               : -1;
}

upload_manager_t *upload_manager_create(const app_config_t *config) {
    upload_manager_t *manager;
    const char *destination;
#ifdef PS5LOCALSEND_HOST
    char host_destination[CONFIG_DESTINATION_CAPACITY];
    const char *lease_override;
#endif
    if (config == NULL) return NULL;
    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) return NULL;
    manager->fd = -1;
#ifdef PS5LOCALSEND_HOST
    destination = getenv("PS5LOCALSEND_UPLOAD_DIR");
    if (destination == NULL || destination[0] != '/') {
        int count = snprintf(host_destination, sizeof(host_destination), "%s/inbox", platform_data_dir());
        if (count < 0 || (size_t)count >= sizeof(host_destination)) { free(manager); return NULL; }
        destination = host_destination;
    }
#else
    destination = config->destination;
#endif
    if (strlen(destination) >= sizeof(manager->destination) ||
        ensure_destination_directory(destination) != 0 ||
        pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager); return NULL;
    }
    if (pthread_cond_init(&manager->condition, NULL) != 0) {
        pthread_mutex_destroy(&manager->mutex);
        free(manager);
        return NULL;
    }
#ifdef PS5LOCALSEND_HOST
    manager->write_buffer = malloc(UPLOAD_WRITE_BUFFER_ALLOCATION_BYTES);
#else
    {
        void *aligned_buffer = NULL;
        if (posix_memalign(&aligned_buffer, UPLOAD_WRITE_ALIGNMENT_BYTES,
                           UPLOAD_WRITE_BUFFER_ALLOCATION_BYTES) == 0) {
            manager->write_buffer = aligned_buffer;
        }
    }
#endif
    if (manager->write_buffer == NULL) {
        pthread_cond_destroy(&manager->condition);
        pthread_mutex_destroy(&manager->mutex);
        free(manager);
        return NULL;
    }
    if (pthread_create(&manager->writer_thread, NULL, upload_writer_main,
                       manager) != 0) {
        free(manager->write_buffer);
        pthread_cond_destroy(&manager->condition);
        pthread_mutex_destroy(&manager->mutex);
        free(manager);
        return NULL;
    }
    manager->writer_thread_started = 1;
    memcpy(manager->destination, destination, strlen(destination) + 1U);
    cleanup_stale_parts(manager->destination);
    manager->max_file_bytes = config->max_file_bytes;
    manager->max_files = config->max_files_per_session;
    manager->lease_seconds = UPLOAD_LEASE_SECONDS;
#ifdef PS5LOCALSEND_HOST
    lease_override = getenv("PS5LOCALSEND_UPLOAD_LEASE_SECONDS");
    if (lease_override != NULL) {
        char *end = NULL;
        unsigned long parsed;
        errno = 0;
        parsed = strtoul(lease_override, &end, 10);
        if (errno == 0 && end != lease_override && *end == '\0' &&
            parsed >= 1UL && parsed <= 3600UL)
            manager->lease_seconds = (unsigned int)parsed;
    }
#endif
    return manager;
}

void upload_manager_destroy(upload_manager_t *manager) {
    if (manager == NULL) return;
    pthread_mutex_lock(&manager->mutex);
    while (manager->finalizing_chunks != 0 || manager->active_chunk_writes != 0U) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    manager->writer_discard = 1;
    manager->writer_shutdown = 1;
    (void)pthread_cond_broadcast(&manager->condition);
    pthread_mutex_unlock(&manager->mutex);
    if (manager->writer_thread_started != 0) {
        (void)pthread_join(manager->writer_thread, NULL);
        manager->writer_thread_started = 0;
    }
    pthread_mutex_lock(&manager->mutex);
    if (clear_slot(manager, 1) != 0)
        fprintf(stderr, "warning: unable to clean up upload during shutdown\n");
    pthread_mutex_unlock(&manager->mutex);
    pthread_cond_destroy(&manager->condition);
    pthread_mutex_destroy(&manager->mutex);
    free(manager->write_buffer);
    memset(manager, 0, sizeof(*manager)); free(manager);
}

upload_result_t upload_manager_set_destination(upload_manager_t *manager,
                                               const char *destination) {
    const char *effective = destination;
#ifdef PS5LOCALSEND_HOST
    const char *override = getenv("PS5LOCALSEND_UPLOAD_DIR");
    if (override != NULL && override[0] == '/') {
        effective = override;
    }
#endif
    if (manager == NULL || effective == NULL || effective[0] != '/' ||
        strlen(effective) >= sizeof(manager->destination)) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (manager->occupied) {
        pthread_mutex_unlock(&manager->mutex);
        return UPLOAD_BUSY;
    }
    if (ensure_destination_directory(effective) != 0) {
        upload_result_t result = errno == ENOSPC ? UPLOAD_STORAGE_FULL
                                                 : UPLOAD_IO_ERROR;
        pthread_mutex_unlock(&manager->mutex);
        return result;
    }
    if (strcmp(manager->destination, effective) != 0) {
        memcpy(manager->destination, effective, strlen(effective) + 1U);
        cleanup_stale_parts(manager->destination);
    }
    pthread_mutex_unlock(&manager->mutex);
    return UPLOAD_OK;
}

upload_result_t upload_prepare(upload_manager_t *manager, const char *client,
                               const char *authorization, const unsigned char *json,
                               size_t json_size, upload_prepared_t *prepared) {
    char requested[SAFE_PATH_NAME_BYTES + 1U], hash[SHA256_HEX_BYTES + 1U];
    uint64_t size;
    struct statvfs space;
    upload_result_t result = UPLOAD_OK;
    if (manager == NULL || client == NULL || authorization == NULL || prepared == NULL || strlen(client) >= sizeof(manager->client) || strlen(authorization) >= sizeof(manager->authorization) || json_size > UPLOAD_METADATA_BYTES || parse_metadata(json, json_size, requested, sizeof(requested), &size, hash) != 0) return UPLOAD_INVALID;
    if (size > manager->max_file_bytes) return UPLOAD_TOO_LARGE;
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (manager->occupied && !manager->started &&
        strcmp(manager->authorization, authorization) != 0)
        if (clear_slot(manager, 1) != 0)
            fprintf(stderr, "warning: unable to clean up superseded upload\n");
    if (manager->occupied) result = UPLOAD_BUSY;
    else {
        int part_count;
        if (strcmp(manager->completed_authorization, authorization) != 0) {
            manager->completed_files = 0U;
            memcpy(manager->completed_authorization, authorization, strlen(authorization) + 1U);
        }
        if (manager->completed_files >= manager->max_files) result = UPLOAD_LIMIT_REACHED;
        else if (statvfs(manager->destination, &space) != 0 ||
                 (space.f_frsize != 0U &&
                  (size + (uint64_t)space.f_frsize - 1U) /
                      (uint64_t)space.f_frsize > (uint64_t)space.f_bavail)) result = UPLOAD_STORAGE_FULL;
        else if (safe_path_unique_name(manager->destination, requested, manager->final_name) != 0 ||
                 safe_path_join(manager->destination, manager->final_name, manager->final_path, sizeof(manager->final_path)) != 0) result = UPLOAD_IO_ERROR;
        else if (random_id(manager->upload_id) != 0) {
            result = UPLOAD_IO_ERROR;
        } else {
            part_count = snprintf(manager->part_path,
                                  sizeof(manager->part_path),
                                  "%s/.ps5localsend-%s.part",
                                  manager->destination, manager->upload_id);
            if (part_count < 0 ||
                (size_t)part_count >= sizeof(manager->part_path)) {
                result = UPLOAD_IO_ERROR;
            }
        }
    }
    if (result == UPLOAD_OK) {
        manager->preparation_start_ns = monotonic_nanoseconds();
        manager->fd = open_part_file(manager->part_path, &manager->direct_io);
        if (manager->fd < 0) result = errno == ENOSPC ? UPLOAD_STORAGE_FULL : UPLOAD_IO_ERROR;
        else if (reserve_upload_space(manager->fd, size) != 0)
            result = errno == ENOSPC ? UPLOAD_STORAGE_FULL : UPLOAD_IO_ERROR;
        else {
            size_t chunk_count = size == 0U
                                     ? 0U
                                     : (size_t)((size - 1U) /
                                                (uint64_t)UPLOAD_CHUNK_BYTES) +
                                           1U;
            if (chunk_count > 0U) {
                manager->chunk_states = calloc(chunk_count,
                                               sizeof(*manager->chunk_states));
                manager->chunk_progress = calloc(chunk_count,
                                                  sizeof(*manager->chunk_progress));
                manager->chunk_buffers = calloc(chunk_count,
                                                 sizeof(*manager->chunk_buffers));
                if (manager->chunk_states == NULL ||
                    manager->chunk_progress == NULL ||
                    manager->chunk_buffers == NULL) {
                    result = UPLOAD_IO_ERROR;
                }
            }
            if (result != UPLOAD_OK) {
                errno = ENOMEM;
            }
        }
        if (result == UPLOAD_OK) {
            manager->preparation_elapsed_ns = elapsed_nanoseconds(
                manager->preparation_start_ns, monotonic_nanoseconds());
            manager->occupied = 1; manager->expected_size = size;
            manager->chunk_count = size == 0U
                                       ? 0U
                                       : (size_t)((size - 1U) /
                                                  (uint64_t)UPLOAD_CHUNK_BYTES) +
                                             1U;
            manager->chunks_completed = 0U;
            manager->next_chunk_to_write = 0U;
            manager->active_chunk_buffers = 0U;
            manager->last_activity = current_seconds();
            memcpy(manager->client, client, strlen(client) + 1U);
            memcpy(manager->authorization, authorization, strlen(authorization) + 1U);
            memcpy(manager->expected_hash, hash, sizeof(hash));
            memcpy(prepared->upload_id, manager->upload_id, sizeof(prepared->upload_id));
            memcpy(prepared->final_name, manager->final_name, sizeof(prepared->final_name));
            prepared->size = size;
        }
    }
    if (result != UPLOAD_OK && manager->fd >= 0 &&
        clear_slot(manager, 1) != 0)
        fprintf(stderr, "warning: unable to clean up failed upload preparation\n");
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_begin(upload_manager_t *manager, const char *id, const char *client, const char *authorization, uint64_t length) {
    upload_result_t result;
    if (manager == NULL || id == NULL) return UPLOAD_INVALID;
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || strcmp(manager->upload_id, id) != 0) result = UPLOAD_NOT_FOUND;
    else if (!binding_matches(manager, client, authorization)) result = UPLOAD_FORBIDDEN;
    else if (manager->started || manager->chunk_transport != 0)
        result = UPLOAD_BUSY;
    else if (length != manager->expected_size) result = UPLOAD_SIZE_MISMATCH;
    else {
        manager->chunk_transport = 0;
        manager->started = 1;
        manager->last_activity = current_seconds();
        manager->transfer_start_ns = monotonic_nanoseconds();
        manager->write_elapsed_ns = 0U;
        sha256_init(&manager->sha256);
        result = UPLOAD_OK;
    }
    pthread_mutex_unlock(&manager->mutex); return result;
}

upload_result_t upload_write(upload_manager_t *manager, const char *id,
                             const void *data, size_t size) {
    upload_result_t result = UPLOAD_OK;
    const unsigned char *cursor = data;
    size_t remaining = size;
    if (manager == NULL || id == NULL || (data == NULL && size != 0U)) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || !manager->started || strcmp(manager->upload_id, id) != 0) result = UPLOAD_NOT_FOUND;
    else if ((uint64_t)size > manager->expected_size - manager->received) result = UPLOAD_SIZE_MISMATCH;
    else while (remaining > 0U) {
        size_t space;
        size_t count;
        int write_error;
        while (manager->write_queued == UPLOAD_WRITE_QUEUE_DEPTH &&
               manager->writer_error == 0) {
            (void)pthread_cond_wait(&manager->condition, &manager->mutex);
        }
        if (manager->writer_error != 0) {
            write_error = manager->writer_error;
            errno = write_error;
            result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                           : UPLOAD_IO_ERROR;
            break;
        }
        space = UPLOAD_WRITE_BUFFER_BYTES - manager->buffered;
        count = remaining < space ? remaining : space;
        memcpy(write_queue_buffer(manager, manager->write_tail) +
                   manager->buffered,
               cursor, count);
        sha256_update(&manager->sha256, cursor, count);
        cursor += count;
        remaining -= count;
        manager->buffered += count;
        manager->received += (uint64_t)count;
        manager->last_activity = current_seconds();
        if (manager->buffered == UPLOAD_WRITE_BUFFER_BYTES) {
            write_error = enqueue_write_buffer_locked(manager);
            if (write_error != 0) {
                errno = write_error;
                result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                               : UPLOAD_IO_ERROR;
                break;
            }
        }
    }
    pthread_mutex_unlock(&manager->mutex); return result;
}

upload_result_t upload_enable_direct_stream(upload_manager_t *manager,
                                            const char *id) {
    upload_result_t result = UPLOAD_OK;
    if (manager == NULL || id == NULL) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    if (!manager->occupied || !manager->started ||
        strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (manager->chunk_transport != 0 || manager->received != 0U ||
               manager->buffered != 0U || manager->write_queued != 0U ||
               manager->writer_active != 0) {
        result = UPLOAD_BUSY;
    } else {
        manager->fast_direct_stream = 1;
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_write_direct(upload_manager_t *manager, const char *id,
                                    const void *data, size_t size) {
    upload_result_t result = UPLOAD_OK;
    uint64_t write_start_ns;
    uint64_t write_elapsed_ns;
    int write_error;
    if (manager == NULL || id == NULL || (data == NULL && size != 0U)) {
        return UPLOAD_INVALID;
    }
    if (size == 0U) {
        return UPLOAD_OK;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || !manager->started ||
        strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (manager->fast_direct_stream == 0) {
        result = UPLOAD_BUSY;
    } else if ((uint64_t)size > manager->expected_size - manager->received) {
        result = UPLOAD_SIZE_MISMATCH;
    } else if (manager->active_chunk_writes != 0U) {
        result = UPLOAD_BUSY;
    } else {
        ++manager->active_chunk_writes;
        manager->last_activity = current_seconds();
    }
    pthread_mutex_unlock(&manager->mutex);
    if (result != UPLOAD_OK) {
        return result;
    }

    write_start_ns = monotonic_nanoseconds();
    write_error = write_all(manager, data, size);
    write_elapsed_ns = elapsed_nanoseconds(write_start_ns,
                                           monotonic_nanoseconds());

    pthread_mutex_lock(&manager->mutex);
    manager->write_elapsed_ns += write_elapsed_ns;
    if (manager->active_chunk_writes > 0U) {
        --manager->active_chunk_writes;
    }
    if (write_error != 0) {
        manager->writer_error = write_error;
        errno = write_error;
        result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                       : UPLOAD_IO_ERROR;
    } else {
        manager->received += (uint64_t)size;
        manager->last_activity = current_seconds();
    }
    (void)pthread_cond_broadcast(&manager->condition);
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_chunk_begin(upload_manager_t *manager,
                                   const char *id, const char *client,
                                   const char *authorization, uint64_t total,
                                   uint64_t offset, uint64_t length) {
    upload_result_t result = UPLOAD_OK;
    size_t index;
    if (manager == NULL || id == NULL || client == NULL ||
        authorization == NULL || length == 0U) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (!binding_matches(manager, client, authorization)) {
        result = UPLOAD_FORBIDDEN;
    } else if (manager->finalizing_chunks != 0) {
        result = UPLOAD_BUSY;
    } else if (manager->started && manager->chunk_transport == 0) {
        result = UPLOAD_BUSY;
    } else if (total != manager->expected_size || offset >= total ||
               length > total - offset ||
               (offset % (uint64_t)UPLOAD_CHUNK_BYTES) != 0U ||
               (length != (uint64_t)UPLOAD_CHUNK_BYTES &&
                offset + length != total)) {
        result = UPLOAD_SIZE_MISMATCH;
    } else {
        index = (size_t)(offset / (uint64_t)UPLOAD_CHUNK_BYTES);
        if (index >= manager->chunk_count ||
            chunk_length_for(manager, index) != length) {
            result = UPLOAD_SIZE_MISMATCH;
        } else if (manager->chunk_states == NULL ||
                   manager->chunk_progress == NULL ||
                   manager->chunk_buffers == NULL) {
            result = UPLOAD_INVALID;
        } else if (manager->chunk_states[index] != 0U) {
            result = UPLOAD_BUSY;
        } else if (manager->active_chunk_buffers >=
                   UPLOAD_CHUNK_BUFFER_SLOTS) {
            result = UPLOAD_BACKPRESSURE;
        } else {
            unsigned char *buffer = malloc((size_t)length);
            if (buffer == NULL) {
                errno = ENOMEM;
                result = UPLOAD_IO_ERROR;
            } else {
                manager->chunk_buffers[index] = buffer;
                ++manager->active_chunk_buffers;
                manager->chunk_states[index] = 1U;
                manager->chunk_progress[index] = 0U;
                if (manager->started == 0) {
                    manager->chunk_transport = 1;
                    manager->started = 1;
                    manager->transfer_start_ns = monotonic_nanoseconds();
                    manager->write_elapsed_ns = 0U;
                }
                manager->last_activity = current_seconds();
            }
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_chunk_write(upload_manager_t *manager,
                                   const char *id, uint64_t offset,
                                   const void *data, size_t size) {
    upload_result_t result = UPLOAD_OK;
    size_t index;
    uint64_t relative;
    uint64_t chunk_length;
    if (manager == NULL || id == NULL || (data == NULL && size != 0U)) {
        return UPLOAD_INVALID;
    }
    if (size == 0U) {
        return UPLOAD_OK;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || !manager->started ||
        strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (offset >= manager->expected_size ||
               (uint64_t)size > manager->expected_size - offset) {
        result = UPLOAD_SIZE_MISMATCH;
    } else {
        index = (size_t)(offset / (uint64_t)UPLOAD_CHUNK_BYTES);
        if (index >= manager->chunk_count || manager->chunk_states == NULL ||
            manager->chunk_progress == NULL || manager->chunk_buffers == NULL ||
            manager->chunk_buffers[index] == NULL ||
            manager->chunk_states[index] != 1U) {
            result = UPLOAD_BUSY;
        } else {
            relative = offset - (uint64_t)index *
                                     (uint64_t)UPLOAD_CHUNK_BYTES;
            chunk_length = chunk_length_for(manager, index);
            if (relative != manager->chunk_progress[index] ||
                relative > chunk_length ||
                (uint64_t)size > chunk_length - relative) {
                result = UPLOAD_SIZE_MISMATCH;
            } else {
                memcpy(manager->chunk_buffers[index] + (size_t)relative,
                       data, size);
                manager->chunk_progress[index] += (uint64_t)size;
                manager->received += (uint64_t)size;
                manager->last_activity = current_seconds();
            }
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_chunk_finish(upload_manager_t *manager,
                                    const char *id, uint64_t offset,
                                    uint64_t length) {
    upload_result_t result = UPLOAD_OK;
    size_t index;
    if (manager == NULL || id == NULL || length == 0U) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || !manager->started ||
        strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (offset >= manager->expected_size ||
               (offset % (uint64_t)UPLOAD_CHUNK_BYTES) != 0U) {
        result = UPLOAD_SIZE_MISMATCH;
    } else {
        index = (size_t)(offset / (uint64_t)UPLOAD_CHUNK_BYTES);
        if (index >= manager->chunk_count || manager->chunk_states == NULL ||
            manager->chunk_progress == NULL || manager->chunk_buffers == NULL ||
            manager->chunk_buffers[index] == NULL ||
            manager->chunk_states[index] != 1U) {
            result = UPLOAD_BUSY;
        } else if (chunk_length_for(manager, index) != length ||
                   manager->chunk_progress[index] != length) {
            if (manager->received >= manager->chunk_progress[index]) {
                manager->received -= manager->chunk_progress[index];
            } else {
                manager->received = 0U;
            }
            free(manager->chunk_buffers[index]);
            manager->chunk_buffers[index] = NULL;
            if (manager->active_chunk_buffers > 0U) {
                --manager->active_chunk_buffers;
            }
            manager->chunk_progress[index] = 0U;
            manager->chunk_states[index] = 0U;
            (void)pthread_cond_broadcast(&manager->condition);
            result = UPLOAD_SIZE_MISMATCH;
        } else {
            manager->chunk_states[index] = 2U;
            ++manager->chunks_completed;
            manager->last_activity = current_seconds();
            (void)pthread_cond_broadcast(&manager->condition);
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

void upload_chunk_abort(upload_manager_t *manager, const char *id,
                        uint64_t offset, uint64_t length) {
    size_t index;
    uint64_t progress;
    (void)length;
    if (manager == NULL || id == NULL) {
        return;
    }
    pthread_mutex_lock(&manager->mutex);
    if (manager->occupied && manager->started &&
        strcmp(manager->upload_id, id) == 0 &&
        offset < manager->expected_size &&
        offset % (uint64_t)UPLOAD_CHUNK_BYTES == 0U) {
        index = (size_t)(offset / (uint64_t)UPLOAD_CHUNK_BYTES);
        if (index < manager->chunk_count && manager->chunk_states != NULL &&
            manager->chunk_progress != NULL && manager->chunk_buffers != NULL &&
            manager->chunk_states[index] == 1U) {
            progress = manager->chunk_progress[index];
            if (manager->received >= progress) {
                manager->received -= progress;
            } else {
                manager->received = 0U;
            }
            free(manager->chunk_buffers[index]);
            manager->chunk_buffers[index] = NULL;
            if (manager->active_chunk_buffers > 0U) {
                --manager->active_chunk_buffers;
            }
            manager->chunk_progress[index] = 0U;
            manager->chunk_states[index] = 0U;
            manager->last_activity = current_seconds();
            (void)pthread_cond_broadcast(&manager->condition);
        }
    }
    pthread_mutex_unlock(&manager->mutex);
}

upload_result_t upload_finish(
    upload_manager_t *manager, const char *id,
    char digest[SHA256_HEX_BYTES + 1U]) {
    return upload_finish_with_metrics(manager, id, digest, NULL);
}

upload_result_t upload_finish_with_metrics(
    upload_manager_t *manager, const char *id,
    char digest[SHA256_HEX_BYTES + 1U], upload_metrics_t *metrics) {
    unsigned char raw[SHA256_DIGEST_BYTES];
    upload_metrics_t completed_metrics;
    upload_result_t result = UPLOAD_OK;
    char part_path[SAFE_PATH_FULL_BYTES];
    uint64_t transfer_elapsed_ns = 0U;
    int hash_error = 0;
    int direct_stream = 0;
    int publish_result;
    if (manager == NULL || id == NULL || digest == NULL) {
        return UPLOAD_INVALID;
    }
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || !manager->started ||
        strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (manager->received != manager->expected_size) {
        result = UPLOAD_SIZE_MISMATCH;
    } else if (manager->writer_error != 0) {
        int write_error = manager->writer_error;
        errno = write_error;
        result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                       : UPLOAD_IO_ERROR;
    } else if (manager->fast_direct_stream != 0) {
        direct_stream = 1;
        transfer_elapsed_ns = elapsed_nanoseconds(
            manager->transfer_start_ns, monotonic_nanoseconds());
        memcpy(part_path, manager->part_path, sizeof(part_path));
        manager->finalizing_chunks = 1;
        pthread_mutex_unlock(&manager->mutex);
        if (hash_part_file(part_path, digest) != 0) {
            hash_error = errno;
        }
        pthread_mutex_lock(&manager->mutex);
        if (!manager->occupied || strcmp(manager->upload_id, id) != 0 ||
            manager->finalizing_chunks == 0) {
            result = UPLOAD_NOT_FOUND;
        } else if (hash_error != 0) {
            errno = hash_error;
            result = UPLOAD_IO_ERROR;
        }
    } else {
        int write_error = enqueue_write_buffer_locked(manager);
        if (write_error == 0) {
            write_error = wait_for_write_queue_locked(manager);
        }
        if (write_error != 0) {
            errno = write_error;
            result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                           : UPLOAD_IO_ERROR;
        }
        transfer_elapsed_ns = elapsed_nanoseconds(
            manager->transfer_start_ns, monotonic_nanoseconds());
    }
    if (result == UPLOAD_OK) {
        if (direct_stream == 0) {
            sha256_final(&manager->sha256, raw);
            sha256_hex(raw, digest);
            memset(raw, 0, sizeof(raw));
        }
        if (manager->expected_hash[0] != '\0' && strcmp(manager->expected_hash, digest) != 0) result = UPLOAD_HASH_MISMATCH;
        else if (sync_for_publication(manager->fd) != 0) result = errno == ENOSPC ? UPLOAD_STORAGE_FULL : UPLOAD_IO_ERROR;
        else if (close(manager->fd) != 0) { manager->fd = -1; result = UPLOAD_IO_ERROR; }
        else {
            manager->fd = -1;
            publish_result = publish_part(manager->part_path, manager->final_path);
            if (publish_result < 0) {
                result = errno == EEXIST ? UPLOAD_BUSY : UPLOAD_IO_ERROR;
            } else {
                completed_metrics.preparation_ms = nanoseconds_to_milliseconds(
                    manager->preparation_elapsed_ns);
                completed_metrics.transfer_ms = nanoseconds_to_milliseconds(
                    transfer_elapsed_ns);
                completed_metrics.write_ms = nanoseconds_to_milliseconds(
                    manager->write_elapsed_ns);
                completed_metrics.direct_io = manager->direct_io;
                manager->last_metrics = completed_metrics;
                manager->has_last_metrics = 1;
                manager->last_completed_size = manager->expected_size;
                if (metrics != NULL) {
                    *metrics = completed_metrics;
                }
                if (publish_result == 0 && unlink(manager->part_path) != 0) {
                    fprintf(stderr,
                            "warning: unable to remove completed upload part\n");
                }
                ++manager->completed_files;
                (void)clear_slot(manager, 0);
            }
        }
    }
    if (result != UPLOAD_OK && clear_slot(manager, 1) != 0) {
        fprintf(stderr, "warning: unable to clean up failed upload\n");
        result = UPLOAD_IO_ERROR;
    }
    pthread_mutex_unlock(&manager->mutex); return result;
}

upload_result_t upload_finish_chunks_with_metrics(
    upload_manager_t *manager, const char *id, const char *client,
    const char *authorization, char digest[SHA256_HEX_BYTES + 1U],
    upload_metrics_t *metrics) {
    upload_metrics_t completed_metrics;
    upload_result_t result = UPLOAD_OK;
    char part_path[SAFE_PATH_FULL_BYTES];
    int hash_error = 0;
    int owns_finalization = 0;
    int publish_result;
    if (manager == NULL || id == NULL || client == NULL ||
        authorization == NULL || digest == NULL) {
        return UPLOAD_INVALID;
    }
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    if (!manager->occupied || strcmp(manager->upload_id, id) != 0) {
        result = UPLOAD_NOT_FOUND;
    } else if (!binding_matches(manager, client, authorization)) {
        result = UPLOAD_FORBIDDEN;
    } else if (manager->finalizing_chunks != 0) {
        result = UPLOAD_BUSY;
    } else if (!manager->started) {
        result = UPLOAD_SIZE_MISMATCH;
    } else if (manager->chunks_completed != manager->chunk_count ||
               manager->received != manager->expected_size) {
        result = UPLOAD_SIZE_MISMATCH;
    } else {
        manager->finalizing_chunks = 1;
        owns_finalization = 1;
        (void)pthread_cond_broadcast(&manager->condition);
        while (manager->writer_error == 0 &&
               manager->next_chunk_to_write != manager->chunk_count) {
            (void)pthread_cond_wait(&manager->condition, &manager->mutex);
        }
        if (manager->writer_error != 0) {
            int write_error = manager->writer_error;
            errno = write_error;
            result = write_error == ENOSPC ? UPLOAD_STORAGE_FULL
                                           : UPLOAD_IO_ERROR;
        } else {
            memcpy(part_path, manager->part_path, sizeof(part_path));
        }
        /* Keep the slot alive while the potentially blocking hash pass runs
         * outside the mutex; cancellation and shutdown observe this marker. */
    }
    if (result == UPLOAD_OK && owns_finalization != 0) {
        pthread_mutex_unlock(&manager->mutex);
        if (hash_part_file(part_path, digest) != 0) {
            hash_error = errno;
        }
        pthread_mutex_lock(&manager->mutex);
        if (!manager->occupied || strcmp(manager->upload_id, id) != 0 ||
            manager->finalizing_chunks == 0) {
            result = UPLOAD_NOT_FOUND;
        } else if (hash_error != 0) {
            errno = hash_error;
            result = UPLOAD_IO_ERROR;
        } else if (manager->expected_hash[0] != '\0' &&
                   strcmp(manager->expected_hash, digest) != 0) {
            result = UPLOAD_HASH_MISMATCH;
        } else if (sync_for_publication(manager->fd) != 0) {
            result = errno == ENOSPC ? UPLOAD_STORAGE_FULL : UPLOAD_IO_ERROR;
        } else if (close(manager->fd) != 0) {
            manager->fd = -1;
            result = UPLOAD_IO_ERROR;
        } else {
            manager->fd = -1;
            publish_result = publish_part(manager->part_path,
                                          manager->final_path);
            if (publish_result < 0) {
                result = errno == EEXIST ? UPLOAD_BUSY : UPLOAD_IO_ERROR;
            } else {
                completed_metrics.preparation_ms = nanoseconds_to_milliseconds(
                    manager->preparation_elapsed_ns);
                completed_metrics.transfer_ms = nanoseconds_to_milliseconds(
                    elapsed_nanoseconds(manager->transfer_start_ns,
                                        monotonic_nanoseconds()));
                completed_metrics.write_ms = nanoseconds_to_milliseconds(
                    manager->write_elapsed_ns);
                completed_metrics.direct_io = manager->direct_io;
                manager->last_metrics = completed_metrics;
                manager->has_last_metrics = 1;
                manager->last_completed_size = manager->expected_size;
                if (metrics != NULL) {
                    *metrics = completed_metrics;
                }
                if (publish_result == 0 && unlink(manager->part_path) != 0) {
                    fprintf(stderr,
                            "warning: unable to remove completed upload part\n");
                }
                ++manager->completed_files;
                (void)clear_slot(manager, 0);
            }
        }
        if (manager->finalizing_chunks != 0) {
            manager->finalizing_chunks = 0;
            (void)pthread_cond_broadcast(&manager->condition);
        }
    }
    if (result != UPLOAD_OK && owns_finalization != 0 &&
        manager->occupied && strcmp(manager->upload_id, id) == 0 &&
        clear_slot(manager, 1) != 0) {
        fprintf(stderr, "warning: unable to clean up failed chunk upload\n");
        result = UPLOAD_IO_ERROR;
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

upload_result_t upload_cancel(upload_manager_t *manager, const char *id,
                              const char *client,
                              const char *authorization) {
    upload_result_t result;
    if (manager == NULL || id == NULL || client == NULL ||
        authorization == NULL) {
        return UPLOAD_INVALID;
    }
    pthread_mutex_lock(&manager->mutex);
    while (manager->active_chunk_writes != 0U) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    reap_locked(manager, current_seconds());
    if (!manager->occupied || strcmp(manager->upload_id, id) != 0) result = UPLOAD_NOT_FOUND;
    else if (!binding_matches(manager, client, authorization)) result = UPLOAD_FORBIDDEN;
    else if (manager->finalizing_chunks != 0) result = UPLOAD_BUSY;
    else result = clear_slot(manager, 1) == 0 ? UPLOAD_OK : UPLOAD_IO_ERROR;
    pthread_mutex_unlock(&manager->mutex); return result;
}

void upload_abort(upload_manager_t *manager, const char *id) {
    if (manager == NULL || id == NULL) return;
    pthread_mutex_lock(&manager->mutex);
    while (manager->active_chunk_writes != 0U) {
        (void)pthread_cond_wait(&manager->condition, &manager->mutex);
    }
    if (manager->finalizing_chunks == 0 && manager->occupied && manager->started &&
        strcmp(manager->upload_id, id) == 0 && clear_slot(manager, 1) != 0)
        fprintf(stderr, "warning: unable to clean up aborted upload\n");
    pthread_mutex_unlock(&manager->mutex);
}

int upload_snapshot(upload_manager_t *manager, upload_snapshot_t *snapshot) {
    if (manager == NULL || snapshot == NULL) {
        return -1;
    }
    pthread_mutex_lock(&manager->mutex);
    reap_locked(manager, current_seconds());
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = manager->occupied;
    snapshot->started = manager->started;
    snapshot->received = manager->received;
    snapshot->expected = manager->expected_size;
    snapshot->has_last_metrics = manager->has_last_metrics;
    snapshot->last_completed_size = manager->last_completed_size;
    snapshot->last_metrics = manager->last_metrics;
    if (manager->occupied) {
        memcpy(snapshot->name, manager->final_name, sizeof(snapshot->name));
    }
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}
