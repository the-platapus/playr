/*
 *  Host unit tests for the Playr output stage (scripts/playr test)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp_core.h"
#include "dsp_config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK 256

static int checks, failures;

#define CHECK(cond, ...) do {													\
		checks++;																\
		if (!(cond)) {															\
			failures++;															\
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);						\
			printf(__VA_ARGS__);												\
			printf("\n");														\
		}																		\
	} while (0)

static float scratch_l[CHUNK], scratch_r[CHUNK];
static float delay_mem[DSP_CHANNELS * 4800];

static void setup(dsp_state_t *st, const dsp_config_t *cfg, uint32_t rate) {
	dsp_state_init(st, delay_mem, sizeof(delay_mem) / sizeof(*delay_mem) / DSP_CHANNELS);
	dsp_state_configure(st, cfg, rate);
}

/****************************************************************************************
 * measurement helpers
 */

// steady state gain of a filter chain at one frequency, from a single DFT bin
static double chain_gain_db(dsp_svf_t *chain, int count, uint32_t rate, double freq) {
	const double amplitude = 0.5;
	size_t settle = rate / 2, length = rate, t = 0;
	double re = 0, im = 0;
	float buf[CHUNK];

	for (int i = 0; i < count; i++) chain[i].ic1 = chain[i].ic2 = 0;

	while (t < settle + length) {
		for (int i = 0; i < CHUNK; i++) buf[i] = amplitude * sin(2 * M_PI * freq * (t + i) / rate);
		for (int i = 0; i < count; i++) dsp_svf_run(chain + i, buf, CHUNK);
		for (int i = 0; i < CHUNK; i++, t++) {
			if (t < settle || t >= settle + length) continue;
			double w = 2 * M_PI * freq * t / rate;
			re += buf[i] * cos(w);
			im += buf[i] * sin(w);
		}
	}

	return 20 * log10(2 * sqrt(re * re + im * im) / length / amplitude);
}

static double band_gain_db(dsp_filter_type_t type, float freq, float q, float gain, uint32_t rate, double at) {
	dsp_svf_t f = { 0 };
	dsp_svf_design(&f, type, freq, q, gain, rate);
	return chain_gain_db(&f, 1, rate, at);
}

/****************************************************************************************
 * tests
 */
static void test_band_responses_match_rbj(void) {
	static const struct { dsp_filter_type_t type; float freq, q, gain; } cases[] = {
		{ DSP_PEAK, 1000, 1.0f, 6 }, { DSP_PEAK, 120, 4.0f, -9 }, { DSP_PEAK, 12000, 0.7f, 3 },
		{ DSP_LOWSHELF, 100, 0.707f, 6 }, { DSP_LOWSHELF, 250, 0.5f, -4 },
		{ DSP_HIGHSHELF, 8000, 0.707f, -3 }, { DSP_HIGHSHELF, 3000, 1.0f, 5 },
		{ DSP_LOWPASS, 2000, 0.707f, 0 }, { DSP_LOWPASS, 80, 2.0f, 0 },
		{ DSP_HIGHPASS, 40, 0.707f, 0 }, { DSP_HIGHPASS, 500, 1.3f, 0 },
	};
	static const uint32_t rates[] = { 44100, 48000, 96000 };
	static const double probes[] = { 31, 97, 200, 1000, 2500, 7919, 15000 };
	double worst = 0;

	for (size_t r = 0; r < sizeof(rates) / sizeof(*rates); r++) {
		for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
			for (size_t p = 0; p < sizeof(probes) / sizeof(*probes); p++) {
				double expect = dsp_band_response_db(cases[c].type, cases[c].freq, cases[c].q, cases[c].gain, rates[r], probes[p]);
				if (expect < -60) continue;		// below what a float sine measurement resolves
				double got = band_gain_db(cases[c].type, cases[c].freq, cases[c].q, cases[c].gain, rates[r], probes[p]);
				double err = fabs(got - expect);
				if (err > worst) worst = err;
				CHECK(err < 0.01, "%s %.0f Hz q %.2f %+.1f dB @ %u: %.0f Hz expected %.4f dB got %.4f dB",
					  dsp_filter_name(cases[c].type), cases[c].freq, cases[c].q, cases[c].gain, rates[r], probes[p], expect, got);
			}
		}
	}

	printf("  worst deviation from RBJ cookbook response: %.5f dB\n", worst);
}

