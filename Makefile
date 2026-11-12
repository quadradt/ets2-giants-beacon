# Baut das native Linux-Telemetrie-Plugin für ETS2/ATS.
CFLAGS ?= -O2 -Wall -Wextra
GAME   ?= $(HOME)/.local/share/Steam/steamapps/common/Euro Truck Simulator 2

ets2_beacon.so: ets2_beacon.cpp
	$(CC) -x c++ $(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -fPIC -shared -Isdk -o $@ $< -lpthread

install: ets2_beacon.so
	mkdir -p "$(GAME)/bin/linux_x64/plugins"
	install -m 755 ets2_beacon.so "$(GAME)/bin/linux_x64/plugins/"

test: ets2_beacon.so test_harness.c
	$(CC) -O0 -Wall -Isdk -o test_harness test_harness.c -ldl
	./test_harness ./ets2_beacon.so

clean:
	rm -f ets2_beacon.so test_harness
.PHONY: install test clean
