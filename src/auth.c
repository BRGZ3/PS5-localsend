#include "auth.h"

#include "platform.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUTH_MAX_CHALLENGES 8U
#define AUTH_MAX_SESSIONS 4U
#define AUTH_RATE_LIMIT_SECONDS 30U

typedef struct auth_challenge {
    int occupied;
    int used;
    unsigned int attempts;
    uint64_t expires_at;
    uint64_t rate_limited_until;
    char client_address[AUTH_CLIENT_ADDRESS_BYTES];
    char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
    char pin[AUTH_PIN_DIGITS + 1U];
} auth_challenge_t;

typedef struct auth_session {
    int occupied;
    uint64_t expires_at;
    char client_address[AUTH_CLIENT_ADDRESS_BYTES];
    char token[AUTH_TOKEN_HEX_BYTES + 1U];
} auth_session_t;

struct auth_manager {
    pthread_mutex_t mutex;
    unsigned int pin_ttl_seconds;
    unsigned int session_ttl_seconds;
    auth_challenge_t challenges[AUTH_MAX_CHALLENGES];
    auth_session_t sessions[AUTH_MAX_SESSIONS];
};

static void secure_clear(void *buffer, size_t size) {
    volatile unsigned char *cursor = buffer;
    while (size > 0U) {
        *cursor++ = 0U;
        --size;
    }
}

static int constant_time_equal(const char *left, const char *right, size_t size) {
    size_t index;
    unsigned char difference = 0U;
    for (index = 0U; index < size; ++index) {
        difference |= (unsigned char)left[index] ^ (unsigned char)right[index];
    }
    return difference == 0U;
}

static int valid_client_address(const char *address) {
    size_t length;
    if (address == NULL) {
        return 0;
    }
    length = strlen(address);
    return length > 0U && length < AUTH_CLIENT_ADDRESS_BYTES;
}

static void bytes_to_hex(const unsigned char *bytes, size_t size, char *output) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < size; ++index) {
        output[index * 2U] = digits[bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    output[size * 2U] = '\0';
}

static int generate_hex(char *output, size_t random_bytes) {
    unsigned char bytes[AUTH_TOKEN_HEX_BYTES / 2U];
    if (random_bytes > sizeof(bytes) ||
        platform_secure_random(bytes, random_bytes) != 0) {
        secure_clear(bytes, sizeof(bytes));
        return -1;
    }
    bytes_to_hex(bytes, random_bytes, output);
    secure_clear(bytes, sizeof(bytes));
    return 0;
}

static int generate_pin(char output[AUTH_PIN_DIGITS + 1U]) {
    uint32_t value;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % UINT32_C(1000000));
    do {
        if (platform_secure_random(&value, sizeof(value)) != 0) {
            return -1;
        }
    } while (value >= limit);
    value %= UINT32_C(1000000);
    output[0] = (char)('0' + ((value / UINT32_C(100000)) % 10U));
    output[1] = (char)('0' + ((value / UINT32_C(10000)) % 10U));
    output[2] = (char)('0' + ((value / UINT32_C(1000)) % 10U));
    output[3] = (char)('0' + ((value / UINT32_C(100)) % 10U));
    output[4] = (char)('0' + ((value / UINT32_C(10)) % 10U));
    output[5] = (char)('0' + (value % 10U));
    output[6] = '\0';
    secure_clear(&value, sizeof(value));
    return 0;
}

static uint64_t current_time_seconds(void) {
    time_t now = time(NULL);
    return now < (time_t)0 ? 0U : (uint64_t)now;
}

static void clear_challenge(auth_challenge_t *challenge) {
    secure_clear(challenge, sizeof(*challenge));
}

static void clear_session(auth_session_t *session) {
    secure_clear(session, sizeof(*session));
}

static void cleanup_expired(auth_manager_t *manager, uint64_t now) {
    size_t index;
    for (index = 0U; index < AUTH_MAX_CHALLENGES; ++index) {
        auth_challenge_t *challenge = &manager->challenges[index];
        if (challenge->occupied != 0 && challenge->expires_at <= now &&
            challenge->rate_limited_until <= now) {
            clear_challenge(challenge);
        }
    }
    for (index = 0U; index < AUTH_MAX_SESSIONS; ++index) {
        auth_session_t *session = &manager->sessions[index];
        if (session->occupied != 0 && session->expires_at <= now) {
            clear_session(session);
        }
    }
}

