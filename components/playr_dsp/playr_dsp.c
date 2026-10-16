/*
 *  Playr output stage, squeezelite-esp32 glue and "dsp" console command
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "platform_config.h"
#include "dsp_config.h"
#include "dsp_core.h"
#include "playr_dsp.h"

#define CHUNK		256			// frames per pass through the chain
#define MAX_RATE	96000		// delay lines hold DSP_MAX_DELAY_MS at this rate
#define NVS_KEY		"dsp_config"

static const char *TAG = "playr_dsp";

static EXT_RAM_ATTR struct {
	SemaphoreHandle_t lock;
	dsp_config_t cfg;
	dsp_state_t st;
	bool ready;
} dsp;

// touched for every sample, keep them in internal RAM
static float scratch_l[CHUNK], scratch_r[CHUNK];

#define LOCK	xSemaphoreTake(dsp.lock, portMAX_DELAY)
#define UNLOCK	xSemaphoreGive(dsp.lock)

static void print_config(void);

/****************************************************************************************
 * audio path
 */
void playr_dsp_init(void) {
	char err[96];

	if (dsp.ready) return;

	size_t frames = (size_t) (DSP_MAX_DELAY_MS * MAX_RATE / 1000);
	float *mem = heap_caps_calloc(DSP_CHANNELS * frames, sizeof(float), MALLOC_CAP_SPIRAM);
	if (!mem) ESP_LOGW(TAG, "no memory for delay lines, delay disabled");

	dsp.lock = xSemaphoreCreateMutex();
	dsp_state_init(&dsp.st, mem, mem ? frames : 0);

	char *json = config_alloc_get_default(NVS_TYPE_STR, NVS_KEY, "", 0);
	if (!dsp_config_from_json(json, &dsp.cfg, err, sizeof(err))) {
		ESP_LOGE(TAG, "ignoring invalid %s: %s", NVS_KEY, err);
		dsp_config_default(&dsp.cfg);
	}
	free(json);

	dsp_state_configure(&dsp.st, &dsp.cfg, dsp.st.samplerate);
	dsp.ready = true;

	ESP_LOGI(TAG, "output stage %s, role %s, %d eq bands", dsp.cfg.enabled ? "on" : "off",
			 dsp_role_name(dsp.cfg.role), dsp.cfg.band_count);
}

void playr_dsp_set_samplerate(uint32_t samplerate) {
	if (!dsp.ready || !samplerate || samplerate == dsp.st.samplerate) return;
	LOCK;
	dsp_state_configure(&dsp.st, &dsp.cfg, samplerate);
	UNLOCK;
	ESP_LOGI(TAG, "sample rate %u", (unsigned) samplerate);
}

void playr_dsp_reset(void) {
	if (!dsp.ready) return;
	LOCK;
	dsp_state_reset(&dsp.st);
	UNLOCK;
}

void playr_dsp_process_s16(int16_t *buf, size_t frames) {
	if (!dsp.ready) return;
	LOCK;
	dsp_process_s16(&dsp.st, buf, frames, scratch_l, scratch_r, CHUNK);
	UNLOCK;
}

void playr_dsp_process_s32(int32_t *buf, size_t frames) {
	if (!dsp.ready) return;
	LOCK;
	dsp_process_s32(&dsp.st, buf, frames, scratch_l, scratch_r, CHUNK);
	UNLOCK;
}

/****************************************************************************************
 * console
 */
static bool apply(const dsp_config_t *cfg) {
	const char *problem = dsp_config_check(cfg);
	if (problem) {
		printf("%s\n", problem);
		return false;
	}

	LOCK;
	dsp.cfg = *cfg;
	dsp_state_configure(&dsp.st, &dsp.cfg, dsp.st.samplerate);
	UNLOCK;

	char *json = dsp_config_to_json(cfg);
	if (json) {
		config_set_value(NVS_TYPE_STR, NVS_KEY, json);
		free(json);
	}
	return true;
}

static bool parse_float(const char *s, float *v) {
	char *end;
	if (!s) return false;
	*v = strtof(s, &end);
	return end != s && !*end && isfinite(*v);
}

static bool parse_switch(const char *s, bool *v) {
	if (!s) return false;
	if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) *v = true;
	else if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) *v = false;
	else return false;
	return true;
}

static bool parse_channels(const char *s, uint8_t *mask) {
	if (!s || !strcmp(s, "both")) *mask = DSP_CH_BOTH;
	else if (!strcmp(s, "left")) *mask = DSP_CH_LEFT;
	else if (!strcmp(s, "right")) *mask = DSP_CH_RIGHT;
	else return false;
	return true;
}

static const char *channels_name(uint8_t mask) {
	return mask == DSP_CH_LEFT ? "left" : mask == DSP_CH_RIGHT ? "right" : "both";
}