static void test_peak_gain_at_center(void) {
	CHECK(fabs(band_gain_db(DSP_PEAK, 1000, 2, 6, 48000, 1000) - 6) < 0.01, "peak +6 dB at center");
	CHECK(fabs(band_gain_db(DSP_PEAK, 1000, 2, -12, 48000, 1000) + 12) < 0.01, "peak -12 dB at center");
	CHECK(fabs(band_gain_db(DSP_LOWSHELF, 200, 0.707f, 6, 48000, 20) - 6) < 0.05, "lowshelf plateau");
	CHECK(fabs(band_gain_db(DSP_HIGHSHELF, 2000, 0.707f, -6, 48000, 18000) + 6) < 0.1, "highshelf plateau");
}

static void test_lr4_crossover(void) {
	dsp_config_t lp, hp;
	dsp_state_t a, b;
	static float mem_a[2 * 16], mem_b[2 * 16];
	const uint32_t rate = 48000;
	const float xover = 80;

	dsp_config_default(&lp);
	lp.enabled = true;
	lp.lowpass_hz = xover;
	hp = lp;
	hp.lowpass_hz = 0;
	hp.highpass_hz = xover;

	dsp_state_init(&a, mem_a, 16);
	dsp_state_init(&b, mem_b, 16);
	dsp_state_configure(&a, &lp, rate);
	dsp_state_configure(&b, &hp, rate);

	CHECK(a.filter_count[0] == 2 && b.filter_count[0] == 2, "LR4 uses two sections");
	CHECK(fabs(chain_gain_db(a.filter[0], 2, rate, xover) + 6.0206) < 0.01, "LR4 lowpass is -6 dB at the crossover");
	CHECK(fabs(chain_gain_db(b.filter[0], 2, rate, xover) + 6.0206) < 0.01, "LR4 highpass is -6 dB at the crossover");
	CHECK(chain_gain_db(a.filter[0], 2, rate, 4 * xover) < -47, "LR4 lowpass falls 24 dB/oct");

	// low + high must sum flat (all-pass) at every frequency
	static const double probes[] = { 20, 40, 60, 80, 100, 160, 320, 1000, 10000 };
	double worst = 0;
	for (size_t p = 0; p < sizeof(probes) / sizeof(*probes); p++) {
		size_t settle = rate, length = rate, t = 0;
		double re = 0, im = 0;
		float x[CHUNK], y[CHUNK];
		for (int i = 0; i < 2; i++) a.filter[0][i].ic1 = a.filter[0][i].ic2 = b.filter[0][i].ic1 = b.filter[0][i].ic2 = 0;
		while (t < settle + length) {
			for (int i = 0; i < CHUNK; i++) x[i] = y[i] = 0.5 * sin(2 * M_PI * probes[p] * (t + i) / rate);
			for (int i = 0; i < 2; i++) {
				dsp_svf_run(&a.filter[0][i], x, CHUNK);
				dsp_svf_run(&b.filter[0][i], y, CHUNK);
			}
			for (int i = 0; i < CHUNK; i++, t++) {
				if (t < settle || t >= settle + length) continue;
				double w = 2 * M_PI * probes[p] * t / rate, s = x[i] + y[i];
				re += s * cos(w);
				im += s * sin(w);
			}
		}
		double db = 20 * log10(2 * sqrt(re * re + im * im) / length / 0.5);
		if (fabs(db) > worst) worst = fabs(db);
		CHECK(fabs(db) < 0.01, "LR4 sum at %.0f Hz is %.4f dB", probes[p], db);
	}
	printf("  LR4 low + high sum deviation: %.5f dB\n", worst);
}

