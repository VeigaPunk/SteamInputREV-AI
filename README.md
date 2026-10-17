# SteamInputREV-AI

Drive Valve's Steam Input engine from a standalone process — no Steam client
running, no credentials, no game launch. The harness loads the real
`steamclient` library from an existing Steam installation and talks to its
controller engine through the same public C++ ABI games use.

Goal: use a DualSense natively through Steam Input's engine (bindings,
gyro, adaptive triggers, lightbar) without keeping the Steam client alive —
plus the pieces around it: the DualSense's built-in microphone as a normal
Linux input over Bluetooth, and a full control map of the pad.

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

## Tools

### `steaminput-harness` — Steam Input engine driver

See "What works today" above and Usage below.

### `ds5-mic` — DualSense microphone over Bluetooth (software-only)

The DualSense exposes no Bluetooth audio profile; the mic is Opus-encoded
(48 kHz mono, 71-byte frames) and embedded in the pad's normal HID input
report `0x31` (bit 1 of byte 1 marks an audio frame). `ds5-mic` enables the
mic with a CRC-guarded output report `0x32`, decodes the Opus stream, and
plays it into a PipeWire null sink — every app can then select
`ds5-mic-vsink.monitor` as its microphone.

```sh
make mic          # needs libopus, libpulse (Arch: pacman -S opus libpulse)
tools/ds5-mic/ds5-mic                 # run until Ctrl-C
tools/ds5-mic/ds5-mic --wav /tmp/m.wav --secs 10   # or record straight to WAV
```

Operational rules, learned the hard way:

- **The kernel's `hid-playstation` parser misreads audio frames as phantom
  button/gyro input.** While streaming, the pad must be on `hid-generic`
  instead. The tool does the swap itself when run with rights, otherwise
  prints the two `sudo` commands. Restore the gamepad afterwards with
  `sudo modprobe hid_playstation` (the tool restores the driver binding on
  exit; the module reload is needed only after the one-time module unload).
- **Never send `0xFF` in the enable report's byte 4** — it latches the
  controller's hardware mic-mute (orange LED). The correct value is
  `0b11` (mic on) / `0b10` (off), per the DS5Dongle firmware. If the LED is
  lit, tap the pad's mute button once.
- The mic is quiet: default gain is 6.0 (`--gain`).

Verified on hardware: 10 ms Opus frames at ~100 Hz, decoded PCM with real
room content, monitor source recordable end-to-end.

### `ds5-input` — live control identification

Decodes report `0x31` state frames and names every control three ways: raw
byte/bit, kernel event, Steam Input origin index. Press a button, see what
it is. Full table in `docs/dualsense-input-map.md`.

```sh
make input
tools/ds5-input/ds5-input             # interactive
tools/ds5-input/ds5-input --legend    # print the full map
tools/ds5-input/ds5-input --json      # events
```

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

Credits for protocol knowledge (facts only; none of their code is
included — their licenses are GPL-3.0, this project is MIT):

- [tomarai85/dualsense-bridge](https://github.com/tomarai85/dualsense-bridge) —
  documented the DualSense BT mic protocol (report `0x31` audio variants,
  Opus framing, the `0x32` enable, CRC construction, write discipline)
- [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) — firmware that
  drives the pad's audio over BT from a Pico; source of the enable byte
  semantics (`0b11`/`0b10`) that avoid the hardware-mute latch
- Linux `hid-playstation.c` and Steamworks SDK 1.65 headers (both public)
  for the report layout and origin tables
