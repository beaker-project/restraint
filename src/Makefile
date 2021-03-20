SHELL = /bin/bash
CC = gcc

LIBS =
PACKAGES =
PROGRAMS =

PACKAGES += gio-2.0
PACKAGES += gio-unix-2.0
PACKAGES += glib-2.0
PACKAGES += gobject-2.0
PACKAGES += json-c
PACKAGES += libarchive
PACKAGES += libcurl
PACKAGES += libsoup-2.4
PACKAGES += libxml-2.0

PROGRAMS += restraint
PROGRAMS += restraintd
PROGRAMS += rstrnt-abort
PROGRAMS += rstrnt-adjust-watchdog
PROGRAMS += rstrnt-report-log
PROGRAMS += rstrnt-report-result
PROGRAMS += rstrnt-sync

CFLAGS ?= -g

ifneq ($(DEVEL), 1)
    CFLAGS += -O2
endif

CFLAGS += -D_GNU_SOURCE -Wall -std=c99 $(shell pkg-config --cflags $(PACKAGES))

ifeq ($(STATIC), 1)
    # The right thing to do here is `pkg-config --libs --static`, which would
    # include Libs.private in the link command.
    # But really old pkg-config versions don't understand that so let's just
    # hardcode the "private" libs here.
    # The -( -) grouping means we don't have to worry about getting all the
    # dependent libs in the right order (normally pkg-config would do that for
    # us).
    LIBS = -Wl,-Bstatic -Wl,-\( $(shell pkg-config --libs $(PACKAGES)) -lgmodule-2.0 -llzma -lbz2 -lz -lffi -lssl -lcrypto -lselinux -Wl,-\) -Wl,-Bdynamic -lm -pthread -lrt -lresolv -ldl -lutil $(LFLAGS)
else
    LIBS = $(shell pkg-config --libs $(PACKAGES) $(XTRAPKGS)) -lutil -pthread
endif

.PHONY: all
all: $(PROGRAMS)

rstrnt-report-result: cmd_result.o cmd_result_main.o upload.o utils.o cmd_utils.o errors.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

rstrnt-report-log: cmd_log.o cmd_log_main.o upload.o utils.o cmd_utils.o errors.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

rstrnt-adjust-watchdog: cmd_watchdog.o cmd_watchdog_main.o utils.o cmd_utils.o errors.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

rstrnt-abort: cmd_abort.o cmd_abort_main.o utils.o cmd_utils.o errors.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

rstrnt-sync: cmd_sync.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

restraint: client.o errors.o xml.o utils.o process.o restraint_forkpty.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

restraintd: server.o recipe.o task.o fetch.o fetch_git.o fetch_uri.o param.o role.o metadata.o process.o message.o dependency.o utils.o config.o errors.o xml.o env.o restraint_forkpty.o beaker_harness.o logging.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

fetch_git.o: fetch.h fetch_git.h
fetch_uri.o: fetch.h fetch_uri.h
task.o: task.h param.h role.h metadata.h process.h message.h dependency.h config.h errors.h fetch_git.h fetch_uri.h utils.h env.h xml.h
recipe.o: recipe.h param.h role.h task.h metadata.h utils.h config.h xml.h
param.o: param.h
role.o: role.h
server.o: recipe.h task.h server.h
expect_http.o: expect_http.h
role.o: role.h
client.o: client.h
multipart.o: multipart.h
process.o: process.h
message.o: message.h
dependency.o: dependency.h
utils.o: utils.h
config.o: config.h
errors.o: errors.h
xml.o: xml.h
restraint_forkpty.o:
beaker_harness.o:
logging.o: logging.c logging.h task.h

.PHONY: check valgrind
check valgrind:
	make -C ../tests $@

.PHONY: install
install: all
	install -m0755 -d $(DESTDIR)/usr/bin
	install -m0755 $(PROGRAMS) $(DESTDIR)/usr/bin

.PHONY: clean
clean:
	rm -f $(PROGRAMS) *.o
	make -C ../tests clean
