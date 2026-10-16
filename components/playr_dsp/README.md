# playr_dsp: the output stage

Per-device processing between squeezelite's output buffer and the I2S DMA, so it
applies to every source (LMS, AirPlay, Bluetooth, Spotify) and to S/PDIF output.
It runs after upstream's 10-band LMS equalizer, which stays as the listener's
"taste" control; this stage is the speaker's own tuning.

```
 L R ──► role mix ──► preamp ──► highpass LR4 ──► lowpass LR4 ──► EQ bands ──► delay ──► gain/polarity ──► dither ──► I2S
```

| role | outputs |
|---|---|
| `stereo` | L → L, R → R |
| `left` / `right` | that channel on both outputs |
| `mono` | (L + R) / 2 on both outputs |
| `sub` | mono, LR4 lowpass at 80 Hz unless `lowpass` is set |

Disabled, or enabled with nothing to do, the stage is bypassed and output is
bit exact. When both outputs carry the same signal and settings (left, right,
mono and sub roles with `both` bands and equal delays), the chain runs once and
is copied, halving the CPU cost.

## Configuration

Stored as JSON in the NVS key `dsp_config` (also editable in the web UI NVS
editor, applied at boot). Missing keys keep their defaults:

```json
{
  "enabled": true,
  "role": "left",
  "preamp": -6,
  "highpass": 45,
  "lowpass": 0,
  "eq": [
    { "type": "peak", "freq": 120, "q": 1.4, "gain": -4.5 },
    { "type": "highshelf", "freq": 8000, "q": 0.707, "gain": -2, "ch": "right" }
  ],
  "gain": [0, -1.5],
  "delay": 0.25,
  "invert": false,
  "dither": true
}
```

`gain` (dB), `delay` (ms, max 50) and `invert` take one value for both outputs
or `[left, right]`. EQ types: `peak`, `lowshelf`, `highshelf`, `lowpass`,
`highpass`, up to 16 bands; `ch` is `left`, `right` or `both` (default).
Use `preamp` to make headroom for boosts: the `dsp` meter shows the peak level
and counts clipped samples.

## Live tuning from the console

Every change applies immediately, without clicks for parameter tweaks, and is
saved to NVS:

```
dsp                         show settings, peak level and clip count
dsp help
dsp on
dsp role left
dsp highpass 45
dsp eq add peak 120 1.4 -4.5
dsp eq set 1 peak 115 1.6 -5
dsp eq add highshelf 8000 0.707 -2 right
dsp eq del 2
dsp preamp -6
dsp delay 0.25
dsp meter                   peak / clipped since last call, then reset
dsp json                    print the config as JSON
dsp json {"enabled":true,"role":"sub","lowpass":90}
```

A 2.1 system, one Playr module per box (one command per line on each module):

| module | commands |
|---|---|
| left speaker | `dsp role left`, `dsp highpass 80`, `dsp on` |
| right speaker | `dsp role right`, `dsp highpass 80`, `dsp on` |
| subwoofer | `dsp role sub`, `dsp lowpass 80`, `dsp on` |

The satellites' 80 Hz LR4 highpass and the sub's 80 Hz LR4 lowpass sum flat.
Until leader/follower sync exists, each module needs to receive the same
stream itself (for example as an LMS sync group).

## Design notes

Filters are linear trapezoidal state variable filters (Andrew Simper, Cytomic)
rather than the esp-dsp biquads upstream ships. They produce exactly the RBJ
cookbook responses, but esp-dsp's float32 direct form II has a lot of internal
gain at low cutoffs, which shows up as noise. Measured against a double precision
reference on an LR4 highpass at 48 kHz (`host_test`):

| cutoff | SVF noise | float DF2 biquad noise |
|---|---|---|
| 20 Hz | −129 dBFS | −52 dBFS |
| 40 Hz | −131 dBFS | −60 dBFS |
| 80 Hz | −138 dBFS | −73 dBFS |

Coefficients are designed in double precision (only on configuration or sample
rate changes); the per-sample paths are float only, since the ESP32 does
doubles in software. 16-bit output gets TPDF dither (±1 LSB); 32-bit builds
are written without it. Delay lines live in PSRAM (50 ms per channel at up to
96 kHz, half that at 192 kHz).

## Files

- `dsp_core.c`: filters, chain, mixing, delay, requantization. Portable C.
- `dsp_config.c`: JSON form of the configuration (cJSON).
- `playr_dsp.c`: squeezelite glue (NVS, locking) and the `dsp` console command.
- `host_test/`: host unit tests, `scripts/playr test` (runs with ASan/UBSan).
