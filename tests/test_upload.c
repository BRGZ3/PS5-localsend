#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE

#include "config.h"
#include "safe_path.h"
#include "sha256.h"
#include "upload.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

static void hash_text(const char *text, const char *expected) {
    sha256_context_t context;
    unsigned char digest[SHA256_DIGEST_BYTES];
    char hex[SHA256_HEX_BYTES + 1U];
    sha256_init(&context); sha256_update(&context, text, strlen(text));
    sha256_final(&context, digest); sha256_hex(digest, hex);
    assert(strcmp(hex, expected) == 0);
}

static upload_prepared_t prepare(upload_manager_t *manager, const char *json) {
    upload_prepared_t prepared;
    assert(upload_prepare(manager, "127.0.0.1", "Bearer token", (const unsigned char *)json,
                          strlen(json), &prepared) == UPLOAD_OK);
    return prepared;
}

typedef struct chunk_thread_args {
    upload_manager_t *manager;
    const char *upload_id;
    const unsigned char *data;
    uint64_t total;
    uint64_t offset;
    uint64_t length;
    upload_result_t result;
} chunk_thread_args_t;

static void *send_chunk(void *argument) {
    chunk_thread_args_t *args = argument;
    uint64_t sent = 0U;
    args->result = upload_chunk_begin(
        args->manager, args->upload_id, "127.0.0.1", "Bearer chunks",
        args->total, args->offset, args->length);
    while (args->result == UPLOAD_OK && sent < args->length) {
        size_t count = (size_t)(args->length - sent);
        if (count > 65537U) {
            count = 65537U;
        }
        args->result = upload_chunk_write(
            args->manager, args->upload_id, args->offset + sent,
            args->data + args->offset + sent, count);
        sent += (uint64_t)count;
    }
    if (args->result == UPLOAD_OK) {
        args->result = upload_chunk_finish(
            args->manager, args->upload_id, args->offset, args->length);
    }
    return NULL;
}