// double precision RBJ biquad, the reference for numerical accuracy
static void rbj_pass(int highpass, double fc, double q, double rate, double c[5]) {
	double w0 = 2 * M_PI * fc / rate, cw = cos(w0), alpha = sin(w0) / (2 * q), a0 = 1 + alpha;
	double b0 = highpass ? (1 + cw) / 2 : (1 - cw) / 2, b1 = highpass ? -(1 + cw) : 1 - cw;
	c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b0 / a0; c[3] = -2 * cw / a0; c[4] = (1 - alpha) / a0;
}

// power of the error left once the parts correlated with the test tones are removed:
// those are a deterministic ~1e-4 dB response deviation (float coefficients), not noise
static void split_error(const double *e, size_t n, size_t offset, uint32_t rate, const double *tones, int count,
						double *total_db, double *noise_db) {
	double total = 0, tonal = 0;
	for (size_t t = 0; t < n; t++) total += e[t] * e[t];
	for (int k = 0; k < count; k++) {
		double re = 0, im = 0;
		for (size_t t = 0; t < n; t++) {
			double w = 2 * M_PI * tones[k] * (t + offset) / rate;
			re += e[t] * cos(w);
			im += e[t] * sin(w);
		}
		tonal += 2 * (re * re + im * im) / n;
	}
	*total_db = 10 * log10(total / n + 1e-30);
	*noise_db = 10 * log10((total - tonal) / n + 1e-30);
}

static void test_low_cutoff_noise_floor(void) {
	static const double tones[] = { 31.3, 1000.5 };		// whole cycles in 10 s
	const uint32_t rate = 48000;
	const size_t settle = rate, length = 10 * rate;
	double *svf_e = malloc(length * sizeof(double)), *df2_e = malloc(length * sizeof(double));
	double total_db, noise_db, df2_total_db, df2_noise_db;

	for (float fc = 20; fc <= 80; fc *= 2) {
		double c[5], ref_z[2][4] = { { 0 } };
		float df2_c[5], df2_w[2][2] = { { 0 } };
		dsp_svf_t svf[2] = { { 0 } };

		rbj_pass(1, fc, M_SQRT1_2, rate, c);
		for (int i = 0; i < 5; i++) df2_c[i] = c[i];
		for (int i = 0; i < 2; i++) dsp_svf_design(svf + i, DSP_HIGHPASS, fc, M_SQRT1_2, 0, rate);

		for (size_t t = 0; t < settle + length; t++) {
			// near full scale, with a strong low end close to the cutoff
			float xf = 0.45 * sin(2 * M_PI * tones[0] * t / rate) + 0.45 * sin(2 * M_PI * tones[1] * t / rate);

			// reference: double precision direct form 1, LR4 = two sections
			double ref = xf;
			for (int s = 0; s < 2; s++) {
				double *z = ref_z[s];
				double y = c[0] * ref + c[1] * z[0] + c[2] * z[1] - c[3] * z[2] - c[4] * z[3];
				z[1] = z[0]; z[0] = ref; z[3] = z[2]; z[2] = y;
				ref = y;
			}

			// what esp-dsp's dsps_biquad_f32 computes: float32 direct form 2
			float df2 = xf;
			for (int s = 0; s < 2; s++) {
				float *w = df2_w[s];
				float d0 = df2 - df2_c[3] * w[0] - df2_c[4] * w[1];
				df2 = df2_c[0] * d0 + df2_c[1] * w[0] + df2_c[2] * w[1];
				w[1] = w[0]; w[0] = d0;
			}

			float y = xf;
			for (int s = 0; s < 2; s++) dsp_svf_run(svf + s, &y, 1);

			if (t < settle) continue;
			svf_e[t - settle] = y - ref;
			df2_e[t - settle] = df2 - ref;
		}

		split_error(svf_e, length, settle, rate, tones, 2, &total_db, &noise_db);
		split_error(df2_e, length, settle, rate, tones, 2, &df2_total_db, &df2_noise_db);
		printf("  %2.0f Hz LR4 highpass, noise vs double reference: SVF %.1f dBFS (deviation %.1f), float DF2 biquad %.1f dBFS\n",
			   fc, noise_db, total_db, df2_noise_db);
		// PCM5102A dynamic range is 112 dB: stay well under it
		CHECK(noise_db < -120, "SVF noise floor %.1f dBFS at %.0f Hz should be below -120 dBFS", noise_db, fc);
		CHECK(total_db < -100, "SVF deviation %.1f dBFS at %.0f Hz should be below -100 dBFS", total_db, fc);
		CHECK(noise_db < df2_noise_db - 20, "SVF should clearly beat a float DF2 biquad");
	}

	free(svf_e);
	free(df2_e);
}

