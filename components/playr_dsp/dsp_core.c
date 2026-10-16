/*
 *  Playr output stage: channel role, crossover, parametric EQ, gain and delay
 *
 *  See dsp_core.h for the processing order. Filter design follows A. Simper,
 *  "Linear Trap Optimised SVF" (cytomic.com), whose responses are identical to
 *  the RBJ Audio EQ Cookbook ones (dsp_band_response_db uses RBJ to check that).
 */

#include <math.h>
#include <string.h>
#include "dsp_core.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DSP_HOT IRAM_ATTR
#else
#define DSP_HOT
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BUTTERWORTH_Q	0.70710678118654752
#define MAX_FREQ_RATIO	0.49		// of the sample rate, close to Nyquist

static const char *role_names[] = { "stereo", "left", "right", "mono", "sub" };
static const char *filter_names[] = { "peak", "lowshelf", "highshelf", "lowpass", "highpass" };

#define COUNT(a) (sizeof(a) / sizeof(*(a)))
#define IN_RANGE(x, lo, hi) ((x) >= (lo) && (x) <= (hi))		// false for NaN

/****************************************************************************************
 * names
 */
const char *dsp_role_name(dsp_role_t role) {
	return (unsigned) role < COUNT(role_names) ? role_names[role] : "?";
}

bool dsp_role_parse(const char *name, dsp_role_t *role) {
	for (size_t i = 0; name && i < COUNT(role_names); i++) {
		if (!strcmp(name, role_names[i])) {
			*role = (dsp_role_t) i;
			return true;
		}
	}
	return false;
}

const char *dsp_filter_name(dsp_filter_type_t type) {
	return (unsigned) type < COUNT(filter_names) ? filter_names[type] : "?";
}

bool dsp_filter_parse(const char *name, dsp_filter_type_t *type) {
	for (size_t i = 0; name && i < COUNT(filter_names); i++) {
		if (!strcmp(name, filter_names[i])) {
			*type = (dsp_filter_type_t) i;
			return true;
		}
	}
	return false;
}

/****************************************************************************************
 * configuration
 */
void dsp_config_default(dsp_config_t *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->role = DSP_ROLE_STEREO;
	cfg->dither = true;
}

const char *dsp_config_check(const dsp_config_t *cfg) {
	if ((unsigned) cfg->role >= COUNT(role_names)) return "unknown role";
	if (!IN_RANGE(cfg->preamp_db, -60.0f, 24.0f)) return "preamp must be within -60..24 dB";
	if (cfg->highpass_hz != 0 && !IN_RANGE(cfg->highpass_hz, 10.0f, 24000.0f)) return "highpass must be 0 (off) or 10..24000 Hz";
	if (cfg->lowpass_hz != 0 && !IN_RANGE(cfg->lowpass_hz, 10.0f, 24000.0f)) return "lowpass must be 0 (off) or 10..24000 Hz";
	if (!IN_RANGE(cfg->band_count, 0, DSP_MAX_BANDS)) return "too many eq bands";

	for (int i = 0; i < cfg->band_count; i++) {
		const dsp_band_t *b = cfg->bands + i;
		if ((unsigned) b->type >= COUNT(filter_names)) return "unknown eq band type";
		if (!IN_RANGE(b->freq, 10.0f, 24000.0f)) return "eq band freq must be within 10..24000 Hz";
		if (!IN_RANGE(b->q, 0.05f, 50.0f)) return "eq band q must be within 0.05..50";
		if (!IN_RANGE(b->gain_db, -30.0f, 30.0f)) return "eq band gain must be within -30..30 dB";
		if (!IN_RANGE(b->channels, 1, DSP_CH_BOTH)) return "eq band channel must be left, right or both";
	}

	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		if (!IN_RANGE(cfg->gain_db[ch], -60.0f, 24.0f)) return "gain must be within -60..24 dB";
		if (!IN_RANGE(cfg->delay_ms[ch], 0.0f, DSP_MAX_DELAY_MS)) return "delay must be within 0..50 ms";
	}

	return NULL;
}

/****************************************************************************************
 * filter design
 */

// clamp the corner below Nyquist; bands that would sit above it are bypassed instead
static double effective_freq(dsp_filter_type_t type, float freq, uint32_t samplerate, bool *bypass) {
	double max = MAX_FREQ_RATIO * samplerate;
	*bypass = freq > max && type != DSP_HIGHPASS;
	return freq > max ? max : freq;
}

