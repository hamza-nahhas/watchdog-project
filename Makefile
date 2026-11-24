# Watchdog: POSIX signal-based process supervisor
# `make` builds ./watchdog and ./worker; `make test` runs the test scenarios.

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE=700 -Iinclude -g

SRC     = src/main.c src/signal_manager.c src/process_manager.c src/logger.c
OBJ     = $(SRC:src/%.c=build/%.o)

.PHONY: all test clean

all: watchdog worker

watchdog: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p build

worker: workers/worker.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	bash tests/test_scenarios.sh

clean:
	rm -rf build watchdog worker watchdog.log watchdog.pid

-include $(OBJ:.o=.d)
