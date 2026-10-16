/*
 *  Playr output stage: channel role, crossover, parametric EQ, gain and delay
 *
 *  Portable C (no ESP-IDF dependency) so it can be unit tested on the host.
 *  Filters are linear trapezoidal state variable filters (A. Simper, Cytomic),
 *  which give the same responses as the RBJ cookbook biquads but keep a low
 *  noise floor in float32 even for very low cutoffs (sub crossovers).
 *
 *  Processing order for each block:
 *    role mix + preamp -> highpass (LR4) -> lowpass (LR4) -> EQ bands
 *    -> delay -> output gain / polarity -> dither + requantize
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSP_CHANNELS		2
#define DSP_MAX_BANDS		16
#define DSP_MAX_FILTERS		(DSP_MAX_BANDS + 4)		// + 2 x LR4 sections
#define DSP_MAX_DELAY_MS	50.0f
#define DSP_SUB_DEFAULT_HZ	80.0f					// sub lowpass when none set

typedef enum {
	DSP_ROLE_STEREO = 0,	// L -> L, R -> R
	DSP_ROLE_LEFT,			// L on both outputs
	DSP_ROLE_RIGHT,			// R on both outputs
	DSP_ROLE_MONO,			// (L + R) / 2 on both outputs
	DSP_ROLE_SUB,			// mono, lowpassed at DSP_SUB_DEFAULT_HZ unless set
} dsp_role_t;

typedef enum {
	DSP_PEAK = 0,
	DSP_LOWSHELF,
	DSP_HIGHSHELF,
	DSP_LOWPASS,
	DSP_HIGHPASS,
} dsp_filter_type_t;

#define DSP_CH_LEFT		0x01
#define DSP_CH_RIGHT	0x02
#define DSP_CH_BOTH		(DSP_CH_LEFT | DSP_CH_RIGHT)

typedef struct {
	dsp_filter_type_t type;
	float freq;			// Hz
	float q;
	float gain_db;		// peak and shelves only
	uint8_t channels;	// DSP_CH_* mask of output channels
} dsp_band_t;

typedef struct {
	bool enabled;
	dsp_role_t role;
	float preamp_db;				// before the filters: headroom for EQ boosts
	float highpass_hz;				// Linkwitz-Riley 24 dB/oct, 0 = off
	float lowpass_hz;				// Linkwitz-Riley 24 dB/oct, 0 = off
	int band_count;
	dsp_band_t bands[DSP_MAX_BANDS];
	float gain_db[DSP_CHANNELS];	// per output channel trim
	float delay_ms[DSP_CHANNELS];	// per output channel delay
	bool invert[DSP_CHANNELS];		// per output channel polarity
	bool dither;					// TPDF dither when going back to 16 bits
} dsp_config_t;

typedef struct {
	float a1, a2, a3;		// integrator coefficients
	float m0, m1, m2;		// output mix
	float ic1, ic2;			// integrator states
} dsp_svf_t;

typedef struct {
	float *line;
	size_t capacity, length, pos;
} dsp_delay_t;

typedef struct {
	bool active;
	bool shared;			// both outputs identical: filter once, copy
	dsp_role_t role;
	uint32_t samplerate;
	float preamp;
	float out_gain[DSP_CHANNELS];
	int filter_count[DSP_CHANNELS];
	dsp_svf_t filter[DSP_CHANNELS][DSP_MAX_FILTERS];
	dsp_delay_t delay[DSP_CHANNELS];
	bool dither;
	uint32_t rng;
	uint32_t clipped;		// samples clipped since dsp_stats_reset
	float peak;				// largest |sample| before requantize, 1.0 = 0 dBFS
} dsp_state_t;

void dsp_config_default(dsp_config_t *cfg);
const char *dsp_config_check(const dsp_config_t *cfg);	// NULL when valid

/* delay_mem holds DSP_CHANNELS * delay_frames floats and stays owned by the caller */
void dsp_state_init(dsp_state_t *st, float *delay_mem, size_t delay_frames);
void dsp_state_configure(dsp_state_t *st, const dsp_config_t *cfg, uint32_t samplerate);
void dsp_state_reset(dsp_state_t *st);
void dsp_stats_reset(dsp_state_t *st);

/* interleaved stereo, in place; l/r are scratch buffers of chunk frames each */
void dsp_process_s16(dsp_state_t *st, int16_t *buf, size_t frames, float *l, float *r, size_t chunk);
void dsp_process_s32(dsp_state_t *st, int32_t *buf, size_t frames, float *l, float *r, size_t chunk);

/* planar float building blocks used by the above (and by the tests) */
void dsp_filter_block(dsp_state_t *st, float *l, float *r, size_t frames);

void dsp_svf_design(dsp_svf_t *f, dsp_filter_type_t type, float freq, float q, float gain_db, uint32_t samplerate);
void dsp_svf_run(dsp_svf_t *f, float *x, size_t frames);
double dsp_band_response_db(dsp_filter_type_t type, float freq, float q, float gain_db, uint32_t samplerate, double at_hz);

const char *dsp_role_name(dsp_role_t role);
bool dsp_role_parse(const char *name, dsp_role_t *role);
const char *dsp_filter_name(dsp_filter_type_t type);
bool dsp_filter_parse(const char *name, dsp_filter_type_t *type);
