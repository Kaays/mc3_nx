# mc3_nx — Modern Combat 3: Fallen Nation for Nintendo Switch

An unofficial native wrapper that runs the **32-bit ARM (armeabi-v7a) Android
release of Modern Combat 3 1.1.7g** on the Nintendo Switch, as a 32-bit
(AArch32) Horizon process, on the [android32](https://github.com/aks796/android32)
runtime. The game's own engine runs natively on the Tegra X1: no emulation.

**No game code or assets are included.** You need your own copy of the game.

> Version **1.0.0** — status: **playable.** Tested on hardware through the first missions of the
> campaign: menus, gameplay, sound, the Gameloft logo and the briefing
> videos, pause / mission failed / debriefing screens, sleep and HOME.
> See [Status](#status).

## Requirements

- A Switch running Atmosphère, with support for AArch32 programs
  (current android32 ports: "Updated for FW 21+").
- [sphaira](https://github.com/ITotalJustice/sphaira) to install the forwarder
  (the 32-bit program only runs through its own HOME-menu icon).
- Modern Combat 3 **1.1.7g** for Android (`com.gameloft.android.ANMP.GloftM3HM`):
  - the APK (any file name), whose `lib/armeabi-v7a/libModernCombat3.so` has
    SHA-1 `be0d5e8779899e081a538b3930ec711d2df8aeb4`;
  - its two OBB files, from `/sdcard/Android/obb/com.gameloft.android.ANMP.GloftM3HM/`:
    `main.1120.com.gameloft.android.ANMP.GloftM3HM.obb` and
    `patch.11428.com.gameloft.android.ANMP.GloftM3HM.obb`.

## Installation

```
sdmc:/switch/mc3_nx/
├── mc3_nx.nro
├── <anything>.apk
└── obb/
    ├── main.1120.com.gameloft.android.ANMP.GloftM3HM.obb
    └── patch.11428.com.gameloft.android.ANMP.GloftM3HM.obb
```

1. Copy the files as above (the OBBs stay as they are: do not extract them).
2. In sphaira: *Homebrew → Modern Combat 3 → Install Forwarder*.
3. Start the game from its HOME-menu icon. The first start unpacks the
   library from the APK; `config.ini` and `debug.log` appear in the folder.

Saves go to `/switch/mc3_nx/data/`.

## Controls

| Button | In game | In menus |
| --- | --- | --- |
| Left stick | Move | — |
| Right stick | Look | — |
| ZR | Fire | Fire |
| ZL | Aim | Aim |
| Y | Reload | — |
| X | (Xperia "Y" button) | — |
| R | Grenade | — |
| L | Next weapon | — |
| B | Crouch | Back |
| A | — | Select |
| L3 (click) / D-pad up (hold) | Sprint | Up |
| D-pad down | Crouch | Down |
| D-pad left/right | Change weapon | Left/right |
| + / − | Start (pause) / Select | |
| Touch screen | On-screen buttons, prompts | Menus |

A does nothing during play itself: the PS Vita port found that DPAD_CENTER
there triggers an engine bug. On every screen over gameplay (pause, mission
failed, debriefing, skippable animations) A selects and B goes back: the port
reads which menu the game has on top. In-game prompts (doors, pick-ups) are
the game's on-screen buttons — use the touch screen in handheld.

Videos (the Gameloft logo, the briefings): A, B, + or a tap skips.

## config.ini

| Section | Key | Default | |
| --- | --- | --- | --- |
| display | resolution | auto | 720, 1080, auto (1080 docked) |
| display | frame_rate | 60 | 60 or 30 |
| controls | look_speed | 1.0 | right stick speed, 0.3–3.0 |
| controls | sprint_toggle | true | L3 click starts sprint; another click, letting go of the left stick or aiming stops it. false: hold L3 |
| controls | invert_y | false | |
| controls | touch_screen | true | |
| performance | boost_cpu_when_loading | true | |
| debug | trace_files | false | every opened file to debug.log (bug reports) |
| debug | log_java_calls | false | every Java call to debug.log (bug reports) |

## Status

Working on hardware:

- android32 runtime (upstream, see `runtime/UPSTREAM_COMMIT`), every one of
  the library's 362 imports bound (99 GL through Mesa, 260 shims/newlib,
  3 weak hooks left NULL) — `tools/imports_needed.txt`, `source/imports.c`;
- the 1.1.7g library checked by CRC at start-up; the installer's DRM flags;
- the game's 119 SWP instructions (undefined on ARMv8 AArch32) rewritten to
  LDREX/STREX stubs placed over the dead `.rel.dyn` (`source/mc3_swp.c`);
- the PS Vita port's JNI answers on the runtime's JNI through
  `port_jni_invoke`, with immortal strings (the engine frees its local refs
  before it is done with their characters);
- AudioTrack → audout, resampled 44.1 → 48 kHz;
- Xperia Play gamepad path (keys + both touch pads), touch screen, menus
  over gameplay from the game's own Flash menu stack;
- GLES 2 context on Mesa/nouveau, 720p/1080p, 60/30 fps;
- the Gameloft logo and the briefing videos (FFmpeg, optional);
- HOME/sleep: the game paused and resumed.

Not done:

- multiplayer (Gameloft's servers are gone, as on Android).

Known issues:

- After a long sprint the character's heavy breathing can go on until the
  next death or loading screen. The port releases the sprint key every time
  (debug.log shows it); the game keeps its "out of breath" state. Not yet
  known whether the Android game does the same.

## Building

Linux or WSL2 with Docker (the AArch32 toolchain is the vita2hos container
image from ghcr.io).

```sh
git clone https://github.com/aks796/libnx32 ../libnx32 && ../libnx32/build.sh
mkdir -p portlibs32 && curl -L <mesa32 release zip> -o /tmp/mesa32.zip \
  && unzip -q /tmp/mesa32.zip -d portlibs32/      # github.com/aks796/mesa32 releases
./build.sh                  # mc3_nx.nsp
./launcher/build.sh         # launcher/mc3_nx.nro (carries the NSP)
```

Briefing videos (optional): a small FFmpeg (MP4, H.264, MPEG-4, AAC) built
into `portlibs32/` once. With it and Mesa there, the build plays them;
without, the game is told each one finished at once.

```sh
tools/build_ffmpeg.sh
./build.sh clean && ./build.sh && ./launcher/build.sh
```

`source/imports.c` is generated: after a runtime update run
`python3 runtime/tools/gen_imports.py` (the import list is cached in
`tools/imports_needed.txt`; `tools/needed_from_so.py <dir>` rebuilds it from
the game's `.so` without pyelftools).

## Credits

- **Gameloft** — Modern Combat 3: Fallen Nation (not included).
- **Volodymyr Atamanenko (v-atamanenko)** — the PS Vita port
  [mc3-vita](https://github.com/v-atamanenko/mc3-vita), whose init order, DRM
  flags, JNI answers and Xperia input path this port follows.
- **aks796** — [android32](https://github.com/aks796/android32),
  [libnx32](https://github.com/aks796/libnx32), [mesa32](https://github.com/aks796/mesa32).
- **xerpi and the vita2hos team** — the AArch32 Switch environment.
- **Andy Nguyen (TheOfficialFloW), fgsfds** — the original `.so` loader.
- **markaurel13** — [nova3-nx](https://github.com/markaurel13/nova3-nx), the
  Gameloft FPS reference on android32.
- **Switchbrew, devkitPro, Mesa** — libnx, toolchains, graphics.

## License

MIT (see `LICENSE`). The runtime keeps its own notices. Modern Combat is a
trademark of Gameloft; this project is not affiliated with Gameloft or Nintendo.
