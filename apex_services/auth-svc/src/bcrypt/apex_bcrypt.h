// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

/*
 * Minimal bcrypt interface for password hashing.
 * Uses OpenSSL's libcrypto for the Blowfish cipher.
 *
 * Based on the OpenBSD bcrypt implementation (public domain).
 * Adapted for standalone use with OpenSSL.
 */
#ifndef APEX_BCRYPT_H
#define APEX_BCRYPT_H

#ifdef __cplusplus
extern "C"
{
#endif

#define BCRYPT_HASHSIZE 64

    /**
     * Generate a bcrypt salt.
     * @param log_rounds Work factor (4-31)
     * @param salt Output buffer (at least BCRYPT_HASHSIZE bytes)
     * @return 0 on success, -1 on error
     */
    int apex_bcrypt_gensalt(int log_rounds, char salt[BCRYPT_HASHSIZE]);

    /**
     * Hash a password with bcrypt.
     * @param password Null-terminated password string
     * @param salt Salt from apex_bcrypt_gensalt
     * @param hash Output hash (at least BCRYPT_HASHSIZE bytes)
     * @return 0 on success, -1 on error
     */
    int apex_bcrypt_hashpw(const char* password, const char* salt, char hash[BCRYPT_HASHSIZE]);

    /**
     * Verify a password against a hash.
     * @param password Null-terminated password
     * @param hash Stored bcrypt hash
     * @return 0 if match, non-zero otherwise
     */
    int apex_bcrypt_checkpw(const char* password, const char* hash);

#ifdef __cplusplus
}
#endif

#endif /* APEX_BCRYPT_H */