auth_manager_t *auth_manager_create(unsigned int pin_ttl_seconds,
                                    unsigned int session_ttl_seconds) {
    auth_manager_t *manager;
    if (pin_ttl_seconds == 0U || session_ttl_seconds == 0U) {
        return NULL;
    }
    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager);
        return NULL;
    }
    manager->pin_ttl_seconds = pin_ttl_seconds;
    manager->session_ttl_seconds = session_ttl_seconds;
    return manager;
}

void auth_manager_destroy(auth_manager_t *manager) {
    if (manager == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&manager->mutex);
    secure_clear(manager->challenges, sizeof(manager->challenges));
    secure_clear(manager->sessions, sizeof(manager->sessions));
    (void)pthread_mutex_unlock(&manager->mutex);
    (void)pthread_mutex_destroy(&manager->mutex);
    secure_clear(manager, sizeof(*manager));
    free(manager);
}

auth_result_t auth_create_challenge_at(auth_manager_t *manager,
                                       const char *client_address, uint64_t now,
                                       auth_challenge_result_t *result) {
    auth_challenge_t *slot = NULL;
    char pin[AUTH_PIN_DIGITS + 1U];
    char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
    size_t index;
    if (manager == NULL || result == NULL || !valid_client_address(client_address)) {
        return AUTH_INVALID;
    }
    memset(result, 0, sizeof(*result));
    if (generate_hex(challenge_id, AUTH_CHALLENGE_ID_HEX_BYTES / 2U) != 0 ||
        generate_pin(pin) != 0) {
        secure_clear(pin, sizeof(pin));
        secure_clear(challenge_id, sizeof(challenge_id));
        return AUTH_RANDOM_FAILURE;
    }
    (void)pthread_mutex_lock(&manager->mutex);
    cleanup_expired(manager, now);
    for (index = 0U; index < AUTH_MAX_CHALLENGES; ++index) {
        auth_challenge_t *candidate = &manager->challenges[index];
        if (candidate->occupied != 0 &&
            strcmp(candidate->client_address, client_address) == 0) {
            if (candidate->rate_limited_until > now) {
                (void)pthread_mutex_unlock(&manager->mutex);
                secure_clear(pin, sizeof(pin));
                secure_clear(challenge_id, sizeof(challenge_id));
                return AUTH_RATE_LIMITED;
            }
            clear_challenge(candidate);
        }
        if (slot == NULL && candidate->occupied == 0) {
            slot = candidate;
        }
    }
    if (slot == NULL) {
        (void)pthread_mutex_unlock(&manager->mutex);
        secure_clear(pin, sizeof(pin));
        secure_clear(challenge_id, sizeof(challenge_id));
        return AUTH_CAPACITY;
    }
    slot->occupied = 1;
    slot->expires_at = now + manager->pin_ttl_seconds;
    (void)memcpy(slot->client_address, client_address,
                 strlen(client_address) + 1U);
    (void)memcpy(slot->challenge_id, challenge_id, sizeof(challenge_id));
    (void)memcpy(slot->pin, pin, sizeof(pin));
    (void)memcpy(result->challenge_id, challenge_id, sizeof(challenge_id));
    (void)memcpy(result->pin, pin, sizeof(pin));
    result->expires_in_seconds = manager->pin_ttl_seconds;
    (void)pthread_mutex_unlock(&manager->mutex);
    secure_clear(pin, sizeof(pin));
    secure_clear(challenge_id, sizeof(challenge_id));
    return AUTH_OK;
}

auth_result_t auth_create_challenge(auth_manager_t *manager,
                                    const char *client_address,
                                    auth_challenge_result_t *result) {
    return auth_create_challenge_at(manager, client_address,
                                    current_time_seconds(), result);
}

