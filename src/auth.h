#ifndef PS5LOCALSEND_AUTH_H
#define PS5LOCALSEND_AUTH_H

#include <stddef.h>
#include <stdint.h>

#define AUTH_CHALLENGE_ID_HEX_BYTES 32U
#define AUTH_PIN_DIGITS 6U
#define AUTH_TOKEN_HEX_BYTES 64U
#define AUTH_CLIENT_ADDRESS_BYTES 64U
#define AUTH_MAX_ATTEMPTS 5U

typedef struct auth_manager auth_manager_t;

typedef enum auth_result {
    AUTH_OK = 0,
    AUTH_INVALID = 1,
    AUTH_EXPIRED = 2,
    AUTH_USED = 3,
    AUTH_RATE_LIMITED = 4,
    AUTH_CAPACITY = 5,
    AUTH_RANDOM_FAILURE = 6
} auth_result_t;

typedef struct auth_challenge_result {
    char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
    char pin[AUTH_PIN_DIGITS + 1U];
    unsigned int expires_in_seconds;
} auth_challenge_result_t;

typedef struct auth_session_result {
    char token[AUTH_TOKEN_HEX_BYTES + 1U];
    unsigned int expires_in_seconds;
} auth_session_result_t;

auth_manager_t *auth_manager_create(unsigned int pin_ttl_seconds,
                                    unsigned int session_ttl_seconds);
void auth_manager_destroy(auth_manager_t *manager);

auth_result_t auth_create_challenge(auth_manager_t *manager,
                                    const char *client_address,
                                    auth_challenge_result_t *result);
auth_result_t auth_verify_challenge(auth_manager_t *manager,
                                    const char *client_address,
                                    const char *challenge_id, const char *pin,
                                    auth_session_result_t *result);
auth_result_t auth_validate_bearer(auth_manager_t *manager,
                                   const char *client_address,
                                   const char *authorization);

/* Exposed for deterministic state-machine tests; production uses the wrappers. */
auth_result_t auth_create_challenge_at(auth_manager_t *manager,
                                       const char *client_address, uint64_t now,
                                       auth_challenge_result_t *result);
auth_result_t auth_verify_challenge_at(auth_manager_t *manager,
                                       const char *client_address,
                                       const char *challenge_id, const char *pin,
                                       uint64_t now,
                                       auth_session_result_t *result);
auth_result_t auth_validate_bearer_at(auth_manager_t *manager,
                                      const char *client_address,
                                      const char *authorization, uint64_t now);

int auth_parse_verify_json(const unsigned char *body, size_t body_size,
                           char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U],
                           char pin[AUTH_PIN_DIGITS + 1U]);

#endif
