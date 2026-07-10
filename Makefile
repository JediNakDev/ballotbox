CC      := cc
CFLAGS  := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -Llib
LDLIBS  := -ltetrissh -lhtttp -ltetrisbrain -lssl -lcrypto -lpthread
OPENSSL := $(shell brew --prefix openssl)
CFLAGS  += -I$(OPENSSL)/include
LDFLAGS += -L$(OPENSSL)/lib

BIN_DIR := bin
LIB_DIR := lib

LIBS := $(LIB_DIR)/libtetrissh.a $(LIB_DIR)/libhtttp.a $(LIB_DIR)/libtetrisbrain.a

# tetrish system programs (sys, ...) compiled as standalone binaries, PA1-style.
TETRISH_LIB_SRCS := $(wildcard src/tetrish/lib/*.c)
SYSPROG_SRCS     := $(wildcard src/tetrish/system_programs/*.c)
SYSPROG_BINS     := $(SYSPROG_SRCS:src/tetrish/system_programs/%.c=$(BIN_DIR)/%)

BINS := tetrish $(BIN_DIR)/tetrisd $(BIN_DIR)/tetrislogd $(BIN_DIR)/tetrisctl $(BIN_DIR)/tetrisu $(SYSPROG_BINS)

.PHONY: all clean dirs
all: dirs $(LIBS) $(BINS)

dirs:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) var/log var/run

# === Libraries ===
LIBTETRISSH_SRCS    := $(wildcard src/libtetrissh/*.c)
LIBHTTTP_SRCS       := $(wildcard src/libhtttp/*.c)
LIBTETRISBRAIN_SRCS := $(wildcard src/libtetrisbrain/*.c)

LIBTETRISSH_OBJS    := $(LIBTETRISSH_SRCS:.c=.o)
LIBHTTTP_OBJS       := $(LIBHTTTP_SRCS:.c=.o)
LIBTETRISBRAIN_OBJS := $(LIBTETRISBRAIN_SRCS:.c=.o)

# Pattern rule: compile .c -> .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_DIR)/libtetrissh.a: $(LIBTETRISSH_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libhtttp.a: $(LIBHTTTP_OBJS)
	ar rcs $@ $^

$(LIB_DIR)/libtetrisbrain.a: $(LIBTETRISBRAIN_OBJS)
	ar rcs $@ $^

# === Binaries ===
tetrish: $(wildcard src/tetrish/*.c) $(TETRISH_LIB_SRCS) $(LIBS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

# Each system program is its own binary, linked with the shared lib sources.
$(BIN_DIR)/%: src/tetrish/system_programs/%.c $(TETRISH_LIB_SRCS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

$(BIN_DIR)/tetrisd: $(wildcard src/tetrisd/*.c) $(LIBS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrislogd: $(wildcard src/tetrislogd/*.c) $(LIBS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisctl: $(wildcard src/tetrisctl/*.c) $(LIBS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/tetrisu: $(wildcard src/tetrisu/*.c) $(LIBS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)/* $(LIB_DIR)/*.a src/*/*.o