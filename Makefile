VERSION    ?= $(shell cat VERSION)

CC         ?= cc
CFLAGS     ?= -O2 -Wall -Wextra
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
MANDIR     ?= $(PREFIX)/share/man
SYSCONFDIR ?= /etc

# Build output and release archives
BUILD      ?= build
DIST       ?= dist

# blockfont: "shared" links the installed libblockfont (-lblockfont),
# "static" compiles blockfont from source into termtime. The source is
# BLOCKFONT_SRC, cloned at BLOCKFONT_REF when that directory is missing.
BLOCKFONT     ?= shared
BLOCKFONT_URL ?= https://github.com/TheElevatedOne/blockfont.h.git
BLOCKFONT_REF ?= 0.1.1
BLOCKFONT_SRC ?= $(BUILD)/blockfont-$(BLOCKFONT_REF)

# Architecture name used for packages, e.g. x86_64, aarch64, i686, armv7
ARCH       ?= $(shell $(CC) -dumpmachine | cut -d- -f1 | sed 's/^arm$$/armv7/')

CPPFLAGS   += -D_DEFAULT_SOURCE -DSYSCONFDIR='"$(SYSCONFDIR)"' \
              -DTERMTIME_VERSION='"$(VERSION)"' -Isrc

OBJDIR     := $(BUILD)/obj
BIN        := $(BUILD)/termtime
TEST_BIN   := $(BUILD)/unit_tests
LIB_OBJ    := $(OBJDIR)/config.o $(OBJDIR)/render.o $(OBJDIR)/toml.o
HEADERS    := src/config.h src/render.h src/toml.h

ifeq ($(BLOCKFONT),static)
  CPPFLAGS   += -I$(BLOCKFONT_SRC)
  LIB_OBJ    += $(OBJDIR)/blockfont.o
  BF_HEADER  := $(BLOCKFONT_SRC)/blockfont.h
else ifeq ($(BLOCKFONT),shared)
  LDLIBS     += -lblockfont
  BF_HEADER  :=
else
  $(error BLOCKFONT must be "shared" or "static")
endif

PKG_NAME   := termtime-$(VERSION)-$(ARCH)
PKG_DIR    := $(BUILD)/package/$(PKG_NAME)
PKG_FILE   := $(DIST)/$(PKG_NAME).tar.gz

.PHONY: all test test-unit test-cli test-tty install uninstall package clean distclean

all: $(BIN)

$(BIN): $(OBJDIR)/termtime.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c $(HEADERS) $(BF_HEADER) | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/blockfont.o: $(BF_HEADER) | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(BLOCKFONT_SRC)/blockfont.c

$(BLOCKFONT_SRC)/blockfont.h:
	git clone --quiet --depth 1 --branch $(BLOCKFONT_REF) $(BLOCKFONT_URL) $(BLOCKFONT_SRC)

$(OBJDIR):
	mkdir -p $@

$(TEST_BIN): tests/unit_tests.c $(LIB_OBJ) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit_tests.c \
		$(LIB_OBJ) $(LDLIBS)

test: test-unit test-cli test-tty

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

test-cli: $(BIN)
	sh tests/cli_tests.sh ./$(BIN)

test-tty: $(BIN)
	@if command -v python3 >/dev/null 2>&1; then \
		python3 -I tests/tty_tests.py ./$(BIN); \
	else \
		echo "python3 not found, skipping terminal tests"; \
	fi

# The config is only installed when none exists yet,
# so local changes to /etc/termtime/config.toml survive reinstalls.
# Packagers (DESTDIR set) always get the file.
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/termtime
	install -Dm644 termtime.1 $(DESTDIR)$(MANDIR)/man1/termtime.1
	@if [ -n "$(DESTDIR)" ] || [ ! -e $(SYSCONFDIR)/termtime/config.toml ]; then \
		echo install -Dm644 config/config.toml $(DESTDIR)$(SYSCONFDIR)/termtime/config.toml; \
		install -Dm644 config/config.toml $(DESTDIR)$(SYSCONFDIR)/termtime/config.toml; \
	else \
		echo "$(SYSCONFDIR)/termtime/config.toml exists, not overwriting"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/termtime $(DESTDIR)$(MANDIR)/man1/termtime.1
	@echo "Leaving $(DESTDIR)$(SYSCONFDIR)/termtime/config.toml in place"

# Release archive with blockfont built in, so it runs without libblockfont:
#   dist/termtime-VERSION-ARCH.tar.gz and its .sha256
package:
	$(MAKE) BLOCKFONT=static BUILD=$(BUILD)/static-$(ARCH) all
	rm -rf $(PKG_DIR)
	$(MAKE) BLOCKFONT=static BUILD=$(BUILD)/static-$(ARCH) \
		DESTDIR=$(abspath $(PKG_DIR)) PREFIX=/usr install
	install -Dm644 README.md $(PKG_DIR)/usr/share/doc/termtime/README.md
	install -Dm644 LICENSE $(PKG_DIR)/usr/share/licenses/termtime/LICENSE
	mkdir -p $(DIST)
	tar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime="@$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct 2>/dev/null || echo 0)}" \
		-czf $(PKG_FILE) -C $(BUILD)/package $(PKG_NAME)
	cd $(DIST) && sha256sum $(PKG_NAME).tar.gz > $(PKG_NAME).tar.gz.sha256
	@echo "Created $(PKG_FILE)"

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(DIST)
