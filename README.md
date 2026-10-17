# SteamInputREV-AI

Drive Valve's Steam Input engine from a standalone process — no Steam client
running, no credentials, no game launch. The harness loads the real
`steamclient` library from an existing Steam installation and talks to its
controller engine through the same public C++ ABI games use.

Goal: use a DualSense natively through Steam Input's engine (bindings,
gyro, adaptive triggers, lightbar) without keeping the Steam client alive.

## What works today

Verified 2026-10-16 on Linux x86-64 against the Steam client build of
2026-10-13 (`linux64/steamclient.so`) with a DualSense over Bluetooth:

**Hybrid mode (Steam client running, no game launched)** — the Steam
process is the input *master* (it owns `/dev/hidraw*`); the harness is a
consumer that attaches to it:

- `--list` enumerates the DualSense as `PS5/DualSense` with a stable handle
- **all output paths verified on real hardware**: `--led` (lightbar color),
  `--rumble` (both motors), `--trigger-effect feedback/weapon/vibration`
  (DualSense adaptive triggers) — driven from a standalone process, no game

**Standalone engine bring-up (Steam stopped)**:

- `CreateInterface("SteamClient023")` → `CreateLocalUser` →
  `GetISteamInput("SteamInput007")` → `Init()` — the engine boots in a
  foreign process, threads and controller SHM segments included
- clean embedded teardown (see RE notes — the obvious version segfaults)

## What is blocked (the current frontier)

**Standalone device acquisition.** With Steam stopped the engine enumerates
0 controllers — traced with an `LD_PRELOAD` open-logger: the game-side
steamclient never opens `/dev/hidraw*` or `/dev/input/*` at all. Device
ownership lives in the Steam client process (`ubuntu12_32/steam` was
observed holding the DualSense's hidraw fd). Full standalone operation
needs the harness to take that master role (drive the client-core input
service via the internal `IClient*` interfaces — Proton's `lsteamclient`
vendors their vtable layouts and is the map for that work).

**Action data (button/stick events).** `SetInputActionManifestFilePath` is
rejected with `Timed out waiting for game mapping!` even in hybrid mode:
the client *receives* the request (visible in `logs/controller.txt`:
"Opted-in Controller Mask for AppId …", "adopting binding"), but never
confirms a mapping for a process that is not a registered app instance.
`SteamAPI_Init`-style registration (flat `SteamInternal_*` handshake with
the running client) is the missing delta; replicating it without steam_api
is the concrete next step.

Older findings that still stand: the game mapping is not deliverable via
the host IPC object by name (`/dev/shm/u<uid>-ValveIPCSharedObj-Steam` is
exclusive-create and launch-inherited); clean-room runs (empty `/dev/shm`,
no network — `scripts/run-isolated.sh`) behave identically, so nothing
external is accidentally blocking it. Lead: steamclient convar
`@bClientTryRequestManifestWithoutCode`.

**Operational caveat:** failed game-mapping requests wedge the running
client's input state (subsequent enumerations return 0 until Steam is
restarted). Don't run `--manifest` against a live Steam client; recovery
is `steam -shutdown` and start again.

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
steaminput-harness --rumble 40000,40000            # both motors
steaminput-harness --led 255,0,0                   # lightbar red
steaminput-harness --trigger-effect feedback,5,6   # adaptive triggers
steaminput-harness --global-user --list            # attach to running Steam
scripts/run-isolated.sh --list                     # clean-room run
scripts/hwtest.sh                                  # hardware acceptance test
```

`--manifest` (action-level input monitoring) is implemented but gated on
the registration handshake documented under "What is blocked" — and must
not be used against a live Steam client until then (state wedging).

Exit codes: `0` ok · `1` runtime error · `2` setup error · `3` no
controllers found. `--poll 0` runs until Ctrl-C.

## Cautions

- **steamclient spawns background threads and may phone home.** Internal
  asserts write a minidump to `/tmp/dumps` and the bundled crashhandler
  uploads it to Valve's crash server (observed on an unrelated voice/video
  init assert). `scripts/run-isolated.sh` runs the engine with no network,
  which contains this; prefer it for experiments.
- Steam Input grabs controller HID nodes exclusively; the Steam client is
  the device master. Hybrid mode requires the Steam client running (it
  plays master to the harness's consumer). Standalone mode requires it
  stopped — but the master role is not implemented yet, so standalone
  currently enumerates 0 controllers.
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
