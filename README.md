<div align=center>

<img src="extras/banner.png" alt="Banner" width="35%">

</div>
<h1 align=center>GTA: San Andreas - generic Linux port</h1>

This Linux/SDL3 port is based on the MIT-licensed
[gtasa_nx](https://github.com/NaGaa95/gtasa_nx) Android ARM64 loader and shims.
The Linux target is being aligned with the official **v2.11.311** `libGame.so`
(arm64-v8a), matching the upstream Android payload. It runs the user's Android
library natively in a minimal compatibility environment. Version-specific Switch
gameplay patches are not applied by the Linux target. The host input regression
suite covers v2.11.311's per-gamepad callback ABI and retains a fallback for the
older v2.11.264 count-based ABI; AArch64 v2.11.311 target validation still
requires an ARM64 Play Store split.

i made this because there's weird shady "PortMaster" archives going around [from the R36S wiki](https://r36swiki.com/wiki-gtasa.html),
which seemed to have zero build provenance and i have zero clue how it's built, so I decided
to re-port it to a more generic target myself.

And by the way, there is literally no release for this game on PortMaster, so the source of these ports going
around is very shady

### PortMaster launcher

The launcher follows the PortMaster layout and is intended to be started by
PortMaster/EmulationStation:

```text
ports/
├── Grand Theft Auto San Andreas.sh
└── gtasa/
    ├── gtasa_linux
    ├── libs.aarch64/
    │   └── libSDL3.so.0
    ├── libGame.so
    ├── libc++_shared.so
    ├── assetfile.txt
    ├── Adjustable.cfg
    ├── data/
    ├── models/
    ├── texdb/
    └── audio/
```

The launcher sources PortMaster's `control.txt`, imports its controller
mapping, requires an AArch64 device, runs `pm_platform_helper`, and finishes
through `pm_finish`. It writes `gtasa/log.txt` for frontend launches. The
recommended Android extraction workflow is the
[GTAExtractor helper app](https://github.com/RyouVC/GTAExtractor/). The complete
asset inventory and advanced manual extraction guide are in
[ASSET_PREPARATION.md](ASSET_PREPARATION.md).

`libs.aarch64/libSDL3.so.0` is built from the SDL3-to-SDL2 backend fork. It
loads the device's system `libSDL2-2.0.so.0` dynamically so PortMaster systems
retain their patched SDL2 video, audio, and joystick backends. SDL2 is not
bundled. The reproducible workflow uses the PortMaster AArch64 builder image;
the GitHub Actions workflow builds this shim rather than native SDL3.

### Linux input and audio

- SDL3 gamepads feed native controller callbacks directly: face buttons, D-pad,
  Start/Back, shoulders, stick clicks, sticks and triggers. No keyboard emulation
  or keyboard fallback. Startup discovery, hotplug, focus reset, and up to four
  contiguous controller slots are supported. `SDL_GAMECONTROLLERCONFIG` can
  override mappings; the launcher imports PortMaster's mapping when available.
- OpenAL Soft retains spatial mixing through `ALC_SOFT_loopback`; SDL3 owns the
  playback stream (48 kHz stereo float PCM). Knulli's native PipeWire socket is
  `/var/run/pipewire-0`; the launcher fills an unset `XDG_RUNTIME_DIR` and selects
  PipeWire when that socket exists, without overriding explicit audio choices.
- `GTASA_INPUT_DEBUG=1` logs native controller dispatch, and
  `GTASA_AUDIO_DEBUG=1` reports mixed/non-silent PCM counters every five seconds.
  Counters are diagnostics, not proof that speakers are audible.
- The TRIMUI Smart Pro S controller and game audio were user-confirmed on Knulli.

### Exit controls

The launcher handles a PortMaster-style quit chord outside the Android game
input layer: hold **Guide/Home and press Start**, or hold **Back/Select/Minus and
press Start**. This requests a clean shutdown of the native game, audio stream, and
SDL controller handles. Releasing only one button does not exit. A standalone
SDL window's close/quit event also exits.

The game itself receives Start and Back as ordinary native gamepad buttons. The
v2.11.264 Android `implOnBackButtonPressed` entry point is a no-op, so the port
does not claim that Back alone pauses or exits the game.

The quit chord is a compile-time option and is enabled by default. Disable it
with `-DGTASA_QUIT_CHORD=OFF` when configuring CMake, or explicitly enable it
with `-DGTASA_QUIT_CHORD=ON`.

Host tests (SDL3/OpenAL development packages required):

```sh
cmake -S . -B build-linux -DBUILD_TESTING=ON
cmake --build build-linux -j2
ctest --test-dir build-linux --output-on-failure
```

The input test uses SDL virtual gamepads and mocked native callbacks; the audio
test uses real OpenAL mixing with SDL's dummy output. On-device, run
`audio_linux_test` with `SDL_AUDIO_DRIVER=pipewire` for two short tones, or set
`GTASA_AUDIO_TEST_MS=5000` for two five-second tones. See `AGENTS.md` for the
persistent cross-build and hardware-testing workflow.

### Binary release package

After the AArch64 build and vendored runtime files are present, create a
reproducible drop-in tarball with:

```sh
scripts/package-linux.sh 1.0.0
```

This writes `dist/gtasa-linux-1.0.0.tar.gz` and a `.sha256` sidecar. The archive
contains the launcher, `gtasa_linux`, `libs.aarch64/libSDL3.so.0`, the vendored Android NDK C++
runtime, and the 120-entry `assetfile.txt` manifest. It deliberately does not
contain `libGame.so` or proprietary game assets; those are added from the
matching official Android package during installation. The default package also
includes the existing `Adjustable.cfg` console-style HUD layout. Set
`GTASA_CONSOLE_UI=0` when invoking the packager to omit it.

### Original Switch installation (upstream reference only)

You're going to need:
* the **arm64-v8a** `.apk` (and `.obb`) for version **2.11.311**.

To install:
1. Create a folder called `gtasa` in the `switch` folder on your SD card.
2. From the **arm64-v8a** APK, extract these two native libraries to
   `/switch/gtasa/`:
   * `lib/arm64-v8a/libGame.so`
   * `lib/arm64-v8a/libc++_shared.so`
3. Extract the **game data** so the files sit loose under `/switch/gtasa/`,
   preserving their directory structure:
   * everything under the APK's `assets/` folder, **and**
   * the contents of the OBB (`main.*.obb` — it is just a ZIP; the `data/`,
     `models/`, `texdb/`, `audio/`, `text/`, `anim/`, `es2/`, … trees inside it
     go directly in `/switch/gtasa/`).
4. Copy `gtasa_nx.nro` into `/switch/gtasa/`.

Your SD card should end up with at least `/switch/gtasa/gtasa_nx.nro`,
`/switch/gtasa/libGame.so`, `/switch/gtasa/libc++_shared.so`, and the extracted
game data folders (`data/`, `models/`, `texdb/`, `audio/`, …) all inside
`/switch/gtasa/`.

### Notes

This will not work in applet/album mode (it needs the full memory + syscall set).
Launch it through a **game override** (hold R on an installed title) or a
forwarder.

Save games and settings are stored in `/switch/gtasa/`.

The port has a config file at `/switch/gtasa/gtasa_nx.cfg`, created on first run:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720 in
  handheld and 1920x1080 docked
* `trilinear_filter` — `1` forces trilinear texture filtering
* `show_fps` — `1` draws a small FPS counter in the top-left corner
* `fps_cap_30` — `1` enables the wrapper's 30 FPS cap
* `auto_boot_delay` — launcher countdown in seconds (`1`, `3`, `5`, or `10`)
* `ps2_corona_rotation` — `1` PS2 Corona Sun
* `ps2_color_filter` — `1` PS2 Color filter
* `sprint_any_surface` — `0` Sprinting on any surface is allowed
* `remove_air_resistance` — `0` Remove "ExtraAirResistance" flag
* `show_wanted_stars` — `0` Always drawable wanted stars
* `disable_ped_spec` — `1` Removed specular lighting on pedestrians
* `no_offscreen_despawn` — `0` Cars and peds don't despawn when you look away
* `mobile_widgets` — `0` Hidden Mobile Widgets

### Tips and Tricks

- You can input PC cheats by pressing **R3** + **L3** to open the on-screen keyboard. See [CHEATS.md](CHEATS.md) for available and unavailable cheats (you can input cheat codes in lowercase as well as uppercase).
- Due to expired licensing, some songs were cut from the game. See [MUSIC.md](MUSIC.md) for a list of removed tracks and a guide on how to restore them.
- Console-style HUD (optional). Drop a custom `Adjustable.cfg` into `switch/gtasa/` for the console HUD (e.g. radar in the bottom-left corner). Since **v2.11.311** no longer includes `data/360Default1280x720.cfg`, take it from the older **v2.10** build and rename it to `Adjustable.cfg`. It's a leftover from the Xbox 360 version.
- In order to reduce occasional stutters in-game, delete both `switch/gtasa/scache_small_low.txt` and `switch/gtasa/scache_small.txt`, then create a copy of the `switch/gtasa/scache.txt` file to have two version of it. (for example `scache(1).txt` so in the end you end up with both `scache.txt` and `scache(1).txt` inside the `switch/gtasa/` folder), then rename `scache.txt` to `scache_small.txt` and `scache(1).txt` to `scache_small_low.txt` . This will however make the loading screen longer since it needs to compile more shaders ahead.
  - Mesa stores its persistent single-file shader cache under `switch/gtasa/shadercache/mesa_shader_cache_sf`. Delete that directory only when diagnosing a corrupt cache.

### Mod Settings Menu

The SDL launcher opens on every boot with the game cover centered. Select the
cover to launch, or open **Options** to change the port's fixes and features.
Changes are saved immediately to `gtasa_nx.cfg`.

### How to build

**1. Install the Switch portlibs:**

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-mpg123 switch-ffmpeg switch-openal-soft switch-libexpat switch-libzstd switch-zlib
```

**2. Build the `.nro`:**

```sh
make
```

### Credits

* TheOfficialFloW for the method and the original PS Vita work;
* fgsfds for max_nx, which the shared Switch platform layer is based on;
* Gameplay and engine improvements ported from the [GTA:SA PS Vita port](https://github.com/TheOfficialFloW/gtasa_vita).
* Extra patches and hooks adapted from [JPatch](https://github.com/AndroidModLoader/JPatch).

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc.
or Rockstar Games, Inc. "Grand Theft Auto" and "Grand Theft Auto: San Andreas"
are trademarks of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included
in this project. We do not condone piracy in any way, shape or form and encourage
users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is
licensed under the MIT License. Please see the accompanying LICENSE file.
