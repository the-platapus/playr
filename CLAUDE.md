# Playr (fork of squeezelite-esp32)

Goals, roadmap, setup and branches: [PLAYR.md](PLAYR.md). Upstream docs: [README.md](README.md).

## Build and test

- `scripts/playr build | flash | app | monitor | test`. Builds run in upstream's CI docker
  image (ESP-IDF 4.4.8); the host's Python is too new for IDF 4.4, so don't install IDF natively.
- Default target is `I2S-4MFlash`, 16-bit (`PLAYR_TARGET`, `PLAYR_DEPTH`). The build config is the
  gitignored `sdkconfig.playr`. Never edit or commit the tracked `sdkconfig`.
- `scripts/playr test` runs the output-stage host tests (gcc, ASan/UBSan). Run it after any change
  in `components/playr_dsp`.
- Two apps are built from one tree: `recovery.bin` (factory) and `squeezelite.bin` (ota_0). Code that
  only the player needs is pulled in from `components/platform_console/app_squeezelite`.

## Working on the code

- Keep edits to upstream files small and commented so merging `upstream/master-v4.3` stays easy;
  put Playr features in their own components (`components/playr_*`).
- Upstream style: C, tabs, runtime configuration through NVS rather than compile-time `#ifdef`s per
  board.
- Audio path: every source (LMS, AirPlay `raop`, Bluetooth, Spotify `cspot`) ends up in squeezelite's
  `outputbuf`; `components/squeezelite/output_i2s.c` pulls blocks, runs `equalizer_process` then
  `playr_dsp_process_*`, then `i2s_write`.
- ESP32 limits: doubles are software-emulated (keep per-sample code float-only), internal RAM is
  tight with Wi-Fi + BT (big buffers go to PSRAM via `EXT_RAM_ATTR` / `MALLOC_CAP_SPIRAM`), hot loops
  can be `IRAM_ATTR`.

## Hardware

ESP32-WROVER (PSRAM, needed for BT A2DP) + PCM5102A on I2S: `dac_config` =
`model=I2S,bck=26,ws=25,do=22`.
