<div align=center>

<img src="extras/banner.png" alt="Banner" width="35%">

</div>
<h1 align=center>GTA: San Andreas - generic Linux port</h1>

> [!NOTE]
> if you are expecting support on how to install this port on your device, please see the [install section](#install).
> this port is provided as-is, [with no warranty or support](#support).
> please do not bother me or any other maintainers on general requests unless it involves code changes.
> and by then please open a PR on this repository.

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

After the AArch64 build and vendored runtime files are present, create an
auto-installable PortMaster archive with:

```sh
scripts/package-linux.sh 1.0.0
```

This writes `dist/gtasa-1.0.0.zip` and a `.sha256` sidecar using the checked-in
PortMaster metadata under `portmaster/gtasa/`. Its top-level archive layout is:

```text
gtasa/
├── port.json
├── README.md
├── screenshot.png
├── gameinfo.xml
├── Grand Theft Auto San Andreas.sh
└── gtasa/
    ├── licenses/
    ├── gtasa_linux
    ├── libs.aarch64/libSDL3.so.0
    ├── libc++_shared.so
    ├── assetfile.txt
    └── Adjustable.cfg
```

The archive deliberately does not contain `libGame.so` or proprietary game
assets; those are added from the matching official Android package during
installation. Set `GTASA_CONSOLE_UI=0` when invoking the packager to omit
`Adjustable.cfg`.

## Installation

Refer to [ASSET_PREPARATION.md](./ASSET_PREPARATION.md) for how to prepare the game assets.

Install the package onto your favorite frontend (or just run `gtasa_linux` raw if your system supports native SDL3 and Wayland)


## Support

As with other normal open-source projects, support is provided on a best-effort basis. Do not make issues or expect
any guarantees regarding support or bugfixes.

No warranty is provided, as according to [LICENSE](./LICENSE)

## Contributing

Contributions are very welcome! feel free to open issues or submit pull requests as long as it's code related.
Note that patches should try to preserve the original Android port behavior as much as possible, with extra hooks/tweaks being
as minimal as possible. Do not include any game mods or hooks that are not part of the original Android port, unless
for compatibility reasons.

This means no implementing features from various mods that modify gameplay behavior, or bugfixes from the original game.
If they are to be included however, they ***must be opt-in via a configuration option.***

### AML mod support

AML mod support is very basic and may not be working with all mods. This port is based on the latest version of the GSG Android port, not the older
v2.10 version. Most mods may not work with this port at all for this very reason.

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc.
or Rockstar Games, Inc. "Grand Theft Auto" and "Grand Theft Auto: San Andreas"
are trademarks of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included
in this project. We do not condone piracy in any way, shape or form and encourage
users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is
licensed under the MIT License. Please see the accompanying LICENSE file.