void dsp_svf_design(dsp_svf_t *f, dsp_filter_type_t type, float freq, float q, float gain_db, uint32_t samplerate) {
	bool bypass;
	double fc = effective_freq(type, freq, samplerate, &bypass);
	double A = pow(10.0, gain_db / 40.0);
	double g = tan(M_PI * fc / samplerate);
	double k = 1.0 / q;
	double m0 = 1, m1 = 0, m2 = 0;

	if (!bypass) switch (type) {
	case DSP_PEAK:
		k = 1.0 / (q * A);
		m1 = k * (A * A - 1);
		break;
	case DSP_LOWSHELF:
		g /= sqrt(A);
		m1 = k * (A - 1);
		m2 = A * A - 1;
		break;
	case DSP_HIGHSHELF:
		g *= sqrt(A);
		m0 = A * A;
		m1 = k * (1 - A) * A;
		m2 = 1 - A * A;
		break;
	case DSP_LOWPASS:
		m0 = 0;
		m2 = 1;
		break;
	case DSP_HIGHPASS:
		m1 = -k;
		m2 = -1;
		break;
	}

	double a1 = 1.0 / (1.0 + g * (g + k));
	f->a1 = a1;
	f->a2 = g * a1;
	f->a3 = g * g * a1;
	f->m0 = m0;
	f->m1 = m1;
	f->m2 = m2;
}

void DSP_HOT dsp_svf_run(dsp_svf_t *f, float *x, size_t frames) {
	const float a1 = f->a1, a2 = f->a2, a3 = f->a3;
	const float m0 = f->m0, m1 = f->m1, m2 = f->m2;
	float ic1 = f->ic1, ic2 = f->ic2;

	for (size_t i = 0; i < frames; i++) {
		float v0 = x[i];
		float v3 = v0 - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.0f * v1 - ic1;
		ic2 = 2.0f * v2 - ic2;
		x[i] = m0 * v0 + m1 * v1 + m2 * v2;
	}

	// let tails decay to a true zero rather than crawling through denormals
	f->ic1 = fabsf(ic1) < 1e-20f ? 0.0f : ic1;
	f->ic2 = fabsf(ic2) < 1e-20f ? 0.0f : ic2;
}

double dsp_band_response_db(dsp_filter_type_t type, float freq, float q, float gain_db, uint32_t samplerate, double at_hz) {
	bool bypass;
	double fc = effective_freq(type, freq, samplerate, &bypass);
	if (bypass) return 0.0;

	// RBJ Audio EQ Cookbook, independent from the SVF form used for processing
	double A = pow(10.0, gain_db / 40.0);
	double w0 = 2 * M_PI * fc / samplerate;
	double cw = cos(w0), alpha = sin(w0) / (2 * q), sa = 2 * sqrt(A) * alpha;
	double b0, b1, b2, a0, a1, a2;

	switch (type) {
	case DSP_PEAK:
		b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
		a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
		break;
	case DSP_LOWSHELF:
		b0 = A * ((A + 1) - (A - 1) * cw + sa);
		b1 = 2 * A * ((A - 1) - (A + 1) * cw);
		b2 = A * ((A + 1) - (A - 1) * cw - sa);
		a0 = (A + 1) + (A - 1) * cw + sa;
		a1 = -2 * ((A - 1) + (A + 1) * cw);
		a2 = (A + 1) + (A - 1) * cw - sa;
		break;
	case DSP_HIGHSHELF:
		b0 = A * ((A + 1) + (A - 1) * cw + sa);
		b1 = -2 * A * ((A - 1) + (A + 1) * cw);
		b2 = A * ((A + 1) + (A - 1) * cw - sa);
		a0 = (A + 1) - (A - 1) * cw + sa;
		a1 = 2 * ((A - 1) - (A + 1) * cw);
		a2 = (A + 1) - (A - 1) * cw - sa;
		break;
	case DSP_LOWPASS:
		b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
		a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
		break;
	case DSP_HIGHPASS:
	default:
		b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
		a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
		break;
	}

	double w = 2 * M_PI * at_hz / samplerate;
	double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
	double nr = b0 + b1 * c1 + b2 * c2, ni = b1 * s1 + b2 * s2;
	double dr = a0 + a1 * c1 + a2 * c2, di = a1 * s1 + a2 * s2;
	return 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/****************************************************************************************
 * state
 */
void dsp_state_init(dsp_state_t *st, float *delay_mem, size_t delay_frames) {
	memset(st, 0, sizeof(*st));
	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		st->delay[ch].line = delay_mem ? delay_mem + ch * delay_frames : NULL;
		st->delay[ch].capacity = delay_mem ? delay_frames : 0;
	}
	st->rng = 0x9e3779b9;
	st->samplerate = 44100;
}

