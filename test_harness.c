/* Test ohne Spiel: lädt das Plugin wie ETS2 und schaltet den Beacon-Kanal. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

typedef struct { uint32_t type; uint32_t pad; union { struct { uint8_t value; } b; char raw[64]; } u; } value_t;
typedef void (*chan_cb)(const char *, uint32_t, const value_t *, void *);
typedef void (*log_fn)(int, const char *);
typedef int (*reg_ch)(const char *, uint32_t, uint32_t, uint32_t, chan_cb, void *);

static chan_cb beacon_cb;
typedef void (*ev_cb)(uint32_t, const void *, void *);
static ev_cb frame_cb;
static void tick(int sec) { for (int i = 0; i < sec * 10; i++) { if (frame_cb) frame_cb(2, 0, 0); usleep(100000); } }
static void game_log(int t, const char *m) { printf("[log %d] %s\n", t, m); fflush(stdout); }
static int reg_event(uint32_t ev, ev_cb cb, void *ctx) { (void)ctx; if (ev == 2) frame_cb = cb; return 0; }
static int noop(void) { return 0; }
static int reg_channel(const char *n, uint32_t i, uint32_t ty, uint32_t f, chan_cb cb, void *ctx) {
	(void)i; (void)ty; (void)f; (void)ctx;
	printf("register_for_channel(%s)\n", n);
	if (!strcmp(n, "truck.light.beacon")) beacon_cb = cb;
	return 0;
}
struct params {  /* Layout von scs_telemetry_init_params_v100_t */
	const char *game_name, *game_id; uint32_t game_version; uint32_t pad; log_fn log;
	void *reg_ev, *unreg_ev; reg_ch reg_ch_; void *unreg_ch;
};
int main(int argc, char **argv) {
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
	int (*init)(uint32_t, const void *) = dlsym(h, "scs_telemetry_init");
	void (*shut)(void) = dlsym(h, "scs_telemetry_shutdown");
	struct params p = {"Euro Truck Simulator 2", "eut2", 0x10012, 0, game_log,
	                   (void *)reg_event, (void *)noop, reg_channel, (void *)noop};
	printf("init -> %d\n", init(0x10001, &p));
	value_t on = {1 /* bool */, 0, {{1}}}, off = {1, 0, {{0}}};
	tick(1);
	printf("Beacon AN (%d s)\n", argc > 2 ? 15 : 8); fflush(stdout);
	beacon_cb("truck.light.beacon", 0xffffffff, &on, 0);
	tick(argc > 2 ? 15 : 8);
	printf("Beacon AUS\n"); fflush(stdout);
	beacon_cb("truck.light.beacon", 0xffffffff, &off, 0);
	tick(2);
	printf("Beacon AN, dann shutdown\n"); fflush(stdout);
	beacon_cb("truck.light.beacon", 0xffffffff, &on, 0);
	tick(3);
	shut();
	printf("shutdown ok\n");
	return 0;
}
