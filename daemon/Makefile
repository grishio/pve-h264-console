# SPDX-License-Identifier: MIT
PKGS    = gio-2.0 gio-unix-2.0 libavcodec libavutil libswscale libswresample
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# optional: libva for printing the VA driver name in diagnostics
ifeq ($(shell pkg-config --exists libva && echo y),y)
PKGS   += libva
CFLAGS += -DHAVE_VA_VENDOR
endif
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -pthread
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread -lm
PREFIX ?= /usr/local

pve-h264d: pve-h264d.o encoder.o sps.o probe.o vdagent.o guestexec.o audio.o
	$(CC) -o $@ $^ $(LDLIBS)

%.o: %.c pve-h264d.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: pve-h264d
	install -D -m 0755 pve-h264d $(DESTDIR)$(PREFIX)/sbin/pve-h264d

clean:
	rm -f *.o pve-h264d
.PHONY: install clean