int main(void) {
    char directory[1400];
    char temp_root[1400];
    char path[1400];
    app_config_t config;
    upload_manager_t *manager;
    upload_prepared_t prepared;
    upload_snapshot_t snapshot;
    char digest[SHA256_HEX_BYTES + 1U];
    FILE *file;

    hash_text("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash_text("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(safe_path_validate_filename("game.pkg"));
    assert(safe_path_validate_filename("файл.txt"));
    assert(!safe_path_validate_filename("../x"));
    assert(!safe_path_validate_filename("..\\x"));
    assert(!safe_path_validate_filename("%2e%2e%2fx"));
    assert(!safe_path_validate_filename("."));

    assert(realpath("/tmp", temp_root) != NULL);
    {
        int count = snprintf(directory, sizeof(directory),
                             "%s/ps5localsend-upload-XXXXXX", temp_root);
        assert(count > 0 && (size_t)count < sizeof(directory));
    }
    assert(mkdtemp(directory) != NULL);
    assert(setenv("PS5LOCALSEND_UPLOAD_DIR", directory, 1) == 0);
    {
        struct utimbuf old_time;
        char published[1400];
        char managed[1400];
        snprintf(published, sizeof(published), "%s/published.part", directory);
        snprintf(managed, sizeof(managed),
                 "%s/.ps5localsend-"
                 "0123456789abcdef0123456789abcdef.part",
                 directory);
        file = fopen(published, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        file = fopen(managed, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        old_time.actime = time(NULL) - 90000;
        old_time.modtime = old_time.actime;
        assert(utime(published, &old_time) == 0);
        assert(utime(managed, &old_time) == 0);
    }
    config_set_defaults(&config); config.max_file_bytes = 64U; config.max_files_per_session = 8U;
    manager = upload_manager_create(&config); assert(manager != NULL);
    assert(upload_snapshot(NULL, &snapshot) == -1);
    assert(upload_snapshot(manager, &snapshot) == 0);
    assert(snapshot.active == 0 && snapshot.started == 0 && snapshot.received == 0U &&
           snapshot.expected == 0U && snapshot.name[0] == '\0');

    assert(upload_write(NULL, "id", "x", 1U) == UPLOAD_INVALID);
    assert(upload_write(manager, NULL, "x", 1U) == UPLOAD_INVALID);
    assert(upload_write(manager, "id", NULL, 1U) == UPLOAD_INVALID);
    assert(upload_finish(NULL, "id", digest) == UPLOAD_INVALID);
    assert(upload_finish(manager, NULL, digest) == UPLOAD_INVALID);
    assert(upload_finish(manager, "id", NULL) == UPLOAD_INVALID);
    assert(upload_cancel(NULL, "id", "127.0.0.1", "Bearer token") ==
           UPLOAD_INVALID);
    assert(upload_cancel(manager, NULL, "127.0.0.1", "Bearer token") ==
           UPLOAD_INVALID);
    assert(upload_cancel(manager, "id", NULL, "Bearer token") ==
           UPLOAD_INVALID);
    assert(upload_cancel(manager, "id", "127.0.0.1", NULL) ==
           UPLOAD_INVALID);
    assert(upload_prepare(manager, "127.0.0.1", "Bearer token", NULL, 0U,
                          &prepared) == UPLOAD_INVALID);
    upload_abort(NULL, "id");
    upload_abort(manager, NULL);

    {
        const char *metadata =
            "{\"name\":\".ps5localsend-"
            "0123456789abcdef0123456789abcdef.part\","
            "\"size\":1,\"type\":\"application/octet-stream\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)metadata, strlen(metadata), &prepared) ==
            UPLOAD_INVALID);
    }
    snprintf(path, sizeof(path), "%s/published.part", directory);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof(path),
             "%s/.ps5localsend-"
             "0123456789abcdef0123456789abcdef.part",
             directory);
    assert(access(path, F_OK) != 0);

    {
        const char *escaped =
            "{\"name\":\"quote\\\"name.txt\",\"size\":0,"
            "\"type\":\"text/plain\"}";
        const char *bad_escape =
            "{\"name\":\"bad\\bname\",\"size\":0,\"type\":\"x\"}";
        const char *bad_whitespace =
            "{\v\"name\":\"x\",\"size\":0,\"type\":\"x\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)escaped, strlen(escaped), &prepared) ==
            UPLOAD_OK);
        assert(strcmp(prepared.final_name, "quote\"name.txt") == 0);
        assert(upload_cancel(manager, prepared.upload_id, "127.0.0.1",
                             "Bearer token") == UPLOAD_OK);
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)bad_escape, strlen(bad_escape), &prepared) ==
            UPLOAD_INVALID);
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)bad_whitespace, strlen(bad_whitespace),
            &prepared) == UPLOAD_INVALID);
    }

    {
        char part_path[1400];
        char displaced_path[1400];
        int count;
        prepared = prepare(manager,
            "{\"name\":\"cleanup.bin\",\"size\":1,\"type\":\"x\"}");
        count = snprintf(part_path, sizeof(part_path),
                         "%s/.ps5localsend-%s.part", directory,
                         prepared.upload_id);
        assert(count > 0 && (size_t)count < sizeof(part_path));
        count = snprintf(displaced_path, sizeof(displaced_path),
                         "%s/displaced-upload-part", directory);
        assert(count > 0 && (size_t)count < sizeof(displaced_path));
        assert(rename(part_path, displaced_path) == 0);
        assert(mkdir(part_path, 0700) == 0);
        assert(upload_cancel(manager, prepared.upload_id, "127.0.0.1",
                             "Bearer token") == UPLOAD_IO_ERROR);
        assert(rmdir(part_path) == 0);
        assert(unlink(displaced_path) == 0);
    }

    {
        static const unsigned char emoji[4] = {0xf0U, 0x9fU, 0x98U, 0x80U};
        char unicode_name[SAFE_PATH_NAME_BYTES + 1U];
        char unique_name[SAFE_PATH_NAME_BYTES + 1U];
        char original_path[1400];
        size_t index;
        for (index = 0U; index < 63U; ++index) {
            memcpy(unicode_name + index * sizeof(emoji), emoji, sizeof(emoji));
        }
        memcpy(unicode_name + 252U, ".x", 3U);
        assert(strlen(unicode_name) == 254U);
        assert(safe_path_validate_filename(unicode_name));
        assert(safe_path_join(directory, unicode_name, original_path,
                              sizeof(original_path)) == 0);
        file = fopen(original_path, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        assert(safe_path_unique_name(directory, unicode_name, unique_name) == 0);
        assert(safe_path_validate_filename(unique_name));
        assert(strlen(unique_name) <= SAFE_PATH_NAME_BYTES);
        assert(strstr(unique_name, " (1).x") != NULL);
        assert(unlink(original_path) == 0);
    }

    prepared = prepare(manager, "{\"name\":\"empty.txt\",\"size\":0,\"type\":\"text/plain\"}");
    assert(upload_snapshot(manager, &snapshot) == 0);
    assert(snapshot.active == 1 && snapshot.started == 0 &&
           strcmp(snapshot.name, "empty.txt") == 0);
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer wrong", 0U) == UPLOAD_FORBIDDEN);
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 0U) == UPLOAD_OK);
    assert(upload_finish(manager, prepared.upload_id, digest) == UPLOAD_OK);
    assert(strcmp(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    assert(upload_snapshot(manager, &snapshot) == 0 && snapshot.active == 0);

    prepared = prepare(manager, "{\"name\":\"game.pkg\",\"size\":1,\"type\":\"application/octet-stream\",\"sha256\":\"2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881\"}");
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 1U) == UPLOAD_OK);
    assert(upload_write(manager, prepared.upload_id, "x", 1U) == UPLOAD_OK);
    assert(upload_finish(manager, prepared.upload_id, digest) == UPLOAD_OK);

    prepared = prepare(manager, "{\"name\":\"game.pkg\",\"size\":1,\"type\":\"application/octet-stream\"}");
    assert(strcmp(prepared.final_name, "game (1).pkg") == 0);
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 2U) == UPLOAD_SIZE_MISMATCH);
    assert(upload_cancel(manager, prepared.upload_id, "127.0.0.1", "Bearer token") == UPLOAD_OK);

    prepared = prepare(manager, "{\"name\":\"bad.bin\",\"size\":1,\"type\":\"application/octet-stream\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 1U) == UPLOAD_OK);
    assert(upload_write(manager, prepared.upload_id, "x", 1U) == UPLOAD_OK);
    assert(upload_finish(manager, prepared.upload_id, digest) == UPLOAD_HASH_MISMATCH);
    snprintf(path, sizeof(path), "%s/bad.bin", directory); assert(access(path, F_OK) != 0);

    prepared = prepare(manager, "{\"name\":\"abandoned.bin\",\"size\":1,\"type\":\"application/octet-stream\"}");
    {
        upload_prepared_t blocked;
        const char *next = "{\"name\":\"next.bin\",\"size\":1,\"type\":\"application/octet-stream\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)next, strlen(next), &blocked) == UPLOAD_BUSY);
        upload_reap_at(manager, UINT64_MAX);
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)next, strlen(next), &blocked) == UPLOAD_OK);
        assert(upload_cancel(manager, blocked.upload_id, "127.0.0.1", "Bearer token") == UPLOAD_OK);
    }

    prepared = prepare(manager, "{\"name\":\"old-session.bin\",\"size\":1,\"type\":\"application/octet-stream\"}");
    {
        upload_prepared_t replacement;
        const char *next = "{\"name\":\"new-session.bin\",\"size\":1,\"type\":\"application/octet-stream\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer replacement",
            (const unsigned char *)next, strlen(next), &replacement) == UPLOAD_OK);
        assert(upload_cancel(manager, replacement.upload_id, "127.0.0.1", "Bearer replacement") == UPLOAD_OK);
    }

    prepared = prepare(manager, "{\"name\":\"active.bin\",\"size\":1,\"type\":\"application/octet-stream\"}");
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 1U) == UPLOAD_OK);
    {
        upload_prepared_t blocked;
        const char *next = "{\"name\":\"cannot-steal.bin\",\"size\":1,\"type\":\"application/octet-stream\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer replacement",
            (const unsigned char *)next, strlen(next), &blocked) == UPLOAD_BUSY);
    }
    upload_abort(manager, prepared.upload_id);

    prepared = prepare(manager, "{\"name\":\"race.bin\",\"size\":1,\"type\":\"application/octet-stream\"}");
    snprintf(path, sizeof(path), "%s/race.bin", directory);
    file = fopen(path, "wb"); assert(file != NULL); assert(fwrite("z", 1U, 1U, file) == 1U); assert(fclose(file) == 0);
    assert(upload_begin(manager, prepared.upload_id, "127.0.0.1", "Bearer token", 1U) == UPLOAD_OK);
    assert(upload_write(manager, prepared.upload_id, "x", 1U) == UPLOAD_OK);
    assert(upload_finish(manager, prepared.upload_id, digest) == UPLOAD_BUSY);
    file = fopen(path, "rb"); assert(file != NULL); assert(fgetc(file) == 'z'); assert(fclose(file) == 0); assert(unlink(path) == 0);

    {
        const char *bad = "{\"name\":\"../x\",\"size\":1,\"type\":\"x\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)bad, strlen(bad), &prepared) == UPLOAD_INVALID);
    }
    {
        const char *trailing = "{\"name\":\"x\",\"size\":1,\"type\":\"x\",}";
        const char *leading_zero = "{\"name\":\"x\",\"size\":01,\"type\":\"x\"}";
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)trailing, strlen(trailing), &prepared) ==
            UPLOAD_INVALID);
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)leading_zero, strlen(leading_zero),
            &prepared) == UPLOAD_INVALID);
    }
    {
        char maximum_name[SAFE_PATH_NAME_BYTES + 1U];
        char metadata[512];
        memset(maximum_name, 'a', SAFE_PATH_NAME_BYTES);
        maximum_name[SAFE_PATH_NAME_BYTES] = '\0';
        assert(snprintf(metadata, sizeof(metadata),
                        "{\"name\":\"%s\",\"size\":1,"
                        "\"type\":\"application/octet-stream\"}",
                        maximum_name) > 0);
        prepared = prepare(manager, metadata);
        assert(strcmp(prepared.final_name, maximum_name) == 0);
        assert(upload_begin(manager, prepared.upload_id, "127.0.0.1",
                            "Bearer token", 1U) == UPLOAD_OK);
        assert(upload_write(manager, prepared.upload_id, "x", 1U) ==
               UPLOAD_OK);
        assert(upload_finish(manager, prepared.upload_id, digest) == UPLOAD_OK);
        snprintf(path, sizeof(path), "%s/%s", directory, maximum_name);
        assert(access(path, F_OK) == 0);
        assert(unlink(path) == 0);
    }
    upload_manager_destroy(manager);

    {
        upload_manager_t *large_manager;
        unsigned char *large_data;
        size_t large_size =
            UPLOAD_WRITE_BUFFER_BYTES * (UPLOAD_WRITE_QUEUE_DEPTH + 1U) + 17U;
        size_t offset;
        sha256_context_t context;
        unsigned char raw[SHA256_DIGEST_BYTES];
        char expected_hash[SHA256_HEX_BYTES + 1U];
        char metadata[512];
        char large_path[1400];
        FILE *large_file;
        large_data = malloc(large_size);
        assert(large_data != NULL);
        for (offset = 0U; offset < large_size; ++offset) {
            large_data[offset] = (unsigned char)(offset * 31U + 7U);
        }
        sha256_init(&context);
        sha256_update(&context, large_data, large_size);
        sha256_final(&context, raw);
        sha256_hex(raw, expected_hash);
        config.max_file_bytes = (uint64_t)large_size;
        large_manager = upload_manager_create(&config);
        assert(large_manager != NULL);
        assert(snprintf(metadata, sizeof(metadata),
                        "{\"name\":\"buffered.bin\",\"size\":%llu,"
                        "\"type\":\"application/octet-stream\",\"sha256\":\"%s\"}",
                        (unsigned long long)large_size, expected_hash) > 0);
        assert(upload_prepare(large_manager, "127.0.0.1", "Bearer large",
                              (const unsigned char *)metadata,
                              strlen(metadata), &prepared) == UPLOAD_OK);
        assert(upload_begin(large_manager, prepared.upload_id, "127.0.0.1",
                            "Bearer large", large_size) == UPLOAD_OK);
        for (offset = 0U; offset < large_size; offset += 65537U) {
            size_t count = large_size - offset;
            if (count > 65537U) count = 65537U;
            assert(upload_write(large_manager, prepared.upload_id,
                                large_data + offset, count) == UPLOAD_OK);
            if (offset == 0U) {
                assert(upload_snapshot(large_manager, &snapshot) == 0);
                assert(snapshot.active == 1 && snapshot.started == 1 &&
                       snapshot.received == count &&
                       snapshot.expected == large_size);
            }
        }
        assert(upload_finish(large_manager, prepared.upload_id, digest) == UPLOAD_OK);
        assert(strcmp(digest, expected_hash) == 0);
        snprintf(large_path, sizeof(large_path), "%s/buffered.bin", directory);
        large_file = fopen(large_path, "rb");
        assert(large_file != NULL);
        for (offset = 0U; offset < large_size; ++offset) {
            assert(fgetc(large_file) == large_data[offset]);
        }
        assert(fgetc(large_file) == EOF);
        assert(fclose(large_file) == 0);
        assert(unlink(large_path) == 0);

        /* The dedicated fast listener bypasses the producer queue and writes
         * each recv buffer directly in file order, then hashes the completed
         * part outside the network hot path. */
        assert(snprintf(metadata, sizeof(metadata),
                        "{\"name\":\"direct.bin\",\"size\":%llu,"
                        "\"type\":\"application/octet-stream\","
                        "\"sha256\":\"%s\"}",
                        (unsigned long long)large_size, expected_hash) > 0);
        assert(upload_prepare(large_manager, "127.0.0.1", "Bearer direct",
                              (const unsigned char *)metadata,
                              strlen(metadata), &prepared) == UPLOAD_OK);
        assert(upload_begin(large_manager, prepared.upload_id, "127.0.0.1",
                            "Bearer direct", large_size) == UPLOAD_OK);
        assert(upload_enable_direct_stream(large_manager,
                                           prepared.upload_id) == UPLOAD_OK);
        for (offset = 0U; offset < large_size; offset += 1024U * 1024U) {
            size_t count = large_size - offset;
            if (count > 1024U * 1024U) {
                count = 1024U * 1024U;
            }
            assert(upload_write_direct(large_manager, prepared.upload_id,
                                       large_data + offset, count) ==
                   UPLOAD_OK);
        }
        assert(upload_finish(large_manager, prepared.upload_id, digest) ==
               UPLOAD_OK);
        assert(strcmp(digest, expected_hash) == 0);
        snprintf(large_path, sizeof(large_path), "%s/direct.bin", directory);
        large_file = fopen(large_path, "rb");
        assert(large_file != NULL);
        for (offset = 0U; offset < large_size; ++offset) {
            assert(fgetc(large_file) == large_data[offset]);
        }
        assert(fgetc(large_file) == EOF);
        assert(fclose(large_file) == 0);
        assert(unlink(large_path) == 0);

        /* Cancellation must safely stop/discard a queued sequential write
         * before the temporary file descriptor is closed. */
        assert(snprintf(metadata, sizeof(metadata),
                        "{\"name\":\"queued-cancel.bin\",\"size\":%u,"
                        "\"type\":\"application/octet-stream\"}",
                        UPLOAD_WRITE_BUFFER_BYTES + 17U) > 0);
        assert(upload_prepare(large_manager, "127.0.0.1", "Bearer cancel",
                              (const unsigned char *)metadata,
                              strlen(metadata), &prepared) == UPLOAD_OK);
        assert(upload_begin(large_manager, prepared.upload_id, "127.0.0.1",
                            "Bearer cancel",
                            UPLOAD_WRITE_BUFFER_BYTES + 17U) == UPLOAD_OK);
        assert(upload_write(large_manager, prepared.upload_id, large_data,
                            UPLOAD_WRITE_BUFFER_BYTES + 17U) == UPLOAD_OK);
        assert(upload_cancel(large_manager, prepared.upload_id, "127.0.0.1",
                             "Bearer cancel") == UPLOAD_OK);
        snprintf(large_path, sizeof(large_path), "%s/queued-cancel.bin",
                 directory);
        assert(access(large_path, F_OK) != 0 && errno == ENOENT);

        /* Receive extents concurrently and out of order. The dedicated
         * writer must publish the exact original byte order. */
        assert(snprintf(metadata, sizeof(metadata),
                        "{\"name\":\"staged.bin\",\"size\":%llu,"
                        "\"type\":\"application/octet-stream\","
                        "\"sha256\":\"%s\"}",
                        (unsigned long long)large_size, expected_hash) > 0);
        assert(upload_prepare(large_manager, "127.0.0.1", "Bearer chunks",
                              (const unsigned char *)metadata,
                              strlen(metadata), &prepared) == UPLOAD_OK);
        {
            pthread_t threads[UPLOAD_CHUNK_PARALLELISM];
            chunk_thread_args_t args[UPLOAD_CHUNK_PARALLELISM];
            size_t chunk_count = (large_size + UPLOAD_CHUNK_BYTES - 1U) /
                                 UPLOAD_CHUNK_BYTES;
            size_t thread_index;
            assert(chunk_count <= UPLOAD_CHUNK_PARALLELISM);
            memset(args, 0, sizeof(args));
            for (thread_index = 0U; thread_index < chunk_count;
                 ++thread_index) {
                size_t chunk_index = chunk_count - thread_index - 1U;
                uint64_t chunk_offset =
                    (uint64_t)chunk_index * (uint64_t)UPLOAD_CHUNK_BYTES;
                uint64_t chunk_length = (uint64_t)large_size - chunk_offset;
                if (chunk_length > (uint64_t)UPLOAD_CHUNK_BYTES) {
                    chunk_length = (uint64_t)UPLOAD_CHUNK_BYTES;
                }
                args[thread_index].manager = large_manager;
                args[thread_index].upload_id = prepared.upload_id;
                args[thread_index].data = large_data;
                args[thread_index].total = (uint64_t)large_size;
                args[thread_index].offset = chunk_offset;
                args[thread_index].length = chunk_length;
                assert(pthread_create(&threads[thread_index], NULL,
                                      send_chunk, &args[thread_index]) == 0);
            }
            for (thread_index = 0U; thread_index < chunk_count;
                 ++thread_index) {
                assert(pthread_join(threads[thread_index], NULL) == 0);
                assert(args[thread_index].result == UPLOAD_OK);
            }
        }
        assert(upload_finish_chunks_with_metrics(
                   large_manager, prepared.upload_id, "127.0.0.1",
                   "Bearer chunks", digest, NULL) == UPLOAD_OK);
        assert(strcmp(digest, expected_hash) == 0);
        snprintf(large_path, sizeof(large_path), "%s/staged.bin", directory);
        large_file = fopen(large_path, "rb");
        assert(large_file != NULL);
        for (offset = 0U; offset < large_size; ++offset) {
            assert(fgetc(large_file) == large_data[offset]);
        }
        assert(fgetc(large_file) == EOF);
        assert(fclose(large_file) == 0);
        assert(unlink(large_path) == 0);

        upload_manager_destroy(large_manager);
        free(large_data);
    }

    {
        char real_directory[1400];
        char symlink_directory[1400];
        char regular_path[1400];
        snprintf(real_directory, sizeof(real_directory), "%s/real", directory);
        snprintf(symlink_directory, sizeof(symlink_directory), "%s/link", directory);
        snprintf(regular_path, sizeof(regular_path), "%s/not-directory", directory);
        assert(mkdir(real_directory, 0700) == 0);
        assert(symlink(real_directory, symlink_directory) == 0);
        assert(setenv("PS5LOCALSEND_UPLOAD_DIR", symlink_directory, 1) == 0);
        assert(upload_manager_create(&config) == NULL);
        file = fopen(regular_path, "wb"); assert(file != NULL); assert(fclose(file) == 0);
        assert(setenv("PS5LOCALSEND_UPLOAD_DIR", regular_path, 1) == 0);
        assert(upload_manager_create(&config) == NULL);
        assert(unlink(regular_path) == 0);
        assert(unlink(symlink_directory) == 0);
        assert(rmdir(real_directory) == 0);
        assert(setenv("PS5LOCALSEND_UPLOAD_DIR", directory, 1) == 0);
    }

    {
        struct statvfs space;
        char metadata[256];
        uint64_t unavailable;
        assert(statvfs(directory, &space) == 0);
        unavailable = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize + 1U;
        config.max_file_bytes = UINT64_MAX;
        manager = upload_manager_create(&config); assert(manager != NULL);
        snprintf(metadata, sizeof(metadata),
                 "{\"name\":\"full.bin\",\"size\":%llu,\"type\":\"application/octet-stream\"}",
                 (unsigned long long)unavailable);
        assert(upload_prepare(manager, "127.0.0.1", "Bearer token",
            (const unsigned char *)metadata, strlen(metadata), &prepared) ==
            UPLOAD_STORAGE_FULL);
        upload_manager_destroy(manager);
    }

    snprintf(path, sizeof(path), "%s/empty.txt", directory); unlink(path);
    snprintf(path, sizeof(path), "%s/game.pkg", directory); unlink(path);
    snprintf(path, sizeof(path), "%s/published.part", directory); unlink(path);
    file = fopen(path, "rb"); assert(file == NULL);
    assert(rmdir(directory) == 0);
    puts("upload tests passed");
    return 0;
}
