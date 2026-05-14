# Runtime verification of the extra-pads MMIO window

Date: 2026-05-13. Tested against `v0.1.1` (8-pad bump), Linux x64 build with
SDL frontend, `bEnableExtraPads = True` set in `ppsspp.ini`. (Originally
verified at `v0.1.0` with 4 pads.)

## What we ran

`EBOOT.PBP` — the homebrew in this directory. Each VBlank it:

1. Reads 4 × `CtrlData` (16 bytes each) from `0x0E000000`.
2. Reads pad 0 via `sceCtrlReadBufferPositive` as a cross-check.
3. Prints both via `printf` (captured by PPSSPP's PRINTF log channel).

## What we saw

```
SAMPLE 0
  PAD0 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD1 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD2 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD3 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD4 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD5 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD6 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  PAD7 frame=0000412b buttons=00000000 Lx=128 Ly=128 Rx=128 Ry=128
  sceCtrl: frame=0000412b buttons=00000000 Lx=128 Ly=128
SAMPLE 1
  PAD0 frame=00008256 ...
  ...
  sceCtrl: frame=00008256 ...
SAMPLE 2
  PAD0 frame=0000c382 ...
```

## What it proves

| Check | Result |
|-------|--------|
| Reads at `0x0E000000` don't fault | PASS — view is real |
| Each pad slot returns exactly 16 bytes in `CtrlData` layout | PASS |
| `frame` field advances ~60 Hz (16,683 µs per VBlank) | PASS — mirror runs in `__CtrlUpdateLatch` |
| Pad 0 mirror matches `sceCtrlReadBufferPositive` output | PASS — same timestamp, same buttons |
| Pads 1-3 default to centered analog (128) and zero buttons when unbound | PASS |
| `bEnableExtraPads = False` → reads return zeros (still no fault) | PASS (confirmed in a separate run) |

## What's still untested by this homebrew

Host controller binding → pad 2 buttons. Needs:

1. A second physical controller plugged in.
2. Settings → Controls → Virtual Pad → "Pad 2".
3. Map e.g. Cross to that controller's A button.
4. Run this EBOOT.PBP, press A on the second controller, observe `PAD1 buttons=00004000` (`PSP_CTRL_CROSS = 0x4000`).

That last step is the one Euphoric will exercise. Everything below it is verified.

## Reproducing

Linux:
```
ppsspp-tales-mp-linux-x64/PPSSPPSDL EBOOT.PBP \
    --log=/tmp/ppsspp.log --escape-exit --pause-menu-exit -d
# wait a few seconds, ctrl-c, then:
grep "stdout:" /tmp/ppsspp.log | head -20
```

Windows:
```
PPSSPPWindows.exe path\to\EBOOT.PBP
```
The on-screen text shows the same data the log captures.

Make sure `EnableExtraPads = True` is set under `[Control]` in
`%APPDATA%\ppsspp\PSP\SYSTEM\ppsspp.ini` (Windows) or
`~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini` (Linux).
