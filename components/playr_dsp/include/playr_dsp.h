/*
 *  Playr output stage, squeezelite-esp32 glue
 *
 *  Runs in the I2S output thread on every block, whatever the source (LMS,
 *  AirPlay, Bluetooth, Spotify), after the LMS equalizer and before the DMA.
 *  Settings live in NVS "dsp_config" (JSON, see dsp_config.h) and are tuned
 *  live with the "dsp" console command.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

void playr_dsp_init(void);
void playr_dsp_set_samplerate(uint32_t samplerate);
void playr_dsp_reset(void);
void playr_dsp_process_s16(int16_t *buf, size_t frames);
void playr_dsp_process_s32(int32_t *buf, size_t frames);
void register_dsp_cmd(void);
