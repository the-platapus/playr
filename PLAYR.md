# Playr

A small, self-contained audio player module built on squeezelite-esp32. It plays
whatever your device streams (AirPlay, Spotify Connect, Bluetooth, LMS/Squeezebox)
and drives an I2S DAC. Each module knows its own job in a system (stereo, left,
right, mono, sub) and carries its own per-speaker tuning (crossover, EQ, delay),
so it can be dropped into any amplifier or DIY speaker.

This repository is a fork of [sle118/squeezelite-esp32](https://github.com/sle118/squeezelite-esp32).
Upstream does the heavy lifting (protocols, codecs, web UI, OTA). Playr adds the
module-level features on top, keeping changes to upstream files small so that
merging upstream stays easy.

## Roadmap

1. **Single node** on ESP32-WROVER + PCM5102A: flash, configure, test every input.
2. **Output stage** (`components/playr_dsp`): channel role, LR4 crossover,
   parametric EQ, gain, polarity and delay, stored per device in NVS, applied to
   every source. *Done in firmware, to be validated on hardware.*
3. **Leader / follower sync**: the node receiving a stream relays PCM to the
   others with a shared clock (start from an existing ESP32 Snapcast client).
   Goal: sub-millisecond L/R alignment with drift correction.
4. **Module interface**: 5 V in, I2S out, analog line out, I2C/UART for amp control.
5. **PCB**, once the software is proven. Separate LDO for the DAC analog supply,
   keep it away from the RF section.

Protocol notes: Bluetooth A2DP needs the classic ESP32 (the S3/C3/C6 are BLE only).
AirPlay 1 and Spotify Connect work, AirPlay 2 and Google Cast don't (closed),
Miracast is screen mirroring. DLNA/UPnP is the candidate to cover Windows "Cast to
device" and many Android apps.

## Branches and remotes

| | |
|---|---|
| `origin` | https://github.com/the-platapus/playr (this fork) |
| `upstream` | https://github.com/sle118/squeezelite-esp32 |
| `master-v4.3` | tracks upstream, never commit here |
| `playr` | Playr development base |
| `feature/*` | one feature at a time, merged into `playr` |

Pull upstream changes with `git fetch upstream && git merge upstream/master-v4.3`
on `playr`. Note upstream's [CONTRIBUTING.md](CONTRIBUTING.md) before sending
anything back: they don't accept AI-generated PRs that the author cannot fully
explain and maintain.

## Development environment

Everything builds in the same docker image upstream CI uses
(`sle118/squeezelite-esp32-idfv448:4.4.8`, ESP-IDF 4.4.8), so local builds match
releases. `scripts/playr` wraps it.

One-time setup (Ubuntu):

```bash
sudo apt-get install -y docker.io && sudo usermod -aG docker $USER
```

Log out and back in for the docker group to apply, then:

```bash
scripts/playr image     # pull the ~6 GB build image
scripts/playr test      # host unit tests for the output stage (just gcc)
scripts/playr build     # build/recovery.bin + build/squeezelite.bin
scripts/playr flash     # first time: bootloader, partitions, both apps
scripts/playr monitor   # serial console and logs (Ctrl+] to exit)
```

Daily loop: `scripts/playr build && scripts/playr app && scripts/playr monitor`
(`app` only rewrites the squeezelite partition, which is much faster).

Settings go in the environment or in a gitignored `.env.playr` at the root:

```bash
PLAYR_TARGET=I2S-4MFlash    # build-scripts/<target>-sdkconfig.defaults
PLAYR_DEPTH=16              # 16 or 32 bit samples
PLAYR_PORT=/dev/ttyUSB0     # otherwise the first ttyUSB/ttyACM found
SPOTIFY_SECRET='-DCLIENT_ID="..." -DCLIENT_SECRET="..."'
```

The build config is `sdkconfig.playr` (gitignored), created from the target
defaults on first build; `scripts/playr menuconfig` edits it and
`scripts/playr config` resets it. The tracked `sdkconfig` is upstream's and is
left alone.

Spotify Connect: upstream release builds embed a Spotify client id and secret
from their CI secrets. Local builds need your own (Spotify developer dashboard)
in `SPOTIFY_SECRET`, otherwise cspot may not be able to log in.

## First boot

Wiring for a PCM5102A board on a WROVER (avoids the PSRAM pins 16/17 and the
strapping pins):

| PCM5102A | ESP32 |
|---|---|
| VCC, 3.3V, XMT | 3.3V |
| GND, FLT, DMP, SCK, FMT | GND |
| BCK | GPIO 26 |
| LCK | GPIO 25 |
| DIN | GPIO 22 |

1. Join the `squeezelite` Wi-Fi AP (password `squeezelite`) and open 192.168.4.1.
2. Pick your Wi-Fi network; note the address the module gets.
3. In the web UI set `dac_config` to `model=I2S,bck=26,ws=25,do=22` and reboot.
4. AirPlay, Bluetooth and Spotify should now show the player; LMS finds it too.

Telnet logs: set NVS `telnet_enable` to `D` (telnet, still mirrored on serial)
or `Y` (telnet only). The `dsp` command (below) works over both.

## Output stage

See [components/playr_dsp/README.md](components/playr_dsp/README.md).
