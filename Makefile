CC      := cc
CFLAGS  := -Wall -Wextra -Werror=unused-result -O2 -Iinclude
LDFLAGS := -Llib
# --start-group/--end-group: static archives resolve left-to-right and ld
# does not re-scan one it already finished with, so any ordering of our own
# archives here is fragile the moment one of them calls into another (e.g.
# libballotclient's transport.c calling into libtetrissh - see git history,
# this line has been "fixed" by reordering twice already). The group makes
# ld keep re-scanning these until nothing new resolves, so their order here
# stops being load-bearing. System libs (ssl/crypto/pthread) stay outside:
# nothing in the group calls back into them in a cycle that needs it.
LDLIBS  := -Wl,--start-group -ltetrisauth -ltetrissh -lhtttp -lballotclient -lballotbrain -ltetrisdb -lcommon -Wl,--end-group -lssl -lcrypto -lpthread
OPENSSL := $(shell brew --prefix openssl)
CFLAGS  += -I$(OPENSSL)/include
LDFLAGS += -L$(OPENSSL)/lib

BIN_DIR := bin
LIB_DIR := lib

#
# Every header, as a prerequisite for everything that compiles.
#
# Without this, changing a struct in include/ rebuilds nothing: make only sees
# the .c files. That is not a slow build, it is a WRONG one - two objects
# compiled against different versions of the same struct link cleanly and then
# disagree about the layout at runtime, which is a bug with no compiler error
# and no stack trace pointing at its cause.
#
# Deliberately coarse: any header touches everything. At this size that costs a
# couple of seconds, and it cannot be wrong the way a hand-maintained list of
# per-target dependencies eventually is.
#
HEADERS := $(shell find include src -name '*.h' 2>/dev/null)
ifeq ($(HEADERS),)
$(warning include/ yielded no headers: builds will not react to header edits)
endif

LIBS := $(LIB_DIR)/libtetrisauth.a $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotbrain.a \
        $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a \
        $(LIB_DIR)/libtetrisui.a

