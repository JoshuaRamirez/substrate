# substrate -- single-file executables that are complete alone, and can join
# a shared-memory segment owned by one of them.
#
#   make               build everything this OS can build
#   make test          build, then run the whole suite
#   make install       into $(PREFIX); every binary is installed as "sub-*"
#   make uninstall     stop the owner, release the segment, remove the files
#   make help          the rest
#
# No package manager, no dependency tree, no third-party code. Every binary
# is one translation unit.

PREFIX     ?= /usr/local
DESTDIR    ?=
# make defines CC=cc itself, so "?=" would never fire. Only override the
# default; a CC from the environment or the command line still wins.
ifeq ($(origin CC),default)
CC          = clang
endif
EMDB_N     ?= 1000000

bindir      = $(DESTDIR)$(PREFIX)/bin
includedir  = $(DESTDIR)$(PREFIX)/include
INSTALL    ?= install

UNAME := $(shell uname -s)

WARN = -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Wformat=2 \
       -Wformat-security -Wvla -Wcast-qual -Wmissing-prototypes \
       -Wstrict-prototypes -Werror=implicit-function-declaration
HARD = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-common
OPT  ?= -O2
ALL_CFLAGS = $(OPT) $(WARN) $(HARD) -std=c11 $(CFLAGS)

# The embedded table is 40 MB of generated data, so it is never committed --
# it is made here and linked into a section of the binary. The two linkers
# disagree about how you do that, and about nothing else.
#
# -sectalign is not decoration: emdb_rec holds a uint64_t, and a section the
# linker placed on an odd address makes every read of it undefined behaviour.
# UBSan caught it; arm64 had been tolerating it silently.
ifeq ($(UNAME),Darwin)
  EMBED     = -Wl,-sectcreate,__TEXT,__emdb,db.blob \
              -Wl,-sectalign,__TEXT,__emdb,8
  EMBED_DEP = db.blob
  COCOA     = bin/gui bin/top
else
  EMBED     = bin/db.o
  EMBED_DEP = bin/db.o
  COCOA     =
endif

# Built and shipped.
PROGS  = bin/dbd bin/webd bin/look bin/layout bin/bench $(COCOA)
# Built, not shipped: fixtures that exist to be run by test.sh.
FIXTURES = bin/mkdb bin/storm bin/wedge bin/hog bin/blobbench bin/peer

ifneq ($(shell command -v swiftc 2>/dev/null),)
  SWIFT = bin/swiftpeer
else
  SWIFT =
endif

.PHONY: all test check san run clean distclean install uninstall purge stop help

all: $(PROGS) $(FIXTURES) $(SWIFT)
	@rm -f bin/.sanitized
	@echo "built:"
	@ls -lh $(PROGS) $(SWIFT) 2>/dev/null | awk '{printf "  %-16s %s\n", $$9, $$5}'

bin:
	@mkdir -p bin

bin/mkdb: mkdb.c | bin
	$(CC) -O2 -std=c11 $< -o $@

db.blob: bin/mkdb
	./bin/mkdb $(EMDB_N) $@

# GNU ld names the symbols after the input path, so db.blob must be named
# exactly that here -- emdb.h looks for _binary_db_blob_start.
bin/db.o: db.blob | bin
	ld -r -b binary -o $@ db.blob

bin/dbd:       dbd.c       substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/layout:    layout.c    substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/bench:     bench.c     substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/storm:     storm.c     substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/wedge:     wedge.c     substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/hog:       hog.c       substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/peer:      peer.c      substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@
bin/blobbench: blobbench.c substrate.h | bin ; $(CC) $(ALL_CFLAGS) $< -o $@

bin/webd: webd.c substrate.h emdb.h $(EMBED_DEP) | bin
	$(CC) $(ALL_CFLAGS) $< -o $@ $(EMBED)
bin/look: look.c substrate.h emdb.h $(EMBED_DEP) | bin
	$(CC) $(ALL_CFLAGS) $< -o $@ $(EMBED)

# Cocoa, so macOS only. Not a webview, no HTML, no toolkit to install.
bin/gui: gui.m substrate.h | bin
	$(CC) $(OPT) -Wall -Wextra -Wshadow $(HARD) -fobjc-arc -framework Cocoa $< -o $@
bin/top: top.m substrate.h | bin
	$(CC) $(OPT) -Wall -Wextra -Wshadow $(HARD) -fobjc-arc -framework Cocoa $< -o $@

# A second compiled toolchain that shares nothing with the above but the format.
bin/swiftpeer: swiftpeer.swift | bin
	@swiftc -O $< -o $@ 2>/dev/null || echo "  (swiftpeer skipped: swiftc failed)"

