/*
 * savegame.h – erkennt, welche Rundumleuchten am aktuellen LKW verbaut sind.
 *
 * Liest den neuesten Spielstand (game.sii) von ETS2/ATS. Verschlüsselt (ScsC),
 * binär (BSII) und als Text (SiiN) werden unterstützt. Dann sucht es den LKW
 * des Spielers und schlägt dessen Zubehör in der Tabelle nach.
 */
#ifndef ETS2_BEACON_SAVEGAME_H
#define ETS2_BEACON_SAVEGAME_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BEACON_UNKNOWN = -1, BEACON_ROUND = 0, BEACON_BLINK = 1 };

typedef struct {
	/* Ergebnis */
	int mode;             /* BEACON_ROUND / BEACON_BLINK / BEACON_UNKNOWN */
	int n_round, n_blink; /* gefundene Leuchten (zur Info) */
	char truck[96];       /* z. B. "scania.r_2016" */
	char plate[64];       /* Kennzeichen laut Spielstand */
	char reason[200];     /* Kurzbeschreibung für das Log */
} beacon_info_t;

/* Hinweise aus der Telemetrie, um den richtigen LKW zu finden (dürfen leer sein). */
typedef struct {
	const char *truck_id;      /* Telemetrie-Attribut "id", z. B. "vehicle.scania.r_2016" */
	const char *license_plate; /* Telemetrie-Attribut "license_plate" */
} truck_hint_t;

/* Sucht den zuletzt geschriebenen game.sii. game_id: "eut2" oder "ats".
 * Liefert 0 bei Erfolg. */
int save_find_latest(const char *game_id, char *path, size_t n, time_t *mtime);

/* Lädt eine Zusatz-Tabelle "schlüssel<TAB>round|blink" (z. B. für Mods). */
void save_load_extra_table(const char *path);

/* Wertet einen Spielstand aus. Liefert 0, wenn er gelesen werden konnte. */
int save_detect(const char *path, const truck_hint_t *hint, beacon_info_t *out);

#ifdef __cplusplus
}
#endif
#endif