static void test_roles(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	int16_t buf[2 * 8];

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.dither = false;

#define FILL() for (int i = 0; i < 8; i++) { buf[2*i] = 1000; buf[2*i+1] = -3000; }

	cfg.role = DSP_ROLE_LEFT;
	setup(&st, &cfg, 48000);
	CHECK(st.shared, "left role runs one shared chain");
	FILL(); dsp_process_s16(&st, buf, 8, scratch_l, scratch_r, CHUNK);
	CHECK(buf[0] == 1000 && buf[1] == 1000 && buf[14] == 1000 && buf[15] == 1000, "left role: got %d %d", buf[0], buf[1]);

	cfg.role = DSP_ROLE_RIGHT;
	setup(&st, &cfg, 48000);
	FILL(); dsp_process_s16(&st, buf, 8, scratch_l, scratch_r, CHUNK);
	CHECK(buf[0] == -3000 && buf[1] == -3000, "right role: got %d %d", buf[0], buf[1]);

	cfg.role = DSP_ROLE_MONO;
	setup(&st, &cfg, 48000);
	FILL(); dsp_process_s16(&st, buf, 8, scratch_l, scratch_r, CHUNK);
	CHECK(buf[0] == -1000 && buf[1] == -1000, "mono role: got %d %d", buf[0], buf[1]);

	cfg.role = DSP_ROLE_STEREO;
	cfg.invert[1] = true;
	cfg.preamp_db = -6.0206f;
	setup(&st, &cfg, 48000);
	CHECK(!st.shared, "stereo role keeps two chains");
	FILL(); dsp_process_s16(&st, buf, 8, scratch_l, scratch_r, CHUNK);
	CHECK(buf[0] == 500 && buf[1] == 1500, "stereo, -6 dB, right inverted: got %d %d", buf[0], buf[1]);

	// same through the 32 bit path
	int32_t buf32[2 * 4] = { 1 << 24, -(3 << 24), 1 << 24, -(3 << 24), 1 << 24, -(3 << 24), 1 << 24, -(3 << 24) };
	cfg.role = DSP_ROLE_MONO;
	cfg.preamp_db = 0;
	cfg.invert[1] = false;
	setup(&st, &cfg, 48000);
	dsp_process_s32(&st, buf32, 4, scratch_l, scratch_r, CHUNK);
	CHECK(buf32[0] == -(1 << 24) && buf32[1] == -(1 << 24), "mono role 32 bits: got %d %d", buf32[0], buf32[1]);
#undef FILL
}

static void test_sub_role(void) {
	dsp_config_t cfg;
	dsp_state_t st;

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.role = DSP_ROLE_SUB;
	setup(&st, &cfg, 48000);
	CHECK(st.filter_count[0] == 2, "sub role adds an LR4 lowpass by default");
	CHECK(fabs(chain_gain_db(st.filter[0], 2, 48000, DSP_SUB_DEFAULT_HZ) + 6.02) < 0.02, "sub default crossover at %.0f Hz", DSP_SUB_DEFAULT_HZ);

	cfg.lowpass_hz = 120;
	setup(&st, &cfg, 48000);
	CHECK(fabs(chain_gain_db(st.filter[0], 2, 48000, 120) + 6.02) < 0.02, "sub uses the configured lowpass");
}

