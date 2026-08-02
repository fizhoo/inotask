CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -fanalyzer
DEPFLAGS = -MMD -MP
LDFLAGS =
CFG ?= inotaskd.cfg
LIVE_DELAY ?= 1
SCAN_BUILD ?= scan-build-19
SCAN_CC ?= clang-19
SCAN_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
SCAN_REPORTS ?= scan-build-reports
SAN_CC ?= gcc
SAN_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS ?= -fsanitize=address,undefined

SRC = inotask_main.c inotask_load.c inotask_parser.c inotask_lexer.c \
      inotask_config.c inotask_validate.c inotask_runtime.c inotask_log.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

all: inotask

inotask: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) inotask

run: inotask
	./inotask $(CFG)

check: inotask
	./inotask --check $(CFG)

scan: clean
	$(SCAN_BUILD) --status-bugs -o $(SCAN_REPORTS) \
		$(MAKE) CC=$(SCAN_CC) CFLAGS="$(SCAN_CFLAGS)"

san: clean
	$(MAKE) CC=$(SAN_CC) CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)"

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

.PHONY: all clean run check scan san live edit

-include $(DEP)