auth_result_t auth_verify_challenge_at(auth_manager_t *manager,
                                       const char *client_address,
                                       const char *challenge_id, const char *pin,
                                       uint64_t now,
                                       auth_session_result_t *result) {
    auth_challenge_t *challenge = NULL;
    char token[AUTH_TOKEN_HEX_BYTES + 1U];
    size_t index;
    int pin_matches;
    if (manager == NULL || result == NULL || !valid_client_address(client_address) ||
        challenge_id == NULL || pin == NULL ||
        strlen(challenge_id) != AUTH_CHALLENGE_ID_HEX_BYTES ||
        strlen(pin) != AUTH_PIN_DIGITS) {
        return AUTH_INVALID;
    }
    memset(result, 0, sizeof(*result));
    (void)pthread_mutex_lock(&manager->mutex);
    for (index = 0U; index < AUTH_MAX_CHALLENGES; ++index) {
        auth_challenge_t *candidate = &manager->challenges[index];
        if (candidate->occupied != 0 &&
            constant_time_equal(candidate->challenge_id, challenge_id,
                                AUTH_CHALLENGE_ID_HEX_BYTES)) {
            challenge = candidate;
            break;
        }
    }
    if (challenge == NULL ||
        strcmp(challenge->client_address, client_address) != 0) {
        (void)pthread_mutex_unlock(&manager->mutex);
        return AUTH_INVALID;
    }
    if (challenge->used != 0) {
        (void)pthread_mutex_unlock(&manager->mutex);
        return AUTH_USED;
    }
    if (challenge->expires_at <= now) {
        clear_challenge(challenge);
        (void)pthread_mutex_unlock(&manager->mutex);
        return AUTH_EXPIRED;
    }
    if (challenge->attempts >= AUTH_MAX_ATTEMPTS) {
        (void)pthread_mutex_unlock(&manager->mutex);
        return AUTH_RATE_LIMITED;
    }
    pin_matches = constant_time_equal(challenge->pin, pin, AUTH_PIN_DIGITS);
    if (!pin_matches) {
        int limited;
        ++challenge->attempts;
        limited = challenge->attempts >= AUTH_MAX_ATTEMPTS;
        if (limited) {
            secure_clear(challenge->pin, sizeof(challenge->pin));
            challenge->rate_limited_until = now + AUTH_RATE_LIMIT_SECONDS;
        }
        (void)pthread_mutex_unlock(&manager->mutex);
        return limited ? AUTH_RATE_LIMITED : AUTH_INVALID;
    }
    if (generate_hex(token, AUTH_TOKEN_HEX_BYTES / 2U) != 0) {
        (void)pthread_mutex_unlock(&manager->mutex);
        return AUTH_RANDOM_FAILURE;
    }
    challenge->used = 1;
    secure_clear(challenge->pin, sizeof(challenge->pin));
    for (index = 0U; index < AUTH_MAX_SESSIONS; ++index) {
        clear_session(&manager->sessions[index]);
    }
    manager->sessions[0].occupied = 1;
    manager->sessions[0].expires_at = now + manager->session_ttl_seconds;
    (void)memcpy(manager->sessions[0].client_address, client_address,
                 strlen(client_address) + 1U);
    (void)memcpy(manager->sessions[0].token, token, sizeof(token));
    (void)memcpy(result->token, token, sizeof(token));
    result->expires_in_seconds = manager->session_ttl_seconds;
    (void)pthread_mutex_unlock(&manager->mutex);
    secure_clear(token, sizeof(token));
    return AUTH_OK;
}

auth_result_t auth_verify_challenge(auth_manager_t *manager,
                                    const char *client_address,
                                    const char *challenge_id, const char *pin,
                                    auth_session_result_t *result) {
    return auth_verify_challenge_at(manager, client_address, challenge_id, pin,
                                    current_time_seconds(), result);
}

