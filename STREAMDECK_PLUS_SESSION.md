# StreamDeck+ Implementation Session Summary

**Branch:** `feat/add-streamdeck-rit-xit` (merged, deleted)
**PR #3236:** https://github.com/aethersdr/AetherSDR/pull/3236 — merged 2026-05-28
**PR #3250:** https://github.com/aethersdr/AetherSDR/pull/3250 — robustness follow-ups, merged same day
**Date:** 2026-05-27 / 2026-05-28

---

## What Was Built

Full Elgato StreamDeck+ (VID `0x0FD9`, PID `0x0084`) integration for AetherSDR, implementing GitHub issue #1510. The device's 4 encoder dials, 4 encoder push buttons, 8 LCD buttons, and touchscreen strip are all wired up and configurable from the Settings → Radio → Serial tab.

---

## Features

### Encoder Dials (turn)
- Each of the 4 dials maps to a configurable wheel action
- Defaults: Enc 1 = Tune, Enc 2 = RIT, Enc 3 = XIT, Enc 4 = Master Volume
- Settings keys: `HidEncoderAction0`–`HidEncoderAction3`

### Encoder Push Buttons
- Each push dispatches a configurable action
- Defaults: Enc 1 push = Cycle Tuning Step, Enc 2 push = Toggle RIT, Enc 3 push = Toggle XIT
- Settings keys: `HidEncoderPushAction0`–`HidEncoderPushAction3`
- Release events suppressed; only press fires

### 8 LCD Buttons
- Each button configurable to one of 19 actions: MOX, Tune, RIT/XIT toggle/clear, Step Up/Down, Mute, Lock, APF, AGC cycle, Band/Segment Zoom, Next/Prev Slice, Volume ±5, Split
- Button images rendered with action label and colour-coded background (red = TX, green = RIT/XIT, purple = volume, blue-gray = navigation/other)
- Labels update immediately when settings change — no reconnect needed
- Settings keys: `HidKeyAction0`–`HidKeyAction7`

### Touchscreen Strip
- Full 800×100 JPEG rendered above the dials
- Four 200×100 sections, one per encoder:
  - Top half: turn action label (dark blue background)
  - Bottom half: push action label + ON/OFF state (green when RIT/XIT active, dark red when off)
- Updates live when active slice's RIT/XIT state changes
- Updates when active slice switches

---

## Settings UI

Settings → Radio → Serial tab — three new group boxes ordered top-to-bottom to match the physical device:

1. **StreamDeck+ LCD Button Actions** — 8 combos in 2×4 grid (Keys 1–8)
2. **HID Encoder / StreamDeck+ Encoders** — 4 combos for dial turn actions
3. **StreamDeck+ Encoder Push Actions** — 4 combos for dial push actions

---

## HID Protocol Details

All verified against `python-elgato-streamdeck` v0.9.8.

### Input (reading events)
- 14-byte reports, `hid_read`
- `buf[0]` = report ID `0x01` (always present, strip it)
- `buf[1]` = event type: `0x00` = LCD key, `0x02` = touchscreen, `0x03` = dial
- Dial turn: type `0x03`, sub-type `0x01` at `buf[4]`, signed int8 deltas at `buf[5..8]`
- Dial push: type `0x03`, sub-type `0x00` at `buf[4]`, bool states at `buf[5..8]`
- LCD keys: type `0x00`, key states at `buf[4..11]`
- Button numbering: LCD keys 1–8, encoder press buttons 9–12

### Key image output (120×120 JPEG)
- 1024-byte `hid_write` packets
- 8-byte header: `[0x02, 0x07, key_index, is_last, len_lo, len_hi, page_lo, page_hi]`
- Payload: up to 1016 bytes of JPEG per packet
- `KEY_FLIP = (False, False)` — no image transform needed

### Touchscreen output (800×100 JPEG)
- 1024-byte `hid_write` packets
- 16-byte header: `[0x02, 0x0c, x_lo, x_hi, y_lo, y_hi, w_lo, w_hi, h_lo, h_hi, is_last, page_lo, page_hi, len_lo, len_hi, 0x00]`
- Payload: up to 1008 bytes of JPEG per packet

### Important: use `hid_write`, NOT `hid_send_feature_report`
Despite the Python library calling its wrapper `_send_feature_report`, it calls `device.write()` → `hid_write`. Using `hid_send_feature_report` (HIDIOCSFEATURE ioctl on Linux) corrupts device state and breaks `hid_read`.

---

## Files Changed

