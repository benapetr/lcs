CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_GNU_SOURCE -Isrc
CFLAGS += -MMD -MP
LDFLAGS ?=
LDLIBS ?=
WITH_SYSTEMD ?= 0
ifeq ($(WITH_SYSTEMD),1)
CPPFLAGS += -DHAVE_SYSTEMD $(shell pkg-config --cflags libsystemd)
LDLIBS += $(shell pkg-config --libs libsystemd)
endif
prefix ?= /usr
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
sbindir ?= $(exec_prefix)/sbin
mandir ?= $(prefix)/share/man
datadir ?= $(prefix)/share

COMMON_OBJS = src/config.o src/log.o src/protocol.o src/util.o
LCSD_OBJS = src/lcsd.o src/vip.o src/cluster.o src/peer.o src/lease.o \
            src/resources.o src/group.o src/local_client.o src/move.o src/metrics.o src/epoll_util.o \
            src/scheduler.o src/systemd_service.o \
            $(COMMON_OBJS)
LCS_OBJS = src/lcs.o $(COMMON_OBJS)

.PHONY: all clean install

all: lcsd lcs

lcsd: $(LCSD_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCSD_OBJS) $(LDLIBS)

lcs: $(LCS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCS_OBJS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f lcs lcsd src/*.o src/*.d

install: all
	install -d $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(mandir)/man8
	install -d $(DESTDIR)$(datadir)/lcs
	install -m 0755 lcsd $(DESTDIR)$(sbindir)/lcsd
	install -m 0755 lcs $(DESTDIR)$(bindir)/lcs
	install -m 0644 packaging/lcs.8 $(DESTDIR)$(mandir)/man8/lcs.8
	install -m 0644 packaging/lcsd.8 $(DESTDIR)$(mandir)/man8/lcsd.8
	install -m 0644 packaging/lcs.conf $(DESTDIR)$(datadir)/lcs/lcs.conf.example

-include src/*.d
