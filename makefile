CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -fanalyzer
DEPFLAGS = -MMD -MP
LDFLAGS =
CFG ?= inotaskd.cfg
LIVE_DELAY ?= 1

OBJ = inotask_main.o inotask_load.o inotask_parser.o inotask_lexer.o \
      inotask_config.o inotask_validate.o inotask_runtime.o inotask_log.o

all: inotask

inotask: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) inotask

run: inotask
	./inotask $(CFG)

live: inotask
	while :; do \
		clear; \
		printf 'Live config view: %s\n' "$(CFG)"; \
		printf 'Refresh every %ss. Press Ctrl-C to stop.\n\n' "$(LIVE_DELAY)"; \
		./inotask "$(CFG)" || true; \
		sleep "$(LIVE_DELAY)"; \
	done

edit:
	nano $(CFG)

.PHONY: all clean run live edit

-include $(OBJ:.o=.d)
