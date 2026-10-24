CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_GNU_SOURCE -Isrc
JANSSON_CFLAGS ?= $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS ?= $(shell pkg-config --libs jansson 2>/dev/null)
CRYPTO_CFLAGS ?= $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS ?= $(shell pkg-config --libs libcrypto 2>/dev/null)
CFLAGS += -MMD -MP
LDFLAGS ?=
LDLIBS ?=
WITH_SYSTEMD ?= 1
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
localstatedir ?= /var

COMMON_OBJS = src/config.o src/log.o src/protocol.o src/util.o src/source_allowlist.o
LCSD_OBJS = src/lcsd.o src/vip.o src/cluster.o src/peer.o src/lease.o \
            src/resources.o src/group.o src/cli_server.o src/move.o src/metrics.o src/epoll_util.o src/config_reload.o \
            src/scheduler.o src/systemd_service.o src/resource_config.o src/config_store.o src/config_bootstrap.o \
            src/config_agreement.o src/config_agreement_worker.o src/config_transfer.o \
            $(COMMON_OBJS)
LCS_OBJS = src/lcs.o $(COMMON_OBJS)

.PHONY: all clean install check-json-deps

all: lcsd lcs

lcsd: $(LCSD_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCSD_OBJS) $(LDLIBS) $(JANSSON_LIBS) $(CRYPTO_LIBS)

lcs: $(LCS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCS_OBJS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/resource_config.o: CPPFLAGS += $(JANSSON_CFLAGS)
src/resource_config.o: | check-json-deps
src/config_agreement.o: CPPFLAGS += $(JANSSON_CFLAGS) $(CRYPTO_CFLAGS)
src/config_agreement.o: | check-json-deps

check-json-deps:
	@pkg-config --exists jansson libcrypto || { echo "JSON/hash development files required: install libjansson-dev and libssl-dev (Debian/Ubuntu), or jansson-devel and openssl-devel (RPM), plus pkg-config." >&2; exit 1; }

clean:
	rm -f lcs lcsd src/*.o src/*.d

install: all
	install -d $(DESTDIR)$(sbindir)
	install -d $(DESTDIR)$(bindir)
	install -d $(DESTDIR)$(mandir)/man8
	install -d $(DESTDIR)$(datadir)/lcs
	install -d -m 0700 $(DESTDIR)$(localstatedir)/lib/lcs
	install -m 0755 lcsd $(DESTDIR)$(sbindir)/lcsd
	install -m 0755 lcs $(DESTDIR)$(bindir)/lcs
	install -m 0644 packaging/lcs.8 $(DESTDIR)$(mandir)/man8/lcs.8
	install -m 0644 packaging/lcsd.8 $(DESTDIR)$(mandir)/man8/lcsd.8
	install -m 0644 packaging/lcs.conf $(DESTDIR)$(datadir)/lcs/lcs.conf.example

-include src/*.d
