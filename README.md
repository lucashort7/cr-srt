# cr-srt

An SRT-style overlay for CONTROL Resonant: the numbers the game hides, drawn on top of the game. SRT is the
Resident Evil speedrun community's name for this kind of tool (Speed Run Tool); the reference is
[SRTPluginProviderRE9](https://github.com/SpeedrunTooling/SRTPluginProviderRE9).

**Status: prototype.** It draws the player's health (`health 1302 / 1430 (91.1%)`) on the left edge and
`dll injected: yes` at the bottom centre. Next: the health of every enemy within a radius of the player.

## How it works

A DLL loaded into the game. It finds two functions in the exe by byte pattern and caches their offsets per exe build,
hooks the ECS system the game runs once per tick to read the player's Health row, and hooks the game's Present
wrapper to draw with Dear ImGui on the game's own D3D12 device, right before each frame is shown.

| file | what it does |
| :--- | :--- |
| `src/dllmain.cpp` | the entry, and the boot: cache or solve, then hook |
| `src/game.h` | every offset, hash and pattern that belongs to the game; a patch changes this file |
| `src/names.h` | what a faction and a kind are called: two tables written by hand, grown as the game is played |
| `src/cache.*`, `src/solve.*`, `src/pe.*` | exe offsets: cached per build, found by pattern on a miss |
| `src/ecs.*`, `src/player.*` | reading the ECS world: archetype, chunk, a component's row |
| `src/hooks/runner.*` | the per-tick hook that reads the player's rows and publishes them, with the world, to the overlay and the walk |
| `src/enemies.*` | the enemy walk on a thread of ours, every `[enemies] interval_ms` of the ini, off the runner's last world |
| `src/hooks/present.*` | the per-frame hook that hands the swapchain to the overlay |
| `src/overlay/swap.cpp` | getting a turn at Present: a vtable copy on the game's swapchain object |
| `src/overlay/renderer.*` | Dear ImGui on D3D12: one command list a frame |
| `src/overlay/ui.*` | what is drawn: the zone, the player, the enemies within `[enemies] radius` (at most `[enemies] rows`) and, while one is near, the allies under them (at most `[allies] rows`) |
| `src/unload.*` | taking the DLL out of a running game without crashing it |
| `src/memory.*`, `src/log.*` | pointer checks before every read; the log |

## Build

Windows, MinGW-w64 g++ 15 (the nuwen distro at `C:\MinGW`), CMake and Ninja. From the repo folder:

```bat
build.bat                 :: builds and copies to cr-srt-dev.dll
build.bat cr-srt.dll      :: or any other name
```

From WSL: `cmd.exe /c build.bat`, with the repo as the working directory.

## Run

Inject `cr-srt-dev.dll` into the running game with any DLL injector. It writes `cr-srt-dev.log` beside itself.
To take it out, create `cr-srt-dev.unload` beside it; never eject it from outside, which unmaps code under a live
hook. Start RTSS before the game if you use it: closing RTSS mid-session crashes the game, with or without cr-srt.

## Origin

Grown from `crhello`, the hello-world DLL this overlay was prototyped in. Vendored: Dear ImGui (MIT) and MinHook
(BSD-2), each with its licence and a `VENDORED.md` naming the commit.
