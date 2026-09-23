CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -Iinclude
SRC     = src/main.c src/parser.c src/executor.c src/builtins.c src/jobs.c src/signals.c src/env.c src/alias.c src/history.c
TARGET  = acsh

all: $(TARGET)

$(TARGET): $(SRC) include/acsh.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
