#include <stdio.h>
#include "libtetrissh/tetrissh.h"
#include "common.h"

_Static_assert(sizeof(((session_t *)0)->key) == SESSION_KEY_LEN,
               "session_t.key must match SESSION_KEY_LEN in common.h");

/* Silent by default: apps own stdout/stderr. -DTSSH_DEBUG for diagnostics. */
#ifdef TSSH_DEBUG
#define TSSH_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define TSSH_LOG(...) ((void)0)
#endif

/* Sanity caps on attacker-controlled handshake length fields — anything
 * larger is a malformed/hostile peer, not a real message (S2). */
#define TSSH_NONCE_LEN 32
#define TSSH_MAX_NONCE_LEN 64
#define TSSH_MAX_SIG_LEN 512
#define TSSH_MAX_CERT_LEN 8192

/* Traffic frames use a 4-byte BE length prefix (spec format). The handshake
 * keeps common.c's 8-byte send_int/INT_BYTES format — that exchange is
 * lib-private and never seen by the application layer. */
#define FRAME_PREFIX_BYTES 4

static int send_u32(int fd, uint32_t value)
{
    unsigned char buf[FRAME_PREFIX_BYTES] = {
        (unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value
    };
    return send_all(fd, buf, FRAME_PREFIX_BYTES);
}

static int read_u32(int fd, uint32_t *value)
{
    unsigned char *buf = read_bytes(fd, FRAME_PREFIX_BYTES);
    if (!buf)
        return -1;
    *value = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
             ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    free(buf);
    return 0;
}

/* --- Handshake ----------------------------------------------------------- */

/* Client side. See tetrissh.h. Single-exit cleanup: every failure sets rc
 * and jumps to fail, which frees all heap objects (NULL-safe) and cleanses
 * the stack copy of the session key. s is only written on success. */
int session_connect(session_t *s, int fd, const char *ca_path)
{
    int rc = SESSION_OK;
    unsigned char *signed_nonce_len_buf = NULL;
    unsigned char *signed_nonce = NULL;
    unsigned char *server_cert_len_buf = NULL;
    unsigned char *server_cert_buf = NULL;
    unsigned char *sym_key = NULL;
    X509 *server_cert = NULL;
    EVP_PKEY *server_pub = NULL;
    unsigned char session_key[SESSION_KEY_LEN];
    unsigned char nonce[TSSH_NONCE_LEN];

    memset(s, 0, sizeof(*s));

    if (RAND_bytes(nonce, sizeof(nonce)) != 1)
        return SESSION_ERR_CRYPTO;
    size_t nonce_len = sizeof(nonce);

    if (send_int(fd, nonce_len) < 0 || send_all(fd, nonce, nonce_len) < 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }

    signed_nonce_len_buf = read_bytes(fd, INT_BYTES);
    if (!signed_nonce_len_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    uint64_t signed_nonce_len = bytes_to_int(signed_nonce_len_buf);
    if (signed_nonce_len == 0 || signed_nonce_len > TSSH_MAX_SIG_LEN)
    {
        rc = SESSION_ERR_PROTO;
        goto fail;
    }
    signed_nonce = read_bytes(fd, signed_nonce_len);
    if (!signed_nonce)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }

    server_cert_len_buf = read_bytes(fd, INT_BYTES);
    if (!server_cert_len_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    uint64_t server_cert_len = bytes_to_int(server_cert_len_buf);
    if (server_cert_len == 0 || server_cert_len > TSSH_MAX_CERT_LEN)
    {
        rc = SESSION_ERR_PROTO;
        goto fail;
    }
    server_cert_buf = read_bytes(fd, server_cert_len);
    if (!server_cert_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }

    server_cert = load_cert_bytes(server_cert_buf, server_cert_len);
    if (!server_cert || !verify_server_cert(server_cert, ca_path))
    {
        TSSH_LOG("tetrissh: server cert not verified\n");
        rc = SESSION_ERR_AUTH;
        goto fail;
    }
    TSSH_LOG("tetrissh: verified server cert\n");

    if (!verify_message_pss(server_cert, signed_nonce, signed_nonce_len, nonce, nonce_len))
    {
        TSSH_LOG("tetrissh: signed nonce not verified\n");
        rc = SESSION_ERR_AUTH;
        goto fail;
    }
    TSSH_LOG("tetrissh: verified signed nonce\n");

    server_pub = X509_get_pubkey(server_cert);
    if (!server_pub)
    {
        rc = SESSION_ERR_CRYPTO;
        goto fail;
    }

    if (generate_session_key(session_key) < 0)
    {
        rc = SESSION_ERR_CRYPTO;
        goto fail;
    }
    size_t sym_key_len;
    sym_key = rsa_encrypt_block(server_pub, session_key, sizeof(session_key), &sym_key_len, 1);
    if (!sym_key)
    {
        rc = SESSION_ERR_CRYPTO;
        goto fail;
    }
    if (send_int(fd, sym_key_len) < 0 || send_all(fd, sym_key, sym_key_len) < 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }

    s->fd = fd;
    memcpy(s->key, session_key, SESSION_KEY_LEN);
    s->established = 1;
    TSSH_LOG("tetrissh: session handshake complete\n");

fail:
    OPENSSL_cleanse(session_key, sizeof(session_key));
    free(signed_nonce_len_buf);
    free(signed_nonce);
    free(server_cert_len_buf);
    free(server_cert_buf);
    free(sym_key);
    X509_free(server_cert);
    EVP_PKEY_free(server_pub);
    return rc;
}

/* Server side. See tetrissh.h. Same single-exit cleanup discipline as
 * session_connect; the decrypted key buffer is cleansed before free. */
int session_accept(session_t *s, int fd, EVP_PKEY *priv, const char *cert_path)
{
    int rc = SESSION_OK;
    unsigned char *nonce_len_buf = NULL;
    unsigned char *nonce = NULL;
    unsigned char *signed_nonce = NULL;
    unsigned char *cert_data = NULL;
    unsigned char *symkey_len_buf = NULL;
    unsigned char *symkey_buf = NULL;
    unsigned char *key_buf = NULL;
    FILE *cert_fp = NULL;
    size_t session_key_len = 0;

    memset(s, 0, sizeof(*s));

    nonce_len_buf = read_bytes(fd, INT_BYTES);
    if (!nonce_len_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    uint64_t nonce_len = bytes_to_int(nonce_len_buf);
    if (nonce_len == 0 || nonce_len > TSSH_MAX_NONCE_LEN)
    {
        rc = SESSION_ERR_PROTO;
        goto fail;
    }

    nonce = read_bytes(fd, nonce_len);
    if (!nonce)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    size_t signed_len;
    signed_nonce = sign_message_pss(priv, nonce, nonce_len, &signed_len);
    if (!signed_nonce)
    {
        rc = SESSION_ERR_CRYPTO;
        goto fail;
    }

    if (send_int(fd, signed_len) < 0 || send_all(fd, signed_nonce, signed_len) < 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }

    cert_fp = fopen(cert_path, "rb");
    if (!cert_fp)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    if (fseek(cert_fp, 0, SEEK_END) != 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    long cert_size_l = ftell(cert_fp);
    if (cert_size_l <= 0 || fseek(cert_fp, 0, SEEK_SET) != 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    size_t cert_size = (size_t)cert_size_l;

    cert_data = malloc(cert_size);
    if (!cert_data)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    if (fread(cert_data, 1, cert_size, cert_fp) != cert_size)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    fclose(cert_fp);
    cert_fp = NULL;

    if (send_int(fd, cert_size) < 0 || send_all(fd, cert_data, cert_size) < 0)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    TSSH_LOG("tetrissh: authentication phase done\n");

    symkey_len_buf = read_bytes(fd, INT_BYTES);
    if (!symkey_len_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    uint64_t symkey_len = bytes_to_int(symkey_len_buf);
    if (symkey_len != RSA_KEY_BYTES)
    {
        rc = SESSION_ERR_PROTO;
        goto fail;
    }

    symkey_buf = read_bytes(fd, symkey_len);
    if (!symkey_buf)
    {
        rc = SESSION_ERR_IO;
        goto fail;
    }
    key_buf = rsa_decrypt_block(priv, symkey_buf, symkey_len, &session_key_len, 1);
    if (!key_buf || session_key_len != SESSION_KEY_LEN)
    {
        TSSH_LOG("tetrissh: session key handshake failed\n");
        rc = SESSION_ERR_CRYPTO;
        goto fail;
    }
    memcpy(s->key, key_buf, SESSION_KEY_LEN);
    s->fd = fd;
    s->established = 1;
    TSSH_LOG("tetrissh: session handshake complete\n");

fail:
    if (cert_fp)
        fclose(cert_fp);
    if (key_buf)
        OPENSSL_cleanse(key_buf, session_key_len);
    free(nonce_len_buf);
    free(nonce);
    free(signed_nonce);
    free(cert_data);
    free(symkey_len_buf);
    free(symkey_buf);
    free(key_buf);
    return rc;
}

/* --- Traffic (only valid after handshake) -------------------------------- */

int session_send(session_t *s, const uint8_t *buf, uint32_t len)
{
    if (!s->established)
    {
        TSSH_LOG("tetrissh: send before handshake\n");
        return SESSION_ERR_PROTO;
    }
    /* Ciphertext is never smaller than plaintext, so this can't fit. */
    if (len > SESSION_MAX_FRAME)
        return SESSION_ERR_TOOBIG;
    size_t enc_block;
    unsigned char *message = session_encrypt(s->key, buf, len, &enc_block);
    if (!message)
        return SESSION_ERR_CRYPTO;
    /* Limit is defined on the wire frame — same quantity recv checks (A2). */
    if (enc_block > SESSION_MAX_FRAME)
    {
        free(message);
        return SESSION_ERR_TOOBIG;
    }
    int rc = SESSION_OK;
    if (send_u32(s->fd, (uint32_t)enc_block) < 0 || send_all(s->fd, message, enc_block) < 0)
        rc = SESSION_ERR_IO;
    free(message);
    return rc;
}

int session_recv(session_t *s, uint8_t *buf, uint32_t *len)
{
    if (!s->established)
    {
        TSSH_LOG("tetrissh: recv before handshake\n");
        return SESSION_ERR_PROTO;
    }
    uint32_t token_len;
    if (read_u32(s->fd, &token_len) < 0)
        return SESSION_ERR_IO;

    if (token_len > SESSION_MAX_FRAME)
    {
        s->established = 0; /* stream out of sync — unrecoverable (A3) */
        return SESSION_ERR_TOOBIG;
    }

    unsigned char *token = read_bytes(s->fd, token_len);
    if (!token)
        return SESSION_ERR_IO;
    size_t plain_len;
    unsigned char *plain = session_decrypt(s->key, token, token_len, &plain_len);
    free(token);
    if (!plain)
        return SESSION_ERR_CRYPTO;

    if (plain_len > *len)
    {
        free(plain);
        return SESSION_ERR_NOSPACE; /* frame consumed; stream still in sync */
    }
    memcpy(buf, plain, plain_len);
    *len = (uint32_t)plain_len;
    free(plain);
    return SESSION_OK;
}

/* --- Teardown ------------------------------------------------------------ */

void session_close(session_t *s)
{
    OPENSSL_cleanse(s->key, SESSION_KEY_LEN);
    s->established = 0;
}
