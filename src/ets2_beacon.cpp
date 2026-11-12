/*
 * ets2-giants-beacon – SCS-Telemetrie-Plugin (Linux) für die GIANTS-USB-Rundumleuchte.
 *
 * Spiegelt den Kanal "truck.light.beacon" aus Euro Truck Simulator 2 /
 * American Truck Simulator auf alle angeschlossenen GIANTS-Beacons
 * (USB 340d:1710). Das Plugin schreibt selbst auf /dev/hidrawN, ein
 * Hintergrunddienst ist nicht nötig.
 *
 * Modus "auto": Das Plugin liest den Spielstand und erkennt, ob am LKW
 * drehende Rundumleuchten (-> round) oder LED-Blitzer (-> blink) verbaut sind.
 *
 * Konfiguration: ~/.config/ets2-beacon/config
 *   mode=auto|round|blink   (Standard auto)
 *   fallback=round|blink    (wenn auto nichts erkennt, Standard round)
 *
 * (C++ nur, weil die SCS-SDK-Header C++ verlangen; der Code ist schlichtes C.)
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

#include "savegame.h"

/* Alte Symbolversionen erzwingen, damit das Plugin auch in älteren Steam-
 * Runtimes (glibc < 2.34) lädt. Moderne glibc liefert diese weiterhin mit. */
__asm__(".symver pthread_create,pthread_create@GLIBC_2.2.5");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.2.5");

#define MAX_DEV 8
#define RESEND_SEC 3.0      /* Hardware schaltet nach 10 s von selbst ab */
#define RESCAN_SEC 5.0      /* Hotplug: Geräte regelmäßig neu suchen */
#define SAVECHECK_SEC 30.0  /* bei laufender Leuchte: neuer Spielstand? */

/* Gleiche Payloads wie giants-beacon (Report-ID 0 + 9 Byte). */
static const unsigned char P_OFF[10]   = {0x00, 0xFF, 0x00, 0x00, 0x64, 0x00, 0x32, 0x9E, 0xD7, 0x0D};
static const unsigned char P_BLINK[10] = {0x00, 0xFF, 0x07, 0xFF, 0x64, 0xFF, 0xEB, 0x7D, 0x9A, 0x03};
static const unsigned char P_ROUND[10] = {0x00, 0xFF, 0x01, 0x66, 0xC8, 0xFF, 0xAD, 0x52, 0x81, 0xD6};

enum { CFG_AUTO = -1 };

static scs_log_t game_log = NULL;
static pthread_t worker;
static int worker_started = 0;
static volatile int running = 0;
static volatile int beacon_on = 0;       /* vom Spiel-Thread gesetzt */
static volatile int need_detect = 1;     /* LKW-Wechsel o. ä. -> Spielstand neu lesen */
static int cfg_mode = CFG_AUTO;          /* CFG_AUTO, BEACON_ROUND, BEACON_BLINK */
static int cfg_fallback = BEACON_ROUND;
static volatile int detected = BEACON_UNKNOWN;
static char game_id[16] = "eut2";

/* Telemetrie-Daten des aktuellen LKW (geschützt durch hint_lock) */
static pthread_mutex_t hint_lock = PTHREAD_MUTEX_INITIALIZER;
static char hint_truck[96], hint_plate[64];

static int devs[MAX_DEV];
static int ndev = 0;

/* ---------- Log (nur im Spiel-Thread an die SDK-API weitergeben) ---------- */

#define LOGQ 16
static pthread_mutex_t logq_lock = PTHREAD_MUTEX_INITIALIZER;
static char logq_msg[LOGQ][240];
static scs_log_type_t logq_type[LOGQ];
static int logq_n = 0;

static void logf_(scs_log_type_t type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(scs_log_type_t type, const char *fmt, ...)
{
	pthread_mutex_lock(&logq_lock);
	if (logq_n < LOGQ) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(logq_msg[logq_n], sizeof logq_msg[0], fmt, ap);
		va_end(ap);
		logq_type[logq_n++] = type;
	}
	pthread_mutex_unlock(&logq_lock);
}

static void flush_log(void)
{
	pthread_mutex_lock(&logq_lock);
	for (int i = 0; i < logq_n; i++)
		if (game_log)
			game_log(logq_type[i], logq_msg[i]);
	logq_n = 0;
	pthread_mutex_unlock(&logq_lock);
}

/* ---------- USB ---------- */

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void close_devs(void)
{
	for (int i = 0; i < ndev; i++)
		close(devs[i]);
	ndev = 0;
}

/* Sucht /sys/class/hidraw/hidrawN mit HID_ID ...:0000340D:00001710. */
static void scan_devs(void)
{
	close_devs();
	DIR *d = opendir("/sys/class/hidraw");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && ndev < MAX_DEV) {
		if (strncmp(e->d_name, "hidraw", 6) != 0)
			continue;
		char path[300], line[128];
		snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		int match = 0;
		while (fgets(line, sizeof line, f))
			if (strncmp(line, "HID_ID=", 7) == 0 && strstr(line, ":0000340D:00001710"))
				match = 1;
		fclose(f);
		if (!match)
			continue;
		snprintf(path, sizeof path, "/dev/%s", e->d_name);
		int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0)
			devs[ndev++] = fd;
		else
			logf_(SCS_LOG_TYPE_warning, "[ets2-beacon] %s nicht oeffenbar: %s (udev-Regel installiert?)",
			      path, strerror(errno));
	}
	closedir(d);
}

