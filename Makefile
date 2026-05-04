CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_GNU_SOURCE -Isrc
LDFLAGS ?=

COMMON_OBJS = src/config.o src/log.o src/protocol.o src/util.o
LCSD_OBJS = src/lcsd.o src/vip.o src/cluster.o src/peer.o src/lease.o \
            src/resources.o src/local_client.o src/metrics.o src/epoll_util.o \
            $(COMMON_OBJS)
LCS_OBJS = src/lcs.o $(COMMON_OBJS)

.PHONY: all clean install

all: lcsd lcs

lcsd: $(LCSD_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCSD_OBJS)

lcs: $(LCS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LCS_OBJS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f lcs lcsd src/*.o

install: all
	install -d $(DESTDIR)/usr/sbin
	install -d $(DESTDIR)/usr/bin
	install -m 0755 lcsd $(DESTDIR)/usr/sbin/lcsd
	install -m 0755 lcs $(DESTDIR)/usr/bin/lcs
