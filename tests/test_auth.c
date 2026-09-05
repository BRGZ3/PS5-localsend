#include "auth.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void assert_hex(const char *text, size_t length) {
    size_t index;
    assert(strlen(text) == length);
    for (index = 0U; index < length; ++index) {
        assert(isxdigit((unsigned char)text[index]) != 0);
    }
}

static void assert_pin(const char *pin) {
    size_t index;
    assert(strlen(pin) == AUTH_PIN_DIGITS);
    for (index = 0U; index < AUTH_PIN_DIGITS; ++index) {
        assert(isdigit((unsigned char)pin[index]) != 0);
    }
}

int main(void) {
    auth_manager_t *manager = auth_manager_create(120U, 900U);
    auth_challenge_result_t challenge;
    auth_challenge_result_t expired;
    auth_challenge_result_t limited;
    auth_session_result_t session;
    char authorization[7U + AUTH_TOKEN_HEX_BYTES + 1U];
    char challenge_id[AUTH_CHALLENGE_ID_HEX_BYTES + 1U];
    char pin[AUTH_PIN_DIGITS + 1U];
    unsigned int attempt;

    assert(manager != NULL);
    assert(auth_create_challenge_at(manager, "192.0.2.10", 1000U,
                                    &challenge) == AUTH_OK);
    assert_hex(challenge.challenge_id, AUTH_CHALLENGE_ID_HEX_BYTES);
    assert_pin(challenge.pin);
    assert(challenge.expires_in_seconds == 120U);
    assert(auth_verify_challenge_at(manager, "192.0.2.11",
                                    challenge.challenge_id, challenge.pin,
                                    1001U, &session) == AUTH_INVALID);
    assert(auth_verify_challenge_at(manager, "192.0.2.10",
                                    challenge.challenge_id, challenge.pin,
                                    1001U, &session) == AUTH_OK);
    assert_hex(session.token, AUTH_TOKEN_HEX_BYTES);
    assert(session.expires_in_seconds == 900U);
    (void)snprintf(authorization, sizeof(authorization), "Bearer %s",
                   session.token);
    assert(auth_validate_bearer_at(manager, "192.0.2.10", authorization,
                                   1002U) == AUTH_OK);
    assert(auth_validate_bearer_at(manager, "192.0.2.11", authorization,
                                   1002U) == AUTH_INVALID);
    assert(auth_verify_challenge_at(manager, "192.0.2.10",
                                    challenge.challenge_id, challenge.pin,
                                    1002U, &session) == AUTH_USED);
    assert(auth_validate_bearer_at(manager, "192.0.2.10", authorization,
                                   1902U) == AUTH_INVALID);

    assert(auth_create_challenge_at(manager, "192.0.2.20", 2000U,
                                    &expired) == AUTH_OK);
    assert(auth_verify_challenge_at(manager, "192.0.2.20", expired.challenge_id,
                                    expired.pin, 2121U, &session) == AUTH_EXPIRED);

    assert(auth_create_challenge_at(manager, "192.0.2.30", 3000U,
                                    &limited) == AUTH_OK);
    for (attempt = 1U; attempt < AUTH_MAX_ATTEMPTS; ++attempt) {
        assert(auth_verify_challenge_at(manager, "192.0.2.30",
                                        limited.challenge_id, "999999", 3001U,
                                        &session) == AUTH_INVALID);
    }
    assert(auth_verify_challenge_at(manager, "192.0.2.30", limited.challenge_id,
                                    "999999", 3001U,
                                    &session) == AUTH_RATE_LIMITED);
    assert(auth_verify_challenge_at(manager, "192.0.2.30", limited.challenge_id,
                                    limited.pin, 3001U,
                                    &session) == AUTH_RATE_LIMITED);
    assert(auth_create_challenge_at(manager, "192.0.2.30", 3002U,
                                    &limited) == AUTH_RATE_LIMITED);
    assert(auth_create_challenge_at(manager, "192.0.2.30", 3031U,
                                    &limited) == AUTH_OK);

    assert(auth_parse_verify_json(
               (const unsigned char *)
                   "{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                   "\"pin\":\"123456\"}",
               strlen("{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                      "\"pin\":\"123456\"}"),
               challenge_id, pin) == 0);
    assert(strcmp(challenge_id, "0123456789abcdef0123456789abcdef") == 0);
    assert(strcmp(pin, "123456") == 0);
    assert(auth_parse_verify_json(
               (const unsigned char *)
                   "{\"pin\":\"123456\",\"challengeId\":"
                   "\"0123456789abcdef0123456789abcdef\",\"extra\":\"x\"}",
               strlen("{\"pin\":\"123456\",\"challengeId\":"
                      "\"0123456789abcdef0123456789abcdef\",\"extra\":\"x\"}"),
               challenge_id, pin) != 0);
    assert(auth_parse_verify_json(
               (const unsigned char *)
                   "{\v\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                   "\"pin\":\"123456\"}",
               strlen("{\v\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                      "\"pin\":\"123456\"}"),
               challenge_id, pin) != 0);
    assert(auth_parse_verify_json(
               (const unsigned char *)
                   "{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                   "\"pin\":123456}",
               strlen("{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                      "\"pin\":123456}"),
               challenge_id, pin) != 0);
    assert(auth_parse_verify_json(
               (const unsigned char *)
                   "{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                   "\"pin\":\"123456\",}",
               strlen("{\"challengeId\":\"0123456789abcdef0123456789abcdef\","
                      "\"pin\":\"123456\",}"),
               challenge_id, pin) != 0);

    auth_manager_destroy(manager);
    puts("auth tests passed");
    return 0;
}