auth_result_t auth_validate_bearer_at(auth_manager_t *manager,
                                      const char *client_address,
                                      const char *authorization, uint64_t now) {
    const char *token;
    size_t index;
    auth_result_t result = AUTH_INVALID;
    if (manager == NULL || !valid_client_address(client_address) ||
        authorization == NULL || strncmp(authorization, "Bearer ", 7U) != 0) {
        return AUTH_INVALID;
    }
    token = authorization + 7U;
    if (strlen(token) != AUTH_TOKEN_HEX_BYTES) {
        return AUTH_INVALID;
    }
    (void)pthread_mutex_lock(&manager->mutex);
    cleanup_expired(manager, now);
    for (index = 0U; index < AUTH_MAX_SESSIONS; ++index) {
        auth_session_t *session = &manager->sessions[index];
        if (session->occupied != 0 &&
            constant_time_equal(session->token, token, AUTH_TOKEN_HEX_BYTES) &&
            strcmp(session->client_address, client_address) == 0) {
            result = AUTH_OK;
        }
    }
    (void)pthread_mutex_unlock(&manager->mutex);
    return result;
}

auth_result_t auth_validate_bearer(auth_manager_t *manager,
                                   const char *client_address,
                                   const char *authorization) {
    return auth_validate_bearer_at(manager, client_address, authorization,
                                   current_time_seconds());
}

static void skip_space(const unsigned char **cursor, const unsigned char *end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t' ||
                             **cursor == '\n' || **cursor == '\r')) {
        ++*cursor;
    }
}

static int parse_literal_string(const unsigned char **cursor,
                                const unsigned char *end, char *output,
                                size_t capacity) {
    size_t length = 0U;
    if (*cursor >= end || **cursor != '"') {
        return -1;
    }
    ++*cursor;
    while (*cursor < end && **cursor != '"') {
        unsigned char character = **cursor;
        if (character < 0x20U || character == '\\' || length + 1U >= capacity) {
            return -1;
        }
        output[length++] = (char)character;
        ++*cursor;
    }
    if (*cursor >= end || **cursor != '"') {
        return -1;
    }
    ++*cursor;
    output[length] = '\0';
    return 0;
}

int auth_parse_verify_json(const unsigned char *body, size_t body_size,
                           char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U],
                           char pin[AUTH_PIN_DIGITS + 1U]) {
    const unsigned char *cursor = body;
    const unsigned char *end;
    unsigned int seen = 0U;
    if (body == NULL || body_size == 0U || challenge_id == NULL || pin == NULL) {
        return -1;
    }
    end = body + body_size;
    challenge_id[0] = '\0';
    pin[0] = '\0';
    skip_space(&cursor, end);
    if (cursor >= end || *cursor++ != '{') {
        return -1;
    }
    for (;;) {
        char key[16];
        char value[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
        skip_space(&cursor, end);
        if (cursor < end && *cursor == '}') {
            ++cursor;
            break;
        }
        if (parse_literal_string(&cursor, end, key, sizeof(key)) != 0) {
            return -1;
        }
        skip_space(&cursor, end);
        if (cursor >= end || *cursor++ != ':') {
            return -1;
        }
        skip_space(&cursor, end);
        if (parse_literal_string(&cursor, end, value, sizeof(value)) != 0) {
            return -1;
        }
        if (strcmp(key, "challengeId") == 0 && (seen & 1U) == 0U &&
            strlen(value) == AUTH_CHALLENGE_ID_HEX_BYTES) {
            (void)memcpy(challenge_id, value, sizeof(value));
            seen |= 1U;
        } else if (strcmp(key, "pin") == 0 && (seen & 2U) == 0U &&
                   strlen(value) == AUTH_PIN_DIGITS) {
            size_t index;
            for (index = 0U; index < AUTH_PIN_DIGITS; ++index) {
                if (!isdigit((unsigned char)value[index])) {
                    return -1;
                }
            }
            (void)memcpy(pin, value, AUTH_PIN_DIGITS + 1U);
            seen |= 2U;
        } else {
            return -1;
        }
        skip_space(&cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            skip_space(&cursor, end);
            if (cursor >= end || *cursor == '}') {
                return -1;
            }
            continue;
        }
        if (cursor < end && *cursor == '}') {
            ++cursor;
            break;
        }
        return -1;
    }
    skip_space(&cursor, end);
    return cursor == end && seen == 3U ? 0 : -1;
}
