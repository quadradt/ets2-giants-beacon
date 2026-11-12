/* Test ohne Spiel: lädt das Plugin wie ETS2, meldet einen LKW und schaltet den
 * Beacon-Kanal. Steuert die echten Leuchten an, wenn sie angeschlossen sind.
 * Aufruf: test_harness ./ets2_giants_beacon.so [Kennzeichen] [LKW-ID] */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t type; uint32_t pad; union { uint8_t b; const char *str; char raw[40]; } u; } value_t;
typedef struct { const char *name; uint32_t index; uint32_t pad; value_t value; } named_t;
typedef struct { const char *id; const named_t *attributes; } config_t;
typedef void (*chan_cb)(const char *, uint32_t, const value_t *, void *);
typedef void (*ev_cb)(uint32_t, const void *, void *);
typedef void (*log_fn)(int, const char *);

static chan_cb beacon_cb;
static ev_cb frame_cb, config_cb;
static void game_log(int t, const char *m) { printf("[log %d] %s\n", t, m); fflush(stdout); }
static int reg_event(uint32_t ev, ev_cb cb, void *ctx) {
	(void)ctx;
	if (ev == 2) frame_cb = cb;
	if (ev == 5) config_cb = cb;
	return 0;
}
static int noop(void) { return 0; }
static int reg_channel(const char *n, uint32_t i, uint32_t ty, uint32_t f, chan_cb cb, void *ctx) {
	(void)i; (void)ty; (void)f; (void)ctx;
	if (!strcmp(n, "truck.light.beacon")) beacon_cb = cb;
	return 0;
}
struct params {  /* Layout von scs_telemetry_init_params_v100_t */
	const char *game_name, *game_id; uint32_t game_version; uint32_t pad; log_fn log;
	void *reg_ev, *unreg_ev, *reg_ch, *unreg_ch;
};
static void tick(double sec) { for (int i = 0; i < sec * 10; i++) { if (frame_cb) frame_cb(2, 0, 0); usleep(100000); } }
static void beacon(int on) {
	value_t v = {1 /* bool */, 0, {0}};
	v.u.b = (uint8_t)on;
	beacon_cb("truck.light.beacon", 0xffffffff, &v, 0);
}

int main(int argc, char **argv) {
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
	int (*init)(uint32_t, const void *) = (int (*)(uint32_t, const void *))dlsym(h, "scs_telemetry_init");
	void (*shut)(void) = (void (*)(void))dlsym(h, "scs_telemetry_shutdown");
	struct params p = {"Euro Truck Simulator 2", "eut2", 0x10012, 0, game_log,
	                   (void *)reg_event, (void *)noop, (void *)reg_channel, (void *)noop};
	int rc = init(0x10001, &p);
	printf("init -> %d\n", rc);
	if (rc) return 1;
	if (argc > 2 && config_cb) {
		named_t attrs[3];
		memset(attrs, 0, sizeof attrs);
		attrs[0].name = "license.plate"; attrs[0].value.type = 12; attrs[0].value.u.str = argv[2];
		attrs[1].name = "id"; attrs[1].value.type = 12; attrs[1].value.u.str = argc > 3 ? argv[3] : "";
		config_t cfg = {"truck", attrs};
		printf("config-event: truck plate=%s\n", argv[2]);
		config_cb(5, &cfg, 0);
	}
	tick(1.5);
	printf("== Beacon AN (5 s)\n"); fflush(stdout);
	beacon(1);
	tick(5);
	printf("== Beacon AUS\n"); fflush(stdout);
	beacon(0);
	tick(1.5);
	shut();
	printf("shutdown ok\n");
	return 0;
}