// <type> <Hz> [q] [dB] [left|right|both]
static bool parse_band(int argc, char **argv, dsp_band_t *band) {
	band->q = 0.707f;
	band->gain_db = 0;
	band->channels = DSP_CH_BOTH;

	if (argc < 2 || !dsp_filter_parse(argv[0], &band->type) || !parse_float(argv[1], &band->freq)) return false;
	if (argc > 2 && !parse_float(argv[2], &band->q)) return false;
	if (argc > 3 && !parse_float(argv[3], &band->gain_db)) return false;
	if (argc > 4 && !parse_channels(argv[4], &band->channels)) return false;
	return argc <= 5;
}

static void print_pair(const char *name, const float *v, const char *unit, int decimals) {
	if (v[0] == v[1]) printf("%s %.*f %s", name, decimals, v[0], unit);
	else printf("%s L %.*f R %.*f %s", name, decimals, v[0], decimals, v[1], unit);
}

static void print_bands(const dsp_config_t *cfg) {
	if (!cfg->band_count) printf("  eq: none\n");
	for (int i = 0; i < cfg->band_count; i++) {
		const dsp_band_t *b = cfg->bands + i;
		printf("  eq %d: %-9s %7.1f Hz  q %5.2f", i + 1, dsp_filter_name(b->type), b->freq, b->q);
		if (b->type == DSP_PEAK || b->type == DSP_LOWSHELF || b->type == DSP_HIGHSHELF) printf("  %+5.1f dB", b->gain_db);
		printf("  %s\n", channels_name(b->channels));
	}
}

static void print_config(void) {
	dsp_config_t cfg;
	uint32_t rate, clipped;
	float peak;
	bool active, shared;

	LOCK;
	cfg = dsp.cfg;
	rate = dsp.st.samplerate;
	active = dsp.st.active;
	shared = dsp.st.shared;
	peak = dsp.st.peak;
	clipped = dsp.st.clipped;
	UNLOCK;

	printf("output stage: %s%s, %u Hz, role %s%s\n", cfg.enabled ? "on" : "off",
		   cfg.enabled && !active ? " (neutral, bypassed)" : "", (unsigned) rate,
		   dsp_role_name(cfg.role), shared ? " (one shared chain)" : "");
	printf("  preamp %.1f dB, highpass ", cfg.preamp_db);
	if (cfg.highpass_hz) printf("%.0f Hz LR4", cfg.highpass_hz); else printf("off");
	printf(", lowpass ");
	if (cfg.lowpass_hz) printf("%.0f Hz LR4\n", cfg.lowpass_hz);
	else if (cfg.role == DSP_ROLE_SUB) printf("%.0f Hz LR4 (sub default)\n", DSP_SUB_DEFAULT_HZ);
	else printf("off\n");
	print_bands(&cfg);
	printf("  ");
	print_pair("gain", cfg.gain_db, "dB", 1);
	printf(", ");
	print_pair("delay", cfg.delay_ms, "ms", 3);
	printf(", invert %s, dither %s\n",
		   cfg.invert[0] && cfg.invert[1] ? "both" : cfg.invert[0] ? "left" : cfg.invert[1] ? "right" : "none",
		   cfg.dither ? "on" : "off");
	if (active) {
		printf("  peak %.1f dBFS, %u clipped samples (dsp meter resets)\n",
			   peak > 0 ? 20 * log10f(peak) : -INFINITY, (unsigned) clipped);
	}
}

static void usage(void) {
	printf("dsp                                 show settings and meters\n"
		   "dsp on|off\n"
		   "dsp role stereo|left|right|mono|sub\n"
		   "dsp preamp <dB>                     headroom before the filters\n"
		   "dsp highpass <Hz>                   LR4, 0 = off\n"
		   "dsp lowpass <Hz>                    LR4, 0 = off (sub defaults to %.0f)\n"
		   "dsp gain <dB> [<dB right>]\n"
		   "dsp delay <ms> [<ms right>]         up to %.0f ms\n"
		   "dsp invert none|left|right|both\n"
		   "dsp dither on|off\n"
		   "dsp eq add <type> <Hz> [q] [dB] [left|right|both]\n"
		   "dsp eq set <n> <type> <Hz> [q] [dB] [left|right|both]\n"
		   "dsp eq del <n> | dsp eq clear\n"
		   "      type: peak lowshelf highshelf lowpass highpass\n"
		   "dsp json [<json>]                   print or replace the whole config\n"
		   "dsp meter                           peak and clipped samples, then reset\n"
		   "dsp defaults                        back to defaults (off)\n",
		   DSP_SUB_DEFAULT_HZ, DSP_MAX_DELAY_MS);
}

