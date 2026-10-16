/*
 *  Playr output stage: JSON form of dsp_config_t, as stored in NVS "dsp_config"
 *
 *  {
 *    "enabled": true,
 *    "role": "left",                stereo | left | right | mono | sub
 *    "preamp": -6,                  dB, before the filters
 *    "highpass": 0,                 Hz, LR4, 0 = off
 *    "lowpass": 0,                  Hz, LR4, 0 = off (sub role defaults to 80)
 *    "eq": [ { "type": "peak", "freq": 120, "q": 1.4, "gain": -4.5, "ch": "both" } ],
 *    "gain": [0, 0],                dB per output channel, or one number for both
 *    "delay": [0, 0],               ms per output channel, or one number for both
 *    "invert": [false, false],      per output channel, or one bool for both
 *    "dither": true
 *  }
 *
 *  Missing keys keep their defaults, unknown keys are rejected.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "dsp_core.h"

bool dsp_config_from_json(const char *json, dsp_config_t *cfg, char *err, size_t err_size);
char *dsp_config_to_json(const dsp_config_t *cfg);		// caller frees