static void test_delay(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	int16_t buf[2 * 200] = { 0 };

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.dither = false;
	cfg.delay_ms[0] = 1.0f;		// 48 samples at 48 kHz
	cfg.delay_ms[1] = 2.5f;		// 120 samples
	setup(&st, &cfg, 48000);

	buf[0] = 10000;
	buf[1] = 20000;
	// odd sizes on purpose, to cross chunk and ring buffer boundaries
	dsp_process_s16(&st, buf, 77, scratch_l, scratch_r, 13);
	dsp_process_s16(&st, buf + 2 * 77, 123, scratch_l, scratch_r, 13);

	int left_at = -1, right_at = -1;
	for (int i = 0; i < 200; i++) {
		if (buf[2*i] == 10000) left_at = i;
		if (buf[2*i+1] == 20000) right_at = i;
	}
	CHECK(left_at == 48, "left impulse expected at 48, found at %d", left_at);
	CHECK(right_at == 120, "right impulse expected at 120, found at %d", right_at);

	// beyond the delay line capacity it is clamped, never overruns
	cfg.delay_ms[0] = cfg.delay_ms[1] = DSP_MAX_DELAY_MS;
	setup(&st, &cfg, 192000);
	CHECK(st.delay[0].length == st.delay[0].capacity, "delay clamped to capacity at high rates");
}

static void test_passthrough(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	int16_t buf[2 * 64], copy[2 * 64];

	for (int i = 0; i < 2 * 64; i++) buf[i] = copy[i] = (int16_t) (rand() - RAND_MAX / 2);

	dsp_config_default(&cfg);
	setup(&st, &cfg, 44100);
	dsp_process_s16(&st, buf, 64, scratch_l, scratch_r, CHUNK);
	CHECK(!memcmp(buf, copy, sizeof(buf)), "disabled is bit exact");

	cfg.enabled = true;
	setup(&st, &cfg, 44100);
	CHECK(!st.active, "enabled but neutral stays inactive");
	dsp_process_s16(&st, buf, 64, scratch_l, scratch_r, CHUNK);
	CHECK(!memcmp(buf, copy, sizeof(buf)), "enabled but neutral is bit exact");
}

static void test_clipping(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	int16_t buf[2 * 4] = { 30000, -30000, 20000, -20000, 1000, -1000, 0, 0 };

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.dither = false;
	cfg.gain_db[0] = cfg.gain_db[1] = 6.0206f;
	setup(&st, &cfg, 44100);
	dsp_process_s16(&st, buf, 4, scratch_l, scratch_r, CHUNK);

	CHECK(buf[0] == 32767 && buf[1] == -32768, "saturates instead of wrapping: %d %d", buf[0], buf[1]);
	CHECK(buf[2] == 32767 && buf[3] == -32768, "saturates: %d %d", buf[2], buf[3]);
	CHECK(buf[4] == 2000 && buf[5] == -2000, "in range samples doubled: %d %d", buf[4], buf[5]);
	CHECK(st.clipped == 4, "clip counter is %u", (unsigned) st.clipped);
	CHECK(fabsf(st.peak - 60000.0f / 32768.0f) < 1e-3f, "peak %.4f", st.peak);
}