static int dsp_cmd(int argc, char **argv) {
	dsp_config_t cfg;
	const char *cmd = argc > 1 ? argv[1] : NULL;
	bool ok = true;

	if (!dsp.ready) {
		printf("output stage not started (no I2S output?)\n");
		return 1;
	}

	LOCK;
	cfg = dsp.cfg;
	UNLOCK;

	if (!cmd) {
		print_config();
		return 0;
	} else if (!strcmp(cmd, "help")) {
		usage();
		return 0;
	} else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
		cfg.enabled = cmd[1] == 'n';
	} else if (!strcmp(cmd, "role")) {
		ok = argc == 3 && dsp_role_parse(argv[2], &cfg.role);
	} else if (!strcmp(cmd, "preamp")) {
		ok = argc == 3 && parse_float(argv[2], &cfg.preamp_db);
	} else if (!strcmp(cmd, "highpass")) {
		ok = argc == 3 && parse_float(argv[2], &cfg.highpass_hz);
	} else if (!strcmp(cmd, "lowpass")) {
		ok = argc == 3 && parse_float(argv[2], &cfg.lowpass_hz);
	} else if (!strcmp(cmd, "gain") || !strcmp(cmd, "delay")) {
		float *v = cmd[0] == 'g' ? cfg.gain_db : cfg.delay_ms;
		ok = (argc == 3 || argc == 4) && parse_float(argv[2], v) && parse_float(argv[argc - 1], v + 1);
	} else if (!strcmp(cmd, "invert")) {
		uint8_t mask = 0;
		ok = argc == 3 && (!strcmp(argv[2], "none") || parse_channels(argv[2], &mask));
		cfg.invert[0] = mask & DSP_CH_LEFT;
		cfg.invert[1] = mask & DSP_CH_RIGHT;
	} else if (!strcmp(cmd, "dither")) {
		ok = argc == 3 && parse_switch(argv[2], &cfg.dither);
	} else if (!strcmp(cmd, "eq")) {
		const char *sub = argc > 2 ? argv[2] : NULL;
		int n = argc > 3 ? atoi(argv[3]) : 0;
		if (!sub) {
			print_bands(&cfg);
			return 0;
		} else if (!strcmp(sub, "add")) {
			ok = cfg.band_count < DSP_MAX_BANDS && parse_band(argc - 3, argv + 3, cfg.bands + cfg.band_count);
			if (ok) cfg.band_count++;
			else if (cfg.band_count == DSP_MAX_BANDS) printf("all %d bands in use\n", DSP_MAX_BANDS);
		} else if (!strcmp(sub, "set")) {
			ok = n >= 1 && n <= cfg.band_count && parse_band(argc - 4, argv + 4, cfg.bands + n - 1);
		} else if (!strcmp(sub, "del")) {
			ok = n >= 1 && n <= cfg.band_count;
			if (ok) {
				memmove(cfg.bands + n - 1, cfg.bands + n, (cfg.band_count - n) * sizeof(*cfg.bands));
				cfg.band_count--;
			}
		} else if (!strcmp(sub, "clear")) {
			cfg.band_count = 0;
		} else {
			ok = false;
		}
	} else if (!strcmp(cmd, "json")) {
		if (argc == 2) {
			char *json = dsp_config_to_json(&cfg);
			printf("%s\n", json ? json : "out of memory");
			free(json);
			return 0;
		}
		// the console splits on spaces: glue the arguments back together
		size_t len = 1;
		for (int i = 2; i < argc; i++) len += strlen(argv[i]) + 1;
		char *json = calloc(1, len), err[96];
		for (int i = 2; json && i < argc; i++) {
			strcat(json, argv[i]);
			strcat(json, " ");
		}
		ok = json && dsp_config_from_json(json, &cfg, err, sizeof(err));
		if (json && !ok) printf("%s\n", err);
		free(json);
		if (!ok) return 1;
	} else if (!strcmp(cmd, "meter")) {
		LOCK;
		float peak = dsp.st.peak;
		uint32_t clipped = dsp.st.clipped;
		dsp_stats_reset(&dsp.st);
		UNLOCK;
		printf("peak %.1f dBFS, %u clipped samples\n", peak > 0 ? 20 * log10f(peak) : -INFINITY, (unsigned) clipped);
		return 0;
	} else if (!strcmp(cmd, "defaults")) {
		dsp_config_default(&cfg);
	} else {
		ok = false;
	}

	if (!ok) {
		usage();
		return 1;
	}

	if (!apply(&cfg)) return 1;
	print_config();
	return 0;
}

void register_dsp_cmd(void) {
	const esp_console_cmd_t cmd = {
		.command = "dsp",
		.help = "Playr output stage: role, crossover, EQ, gain, delay. 'dsp help' for details",
		.hint = NULL,
		.func = &dsp_cmd,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