# The marker lets test.sh know the binaries are instrumented. ASan makes the
# owner roughly an order of magnitude slower, and the call timeout is measured
# on a wall clock -- so at high client counts, timeouts here are the system
# working, not failing. test.sh checks the weaker (and correct) invariant.
san:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory \
	   OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" all
	@touch bin/.sanitized
	@echo "  (instrumented: bin/.sanitized)"

test: all
	./test.sh
check: test

run: all
	./run.sh

clean:
	rm -rf bin
distclean: clean
	rm -f db.blob substrate.db

# ---------------------------------------------------------------- install --
#
# Installed under a "sub-" prefix, and not out of tidiness: this repo builds
# programs called `top` and `look`, and both of those are real commands that
# already live on the PATH. Installing them bare would shadow them.
INSTALLED = sub-dbd sub-webd sub-look sub-layout sub-bench

install: all
	$(INSTALL) -d $(bindir) $(includedir)
	$(INSTALL) -m 755 bin/dbd    $(bindir)/sub-dbd
	$(INSTALL) -m 755 bin/webd   $(bindir)/sub-webd
	$(INSTALL) -m 755 bin/look   $(bindir)/sub-look
	$(INSTALL) -m 755 bin/layout $(bindir)/sub-layout
	$(INSTALL) -m 755 bin/bench  $(bindir)/sub-bench
	$(INSTALL) -m 755 pypeer.py  $(bindir)/sub-pypeer
	@[ -x bin/gui ]       && $(INSTALL) -m 755 bin/gui       $(bindir)/sub-gui       || true
	@[ -x bin/top ]       && $(INSTALL) -m 755 bin/top       $(bindir)/sub-top       || true
	@[ -x bin/swiftpeer ] && $(INSTALL) -m 755 bin/swiftpeer $(bindir)/sub-swiftpeer || true
	$(INSTALL) -m 644 substrate.h $(includedir)/substrate.h
	@echo
	@echo "installed into $(bindir):"
	@ls $(bindir) | grep '^sub-' | sed 's/^/  /'
	@echo "  $(includedir)/substrate.h"
	@echo
	@echo "start it:  sub-dbd &  then  sub-webd --join"
	@echo "stop it:   sub-dbd --stop"

# Stop the owner without removing anything. Its own exit path is the only one
# that persists the count and releases the segment name.
stop:
	@if command -v sub-dbd >/dev/null 2>&1; then sub-dbd --stop; \
	 elif [ -x bin/dbd ]; then ./bin/dbd --stop; \
	 else echo "nothing to stop: no dbd built or installed"; fi

# Uninstall is not just rm. A running owner holds a shared-memory object whose
# NAME outlives every process that mapped it -- remove the binaries only, and
# /sub.v8 sits on the machine until reboot. So: stop, release, then delete.
#
# The database file is the user's data, so it survives. `make purge` takes it.
uninstall:
	-@$(MAKE) --no-print-directory stop
	-@if [ -x $(bindir)/sub-dbd ]; then $(bindir)/sub-dbd --unlink; \
	  elif [ -x bin/dbd ]; then ./bin/dbd --unlink; fi
	rm -f $(bindir)/sub-dbd $(bindir)/sub-webd $(bindir)/sub-look \
	      $(bindir)/sub-layout $(bindir)/sub-bench $(bindir)/sub-pypeer \
	      $(bindir)/sub-gui $(bindir)/sub-top $(bindir)/sub-swiftpeer
	rm -f $(includedir)/substrate.h
	@echo "uninstalled from $(bindir)"
	@echo "the database file was left alone -- 'make purge' removes it too"

purge: uninstall
	rm -f substrate.db db.blob
	@echo "removed substrate.db and db.blob"

help:
	@echo 'substrate'
	@echo
	@echo '  make              build everything this OS can build'
	@echo '  make test         build, then run the whole suite'
	@echo '  make san          rebuild under ASan + UBSan'
	@echo '  make run          the live demo: owner, web, gui, dashboard'
	@echo
	@echo '  make install      into $$(PREFIX), as sub-dbd, sub-webd, ...'
	@echo '  make uninstall    stop the owner, release /sub.v8, remove the files'
	@echo '  make purge        uninstall, and delete the database too'
	@echo '  make stop         stop the owner, change nothing else'
	@echo
	@echo '  make clean        remove bin/'
	@echo '  make distclean    also remove db.blob and substrate.db'
	@echo
	@echo '  PREFIX=$(PREFIX)   DESTDIR=$(DESTDIR)   CC=$(CC)   EMDB_N=$(EMDB_N)'
