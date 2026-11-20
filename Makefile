# The code shared by the pac tools (config_file, setting, ...)
PACOMMON ?= pacommon
ifeq ($(wildcard $(PACOMMON)/pac.mk),)
$(error pacommon is not at $(PACOMMON): run: git submodule update --init, or set PACOMMON=path/to/pacommon)
endif
include $(PACOMMON)/pac.mk

CC ?= gcc

# SDBUS=0 builds without the print and Bluetooth watchers: no libsystemd (sd-bus).
SDBUS ?= 1

PKGS =
ifeq ($(SDBUS),1)
PKGS += libsystemd
endif

CFLAGS ?= -Os -fno-plt -Wall -Wextra -Wshadow -Wvla -Wformat-security
LDFLAGS ?= -s
# Hardening: fortified libc calls, stack protector, PIE, full RELRO and immediate binding.
# Control-flow protection only exists on x86 compilers, so ask the compiler.
CET_CFLAGS := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fcf-protection=full \
                -o /dev/null 2>/dev/null && echo -fcf-protection=full)
ALL_CFLAGS := $(CFLAGS) -DSYSCONFDIR='"$(SYSCONFDIR)"' -ffunction-sections -fdata-sections -std=gnu11 -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong \
              -fstack-clash-protection $(CET_CFLAGS) -fPIE -MMD -MP -Isrc -I$(PACOMMON)/src $(if $(PKGS),$(shell pkg-config --cflags $(PKGS)))
ALL_LDFLAGS := $(LDFLAGS) -Wl,--as-needed -Wl,--gc-sections -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
ALL_LIBS := $(LDLIBS) $(if $(PKGS),$(shell pkg-config --libs $(PKGS)))

SRCS = $(wildcard src/*.c)
ifneq ($(SDBUS),1)
SRCS := $(filter-out src/src_print.c src/src_bt.c src/src_wifi.c,$(SRCS))
ALL_CFLAGS += -DNO_SDBUS
endif
COMMON_SRCS = $(PACOMMON)/src/config_file.c $(PACOMMON)/src/length.c $(PACOMMON)/src/setting.c
COMMON_OBJS = $(COMMON_SRCS:$(PACOMMON)/src/%.c=build/pacommon_%.o)
OBJS = $(SRCS:src/%.c=build/%.o) $(COMMON_OBJS)
TARGET = pacreactd

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
UNITDIR ?= $(PREFIX)/lib/systemd/user

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(ALL_LDFLAGS) $^ -o $@ $(ALL_LIBS)

build/pacommon_%.o: $(PACOMMON)/src/%.c | build
	$(CC) $(ALL_CFLAGS) -DPACOMMON_I18N='"i18n.h"' -c $< -o $@

build/%.o: src/%.c | build
	$(CC) $(ALL_CFLAGS) -c $< -o $@

build:
	mkdir -p build

# The tests build the logic on its own, with stubs for the loop. No display, no running daemon.
TEST_CC = $(CC) $(CFLAGS) -D_GNU_SOURCE -Wall -Wextra -Isrc -I$(PACOMMON)/src
TESTS = state server event input ipc temp power mem net jack audio camera usb backlight locks disk probe
ifeq ($(SDBUS),1)
TESTS += print bt wifi
endif
TEST_SRC_state = src/state.c
TEST_SRC_server = src/server.c src/state.c src/event.c
TEST_SRC_temp = src/util.c $(PACOMMON)/src/setting.c
TEST_SRC_power =
TEST_SRC_mem =
TEST_SRC_net = $(PACOMMON)/src/setting.c src/util.c
TEST_SRC_jack = src/evdev.c $(PACOMMON)/src/setting.c src/state.c src/event.c
TEST_SRC_audio = $(PACOMMON)/src/setting.c src/state.c src/event.c
TEST_SRC_event = src/event.c src/state.c
TEST_SRC_ipc = src/ipc.c
TEST_SRC_camera = $(PACOMMON)/src/setting.c src/state.c src/event.c src/util.c
TEST_SRC_input = $(PACOMMON)/src/setting.c src/state.c src/event.c src/util.c
TEST_SRC_usb = $(PACOMMON)/src/setting.c src/state.c src/event.c src/util.c
TEST_LIBS_usb = $(shell pkg-config --libs libsystemd 2>/dev/null)
TEST_SRC_disk = $(PACOMMON)/src/setting.c src/util.c src/state.c
TEST_SRC_probe = src/src_probe.c src/state.c src/util.c $(PACOMMON)/src/setting.c
TEST_SRC_locks = src/evdev.c $(PACOMMON)/src/setting.c
TEST_SRC_backlight =
TEST_SRC_bt = $(PACOMMON)/src/setting.c src/state.c src/event.c
TEST_SRC_wifi = $(PACOMMON)/src/setting.c
TEST_LIBS_wifi = $(shell pkg-config --libs libsystemd 2>/dev/null)
TEST_LIBS_bt = $(shell pkg-config --libs libsystemd 2>/dev/null)
TEST_SRC_print = $(PACOMMON)/src/setting.c src/state.c src/event.c
TEST_LIBS_print = $(shell pkg-config --libs libsystemd 2>/dev/null)

test: $(TESTS:%=build/test_%)
	@for t in $^; do ./$$t || exit 1; done

build/test_%: tests/test_%.c $(wildcard src/*.c src/*.h tests/*.h) | build
	$(TEST_CC) tests/test_$*.c $(TEST_SRC_$*) -o $@ $(TEST_LIBS_$*)

clean:
	rm -rf build $(TARGET)

install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 pacreactd.conf $(DESTDIR)$(PREFIX)/share/doc/pacreactd/pacreactd.conf
	install -Dm644 docs/protocol.md $(DESTDIR)$(PREFIX)/share/doc/pacreactd/protocol.md
	$(call pac_install_keep,pacreactd.conf,$(DESTDIR)$(SYSCONFDIR)/pac/pacreactd.conf,644)
	$(pac_install_shared_conf)
	install -Dm644 docs/pacreactd.1 $(DESTDIR)$(PREFIX)/share/man/man1/pacreactd.1
	sed 's|@BINDIR@|$(BINDIR)|' contrib/pacreactd.service.in > build/pacreactd.service
	$(call pac_install_keep,build/pacreactd.service,$(DESTDIR)$(UNITDIR)/pacreactd.service,644)
	$(call pac_install_keep,contrib/pacreactd.socket,$(DESTDIR)$(UNITDIR)/pacreactd.socket,644)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(PREFIX)/share/doc/pacreactd/pacreactd.conf $(DESTDIR)$(PREFIX)/share/doc/pacreactd/protocol.md
	$(call pac_uninstall_keep,pacreactd.conf,$(DESTDIR)$(SYSCONFDIR)/pac/pacreactd.conf)
	$(call pac_uninstall_keep,build/pacreactd.service,$(DESTDIR)$(UNITDIR)/pacreactd.service)
	$(call pac_uninstall_keep,contrib/pacreactd.socket,$(DESTDIR)$(UNITDIR)/pacreactd.socket)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/pacreactd.1

-include $(wildcard build/*.d)

# The tests again under AddressSanitizer and UBSan.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-g -fsanitize=address,undefined $(CFLAGS)" LDFLAGS="-fsanitize=address,undefined $(LDFLAGS)"

.PHONY: sanitize all clean install uninstall test
