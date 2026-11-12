/* savecheck – zeigt, welche Rundumleuchte das Plugin für den aktuellen LKW erkennt.
 * Aufruf: savecheck [game.sii] [--plate KENNZEICHEN] [--truck vehicle.scania.r_2016] [--ats] */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../src/savegame.h"

int main(int argc, char **argv)
{
	const char *path = NULL, *game = "eut2";
	truck_hint_t hint = {NULL, NULL};
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--plate") && i + 1 < argc) hint.license_plate = argv[++i];
		else if (!strcmp(argv[i], "--truck") && i + 1 < argc) hint.truck_id = argv[++i];
		else if (!strcmp(argv[i], "--table") && i + 1 < argc) save_load_extra_table(argv[++i]);
		else if (!strcmp(argv[i], "--ats")) game = "ats";
		else path = argv[i];
	}
	char found[1024];
	if (!path) {
		time_t mt;
		if (save_find_latest(game, found, sizeof found, &mt)) {
			fprintf(stderr, "kein Spielstand gefunden\n");
			return 1;
		}
		path = found;
	}
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	beacon_info_t info;
	int rc = save_detect(path, &hint, &info);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("Spielstand: %s\n", path);
	printf("LKW:        %s (%s)\n", info.truck[0] ? info.truck : "-", info.plate[0] ? info.plate : "-");
	printf("Ergebnis:   %s – %s\n", info.mode == BEACON_BLINK ? "blink" : info.mode == BEACON_ROUND ? "round" : "unbekannt",
	       info.reason);
	printf("Dauer:      %.0f ms\n", (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
	return rc ? 1 : 0;
}
