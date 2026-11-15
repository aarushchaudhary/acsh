CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Iinclude
SRC     = src/main.c src/lexer.c src/parse.c src/exec.c src/expand.c src/builtins.c src/jobs.c src/signals.c src/env.c src/vars.c src/alias.c src/history.c src/functions.c src/subst.c src/test_builtin.c src/printf.c src/limits.c src/prompt.c
HDR     = include/acsh.h include/syntax.h src/lex.h
TARGET  = acsh

# make MINIMAL=1: no line editor, Tab completion or history suggestions
# (lines are read plainly, as ash without editing). Everything else,
# history and the prompt included, stays.
ifeq ($(MINIMAL),1)
CFLAGS += -DACSH_MINIMAL
else
SRC    += src/lineedit.c src/complete.c
endif

all: $(TARGET)

# rebuilt whenever the compiler or flags change (make MINIMAL=1 after make)
.build-flags: FORCE
	@echo '$(CC) $(CFLAGS) $(MINIMAL)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(MINIMAL)' > $@

$(TARGET): $(SRC) $(HDR) .build-flags
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# A self-contained binary (no shared libraries), built for size and
# stripped: the release build. Meant for musl: on Alpine plain
# `make static` (MINIMAL=1 works too); on a glibc system install
# musl-tools and run `make static CC=musl-gcc`.
# Not position-independent: a static PIE carries a relocation for every
# pointer in its tables and rewrites them at startup (bigger, and more
# pages touched); BusyBox's static build is not PIE either.
SIZEFLAGS = -Os -ffunction-sections -fdata-sections -Wl,--gc-sections \
            -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie -no-pie
static: acsh-static

acsh-static: $(SRC) $(HDR) .build-flags
	$(CC) $(CFLAGS) $(SIZEFLAGS) -static -s -o acsh-static $(SRC)

# libFuzzer targets (clang): the parser, and expansion with command
# substitution switched off. Run e.g. ./fuzz_parse -max_total_time=60 corpus/
FUZZFLAGS = -g -O1 -std=c11 -Iinclude -fsanitize=fuzzer,address,undefined -DACSH_FUZZ -Dmain=acsh_main
fuzz: fuzz_parse fuzz_expand

fuzz_parse: tests/fuzz/fuzz_parse.c $(SRC) $(HDR)
	clang $(FUZZFLAGS) -o fuzz_parse tests/fuzz/fuzz_parse.c $(SRC)

fuzz_expand: tests/fuzz/fuzz_expand.c $(SRC) $(HDR)
	clang $(FUZZFLAGS) -o fuzz_expand tests/fuzz/fuzz_expand.c $(SRC)

# The interactive tests (a C program driving acsh on a pseudo-terminal),
# and every suite: make check (MINIMAL=1 is passed on to the pty tests).
tests/pty/pty_test: tests/pty/pty_test.c
	$(CC) -O2 -Wall -Wextra -std=c11 -o tests/pty/pty_test tests/pty/pty_test.c

check: $(TARGET) tests/pty/pty_test
	./$(TARGET) test_acsh.sh
	tests/run_diff.sh ./$(TARGET)
	tests/pty/pty_test $(if $(filter 1,$(MINIMAL)),--minimal) ./$(TARGET)
	tests/bash_extras/run.sh ./$(TARGET)
	tests/realworld/run.sh ./$(TARGET)

# The community re-check on the release build: one row per complaint.
recheck: static
	tests/complaints.sh ./acsh-static $(if $(filter 1,$(MINIMAL)),--minimal)

clean:
	rm -f $(TARGET) acsh-static fuzz_parse fuzz_expand .build-flags tests/pty/pty_test

.PHONY: all static fuzz check recheck clean FORCE
