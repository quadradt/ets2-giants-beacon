/*
 * ets2-beacon – SCS-Telemetrie-Plugin (Linux) für die GIANTS-USB-Rundumleuchte.
 *
 * Spiegelt den Kanal "truck.light.beacon" aus Euro Truck Simulator 2 /
 * American Truck Simulator auf alle angeschlossenen GIANTS-Beacons
 * (USB 340d:1710). Kein Hintergrunddienst nötig: das Plugin schreibt selbst
 * auf /dev/hidrawN. Zugriffsrechte kommen von der udev-Regel aus giants-beacon.
 *
 * (C++ nur, weil die SCS-SDK-Header C++ verlangen; Code ist schlichtes C.)
 * Modus: Datei ~/.config/ets2-beacon/mode mit "round" (Standard) oder "blink".
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

/* Alte Symbolversionen erzwingen, damit das Plugin auch in älteren Steam-
 * Runtimes (glibc < 2.34) lädt. Moderne glibc liefert diese weiterhin mit. */
__asm__(".symver pthread_create,pthread_create@GLIBC_2.2.5");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.2.5");

#define MAX_DEV 8
#define RESEND_SEC 3.0   /* Hardware schaltet nach 10 s von selbst ab */
#define RESCAN_SEC 5.0   /* Hotplug: Geräte regelmäßig neu suchen */

/* Gleiche Payloads wie giants-beacon (Report-ID 0 + 9 Byte). */
static const unsigned char P_OFF[10]   = {0x00, 0xFF, 0x00, 0x00, 0x64, 0x00, 0x32, 0x9E, 0xD7, 0x0D};
static const unsigned char P_BLINK[10] = {0x00, 0xFF, 0x07, 0xFF, 0x64, 0xFF, 0xEB, 0x7D, 0x9A, 0x03};
static const unsigned char P_ROUND[10] = {0x00, 0xFF, 0x01, 0x66, 0xC8, 0xFF, 0xAD, 0x52, 0x81, 0xD6};

static scs_log_t game_log = NULL;
static pthread_t worker;
static int worker_started = 0;
static volatile int running = 0;
static volatile int beacon_on = 0;   /* vom Spiel-Thread gesetzt */
static const unsigned char *mode_payload = P_ROUND;

static int devs[MAX_DEV];
static int ndev = 0;

static void logf_(scs_log_type_t type, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Log-Nachrichten aus dem Worker werden gepuffert und im Spiel-Thread
 * (frame_end) ausgegeben – die SDK-API ist nur dort sicher aufrufbar. */
#define LOGQ 16
static pthread_mutex_t logq_lock = PTHREAD_MUTEX_INITIALIZER;
static char logq_msg[LOGQ][200];
static scs_log_type_t logq_type[LOGQ];
static int logq_n = 0;

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

static SCSAPI_VOID on_frame_end(const scs_event_t event, const void *const event_info,
                                const scs_context_t context)
{
	(void)event; (void)event_info; (void)context;
	flush_log();
}

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
			logf_(SCS_LOG_TYPE_warning, "[ets2-beacon] %s nicht oeffenbar: %s (udev-Regel?)",
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

static void *worker_main(void *arg)
{
	(void)arg;
	int last = -1, last_ndev = -1;
	double last_send = 0, last_scan = 0;

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

		if (on != last || (on && t - last_send >= RESEND_SEC)) {
			int ok = send_all(on ? mode_payload : P_OFF);
			if (on != last)
				logf_(SCS_LOG_TYPE_message, "[ets2-beacon] Rundumleuchte %s (%d/%d Geraete)",
				      on ? "AN" : "AUS", ok, ndev);
			last = on;
			last_send = t;
		}
		usleep(100 * 1000);
	}
	send_all(P_OFF);
	close_devs();
	return NULL;
}

static void load_mode(void)
{
	char path[512];
	const char *cfg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (cfg && *cfg)
		snprintf(path, sizeof path, "%s/ets2-beacon/mode", cfg);
	else
		snprintf(path, sizeof path, "%s/.config/ets2-beacon/mode", home ? home : "");
	const char *env = getenv("ETS2_BEACON_MODE");
	char buf[32] = "";
	if (env)
		snprintf(buf, sizeof buf, "%s", env);
	else {
		FILE *f = fopen(path, "r");
		if (f) {
			if (!fgets(buf, sizeof buf, f))
				buf[0] = 0;
			fclose(f);
		}
	}
	mode_payload = strncmp(buf, "blink", 5) == 0 ? P_BLINK : P_ROUND;
}

static SCSAPI_VOID on_beacon(const scs_string_t name, const scs_u32_t index,
                             const scs_value_t *const value, const scs_context_t context)
{
	(void)name; (void)index; (void)context;
	beacon_on = (value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value) ? 1 : 0;
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01)
		return SCS_RESULT_unsupported;

	const scs_telemetry_init_params_v100_t *const p = (const scs_telemetry_init_params_v100_t *)params;
	game_log = p->common.log;

	if (p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_light_beacon, SCS_U32_NIL,
	                            SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
	                            on_beacon, NULL) != SCS_RESULT_ok) {
		logf_(SCS_LOG_TYPE_error, "[ets2-beacon] Kanal truck.light.beacon nicht verfuegbar");
		return SCS_RESULT_generic_error;
	}

	p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_frame_end, NULL);

	load_mode();
	beacon_on = 0;
	running = 1;
	if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		running = 0;
		logf_(SCS_LOG_TYPE_error, "[ets2-beacon] Thread-Start fehlgeschlagen");
		return SCS_RESULT_generic_error;
	}
	worker_started = 1;
	logf_(SCS_LOG_TYPE_message, "[ets2-beacon] geladen (%s, Modus %s)",
	      p->common.game_id, mode_payload == P_BLINK ? "blink" : "round");
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