# tetrish system programs (sys, ...) compiled as standalone binaries, PA1-style.
TETRISH_LIB_SRCS := $(wildcard src/tetrish/lib/*.c)
SYSPROG_SRCS     := $(wildcard src/tetrish/system_programs/*.c)
SYSPROG_BINS     := $(SYSPROG_SRCS:src/tetrish/system_programs/%.c=$(BIN_DIR)/%)

BINS := tetrish $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(BIN_DIR)/tetrislogd $(BIN_DIR)/tetrisdb $(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu $(SYSPROG_BINS)

.PHONY: all clean dirs
all: dirs $(LIBS) $(BINS)

dirs:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) var/log var/run

# === Libraries ===
LIBTETRISAUTH_SRCS   := $(wildcard src/libtetrisauth/*.c)
LIBTETRISSH_SRCS     := $(wildcard src/libtetrissh/*.c)
LIBHTTTP_SRCS        := $(wildcard src/libhtttp/*.c)
LIBBALLOTBRAIN_SRCS  := $(wildcard src/libballotbrain/*.c)
LIBBALLOTCLIENT_SRCS := $(wildcard src/libballotclient/*.c)
LIBTETRISDB_SRCS     := $(wildcard src/libtetrisdb/*.c) \
                        $(wildcard src/libtetrisdb/pipe/*.c) \
                        $(wildcard src/libtetrisdb/socket/*.c)
LIBCOMMON_SRCS       := $(wildcard src/libcommon/*.c)
LIBTETRISUI_SRCS     := $(wildcard src/libtetrisui/*.c)

LIBTETRISAUTH_OBJS   := $(LIBTETRISAUTH_SRCS:.c=.o)
LIBTETRISSH_OBJS     := $(LIBTETRISSH_SRCS:.c=.o)
LIBHTTTP_OBJS        := $(LIBHTTTP_SRCS:.c=.o)
LIBBALLOTBRAIN_OBJS  := $(LIBBALLOTBRAIN_SRCS:.c=.o)
LIBBALLOTCLIENT_OBJS := $(LIBBALLOTCLIENT_SRCS:.c=.o)
LIBTETRISDB_OBJS     := $(LIBTETRISDB_SRCS:.c=.o)
LIBCOMMON_OBJS       := $(LIBCOMMON_SRCS:.c=.o)
LIBTETRISUI_OBJS     := $(LIBTETRISUI_SRCS:.c=.o)

# Pattern rule: compile .c -> .o
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_DIR)/libtetrisauth.a: $(LIBTETRISAUTH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrissh.a: $(LIBTETRISSH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libhtttp.a: $(LIBHTTTP_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libballotbrain.a: $(LIBBALLOTBRAIN_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libballotclient.a: $(LIBBALLOTCLIENT_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisdb.a: $(LIBTETRISDB_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libcommon.a: $(LIBCOMMON_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisui.a: $(LIBTETRISUI_OBJS)
	ar rcs $@ $^

# === Binaries ===
tetrish: $(wildcard src/tetrish/*.c) $(TETRISH_LIB_SRCS) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# Each system program is its own binary, linked with the shared lib sources.
$(BIN_DIR)/%: src/tetrish/system_programs/%.c $(TETRISH_LIB_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# ballotd and bin/ballot_session are two separate programs: ballotd forks and
# execs bin/ballot_session per voter connection (SESSION_BIN in main.c), so
# session.c has its own main() and must not be linked into ballotd - same
# reasoning as tetrisd/session.c in the sibling tetriSH project. Listed
# explicitly rather than wildcarded for exactly that reason.
$(BIN_DIR)/ballotd: src/ballotd/main.c src/ballotd/dispatch.c src/ballotd/control_plane.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/ballot_session: src/ballotd/session.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrislogd: $(wildcard src/tetrislogd/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisdb: $(wildcard src/tetrisdb/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# ballotctl and ballotu are the only binaries that draw, so -ltetrisui and
# -lncurses are scoped to them rather than added to the global LDLIBS.
$(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu: LDLIBS += -ltetrisui -lncurses

# The real client: src/ballotctl/ballotctl.c only - same reasoning as
# ballotu below (main.c/mock.c/mock.h/screens.c stay on disk, unbuilt).
$(BIN_DIR)/ballotctl: src/ballotctl/ballotctl.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# The real client: src/ballotu/ballotu.c only. main.c/mock.c/mock.h/
# screens.c stay on disk (the old mock demo) but are deliberately excluded
# here rather than wildcarded - both define main(), and screens.c's
# screen_* functions would otherwise collide with ballotu.c's.
$(BIN_DIR)/ballotu: src/ballotu/ballotu.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# === Unit tests (Unity) ===
UNITY_DIR   := external/2026-pa1-50005-6767/tests/unity
TEST_SRCS   := $(wildcard tests/unit/test_*.c)
TEST_BINS   := $(TEST_SRCS:tests/unit/%.c=$(BIN_DIR)/%)
TEST_CFLAGS := $(CFLAGS) -I$(UNITY_DIR) -Itests/unit/support

# libballotclient reuses symbols from libballotbrain, so it must precede it.
# -lhtttp is for test_codec, which exercises the wire codec directly against
# real htttp_parse/serialize rather than through a seam.
# -ltetrissh -lssl -lcrypto: voter.c's bu_join/bu_submit_vote reference the
# real bcl_send (transport.c) at the object-file level even when a test only
# calls voter.c's pure functions and never a seam-calling one - the archive
# pulls in the whole of voter.o, and now that bcl_send is real (not the old
# no-dependency stub) that drags libtetrissh in too. A test that defines its
# own bcl_send (fake_client_seams.h) keeps transport.o itself out, but these
# libs still need to be on the link line for the tests that do not.
# -ltetrisdb: bb_alloc_id (ballotbrain.c, always linked - bb_create/bb_destroy
# live there too) calls tdb_socket_* directly. No test needs to fake it (it
# degrades to a fixed id when unreachable), but the symbols still need to
# resolve.
# -lcommon: bc_fold_eligible (admin.c) calls libcommon/playername.c's
# player_name_ok/player_name_fold directly (the same fold every real
# username goes through), so libballotclient.a now has an unresolved
# reference into libcommon.a for every test that links it - which, per the
# note above, is all of them.
# --start-group: same archive-ordering fragility as the top-level LDLIBS -
# see that comment. Each test defines the seams it wants to substitute;
# because the libraries are static archives, a seam defined in the test keeps
# the real member out of the binary (see tests/unit/support/fake_*_seams.h).
TEST_LDLIBS := -L$(LIB_DIR) -Wl,--start-group -lballotclient -lballotbrain -lhtttp -ltetrissh -ltetrisdb -lcommon -Wl,--end-group -lssl -lcrypto -lpthread

$(BIN_DIR)/test_%: tests/unit/test_%.c $(wildcard tests/unit/support/*.h) $(UNITY_DIR)/unity.c $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a $(HEADERS)
	$(CC) $(TEST_CFLAGS) $< $(UNITY_DIR)/unity.c -o $@ $(TEST_LDLIBS)

# test_db is not a Unity test: it brings its own harness and lives in tests/
# rather than tests/unit/, so it needs an explicit rule to beat the pattern
# rule above. It spawns a real PipeRunner child and skips those cases when
# java or the jar is missing, so it stays runnable on a machine without a JVM.
$(BIN_DIR)/test_db: tests/test_db.c $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_db.c -o $@ $(LDFLAGS) -ltetrisdb -lcommon -lpthread

$(BIN_DIR)/test_auth: tests/test_auth.c $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/test_jwt: tests/test_jwt.c src/libtetrisauth/jwt.c $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) -lcrypto

$(BIN_DIR)/test_rc: tests/test_rc.c src/tetrislogd/config.c $(BIN_DIR)/tetrislogd $(LIB_DIR)/libtetrisauth.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_rc.c src/tetrislogd/config.c -o $@ $(LDFLAGS) -ltetrisauth -ltetrisdb -lcommon

$(BIN_DIR)/test_tetrisdb: tests/test_tetrisdb.c $(BIN_DIR)/tetrisdb $(HEADERS)
	$(CC) $(CFLAGS) tests/test_tetrisdb.c -o $@ -lpthread

# Same story as test_db, plus it spawns the real bin/tetrislogd over a socket,
# so the daemon is a build prerequisite rather than just a runtime assumption.
$(BIN_DIR)/test_logd: tests/test_logd.c $(LIB_DIR)/libcommon.a $(BIN_DIR)/tetrislogd $(HEADERS)
	$(CC) $(CFLAGS) tests/test_logd.c -o $@ $(LDFLAGS) -lcommon

# Real-process E2E for ballotd: forks the real bin/ballotd, which itself
# forks bin/ballot_session per voter connection, so both are build
# prerequisites rather than just runtime assumptions (same story as
# test_logd needing bin/tetrislogd). Needs libtetrissh/libhtttp/libballot*
# directly for the client-side handshake and codec calls the test makes.
$(BIN_DIR)/test_ballotd: tests/test_ballotd.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(HEADERS)
	$(CC) $(CFLAGS) tests/test_ballotd.c -o $@ $(LDFLAGS) -lballotclient -lballotbrain -ltetrissh -lhtttp -ltetrisdb -lcommon -lssl -lcrypto -lpthread

# Real-process E2E for the client side of the same picture: drives the real
# bcl_connect/bu_join/bcl_send (src/libballotclient/transport.c) - the same
# calls ballotu.c makes - against a real bin/ballotd, rather than a
# hand-rolled socket harness like test_ballotd.c's.
$(BIN_DIR)/test_client_transport: tests/test_client_transport.c $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a $(BIN_DIR)/ballotd $(BIN_DIR)/ballot_session $(HEADERS)
	$(CC) $(CFLAGS) tests/test_client_transport.c -o $@ $(LDFLAGS) -lballotclient -lballotbrain -ltetrissh -lhtttp -ltetrisdb -lcommon -lssl -lcrypto -lpthread

.PHONY: test
test: dirs $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd $(BIN_DIR)/test_auth $(BIN_DIR)/test_jwt $(BIN_DIR)/test_rc $(BIN_DIR)/test_tetrisdb $(BIN_DIR)/test_ballotd $(BIN_DIR)/test_client_transport
	@fail=0; \
	for t in $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd $(BIN_DIR)/test_rc $(BIN_DIR)/test_tetrisdb $(BIN_DIR)/test_jwt $(BIN_DIR)/test_auth $(BIN_DIR)/test_ballotd $(BIN_DIR)/test_client_transport; do \
	  echo "== $$t =="; \
	  $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL UNIT TESTS PASSED"

.PHONY: test-ci
test-ci: dirs $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd $(BIN_DIR)/test_auth $(BIN_DIR)/test_jwt $(BIN_DIR)/test_rc
	@fail=0; \
	for t in $(TEST_BINS) $(BIN_DIR)/test_logd $(BIN_DIR)/test_rc $(BIN_DIR)/test_jwt; do \
	  echo "== $$t =="; \
	  $$t || fail=1; \
	done; \
	for t in $(BIN_DIR)/test_db $(BIN_DIR)/test_auth; do \
	  echo "== $$t =="; \
	  TETRISH_NO_RUNNER=1 $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL CI TESTS PASSED"

# Build from scratch and drop straight into the shell. `all` alone can leave a
# stale binary behind when a source is removed rather than changed, and the
# shell is the entry point everything else is reached through.
#
# clean and all are sequenced by recursive $(MAKE), not by listing them as
# prerequisites. As prerequisites they are unordered, so under -j the rm can
# land in the middle of the build it was meant to precede.
.PHONY: start
start:
	$(MAKE) clean
	$(MAKE) all
	./tetrish

clean:
	rm -rf $(BIN_DIR)/* $(LIB_DIR)/*.a src/*/*.o src/*/*/*.o
