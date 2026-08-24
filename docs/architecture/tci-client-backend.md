# TCI client backend (family `tci`)

AetherSDR as a **client** of somebody else's TCI server — the inverse of
everything else named `Tci*` in this tree.

| | Server direction (existing) | Client direction (this doc) |
|---|---|---|
| Code | `src/core/TciServer.*`, `src/core/TciProtocol.*` | `src/core/backends/tci/` |
| Role | AetherSDR pretends to be a radio | AetherSDR dials a radio |
| Peer | a logging program, WSJT-X, TCI Remote | a TCI server fronting hardware |

## Why

It puts an entire class of hardware in reach that AetherSDR has no wire
protocol for, at the cost of one backend. The
[`k3-tci-bridge`](https://github.com/motoham88/k3-tci-bridge) project fronts an
Elecraft K3/K3S with a Raspberry Pi that speaks CAT to the radio and TCI to the
network; anything else speaking TCI (ExpertSDR3, SunSDR) arrives for free.

## Shape

A **pure seam backend**, like Icom and HL2: it owns no `RadioConnection` and no
`PanadapterStream`, so every model update leaves as a normalized delta and
`RadioModel::setupBackend`'s `dynamic_cast` chain correctly skips it.

It is **radio-authoritative**. The server reports its own vfo/modulation/filter
in the init burst and on every front-panel change, so the backend declares no
`clientSettingsDomains` and is never pushed a restored state (Constitution
Principles II and III). What the radio says it is doing wins.

```
QWebSocket ──text──> TciClientCodec::parseMessage ──> TciBackend::handleEvent
                                                          │
                                                          ├─ sliceChanged(0, …)
                                                          ├─ meterUpdate("SLC:LEVEL", …)
                                                          └─ transmitChanged(…)
QWebSocket ──binary─> parseStreamHeader + payloadToFloat32
                                │
                        Resampler 48k → 24k
                                │
                        audioFrameReady(pcm24kStereoFloat)
```

### No spectrum, as a property of the radio

`capabilities().maxPanadapters` is **0**. A K3 has no panadapter output to
bridge — the bridge project measured the P3's tap and documented why it cannot
supply one — so the UI omits the panadapter surface rather than opening a
window that would stay empty. This is the path `IcomCivBackend` already takes
for a scope-less radio (`maxPanadapters = m.hasScope ? m.receivers : 0`).

IQ (type 0) and AetherSDR's own spectrum extension (type 4) are dropped on
arrival for the same reason: with no panadapter there is nowhere to put them.

### One slice, not two

TCI's `channels_count:2` is VFO A and VFO B on **one** receiver — a split pair,
not a second demodulator — so it does not become a second slice. `maxSlices` is
1 and the backend drives trx 0. A server reporting `trx_count > 1` is logged
and its extra transceivers ignored; wiring one needs its own slice identity and
audio routing.

## Three things that will bite

### The mode tables are not interchangeable

`TciProtocol` (server) maps AetherSDR's `CW` to the wire as `cw` and `CWL` as
`cwr`. The K3 bridge advertises `cwl,cwu` and emits **neither** of those
spellings. A client reusing the server's table reads every CW contact on the
**wrong sideband** and reports no error anywhere — the failure the bridge's own
command map documents under "Sideband polarity — measured, not assumed".

So `TciClientCodec::modeFromWire` is alias-tolerant by design: `cw`/`cwu` both
mean upper, `cwr`/`cwl` both mean lower. An unrecognised mode returns empty with
`ok == false` rather than falling back to USB, because inventing a mode puts the
slice in one the radio is not actually in.

### Ordering at connect is load-bearing

```
ready;  →  emit connected()      RadioModel stages+clears the old session here
        →  publishMeterDefs()    a value whose meter has no def is DISCARDED
        →  publishInitialSlice() seeded from the cached init-burst values
```

Each line was a bug found against live hardware:

- Publishing the slice **before** `connected()` let RadioModel's post-connect
  sweep destroy it. Radio ready, audio streaming, `sliceCount` 0.
- The init burst reports `vfo`/`modulation`/`rx_filter_band` **before**
  `ready;`, so the per-field handlers emitted them into a slice that did not
  exist. The slice came up at 0 Hz in USB on a radio sitting on 14.038510 CWL.
- `meterUpdate` for a meter with no `meterDefined` is silently dropped by
  `MeterModel::updateValueByName`. Every S-meter reading was computed, emitted
  and discarded — invisible from the backend, which goes on emitting correct
  values forever.

### The build guard has two halves

`Qt6::WebSockets` is an optional `find_package`. `TciBackend` is guarded with
`#ifdef HAVE_WEBSOCKETS` **and so is its branch in `RadioModel::makeBackend`** —
an unguarded factory reference compiles and then fails at *link*. Sources are
listed unconditionally in `CMakeLists.txt`, matching how `TciServer`/
`TciProtocol` are already handled.

`TciClientCodec` carries **no** socket dependency and its test is registered
outside the `Qt6WebSockets_FOUND` guard, so it still runs on a build without
WebSockets — the configuration where a mistake in it would otherwise go
unnoticed.

## Wire notes, measured

Captured from a live K3 bridge, not assumed:

- Binary frame header is **64 bytes, 16 × uint32 little-endian**, matching the
  `TciAudioHeader` that `TciServer.cpp` writes.
- `length` counts **total interleaved samples**, not per-channel frames: a
  16448-byte frame carries `length = 4096` = 2048 stereo frames.
- RX audio arrives `format = 3` (float32), 48 kHz, 2 channels, ~22 frames/s.
- The decoder clamps to what **arrived**, not to what `length` claims — a
  truncated frame whose header still says 4096 would otherwise overread 16 kB.

`audio_start:<trx>;` is required. The bridge sends nothing until it arrives,
and a client that never asks hears silence with no error at either end.

## Adding a family

`src/core/RadioFamilies.h` is the one list. It exists because the set was
previously spelled out inline in four places — two normalizers and a validator
in `ConnectionPanel`, and the `connect ip` verb in `AutomationServer` — which
had already drifted apart. The automation one was the easiest to miss: a family
could be wired through the picker, the factory and the backend and still be
rejected by the bridge with *"radio type must be flex, hl2 or icom"*.

It lives in **core**, not gui, precisely so `AutomationServer` can use it
without including a panel header.

## Not implemented

TX audio, CW keying (`cw_macros`), IQ, split, AGC (TCI carries no AGC verb at
all), and the sub-receiver as trx 1. `setKeying` sends `trx:<n>,true;` and is
gated on `canTransmit`, which comes from the server's `receive_only`.
