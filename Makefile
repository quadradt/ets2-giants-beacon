# ets2-giants-beacon – natives Linux-Telemetrie-Plugin für ETS2/ATS.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-format-truncation
GAME    ?= $(HOME)/.local/share/Steam/steamapps/common/Euro Truck Simulator 2
PLUGIN   = ets2_giants_beacon.so
OBJS     = build/ets2_beacon.o build/savegame.o
# Höchste erlaubte glibc-Symbolversion (ältere Steam-Runtimes)
MAX_GLIBC = 2.17

all: $(PLUGIN) savecheck

build/ets2_beacon.o: src/ets2_beacon.cpp src/savegame.h | build
	$(CC) -x c++ -std=gnu++17 -fno-exceptions -fno-rtti -fPIC $(CFLAGS) -Isdk -c -o $@ $<

build/savegame.o: src/savegame.c src/savegame.h src/beacon_table.h | build
	$(CC) -std=gnu11 -fPIC $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

$(PLUGIN): $(OBJS)
	$(CC) -shared -o $@ $(OBJS) -lpthread -ldl
	@./tools/check_symbols.sh $@ $(MAX_GLIBC)

savecheck: tools/savecheck.c src/savegame.c src/savegame.h src/beacon_table.h
	$(CC) -std=gnu11 $(CFLAGS) -Isrc -o $@ tools/savecheck.c src/savegame.c -ldl

install: $(PLUGIN)
	mkdir -p "$(GAME)/bin/linux_x64/plugins"
	install -m 755 $(PLUGIN) "$(GAME)/bin/linux_x64/plugins/"

uninstall:
	rm -f "$(GAME)/bin/linux_x64/plugins/$(PLUGIN)"

test: $(PLUGIN) savecheck tests/test_harness.c
	$(CC) -O0 -Wall -o build/test_harness tests/test_harness.c -ldl
	./tests/run_tests.sh

clean:
	rm -rf build $(PLUGIN) savecheck
.PHONY: all install uninstall test clean
