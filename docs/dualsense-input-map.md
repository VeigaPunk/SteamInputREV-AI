# DualSense input map — Bluetooth HID ↔ kernel ↔ Steam Input

Everything a DualSense sends over Bluetooth arrives in one 78-byte HID
input report, `0x31`. Two variants share that ID: when **bit 1 of byte 1
is clear** the report is controller state (below); when set it carries a
71-byte Opus mic frame (see `tools/ds5-mic/`). Any parser that reads an
audio variant as state will see phantom button/gyro storms — this bit is
the discriminator.

Layout transcribed from `drivers/hid/hid-playstation.c`
(`struct dualsense_input_report`, Bluetooth transport adds the `0x31` id
and seq/tag byte). Origin names/indices parsed from Steamworks SDK 1.65
`isteaminput.h` (`k_EInputActionOrigin`, PS5 block begins at 258).

## Report 0x31 — controller state variant

| offset | size | field | notes |
|---|---|---|---|
| 0 | 1 | report id | `0x31` |
| 1 | 1 | seq/flags | seq `<<4`; bit 1 = audio variant; never parse as state if set |
| 2,3 | 2 | left stick X,Y | u8, center ≈ 127 |
| 4,5 | 2 | right stick X,Y | u8 |
| 6 | 1 | L2 analog | 0–255 |
| 7 | 1 | R2 analog | 0–255 |
| 8 | 1 | sequence number | increments per report |
| 9–12 | 4 | buttons | bit table below |
| 13–16 | 4 | reserved | |
| 17–22 | 6 | gyro x,y,z | s16 LE, raw sensor counts |
| 23–28 | 6 | accel x,y,z | s16 LE |
| 29–32 | 4 | sensor timestamp | u32 LE |
| 33 | 1 | reserved | |
| 34–37 | 4 | touch point 1 | contact, x_lo, x_hi:4\|y_lo:4, y_hi (12-bit each) |
| 38–41 | 4 | touch point 2 | |
| 42–53 | 12 | reserved | |
| 54–56 | 3 | status | byte 0: battery capacity = bits 3:0 ×10 %, charging = bits 5:4 |
| 57–64 | 8 | reserved | |

## Button bits (raw[9+n])

| control | bit | kernel event | Steam Input origin |
|---|---|---|---|
| cross ✕ | 0.5 | `BTN_SOUTH` | 258 `PS5_X` |
| circle ○ | 0.6 | `BTN_EAST` | 259 `PS5_Circle` |
| triangle △ | 0.7 | `BTN_NORTH` | 260 `PS5_Triangle` |
| square □ | 0.4 | `BTN_WEST` | 261 `PS5_Square` |
| L1 | 1.0 | `BTN_TL` | 262 `PS5_LeftBumper` |
| R1 | 1.1 | `BTN_TR` | 263 `PS5_RightBumper` |
| L2 click | 1.2 | `BTN_TL2` | 289 `PS5_LeftTrigger_Click` |
| R2 click | 1.3 | `BTN_TR2` | 291 `PS5_RightTrigger_Click` |
| create | 1.4 | `BTN_SELECT` | 265 `PS5_Create` |
| options | 1.5 | `BTN_START` | 264 `PS5_Option` |
| L3 | 1.6 | `BTN_THUMBL` | 293 `PS5_LeftStick_Click` |
| R3 | 1.7 | `BTN_THUMBR` | 299 `PS5_RightStick_Click` |
| PS home | 2.0 | `BTN_MODE` | — (system-reserved; no PS5 origin exists) |
| touchpad click | 2.1 | touchpad key | 276 `PS5_RightPad_Click` |
| mic mute | 2.2 | mute LED path | 266 `PS5_Mute` |

Analog mappings: left stick → `ABS_X/Y` / origin 292 `PS5_LeftStick_Move`;
right stick → `ABS_RX/RY` / 298 `PS5_RightStick_Move`; L2 → `ABS_Z` / 288
`PS5_LeftTrigger_Pull`; R2 → `ABS_RZ` / 290 `PS5_RightTrigger_Pull`.
D-pad is a 4-bit hat at raw[9] bits 3:0 (0=N clockwise through 7=NW,
8=released) → `ABS_HAT0X/Y` / origins 304–307 `PS5_DPad_N/S/W/E`.
Touch points map to the pad-as-`RightPad` origins (274–283); gyro to
308–311 `PS5_Gyro_*`.

The PS5 origin block also contains grips/Fn buttons (313–316) — real
hardware only on accessory hardware (e.g. Edge back buttons); the stock
pad never reports them.

## How Steam Input interprets the pad

Steam Input does not consume `/dev/input` events for PlayStation pads —
its engine opens the same hidraw report stream and decodes `0x31`
itself (which is why it, too, must be shielded from audio-variant
frames; see the harness README). Each physical control becomes an
**origin** (the table above). User configurations bind origins to
**actions** from a game's action manifest; `GetDigitalActionOrigins`
returns which origins currently drive a given action — the harness
monitor mode prints exactly that association live.

Device identity: `GetInputTypeForHandle` → `k_ESteamInputType_PS5Controller`,
which selects the PS5 origin block and the DualSense glyph set
(`GetGlyphPNGForActionOrigin`).

## Live identification

```sh
tools/ds5-input/ds5-input            # press buttons, get names + bits + origins
tools/ds5-input/ds5-input --legend   # this table
tools/ds5-input/ds5-input --json     # events for scripting
```

Sources: `linux/drivers/hid/hid-playstation.c` (layout, kernel events);
Steamworks SDK 1.65 `isteaminput.h` (origins); live captures on a
2026-10-13 client / firmware 0x1082 DualSense.
