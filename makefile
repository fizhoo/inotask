CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -fanalyzer -g
DEPFLAGS = -MMD -MP
LDFLAGS =
CFG ?= $(if $(wildcard inotaskd.cfg),inotaskd.cfg,inotaskd-sample.conf)
DESTDIR ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
CONFIGDIR ?= $(SYSCONFDIR)/inotask
SYSTEMD_UNIT_DIR ?= $(SYSCONFDIR)/systemd/system
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
TEST_BIN = tests/test_runtime
TEST_SRC = tests/test_runtime.c inotask_config.c inotask_runtime.c inotask_log.c

all: inotask

inotask: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) inotask $(TEST_BIN)

run: inotask
	./inotask $(CFG)

check: inotask
	./inotask --check $(CFG)

$(TEST_BIN): $(TEST_SRC) inotask_config.h inotask_runtime.h inotask_log.h
	$(CC) $(CFLAGS) -I. -o $@ $(TEST_SRC)

test: inotask $(TEST_BIN)
	./$(TEST_BIN)
	sh tests/integration.sh ./inotask

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

install: inotask
	install -D -m 0755 inotask $(DESTDIR)$(BINDIR)/inotask

install-config:
	install -D -m 0644 $(CFG) $(DESTDIR)$(CONFIGDIR)/inotaskd.cfg

install-systemd:
	install -D -m 0644 contrib/systemd/inotask.service \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/inotask.service

.PHONY: all clean run check test scan san live edit install install-config install-systemd

-include $(DEP)