/* Liefert die Zahl erfolgreich beschriebener Geräte. */
static int send_all(const unsigned char *p)
{
	int ok = 0;
	for (int i = 0; i < ndev; i++)
		if (write(devs[i], p, 10) == 10)
			ok++;
		else if (errno != EAGAIN) {
			/* Gerät abgezogen o. ä. – beim nächsten Rescan neu suchen */
			close(devs[i]);
			devs[i] = devs[--ndev];
			i--;
		}
	return ok;
}

/* ---------- Erkennung ---------- */

static int effective_mode(void)
{
	if (cfg_mode != CFG_AUTO)
		return cfg_mode;
	return detected != BEACON_UNKNOWN ? detected : cfg_fallback;
}

static const char *mode_name(int m)
{
	return m == BEACON_BLINK ? "blink" : m == BEACON_ROUND ? "round" : "unbekannt";
}

/* Liest den neuesten Spielstand, wenn sich Datei oder LKW geändert haben. */
static void detect(int force)
{
	static char last_path[1024];
	static time_t last_mt = 0;
	static char last_hint[200];
	char path[1024], hint_key[200], truck[96], plate[64];
	time_t mt = 0;

	if (save_find_latest(game_id, path, sizeof path, &mt)) {
		if (force)
			logf_(SCS_LOG_TYPE_message, "[ets2-beacon] kein Spielstand gefunden, Modus %s",
			      mode_name(effective_mode()));
		return;
	}
	pthread_mutex_lock(&hint_lock);
	snprintf(truck, sizeof truck, "%s", hint_truck);
	snprintf(plate, sizeof plate, "%s", hint_plate);
	pthread_mutex_unlock(&hint_lock);
	snprintf(hint_key, sizeof hint_key, "%s|%s", truck, plate);

	if (!force && mt == last_mt && !strcmp(path, last_path) && !strcmp(hint_key, last_hint))
		return;
	snprintf(last_path, sizeof last_path, "%s", path);
	last_mt = mt;
	snprintf(last_hint, sizeof last_hint, "%s", hint_key);

	truck_hint_t hint = {truck[0] ? truck : NULL, plate[0] ? plate : NULL};
	beacon_info_t info;
	double t0 = now_sec();
	int rc = save_detect(path, &hint, &info);
	int ms = (int)((now_sec() - t0) * 1000);
	if (rc) {
		logf_(SCS_LOG_TYPE_warning, "[ets2-beacon] Spielstand nicht auswertbar (%s), Modus %s",
		      info.reason, mode_name(effective_mode()));
		return;
	}
	detected = info.mode;
	logf_(SCS_LOG_TYPE_message, "[ets2-beacon] LKW %s: %s (%s, %d ms) -> Modus %s",
	      info.truck[0] ? info.truck : "?", mode_name(info.mode), info.reason, ms,
	      mode_name(effective_mode()));
}

static void *worker_main(void *arg)
{
	(void)arg;
	int last = -1, last_ndev = -1, last_mode = -2;
	double last_send = 0, last_scan = 0, last_savecheck = 0;
	int was_on = 0;

	while (running) {
		double t = now_sec();
		int on = beacon_on;

		if (t - last_scan >= RESCAN_SEC) {
			scan_devs();
			last_scan = t;
			if (ndev != last_ndev) {
				logf_(SCS_LOG_TYPE_message, "[ets2-beacon] %d Rundumleuchte(n) gefunden", ndev);
				last_ndev = ndev;
				last = -1; /* neuen Geräten Zustand sofort schicken */
			}
		}

		if (cfg_mode == CFG_AUTO) {
			/* Erkennung: beim Start/LKW-Wechsel, beim Einschalten und gelegentlich danach */
			int force = need_detect;
			if (force || (on && !was_on) || (on && t - last_savecheck >= SAVECHECK_SEC)) {
				need_detect = 0;
				detect(force);
				last_savecheck = now_sec();
			}
		}
		was_on = on;

		int mode = effective_mode();
		if (on != last || mode != last_mode || (on && now_sec() - last_send >= RESEND_SEC)) {
			int ok = send_all(on ? (mode == BEACON_BLINK ? P_BLINK : P_ROUND) : P_OFF);
			if (on != last || (on && mode != last_mode))
				logf_(SCS_LOG_TYPE_message, "[ets2-beacon] Rundumleuchte %s (%d/%d Geraete)",
				      on ? mode_name(mode) : "AUS", ok, ndev);
			last = on;
			last_mode = mode;
			last_send = now_sec();
		}
		usleep(100 * 1000);
	}
	send_all(P_OFF);
	close_devs();
	return NULL;
}