static int build_chain(dsp_svf_t *chain, const dsp_config_t *cfg, uint8_t mask, uint32_t samplerate) {
	float lowpass = cfg->lowpass_hz;
	int n = 0;

	if (!lowpass && cfg->role == DSP_ROLE_SUB) lowpass = DSP_SUB_DEFAULT_HZ;

	// Linkwitz-Riley 4th order = two cascaded 2nd order Butterworth sections
	for (int i = 0; cfg->highpass_hz && i < 2; i++) {
		dsp_svf_design(chain + n++, DSP_HIGHPASS, cfg->highpass_hz, BUTTERWORTH_Q, 0, samplerate);
	}
	for (int i = 0; lowpass && i < 2; i++) {
		dsp_svf_design(chain + n++, DSP_LOWPASS, lowpass, BUTTERWORTH_Q, 0, samplerate);
	}
	for (int i = 0; i < cfg->band_count; i++) {
		const dsp_band_t *b = cfg->bands + i;
		if (b->channels & mask) dsp_svf_design(chain + n++, b->type, b->freq, b->q, b->gain_db, samplerate);
	}

	return n;
}

void dsp_state_configure(dsp_state_t *st, const dsp_config_t *cfg, uint32_t samplerate) {
	bool was_shared = st->shared;
	bool same_bands = true;

	st->samplerate = samplerate;
	st->role = cfg->role;
	st->dither = cfg->dither;
	st->preamp = powf(10.0f, cfg->preamp_db / 20.0f);

	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		dsp_svf_t chain[DSP_MAX_FILTERS];
		int count = build_chain(chain, cfg, ch ? DSP_CH_RIGHT : DSP_CH_LEFT, samplerate);
		bool keep = count == st->filter_count[ch];

		// redesign in place so a gain or frequency tweak during playback does not click;
		// SVF states stay meaningful across coefficient changes
		for (int i = 0; i < count; i++) {
			chain[i].ic1 = keep ? st->filter[ch][i].ic1 : 0.0f;
			chain[i].ic2 = keep ? st->filter[ch][i].ic2 : 0.0f;
		}
		memcpy(st->filter[ch], chain, count * sizeof(*chain));
		st->filter_count[ch] = count;

		st->out_gain[ch] = powf(10.0f, cfg->gain_db[ch] / 20.0f) * (cfg->invert[ch] ? -1.0f : 1.0f);

		dsp_delay_t *d = st->delay + ch;
		size_t length = (size_t) lroundf(cfg->delay_ms[ch] * samplerate / 1000.0f);
		if (length > d->capacity) length = d->capacity;
		if (length != d->length) {
			if (d->line) memset(d->line, 0, d->capacity * sizeof(float));
			d->length = length;
			d->pos = 0;
		}
	}

	for (int i = 0; i < cfg->band_count; i++) same_bands &= cfg->bands[i].channels == DSP_CH_BOTH;

	st->shared = cfg->role != DSP_ROLE_STEREO && same_bands && st->delay[0].length == st->delay[1].length;

	// right channel states are stale after running shared
	if (was_shared && !st->shared) {
		for (int i = 0; i < st->filter_count[1]; i++) st->filter[1][i].ic1 = st->filter[1][i].ic2 = 0.0f;
	}

	// enabled but neutral stays bit exact
	bool neutral = cfg->role == DSP_ROLE_STEREO && cfg->preamp_db == 0 &&
				   !st->filter_count[0] && !st->filter_count[1] &&
				   !st->delay[0].length && !st->delay[1].length &&
				   st->out_gain[0] == 1.0f && st->out_gain[1] == 1.0f;

	st->active = cfg->enabled && !neutral;
}

void dsp_state_reset(dsp_state_t *st) {
	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		for (int i = 0; i < st->filter_count[ch]; i++) st->filter[ch][i].ic1 = st->filter[ch][i].ic2 = 0.0f;
		if (st->delay[ch].line) memset(st->delay[ch].line, 0, st->delay[ch].capacity * sizeof(float));
		st->delay[ch].pos = 0;
	}
}

void dsp_stats_reset(dsp_state_t *st) {
	st->clipped = 0;
	st->peak = 0.0f;
}

/****************************************************************************************
 * processing
 */
static void DSP_HOT delay_run(dsp_delay_t *d, float *x, size_t frames) {
	float *line = d->line;
	size_t pos = d->pos, length = d->length;

	if (!length) return;

	for (size_t i = 0; i < frames; i++) {
		float y = line[pos];
		line[pos] = x[i];
		x[i] = y;
		if (++pos == length) pos = 0;
	}

	d->pos = pos;
}

