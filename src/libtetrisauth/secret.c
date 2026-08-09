/**
 * @file secret.c
 * @brief Auth/jwt_secret: created by the launcher, read by a session.
 *
 * Phase 4d (#58). Contract in include/libtetrisauth/provision.h.
 *
 * A separate pair from tauth.c because two binaries need it: bin/tetrisdb
 * creates, bin/session reads. #55's file-statics are private to the entry
 * point by design, and this cannot be.
 *
 * It does not travel to the reuse project, which is why it is not in jwt.c.
 * The arrow points only inward, into jwt_mint()'s injected (pointer, length):
 * jwt.c never includes this header.
 *
 * NO O_CREAT ANYWHERE IN tauth_secret_load(). The retry was deleted rather
 * than written, because O_EXCL does not make create-and-fill atomic and a
 * creator that dies in that window leaves a zero-length file the length rule
 * then refuses forever. The header carries the full argument; do not helpfully
 * add creation back.
 *
 * No file-static and no cached failure, so chmod 0600 fixes live sessions on
 * their next attempt.
 *
 * tauth_secret_provision() has exactly one caller (bin/tetrisdb start, inside
 * its flock) and therefore exactly one writer at a time - that precondition
 * is the caller's to hold, not this file's to re-derive, so a link() failure
 * here is treated as a real error rather than a race to retry around.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "libtetrisutil/logmsg.h"
#include "libtetrisauth/provision.h"

#define TAUTH_SECRET_REL    "auth/jwt_secret"
#define TAUTH_SECRET_MIN    32
#define TAUTH_SECRET_MAX    64
#define TAUTH_SECRET_CREATE 32

static int build_path(const char *root, char *buf, size_t buflen) {
  int n = snprintf(buf, buflen, "%s/" TAUTH_SECRET_REL, root);
  return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

/* Provisioning reports its reason to an operator through a caller-supplied
 * buffer, so every failure path needs the same NULL-and-length guard. Once,
 * here, rather than at each of a dozen call sites. */
static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
  if (err == NULL || errlen == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, errlen, fmt, ap);
  va_end(ap);
}

/*
 * Validate an already-open fd against #46/#58's rules via fstat, so there is
 * no window between the check and the read. On success writes the byte count
 * to *out_len; on failure writes a short human-readable reason into defect.
 */
static int validate_fd(int fd, long *out_len, char *defect, size_t defectlen) {
  struct stat st;

  if (fstat(fd, &st) != 0) {
    snprintf(defect, defectlen, "fstat failed: %s", strerror(errno));
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    snprintf(defect, defectlen, "not a regular file");
    return -1;
  }
  if (st.st_mode & 077) {
    snprintf(defect, defectlen, "group- or other-accessible (mode 0%o)",
             (unsigned)(st.st_mode & 0777));
    return -1;
  }
  if (st.st_uid != geteuid()) {
    snprintf(defect, defectlen, "not owned by this process (uid %u)",
             (unsigned)st.st_uid);
    return -1;
  }
  if (st.st_size < TAUTH_SECRET_MIN || st.st_size > TAUTH_SECRET_MAX) {
    snprintf(defect, defectlen, "wrong size (%lld bytes, want %d..%d)",
             (long long)st.st_size, TAUTH_SECRET_MIN, TAUTH_SECRET_MAX);
    return -1;
  }

  *out_len = (long)st.st_size;
  return 0;
}

/** Open path read-only and validate it. Returns the fd, which the caller
 * closes, and writes the byte count to *len. On failure returns -1 with the
 * reason in defect, and sets *absent when the file simply is not there - the
 * one failure provisioning treats as "create it" rather than "refuse".
 * O_NONBLOCK IS WHAT MAKES THE S_ISREG CHECK REACHABLE, and it is the whole
 * reason this is not a bare open(). open(O_RDONLY) on a FIFO blocks until a
 * writer appears, so without the flag the hang that S_ISREG exists to remove
 * happens one line earlier, inside open() itself, and the check can never
 * fire. Cleared once the fd is known to be a regular file, where it means
 * nothing either way. */
static int open_validated(const char *path, long *len, char *defect,
                          size_t defectlen, int *absent) {
  if (absent != NULL)
    *absent = 0;

  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    if (absent != NULL && errno == ENOENT)
      *absent = 1;
    snprintf(defect, defectlen, "%s", strerror(errno));
    return -1;
  }

  if (validate_fd(fd, len, defect, defectlen) != 0) {
    close(fd);
    return -1;
  }

  int fl = fcntl(fd, F_GETFL);
  if (fl != -1)
    (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

  return fd;
}

