CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Iinclude
SRC     = src/main.c src/lexer.c src/parse.c src/exec.c src/expand.c src/builtins.c src/jobs.c src/signals.c src/env.c src/vars.c src/alias.c src/history.c src/functions.c src/subst.c src/test_builtin.c src/printf.c
HDR     = include/acsh.h include/syntax.h src/lex.h
TARGET  = acsh

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# A self-contained binary (no shared libraries), size-optimized and
# stripped. Meant for musl: on Alpine plain `make static`; on a glibc
# system install musl-tools and run `make static CC=musl-gcc`.
static: acsh-static

acsh-static: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Os -static -s -o acsh-static $(SRC)

clean:
	rm -f $(TARGET) acsh-static

.PHONY: all static clean
