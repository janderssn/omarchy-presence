# omarchy-presence: presence service and its command-line client.
#
#   make            build both programs
#   make check      build and run the tests
#   make install    install under $(DESTDIR)$(PREFIX)

PREFIX ?= /usr
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror=implicit-function-declaration
SYSTEMD_CFLAGS := $(shell pkg-config --cflags libsystemd)
SYSTEMD_LIBS := $(shell pkg-config --libs libsystemd)

DAEMON_SOURCES := src/presenced.c src/filter.c src/sensor.c src/privacy.c

all: build/omarchy-presenced build/omarchy-presence

build:
	mkdir -p build

build/omarchy-presenced: $(DAEMON_SOURCES) src/filter.h src/sensor.h src/privacy.h | build
	$(CC) $(CFLAGS) $(SYSTEMD_CFLAGS) -o $@ $(DAEMON_SOURCES) $(LDFLAGS) $(SYSTEMD_LIBS)

build/omarchy-presence: src/client.c | build
	$(CC) $(CFLAGS) $(SYSTEMD_CFLAGS) -o $@ src/client.c $(LDFLAGS) $(SYSTEMD_LIBS)

build/test_presence: tests/test_presence.c src/filter.c src/sensor.c src/filter.h src/sensor.h | build
	$(CC) $(CFLAGS) -UNDEBUG -fsanitize=address,undefined -o $@ tests/test_presence.c src/filter.c src/sensor.c

check: build/test_presence
	build/test_presence

install: all
	install -Dm755 build/omarchy-presenced $(DESTDIR)$(PREFIX)/lib/omarchy-presence/omarchy-presenced
	install -Dm755 build/omarchy-presence $(DESTDIR)$(PREFIX)/bin/omarchy-presence
	install -Dm644 data/omarchy-presence.service $(DESTDIR)$(PREFIX)/lib/systemd/system/omarchy-presence.service
	install -Dm644 data/org.omarchy.Presence1.conf $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/org.omarchy.Presence1.conf
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/omarchy-presence/LICENSE

clean:
	rm -rf build

.PHONY: all check install clean