void DSP_HOT dsp_filter_block(dsp_state_t *st, float *l, float *r, size_t frames) {
	for (int i = 0; i < st->filter_count[0]; i++) dsp_svf_run(&st->filter[0][i], l, frames);
	delay_run(st->delay, l, frames);

	if (st->shared) return;

	for (int i = 0; i < st->filter_count[1]; i++) dsp_svf_run(&st->filter[1][i], r, frames);
	delay_run(st->delay + 1, r, frames);
}

// role mix and preamp, in a single pass; shared mode only needs the left buffer
#define MIX_IN(T, SCALE)																\
	static void DSP_HOT mix_in_##T(const dsp_state_t *st, const T *in, float *l, float *r, size_t n) {	\
		const float k = st->preamp * (SCALE);											\
		switch (st->role) {																\
		case DSP_ROLE_STEREO:															\
			for (size_t i = 0; i < n; i++) { l[i] = in[2*i] * k; r[i] = in[2*i+1] * k; }	\
			break;																		\
		case DSP_ROLE_LEFT:																\
		case DSP_ROLE_RIGHT: {															\
			const T *src = in + (st->role == DSP_ROLE_RIGHT);							\
			for (size_t i = 0; i < n; i++) l[i] = src[2*i] * k;							\
			if (!st->shared) memcpy(r, l, n * sizeof(float));							\
			break;																		\
		}																				\
		default: {																		\
			const float h = 0.5f * k;													\
			for (size_t i = 0; i < n; i++) l[i] = ((float) in[2*i] + (float) in[2*i+1]) * h;	\
			if (!st->shared) memcpy(r, l, n * sizeof(float));							\
			break;																		\
		}																				\
		}																				\
	}

MIX_IN(int16_t, 1.0f / 32768.0f)
MIX_IN(int32_t, 1.0f / 2147483648.0f)

static inline float tpdf(uint32_t *state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	// difference of two uniform [0, 1) values: triangular, +/- 1 LSB
	return ((float) (x & 0xffff) - (float) (x >> 16)) * (1.0f / 65536.0f);
}

static void DSP_HOT mix_out_int16_t(dsp_state_t *st, const float *l, const float *r, int16_t *out, size_t n) {
	float peak = 0.0f;		// in LSB here
	uint32_t clipped = st->clipped;

	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		const float *src = (ch && !st->shared) ? r : l;
		const float g = st->out_gain[ch] * 32768.0f;
		for (size_t i = 0; i < n; i++) {
			float v = src[i] * g;
			float a = fabsf(v);
			if (a > peak) peak = a;
			if (st->dither) v += tpdf(&st->rng);
			// offset to positive so that truncation rounds to nearest
			float t = v + 32768.5f;
			int32_t o;
			if (t < 0.0f) { o = -32768; clipped++; }
			else if (t >= 65536.0f) { o = 32767; clipped++; }
			else o = (int32_t) t - 32768;
			out[2*i + ch] = o;
		}
	}

	peak /= 32768.0f;
	if (peak > st->peak) st->peak = peak;
	st->clipped = clipped;
}

static void DSP_HOT mix_out_int32_t(dsp_state_t *st, const float *l, const float *r, int32_t *out, size_t n) {
	float peak = st->peak;
	uint32_t clipped = st->clipped;

	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		const float *src = (ch && !st->shared) ? r : l;
		const float g = st->out_gain[ch];
		for (size_t i = 0; i < n; i++) {
			float v = src[i] * g;
			float a = fabsf(v);
			if (a > peak) peak = a;
			int32_t o;
			if (v >= 1.0f) { o = INT32_MAX; clipped++; }
			else if (v < -1.0f) { o = INT32_MIN; clipped++; }
			else o = (int32_t) (v * 2147483648.0f);
			out[2*i + ch] = o;
		}
	}

	st->peak = peak;
	st->clipped = clipped;
}

#define PROCESS(T, NAME)																	\
	void NAME(dsp_state_t *st, T *buf, size_t frames, float *l, float *r, size_t chunk) {	\
		if (!st->active) return;															\
		while (frames) {																	\
			size_t n = frames < chunk ? frames : chunk;										\
			mix_in_##T(st, buf, l, r, n);													\
			dsp_filter_block(st, l, r, n);													\
			mix_out_##T(st, l, r, buf, n);													\
			buf += 2 * n;																	\
			frames -= n;																	\
		}																					\
	}

PROCESS(int16_t, dsp_process_s16)
PROCESS(int32_t, dsp_process_s32)
