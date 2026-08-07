#ifndef LIBTETRISAUTH_CONFIG_H
#define LIBTETRISAUTH_CONFIG_H

/**
 * @file config.h
 * @brief The auth_ configuration contract.
 *
 * Kept out of tetrisauth.h so its settled three-function seam stays unchanged;
 * bin/tetrisdb is the second consumer and needs only the namespace validator,
 * not the login interface.
 */

#define TAUTH_DEFAULT_MAX_ATTEMPTS 5      /**< auth_max_attempts, 1..100. */
#define TAUTH_DEFAULT_TOKEN_TTL 604800    /**< auth_token_ttl, in seconds. */
#define TAUTH_DEFAULT_PBKDF2_ITERS 600000 /**< auth_pbkdf2_iters. */

/** Complete auth_ namespace, NULL-terminated; read by the drift test in
 * tests/test_rc.c against sample.tetrishrc. */
extern const char *const tauth_rc_keys[];

/**
 * Validates the auth_ directives in rc_path, ignoring every other namespace.
 *
 * Called by bin/tetrisdb start and check, which is the one moment a typo in
 * the shared file can be reported to a human.
 *
 * @param rc_path  File to read.
 * @returns The directive count, or -1 when the file is missing, an auth_ value
 *          is invalid, or an unknown auth_ key is present (message on stderr).
 */
int tauth_rc_validate(const char *rc_path)
    __attribute__((warn_unused_result));

#endif /* LIBTETRISAUTH_CONFIG_H */