int tauth_secret_load(const char *root, unsigned char *out, size_t outlen) {
  char path[PATH_MAX];
  char defect[128];

  if (build_path(root, path, sizeof path) != 0) {
    log_send(LOG_ERROR, "auth/jwt_secret: path too long for root '%s'", root);
    return -1;
  }

  long len;
  int fd = open_validated(path, &len, defect, sizeof defect, NULL);
  if (fd < 0) {
    log_send(LOG_ERROR, "%s: %s", path, defect);
    return -1;
  }

  if ((size_t)len > outlen) {
    log_send(LOG_ERROR,
             "%s: %ld bytes does not fit the caller's %zu-byte buffer",
             path, len, outlen);
    close(fd);
    return -1;
  }

  size_t total = 0;
  while (total < (size_t)len) {
    ssize_t n = read(fd, out + total, (size_t)len - total);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      log_send(LOG_ERROR, "%s: read failed: %s", path, strerror(errno));
      close(fd);
      return -1;
    }
    if (n == 0) {
      log_send(LOG_ERROR, "%s: short read (%zu of %ld bytes)", path, total, len);
      close(fd);
      return -1;
    }
    total += (size_t)n;
  }

  close(fd);
  return (int)total;
}

int tauth_secret_provision(const char *root, char *err, size_t errlen) {
  char path[PATH_MAX];
  char defect[128];

  if (err != NULL && errlen > 0)
    err[0] = '\0';

  if (build_path(root, path, sizeof path) != 0) {
    set_err(err, errlen, "path too long for root '%s'", root);
    return -1;
  }

  /* Present: validate and stop. Idempotent by construction - a second call
   * against an existing, usable key takes this branch and creates nothing.
   * Anything other than "absent" is refused rather than replaced, which is
   * where "it never repairs" actually lives. */
  long len;
  int absent = 0;
  int fd = open_validated(path, &len, defect, sizeof defect, &absent);
  if (fd >= 0) {
    close(fd);
    return 0;
  }
  if (!absent) {
    set_err(err, errlen, "%s: %s", path, defect);
    return -1;
  }

  /* Absent: create. mkstemp + fchmod + write + fsync + link - never rename(),
   * which would silently clobber a winner instead of losing to one. */
  char tmp[PATH_MAX];
  int n = snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
  if (n <= 0 || (size_t)n >= sizeof tmp) {
    set_err(err, errlen, "path too long for root '%s'", root);
    return -1;
  }

  int tfd = mkstemp(tmp);
  if (tfd < 0) {
    set_err(err, errlen, "%s: mkstemp: %s", tmp, strerror(errno));
    return -1;
  }

  /* One unwind from here down: the temp file must not survive any failure,
   * or the next start trips over a stray 0-byte sibling of the real key. */
  unsigned char key[TAUTH_SECRET_CREATE];
  memset(key, 0, sizeof key);

  if (fchmod(tfd, 0600) != 0) {
    set_err(err, errlen, "%s: fchmod: %s", tmp, strerror(errno));
    goto fail;
  }

  if (RAND_bytes(key, sizeof key) != 1) {
    set_err(err, errlen, "RAND_bytes failed");
    goto fail;
  }

  size_t written = 0;
  while (written < sizeof key) {
    ssize_t w = write(tfd, key + written, sizeof key - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      set_err(err, errlen, "%s: write: %s", tmp, strerror(errno));
      goto fail;
    }
    written += (size_t)w;
  }

  if (fsync(tfd) != 0) {
    set_err(err, errlen, "%s: fsync: %s", tmp, strerror(errno));
    goto fail;
  }

  OPENSSL_cleanse(key, sizeof key);
  close(tfd);
  tfd = -1;

  /* link(), not rename(): rename would clobber a key somebody else just won
   * the right to create, and silently. Failing to link is the correct loss. */
  if (link(tmp, path) != 0) {
    set_err(err, errlen, "%s -> %s: link: %s", tmp, path, strerror(errno));
    goto fail;
  }
  unlink(tmp);

  /* Read back what actually landed rather than trusting what was written, so
   * a usable secret on return is observed rather than assumed. */
  fd = open_validated(path, &len, defect, sizeof defect, NULL);
  if (fd < 0) {
    set_err(err, errlen, "%s: %s", path, defect);
    return -1;
  }
  close(fd);
  return 0;

fail:
  OPENSSL_cleanse(key, sizeof key);
  if (tfd >= 0)
    close(tfd);
  unlink(tmp);
  return -1;
}