/* ---------- Konfiguration ---------- */

static int parse_mode(const char *v, int allow_auto)
{
	if (!strncmp(v, "blink", 5))
		return BEACON_BLINK;
	if (!strncmp(v, "round", 5))
		return BEACON_ROUND;
	if (allow_auto && !strncmp(v, "auto", 4))
		return CFG_AUTO;
	return -2;
}

static void load_config(void)
{
	char dir[512], path[600];
	const char *cfg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (cfg && *cfg)
		snprintf(dir, sizeof dir, "%s/ets2-beacon", cfg);
	else
		snprintf(dir, sizeof dir, "%s/.config/ets2-beacon", home ? home : "");

	snprintf(path, sizeof path, "%s/config", dir);
	FILE *f = fopen(path, "r");
	if (f) {
		char line[128];
		while (fgets(line, sizeof line, f)) {
			int m;
			if (!strncmp(line, "mode=", 5) && (m = parse_mode(line + 5, 1)) != -2)
				cfg_mode = m;
			else if (!strncmp(line, "fallback=", 9) && (m = parse_mode(line + 9, 0)) != -2)
				cfg_fallback = m;
		}
		fclose(f);
	}
	const char *env = getenv("ETS2_BEACON_MODE");
	int m;
	if (env && (m = parse_mode(env, 1)) != -2)
		cfg_mode = m;

	snprintf(path, sizeof path, "%s/beacons.tsv", dir);   /* eigene Tabelle (Mods) */
	save_load_extra_table(path);
}

/* ---------- SDK-Rückrufe ---------- */

static SCSAPI_VOID on_beacon(const scs_string_t name, const scs_u32_t index,
                             const scs_value_t *const value, const scs_context_t context)
{
	(void)name; (void)index; (void)context;
	beacon_on = (value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value) ? 1 : 0;
}

static SCSAPI_VOID on_frame_end(const scs_event_t event, const void *const event_info,
                                const scs_context_t context)
{
	(void)event; (void)event_info; (void)context;
	flush_log();
}

/* Konfiguration "truck": Modell-ID und Kennzeichen merken (LKW-Wechsel erkennen). */
static SCSAPI_VOID on_config(const scs_event_t event, const void *const event_info,
                             const scs_context_t context)
{
	(void)event; (void)context;
	const scs_telemetry_configuration_t *cfg = (const scs_telemetry_configuration_t *)event_info;
	if (!cfg || !cfg->id || strcmp(cfg->id, SCS_TELEMETRY_CONFIG_truck) != 0)
		return;
	char truck[96] = "", plate[64] = "";
	for (const scs_named_value_t *a = cfg->attributes; a && a->name; a++) {
		if (a->value.type != SCS_VALUE_TYPE_string || !a->value.value_string.value)
			continue;
		if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id))
			snprintf(truck, sizeof truck, "%s", a->value.value_string.value);
		else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate))
			snprintf(plate, sizeof plate, "%s", a->value.value_string.value);
	}
	pthread_mutex_lock(&hint_lock);
	int changed = strcmp(truck, hint_truck) || strcmp(plate, hint_plate);
	snprintf(hint_truck, sizeof hint_truck, "%s", truck);
	snprintf(hint_plate, sizeof hint_plate, "%s", plate);
	pthread_mutex_unlock(&hint_lock);
	if (changed) {
		need_detect = 1;
		logf_(SCS_LOG_TYPE_message, "[ets2-beacon] LKW gewechselt: id=%s Kennzeichen=%s", truck, plate);
	}
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01)
		return SCS_RESULT_unsupported;

	const scs_telemetry_init_params_v100_t *const p = (const scs_telemetry_init_params_v100_t *)params;
	game_log = p->common.log;
	snprintf(game_id, sizeof game_id, "%s", p->common.game_id ? p->common.game_id : "eut2");

	if (p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_light_beacon, SCS_U32_NIL,
	                            SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
	                            on_beacon, NULL) != SCS_RESULT_ok) {
		game_log(SCS_LOG_TYPE_error, "[ets2-beacon] Kanal truck.light.beacon nicht verfuegbar");
		return SCS_RESULT_generic_error;
	}
	p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_frame_end, NULL);
	p->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_config, NULL);

	load_config();
	beacon_on = 0;
	need_detect = 1;
	running = 1;
	if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		running = 0;
		game_log(SCS_LOG_TYPE_error, "[ets2-beacon] Thread-Start fehlgeschlagen");
		return SCS_RESULT_generic_error;
	}
	worker_started = 1;
	logf_(SCS_LOG_TYPE_message, "[ets2-beacon] geladen (%s, Modus %s, Ersatz %s)", game_id,
	      cfg_mode == CFG_AUTO ? "auto" : mode_name(cfg_mode), mode_name(cfg_fallback));
	return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
	running = 0;
	if (worker_started) {
		pthread_join(worker, NULL);
		worker_started = 0;
	}
	flush_log();
	game_log = NULL;
}
