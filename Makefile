CC      := cc
CFLAGS  := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -Llib
LDLIBS  := -ltetrissh -lhtttp -lballotclient -lballotbrain -ltetrisdb -lcommon -lssl -lcrypto -lpthread
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
HEADERS := $(shell find include -name '*.h')
ifeq ($(HEADERS),)
$(warning include/ yielded no headers: builds will not react to header edits)
endif

LIBS := $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libballotbrain.a \
        $(LIB_DIR)/libballotclient.a $(LIB_DIR)/libtetrisdb.a $(LIB_DIR)/libcommon.a \
        $(LIB_DIR)/libballotui.a

# tetrish system programs (sys, ...) compiled as standalone binaries, PA1-style.
TETRISH_LIB_SRCS := $(wildcard src/tetrish/lib/*.c)
SYSPROG_SRCS     := $(wildcard src/tetrish/system_programs/*.c)
SYSPROG_BINS     := $(SYSPROG_SRCS:src/tetrish/system_programs/%.c=$(BIN_DIR)/%)

BINS := tetrish $(BIN_DIR)/ballotd $(BIN_DIR)/tetrislogd $(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu $(SYSPROG_BINS)

.PHONY: all clean dirs
all: dirs $(LIBS) $(BINS)

dirs:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) var/log var/run

# === Libraries ===
LIBTETRISSH_SRCS     := $(wildcard src/libtetrissh/*.c)
LIBHTTTP_SRCS        := $(wildcard src/libhtttp/*.c)
LIBBALLOTBRAIN_SRCS  := $(wildcard src/libballotbrain/*.c)
LIBBALLOTCLIENT_SRCS := $(wildcard src/libballotclient/*.c)
LIBTETRISDB_SRCS     := $(wildcard src/libtetrisdb/*.c)
LIBCOMMON_SRCS       := $(wildcard src/libcommon/*.c)
LIBBALLOTUI_SRCS     := $(wildcard src/libballotui/*.c)

LIBTETRISSH_OBJS     := $(LIBTETRISSH_SRCS:.c=.o)
LIBHTTTP_OBJS        := $(LIBHTTTP_SRCS:.c=.o)
LIBBALLOTBRAIN_OBJS  := $(LIBBALLOTBRAIN_SRCS:.c=.o)
LIBBALLOTCLIENT_OBJS := $(LIBBALLOTCLIENT_SRCS:.c=.o)
LIBTETRISDB_OBJS     := $(LIBTETRISDB_SRCS:.c=.o)
LIBCOMMON_OBJS       := $(LIBCOMMON_SRCS:.c=.o)
LIBBALLOTUI_OBJS     := $(LIBBALLOTUI_SRCS:.c=.o)

# Pattern rule: compile .c -> .o
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

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

$(LIB_DIR)/libballotui.a: $(LIBBALLOTUI_OBJS)
	ar rcs $@ $^

# === Binaries ===
tetrish: $(wildcard src/tetrish/*.c) $(TETRISH_LIB_SRCS) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# Each system program is its own binary, linked with the shared lib sources.
$(BIN_DIR)/%: src/tetrish/system_programs/%.c $(TETRISH_LIB_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

$(BIN_DIR)/ballotd: $(wildcard src/ballotd/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrislogd: $(wildcard src/tetrislogd/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# ballotctl and ballotu are the only binaries that draw, so -lballotui and
# -lncurses are scoped to them rather than added to the global LDLIBS.
$(BIN_DIR)/ballotctl $(BIN_DIR)/ballotu: LDLIBS += -lballotui -lncurses

$(BIN_DIR)/ballotctl: $(wildcard src/ballotctl/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/ballotu: $(wildcard src/ballotu/*.c) $(LIBS) $(HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# === Unit tests (Unity) ===
UNITY_DIR   := external/2026-pa1-50005-6767/tests/unity
TEST_SRCS   := $(wildcard tests/unit/test_*.c)
TEST_BINS   := $(TEST_SRCS:tests/unit/%.c=$(BIN_DIR)/%)
TEST_CFLAGS := $(CFLAGS) -I$(UNITY_DIR) -Itests/unit/support

# libballotclient reuses symbols from libballotbrain, so it must precede it.
# Each test defines the seams it wants to substitute; because the libraries are
# static archives, a seam defined in the test keeps the real member out of the
# binary (see tests/unit/support/fake_*_seams.h).
TEST_LDLIBS := -L$(LIB_DIR) -lballotclient -lballotbrain -lpthread

$(BIN_DIR)/test_%: tests/unit/test_%.c $(wildcard tests/unit/support/*.h) $(UNITY_DIR)/unity.c $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(HEADERS)
	$(CC) $(TEST_CFLAGS) $< $(UNITY_DIR)/unity.c -o $@ $(TEST_LDLIBS)

# test_db is not a Unity test: it brings its own harness and lives in tests/
# rather than tests/unit/, so it needs an explicit rule to beat the pattern
# rule above. It spawns a real PipeRunner child and skips those cases when
# java or the jar is missing, so it stays runnable on a machine without a JVM.
$(BIN_DIR)/test_db: tests/test_db.c $(LIB_DIR)/libtetrisdb.a $(HEADERS)
	$(CC) $(CFLAGS) tests/test_db.c -o $@ $(LDFLAGS) -ltetrisdb -lpthread

# Same story as test_db, plus it spawns the real bin/tetrislogd over a socket,
# so the daemon is a build prerequisite rather than just a runtime assumption.
$(BIN_DIR)/test_logd: tests/test_logd.c $(LIB_DIR)/libcommon.a $(BIN_DIR)/tetrislogd $(HEADERS)
	$(CC) $(CFLAGS) tests/test_logd.c -o $@ $(LDFLAGS) -lcommon

.PHONY: test
test: dirs $(LIB_DIR)/libballotbrain.a $(LIB_DIR)/libballotclient.a $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd
	@fail=0; \
	for t in $(TEST_BINS) $(BIN_DIR)/test_db $(BIN_DIR)/test_logd; do \
	  echo "== $$t =="; \
	  $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL UNIT TESTS PASSED"

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
	rm -rf $(BIN_DIR)/* $(LIB_DIR)/*.a src/*/*.o