static void test_dither(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	static int16_t buf[2 * 4096];
	long sum = 0;
	int bad = 0, nonzero = 0;

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.gain_db[0] = cfg.gain_db[1] = -0.1f;
	setup(&st, &cfg, 44100);
	memset(buf, 0, sizeof(buf));
	dsp_process_s16(&st, buf, 4096, scratch_l, scratch_r, CHUNK);

	for (int i = 0; i < 2 * 4096; i++) {
		sum += buf[i];
		bad += buf[i] < -1 || buf[i] > 1;
		nonzero += buf[i] != 0;
	}
	CHECK(!bad, "TPDF dither stays within +/- 1 LSB (%d outside)", bad);
	CHECK(nonzero > 2 * 4096 / 8, "dither is present (%d non zero samples)", nonzero);
	CHECK(labs(sum) < 2 * 4096 / 16, "dither has no DC (sum %ld)", sum);
}

static void test_live_update_keeps_state(void) {
	dsp_config_t cfg;
	dsp_state_t st;
	int16_t buf[2 * 256];

	dsp_config_default(&cfg);
	cfg.enabled = true;
	cfg.band_count = 1;
	cfg.bands[0] = (dsp_band_t) { DSP_PEAK, 100, 1, 6, DSP_CH_BOTH };
	setup(&st, &cfg, 48000);

	for (int i = 0; i < 256; i++) buf[2*i] = buf[2*i+1] = 10000 * sin(2 * M_PI * 100 * i / 48000.0);
	dsp_process_s16(&st, buf, 256, scratch_l, scratch_r, CHUNK);
	float ic = st.filter[0][0].ic1;

	cfg.bands[0].gain_db = 5;
	dsp_state_configure(&st, &cfg, 48000);
	CHECK(ic != 0 && st.filter[0][0].ic1 == ic, "tweaking a band keeps filter state (no click)");

	cfg.bands[1] = cfg.bands[0];
	cfg.band_count = 2;
	dsp_state_configure(&st, &cfg, 48000);
	CHECK(st.filter[0][0].ic1 == 0, "adding a band restarts the chain");

	cfg.bands[1].channels = DSP_CH_LEFT;
	cfg.role = DSP_ROLE_LEFT;
	dsp_state_configure(&st, &cfg, 48000);
	CHECK(!st.shared && st.filter_count[0] == 2 && st.filter_count[1] == 1, "per channel band disables sharing");
}

static bool config_equal(const dsp_config_t *a, const dsp_config_t *b) {
	if (a->enabled != b->enabled || a->role != b->role || a->preamp_db != b->preamp_db ||
		a->highpass_hz != b->highpass_hz || a->lowpass_hz != b->lowpass_hz ||
		a->band_count != b->band_count || a->dither != b->dither) return false;
	for (int i = 0; i < a->band_count; i++) {
		const dsp_band_t *x = a->bands + i, *y = b->bands + i;
		if (x->type != y->type || x->freq != y->freq || x->q != y->q || x->gain_db != y->gain_db || x->channels != y->channels) return false;
	}
	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		if (a->gain_db[ch] != b->gain_db[ch] || a->delay_ms[ch] != b->delay_ms[ch] || a->invert[ch] != b->invert[ch]) return false;
	}
	return true;
}

static void test_json_roundtrip(void) {
	const char *json =
		"{\"enabled\":true,\"role\":\"left\",\"preamp\":-6,\"highpass\":45,"
		"\"eq\":[{\"type\":\"peak\",\"freq\":120,\"q\":1.4,\"gain\":-4.5},"
		"{\"type\":\"highshelf\",\"freq\":8000,\"gain\":-2,\"ch\":\"right\"}],"
		"\"gain\":[0,-1.5],\"delay\":0.25,\"invert\":[false,true],\"dither\":false}";
	dsp_config_t cfg, again;
	char err[96] = "";

	CHECK(dsp_config_from_json(json, &cfg, err, sizeof(err)), "parse: %s", err);
	CHECK(cfg.enabled && cfg.role == DSP_ROLE_LEFT && cfg.preamp_db == -6 && cfg.highpass_hz == 45, "top level fields");
	CHECK(cfg.band_count == 2 && cfg.bands[0].type == DSP_PEAK && cfg.bands[0].q == 1.4f, "first band");
	CHECK(cfg.bands[1].type == DSP_HIGHSHELF && fabsf(cfg.bands[1].q - 0.7071f) < 1e-3f && cfg.bands[1].channels == DSP_CH_RIGHT, "second band defaults");
	CHECK(cfg.gain_db[1] == -1.5f && cfg.delay_ms[0] == 0.25f && cfg.delay_ms[1] == 0.25f, "pairs");
	CHECK(!cfg.invert[0] && cfg.invert[1] && !cfg.dither, "bools");

	char *out = dsp_config_to_json(&cfg);
	CHECK(out && strstr(out, "\"q\":1.4,"), "compact numbers: %s", out ? out : "(null)");
	CHECK(dsp_config_from_json(out, &again, err, sizeof(err)), "re-parse: %s", err);
	CHECK(config_equal(&cfg, &again), "round trip is lossless");
	free(out);

	CHECK(dsp_config_from_json("", &cfg, err, sizeof(err)) && !cfg.enabled, "empty means defaults");
}

