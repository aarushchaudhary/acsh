CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -Iinclude
SRC     = src/main.c src/lexer.c src/parse.c src/exec.c src/builtins.c src/jobs.c src/signals.c src/env.c src/alias.c src/history.c src/glob.c src/functions.c src/subst.c src/test_builtin.c
TARGET  = acsh

all: $(TARGET)

$(TARGET): $(SRC) include/acsh.h include/syntax.h src/lex.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
