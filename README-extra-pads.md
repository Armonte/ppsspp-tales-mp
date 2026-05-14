# PPSSPP virtual extra controllers — fork README

Status: **shipping**. v0.1.0 binaries on the [releases page](https://github.com/Armonte/ppsspp-tales-mp/releases/tag/v0.1.0) for Linux x64 and Windows x64. MMIO read path verified end-to-end with a homebrew test ROM (`tests/extra-pads/`, see [VERIFICATION.md](tests/extra-pads/VERIFICATION.md)). Based on upstream PPSSPP at commit `d7a7d2d`.

## What it does

Exposes pads 2-4 to the emulated PSP via a 4 KB virtual MMIO window at `0x0E000000`. A ROM hack reads 16 bytes per pad starting at that base, in the same `SceCtrlData` layout that `sceCtrlReadBufferPositive` produces. Use case: local multiplayer in Tales of Phantasia X / Narikiri Dungeon X via ROM patch.

## Files

- `ppsspp/` — full PPSSPP checkout with the patch already applied. `git diff HEAD` shows the changes.
- `extra-pads-final.patch` — unified diff against upstream `d7a7d2d`. Verified to apply cleanly.
- `ppsspp/build/PPSSPPSDL` — Linux x86_64 binary, built and launches.

## To apply on a fresh checkout

```bash
git clone https://github.com/hrydgard/ppsspp
cd ppsspp
git checkout d7a7d2d
git submodule update --init --recursive --depth 1
git apply /path/to/extra-pads-final.patch
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNITTEST=OFF ..
ninja
```

For Windows mingw build, configure with the appropriate toolchain — none of the patch is platform-specific.

## Enabling the feature

In `ppsspp.ini` under `[Control]`:
```
EnableExtraPads = True
ExtraPadBaseAddress = 234881024   ; = 0x0E000000, only change if you know why
```

Or via the in-emulator UI: Settings → Controls → scroll to bottom → "Virtual Extra Pads (advanced)" → "Enable virtual pads 2-4 (MMIO @ 0x0E000000)". The checkbox is disabled while a game is running (the per-VBlank mirror only starts on init), so set it before loading a ROM.

## Binding pads 2-4 to physical controllers

The Control Mapping screen gets a "Virtual Pad" selector at the top (1-4) when the feature is enabled. The selected pad indexes every new binding you record. Each `MultiInputMapping` gets a `padIndex` field that survives in `ppsspp.ini` via an `@p2` / `@p3` / `@p4` suffix on the binding string (no suffix = pad 1 = real PSP, preserves existing configs).

## Reading from a ROM hack

```c
typedef struct SceCtrlData {
    u32 TimeStamp;
    u32 Buttons;
    u8  Lx, Ly;
    u8  Rsrv[6];
} SceCtrlData;

#define EXTRA_PADS_BASE 0x0E000000

SceCtrlData *pad2 = (SceCtrlData *)(EXTRA_PADS_BASE + 1 * sizeof(SceCtrlData));
SceCtrlData *pad3 = (SceCtrlData *)(EXTRA_PADS_BASE + 2 * sizeof(SceCtrlData));
SceCtrlData *pad4 = (SceCtrlData *)(EXTRA_PADS_BASE + 3 * sizeof(SceCtrlData));

if (pad2->Buttons & PSP_CTRL_CROSS) { /* P2 pressed X */ }
```

Button bitmask, analog scaling, and timestamp semantics are identical to what `sceCtrlReadBufferPositive` writes. Pad 0 is also mirrored at offset 0 for symmetry but games normally just use sceCtrl for that.

Reads on real PSP hardware fault — this region is deliberately chosen to be unmapped. A homebrew that uses this code path will not boot on actual hardware.

## Implementation notes

- **No JIT intercept.** The 4 KB region is a real `MemoryView` in the existing `views[]` table at `Core/MemMap.cpp:118`, mapped at `base + 0x0E000000`. JIT-compiled MIPS loads land on a real host page via `LDR(reg, MEMBASEREG, addrReg)` with no per-load check.
- **The view is always allocated**, regardless of `bEnableExtraPads`. Disabling the flag just stops the per-VBlank mirror; the page still exists, so a homebrew that probes the address reads zeros instead of segfaulting the emulator. Cost: 4 KB.
- **Pads 1-3 don't go through the sceCtrl ring buffer.** Only pad 0 does. The mirror at `0x0E000000+pad*16` is a single snapshot, refreshed once per VBlank — no latch, no wait-thread, no rapid-fire mask, no Daxter analog rotation hack. Game-perspective torn-read window is bounded by one VBlank (~16 ms).
- **Savestate compatibility preserved.** `__CtrlDoState` never serialized `ctrlCurrent`, so the array-of-4 expansion is invisible to savestates.
- **Config compatibility preserved.** Old `ppsspp.ini` files round-trip identically — the `@pN` suffix is only emitted when `padIndex != 0`.

## Known limitations / follow-ups

1. **Display filter in mapping screen is not implemented.** The pad picker stamps the current pad on *new* bindings but doesn't filter the *displayed* binding rows to the currently-selected pad. All bindings show up regardless of which pad is selected. Result: you can see and edit pad-1 bindings while the picker says "Pad 2", which is confusing but not broken — the `padIndex` field on each `MultiInputMapping` is still authoritative. Fix lives in `SingleControlMapper::Refresh` in `UI/ControlMappingScreen.cpp` — skip rows where `mappings[i].padIndex != g_currentEditPad`.
2. **Virtkey-driven analog only drives pad 0.** `ControlMapper::onVKeyAnalog` hardcodes `padIndex = 0`. If a user maps a virtkey like ANALOG_LIGHTLY to pad 2, the modulation will apply to pad 1 instead. Trivial to extend if needed.
3. **No rumble pass-through.** Writes to `0x0E000000` land in the host buffer and are overwritten next VBlank — effectively no-ops. The format leaves room for a back-channel later.
4. **Runtime verification with a ROM hack is the next step.** The build is clean, all new symbols are exported, the binary launches and reaches the menu. What hasn't been tested: an actual game-side `memcpy` from `0x0E000000` returning a populated `SceCtrlData`. A 10-line homebrew that reads the window and `pspDebugScreenPrintf`s the buttons would close that loop.

## Touched files

```
 Core/Config.cpp             |   3 ++
 Core/Config.h               |   7 ++++
 Core/ControlMapper.cpp      |  84 ++++++++++++++++++++-----
 Core/ControlMapper.h        |  17 ++++++---
 Core/HLE/sceCtrl.cpp        |  92 ++++++++++++++++++++++++++++++++++-----------
 Core/HLE/sceCtrl.h          |   9 +++++
 Core/KeyMap.cpp             |  24 +++++++++++-
 Core/KeyMap.h               |  13 +++++--
 Core/MemMap.cpp             |  10 +++++
 Core/MemMap.h               |  21 +++++++++++
 Core/MemMapFunctions.cpp    |   4 ++
 UI/ControlMappingScreen.cpp |  53 ++++++++++++++++++++--
 UI/ControlMappingScreen.h   |   3 +-
 UI/EmuScreen.cpp            |   8 ++--
 UI/EmuScreen.h              |   4 +-
 UI/GameSettingsScreen.cpp   |   5 +++
 16 files changed, 281 insertions(+), 76 deletions(-)
```