static void test_json_errors(void) {
	static const struct { const char *json, *expect; } cases[] = {
		{ "[1,2]", "not a JSON object" },
		{ "{\"enabeld\":true}", "unknown key 'enabeld'" },
		{ "{\"role\":\"center\"}", "role must be" },
		{ "{\"preamp\":\"-6\"}", "'preamp' must be a number" },
		{ "{\"preamp\":30}", "preamp must be within" },
		{ "{\"delay\":[1,2,3]}", "'delay' must be a number or [left, right]" },
		{ "{\"delay\":80}", "delay must be within" },
		{ "{\"eq\":[{\"type\":\"peak\"}]}", "need at least a type and a freq" },
		{ "{\"eq\":[{\"type\":\"bell\",\"freq\":100}]}", "eq type must be" },
		{ "{\"eq\":[{\"type\":\"peak\",\"freq\":100,\"q\":0}]}", "q must be within" },
		{ "{\"eq\":[{\"type\":\"peak\",\"freq\":100,\"ch\":\"center\"}]}", "eq ch must be" },
		{ "{\"eq\":[{\"type\":\"peak\",\"freq\":100,\"Q\":1}]}", "unknown eq key 'Q'" },
	};
	char err[96];
	dsp_config_t cfg;

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		err[0] = '\0';
		bool ok = dsp_config_from_json(cases[i].json, &cfg, err, sizeof(err));
		CHECK(!ok && strstr(err, cases[i].expect), "%s -> '%s' (expected '%s')", cases[i].json, err, cases[i].expect);
	}

	char many[2048] = "{\"eq\":[";
	for (int i = 0; i <= DSP_MAX_BANDS; i++) strcat(many, i ? ",{\"type\":\"peak\",\"freq\":100}" : "{\"type\":\"peak\",\"freq\":100}");
	strcat(many, "]}");
	CHECK(!dsp_config_from_json(many, &cfg, err, sizeof(err)) && strstr(err, "too many"), "band limit: %s", err);
}

int main(void) {
	struct { const char *name; void (*fn)(void); } tests[] = {
		{ "band responses match the RBJ cookbook", test_band_responses_match_rbj },
		{ "peak and shelf gains", test_peak_gain_at_center },
		{ "LR4 crossover", test_lr4_crossover },
		{ "low cutoff noise floor", test_low_cutoff_noise_floor },
		{ "channel roles", test_roles },
		{ "sub role", test_sub_role },
		{ "delay", test_delay },
		{ "bit exact passthrough", test_passthrough },
		{ "clipping and peak meter", test_clipping },
		{ "TPDF dither", test_dither },
		{ "live updates", test_live_update_keeps_state },
		{ "JSON round trip", test_json_roundtrip },
		{ "JSON errors", test_json_errors },
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); i++) {
		int before = failures;
		printf("%s\n", tests[i].name);
		tests[i].fn();
		printf("  %s\n", failures == before ? "ok" : "FAILED");
	}

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
