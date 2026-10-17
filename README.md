# SteamInputREV-AI

Drive Valve's Steam Input engine from a standalone process — no Steam client
running, no credentials, no game launch. The harness loads the real
`steamclient` library from an existing Steam installation and talks to its
controller engine through the same public C++ ABI games use.

Goal: use a DualSense natively through Steam Input's engine (bindings,
gyro, adaptive triggers, lightbar) without keeping the Steam client alive.

## What works today

Verified against the Steam client build of 2026-10-13
(`linux64/steamclient.so`), Linux x86-64, Steam client not required:

- `CreateInterface("SteamClient023")` → `CreateLocalUser` →
  `GetISteamInput("SteamInput007")` → `Init()` — the full engine comes up
  in a foreign process, spawning its worker threads and creating its
  controller shared-memory segments
- controller enumeration (`GetConnectedControllers`) and, with hardware,
  `--list` prints device type (`PS5/DualSense`) and binding revision
- output commands wired and ready: `--rumble`, `--led`,
  `--trigger-effect` (DualSense adaptive triggers), motion polling
- clean embedded teardown (see RE notes — the obvious version segfaults)

## What is blocked (the current frontier)

`SetInputActionManifestFilePath` is rejected with
`Timed out waiting for game mapping!` in a standalone process. Findings so
far:

- the game mapping does **not** arrive via the host Steam's IPC object
  (`/dev/shm/u<uid>-ValveIPCSharedObj-Steam`): attaching to a live Steam by
  symlinking the name fails the engine's exclusive-create ownership checks,
  so game-side steamclient receives its IPC channel **inherited at launch**,
  not by name lookup
- a clean-room run (empty `/dev/shm`, no network — use
  `scripts/run-isolated.sh`) still times out, so the mapping is not
  accidentally blocked by a half-attached client either: a standalone
  process must itself take the master role and write the mapping into the
  `SteamController_*_mem` segments. Reversing that layout is the next task.
- lead worth chasing: steamclient convar
  `@bClientTryRequestManifestWithoutCode`

Everything up to and including device enumeration works without the
manifest; action-level data (per-button/stick events) is what waits on it.

## Install

### omarchy / Arch / any Linux

```sh
./install.sh                 # latest release binary -> ~/.local/bin, or builds from source
# or
make && sudo make install    # from source, /usr/local/bin
# or
makepkg -si                  # Arch package
```

Requires g++ and an existing Steam installation. udev rules from Valve's
[steam-devices](https://github.com/ValveSoftware/steam-devices) repo must be
installed (the `steam` package on Arch ships them as
`/usr/lib/udev/rules.d/60-steam-input.rules`).

### Windows

```bat
scripts\build_windows.bat    # MSVC or MinGW-w64
```

or download `steaminput-harness-windows-x86_64.zip` from Releases and run
`steaminput-harness.exe` from a machine with Steam installed (the exe finds
`steamclient64.dll` via the registry). The Windows path is the same code but
is **untested** — no Windows build has been executed yet.

## Usage

```sh
steaminput-harness --list                          # enumerate devices
steaminput-harness --list --json                   # machine-readable
steaminput-harness --manifest manifests/dualsense_probe.vdf --poll 0
steaminput-harness --rumble 40000,40000            # both motors
steaminput-harness --led 255,0,0                   # lightbar red
steaminput-harness --trigger-effect feedback,5,6   # adaptive triggers
steaminput-harness --manifest ... --motion --json  # gyro/accel events
scripts/run-isolated.sh --list                     # clean-room run
scripts/hwtest.sh                                  # hardware acceptance test
```

Exit codes: `0` ok · `1` runtime error · `2` setup error · `3` no
controllers found. `--poll 0` runs until Ctrl-C.

## Cautions

- **steamclient spawns background threads and may phone home.** Internal
  asserts write a minidump to `/tmp/dumps` and the bundled crashhandler
  uploads it to Valve's crash server (observed on an unrelated voice/video
  init assert). `scripts/run-isolated.sh` runs the engine with no network,
  which contains this; prefer it for experiments.
- Steam Input grabs controller HID nodes exclusively. Stop the Steam client
  (or its "Playstation Configuration Support") before hardware tests, or
  the two will fight over the device.
- The embedded engine must be torn down then left unloaded: the harness
  calls `Shutdown`/`ReleaseUser`/`BShutdownIfAllPipesClosed` and finishes
  with `_exit()`. Returning from `main` normally segfaults (static
  destructors race surviving engine threads), and unloading the library is
  worse.

## RE notes

- ABI provenance: vtables and struct layouts transcribed from the
  Steamworks SDK 1.65 headers Valve vendors in Proton
  (`lsteamclient/steamworks_sdk_165/`):
  - `isteamclient.h` sha256 `cf409526affe5956b561b40b71feb8600acae5971de91115b42a79974ab08387` → `SteamClient023` (38 slots, `GetISteamInput` at 34)
  - `isteaminput.h` sha256 `4cc1887e02d829633385c07582279a5712fbcd5e1b046eade01efba376237093` → `SteamInput007` (48 slots, `SetDualSenseTriggerEffect` at 47)
- The 2026-10-13 client speaks `SteamInput001`–`007` and up to
  `SteamClient023`; the harness defaults can be overridden with
  `--client-iface` / `--input-iface`.
- Action-data structs are `#pragma pack(1)` — `InputAnalogActionData_t` is
  13 bytes, `InputMotionData_t` is 40. Forgetting the packing corrupts
  every returned value.
- Shared-memory topology observed: `/dev/shm/u<uid>-ValveIPCSharedObj-Steam`
  (client IPC, exclusive-create, launch-inherited) and
  `SteamController_{,Fast_,Slow_}Master_mem` (controller state).
- `--teardown none|release|basic|full` exists to bisect shutdown races.

## Legal

Original code, MIT license. Uses only the documented public Steamworks API
surface of a Steam installation the user already has; no Valve or Sony
headers are redistributed. Built for interoperability with the user's own
hardware and software.