| File | Changes |
|---|---|
| `src/core/HidDeviceParser.h` | Added `encoderIndex` field (last) to `HidEvent`; added `StreamDeckPlusParser`; added `encoderCount()` virtual |
| `src/core/HidDeviceParser.cpp` | Implemented `StreamDeckPlusParser::parse()`; added StreamDeck+ to device table and factory |
| `src/core/HidEncoderManager.h` | Added `isStreamDeckPlus()`, `setKeyImage()`, `setKeyImages()`, `setTouchscreenImage()` slots |
| `src/core/HidEncoderManager.cpp` | Implemented LCD key and touchscreen write protocols |
| `src/gui/MainWindow.h` | Added `hidEncoderDefaultPushAction()`, `refreshStreamDeckLabels()`, connection members |
| `src/gui/MainWindow.cpp` | Full HID button dispatch (LCD keys + encoder pushes); `renderTouchscreenJpeg()`; `renderKeyImageJpeg()`; `refreshStreamDeckLabels()`; wired to slice RIT/XIT signals and `serialSettingsChanged` |
| `src/gui/RadioSetupDialog.h` | Added `m_hidEncoderPushActionCombos`, `m_hidKeyActionCombos` arrays |
| `src/gui/RadioSetupDialog.cpp` | Three new group boxes for LCD button actions, dial turn actions, dial push actions |

---

## System Setup Required

Non-root HID access needs a udev rule:

```
# /etc/udev/rules.d/50-elgato-streamdeck.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0fd9", ATTRS{idProduct}=="0084", MODE="0666"
```

Apply with:
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```
Then unplug and replug the device.

---

## Bugs Fixed During Session

| Bug | Root Cause | Fix |
|---|---|---|
| No encoder events received | Wrong protocol assumed (65-byte reports); actual is 14-byte with report ID at `buf[0]`, type at `buf[1]` | Rewrote `StreamDeckPlusParser::parse()` from Python library source |
| All input broken after adding LCD writes | Used `hid_send_feature_report` instead of `hid_write` | Changed to `hid_write` |
| LCD images upside-down | Applied `mirrored(true, true)` based on incorrect reading of Python `KEY_FLIP`; StreamDeck+ is `(False, False)` | Removed the transform |
| Labels not updating on settings change | `serialSettingsChanged` not connected to `refreshStreamDeckLabels()` | Added connection in `wireRadioSetupDialogSignals` |
| `encoderIndex` broke existing parsers | Field inserted in middle of `HidEvent` struct shifted existing 4-field aggregate inits | Moved field to end; used C++20 designated initializers for new code |
| Windows CI failure (check-windows) | `std::min` used in `setKeyImage`/`setTouchscreenImage` without `#include <algorithm>`; MSVC does not pull it in transitively | Added `#include <algorithm>` to `HidEncoderManager.cpp` |

---

## Commits (PR #3236 — squashed to main as `d863608d`)

```
82d2e186  feat(hid): StreamDeck+ support with per-encoder action mapping (#1510)
97b80a80  fix(hid): correct StreamDeck+ HID report layout
757ddeda  feat(streamdeck): encoder push buttons with configurable per-encoder actions
6326d4e0  feat(streamdeck): LCD key labels with live RIT/XIT state
9a0c85b2  fix(streamdeck): use hid_write for LCD images, batch all 8 keys in one call
2d20f159  fix(streamdeck): remove incorrect 180° image flip
45247b10  feat(streamdeck): move labels to touchscreen strip above dials
bb7acbea  feat(streamdeck): configurable actions for all 8 LCD buttons
c052ff8b  fix(streamdeck): refresh LCD labels immediately on settings change
496ea1f4  refactor(streamdeck): reorder Serial tab groups to match physical layout
49a8d9e1  fix(streamdeck): add <algorithm> include for std::min on MSVC
```

## Follow-up PR #3250 — Robustness fixes (merged as `8d746c4e`)

Opened by Jeremy (ten9876 / KK7GWY) based on review of #3236. Three independent fixes:

1. **Simultaneous-event bug** (`HidDeviceParser.cpp`) — `m_prevEncBtns`/`m_prevKeys` were overwritten with `newState` before the inner loop, silently dropping any second-changed bit in the same HID report. Fixed with one-bit-at-a-time commit: `m_prevKeys ^= (1u << i)`.

2. **`hid_write` return check** (`HidEncoderManager.cpp`) — image write loops ignored the return value; on unplug mid-stream the loop spun through remaining packets into a dead handle. Now bails and logs on `written < 0`.

3. **`std::atomic` gate-check members** (`HidEncoderManager.h`) — `m_device`, `m_openVid`, `m_openPid` are written on `m_extCtrlThread` and read on the main thread in `refreshStreamDeckLabels`. Wrapped in `std::atomic` with relaxed ordering (callers treat the result as a hint; the real gate is re-checked inside each queued slot).

---

## CI Journey

| Run | Result | Notes |
|---|---|---|
| First push | check-windows ❌, check-macos ✅, check-paths ✅ | Missing `#include <algorithm>` — MSVC can't find `std::min` |
| After `<algorithm>` fix | All ✅ | Approved by maintainer; PR merged |

---

## Final State

- `main` at `8d746c4e` — all StreamDeck+ code live including #3250 robustness fixes
- Feature branch `feat/add-streamdeck-rit-xit` deleted (local + remote)
- Issue #1510 closed
