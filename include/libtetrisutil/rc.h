#ifndef LIBTETRISUTIL_RC_H
#define LIBTETRISUTIL_RC_H

/**
 * @file rc.h
 * @brief Minimal key=value directive reader for .tetrishrc.
 *
 * This is deliberately not the shell's line classifier
 * (tetrish/lib/rc_parser.h): that one turns each line into a shell command
 * to run, with PATH= as its one special case. This one turns "key = value"
 * lines into calls to a callback and skips everything else - comments,
 * blank lines, PATH=, bare shell commands - so several independent readers
 * (tetrislogd's own options, libtetrisutil's sender tuning, ...) can share one
 * rc file without stepping on each other's directives or on the shell's.
 */

/** The rc file every tetriSH component reads, relative to the working
 * directory, which the supported launcher preserves as the checkout (ADR
 * 0003). One name, defined once, so "which file did this value come from"
 * always has the same answer. */
#define RC_PATH ".tetrishrc"

/** Called once per recognised "key=value" line. Both arguments are trimmed and
 * NUL-terminated, and point into a buffer valid only for the call. */
typedef void (*rc_directive_fn)(const char *key, const char *value, void *ctx);

/**
 * Reads path line by line, calling fn for every key=value directive.
 *
 * Most readers want rc_bind() below. A line is skipped, not an error, if it is
 * blank, starts with '#', has no '=', or has an empty key.
 *
 * @param path  File to read.
 * @param fn    Called per directive.
 * @param ctx   Passed through to fn.
 * @returns The number of directives applied, or -1 when path could not be
 *          opened. A MISSING FILE AND A FILE WITH NOTHING TO SAY ARE DIFFERENT
 *          ANSWERS: several readers share this file, so applying none of its
 *          directives is ordinary, but a reader pointed at a path that does
 *          not exist applies none either and runs to completion looking
 *          healthy on a configuration nobody wrote. ADR 0003.
 */
int rc_load(const char *path, rc_directive_fn fn, void *ctx)
    __attribute__((warn_unused_result));

/* --- typed keys --------------------------------------------------------- *
 *
 * rc_load() hands out strings, which left tetrislogd, libtetrisdb's runner and
 * libtetrisauth's session reader each rebuilding the same whole-string strtol,
 * range check, fixed-size copy and first-bad-value report - about thirty lines
 * three times, with the ranges written twice across two libraries.
 *
 * A reader DECLARES its keys instead of writing a callback. What a key is
 * called, what it holds, where it lands and what range it takes are data, so
 * drift between two readers of one key is visible by reading two tables.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  RC_INT,   /**< Whole-string strtol in [lo, hi], stored as int. */
  RC_SIZE,  /**< The same, stored as size_t. */
  RC_BOOL,  /**< on/yes/true/1 or off/no/false/0, stored as int. */
  RC_STR,   /**< Copied into a fixed char[cap] buffer. */
  RC_CUSTOM /**< Handed to .parse, for a value only the owner knows. */
} rc_type_t;

/** Parses value into field, returning 0 on success. For a key whose spelling
 * belongs to its owner rather than to this reader - a log level, an enum. */
typedef int (*rc_parse_fn)(const char *value, void *field);

/** One recognised key and where its value goes. Build entries with offsetof(),
 * so a table is a description rather than code. */
typedef struct {
  const char *key;
  rc_type_t type;
  size_t off;        /**< offsetof(struct, field); ignored when check_only. */
  long lo, hi;       /**< RC_INT and RC_SIZE: inclusive bounds, both required. */
  size_t cap;        /**< RC_STR: sizeof the destination buffer. */
  size_t max_len;    /**< RC_STR: longest accepted value; 0 means cap - 1. */
  rc_parse_fn parse; /**< RC_CUSTOM only. */
  /** Validate and store nothing, for a key this reader OWNS but does not
   * consume: tetrislogd validates log_send_attempts though libtetrisutil's sender
   * reads it, and bin/tetrisdb validates db_timeout though bin/session reads
   * it. Both used to write that check into a local called `ignored`, which
   * reads like dead code and is the opposite. */
  bool check_only;
} rc_key_t;

/** The first unusable directive, named. An empty key means the load was
 * clean. */
typedef struct {
  char key[64];
  char value[160];
} rc_defect_t;

/** rc_bind()'s failures. A directive count comes back otherwise, exactly as
 * rc_load()'s does. */
enum {
  RC_E_OPEN = -1,   /**< path could not be opened. */
  RC_E_VALUE = -2,  /**< A key of ours had a bad value. */
  RC_E_UNKNOWN = -3 /**< An unknown key in owned_prefix's namespace. */
};

/**
 * Reads path and applies every key in keys[] to dst.
 *
 * Keys outside the table are skipped in silence, which is most of them - the
 * shell's own lines and every other reader's namespace.
 *
 * @param path          File to read; RC_PATH for the shared rc file.
 * @param keys          Table of recognised keys.
 * @param nkeys         Entries in keys.
 * @param dst           Written as parsing proceeds, so seed it with defaults
 *                      first and discard it on a negative return.
 * @param owned_prefix  Namespace this reader owns, e.g. "auth_", making an
 *                      unrecognised key carrying it an error. NULL for the
 *                      in-process readers, which cannot know a whole
 *                      namespace; non-NULL for the operator-facing check run
 *                      where a human is watching.
 * @param defect        Receives the offending key on RC_E_VALUE or
 *                      RC_E_UNKNOWN; may be NULL. The FIRST bad value wins:
 *                      an operator fixing the file top to bottom wants the
 *                      first thing wrong, and every one is fatal either way.
 * @returns The number of directives applied, or one of the RC_E_* values.
 */
int rc_bind(const char *path, const rc_key_t *keys, size_t nkeys, void *dst,
            const char *owned_prefix, rc_defect_t *defect)
    __attribute__((warn_unused_result));

/* Ballotbox's shell classifier remains part of the shared rc contract. */
typedef enum {
    RC_LINE_EMPTY,   /* blank, a comment, or a directive owned by a loader */
    RC_LINE_PATH,    /* "PATH" then '=', with optional spaces around it */
    RC_LINE_COMMAND  /* anything else, after trimming leading whitespace */
} rc_line_type_t;

/*
 * Classify one line for the shell.
 *
 *   On RC_LINE_PATH:    *value points to the substring after "PATH=".
 *   On RC_LINE_COMMAND: *value points to the trimmed command text.
 *   On RC_LINE_EMPTY:   *value is set to NULL.
 *
 * The returned pointer is into the input buffer. Do not free it, and do not
 * modify the contents of the input string.
 *
 * PATH tolerates whitespace around its '=' because every other directive in
 * the file is written "key = value" and read by a trimming parser. Requiring
 * PATH alone to be tight would make the one line that breaks everything the
 * one line that looks like all the others. A line starting with "PATH" that
 * is followed by neither whitespace nor '=' ("PATHETIC") is a command.
 *
 * Directives are RC_LINE_EMPTY rather than commands: they belong to rc_load's
 * callers, and a shell that ran them would answer a file configuring the log
 * daemon with "Command log_path not found" once per line.
 */
rc_line_type_t rc_classify_line(const char *line, const char **value);

#endif /* LIBTETRISUTIL_RC_H */
