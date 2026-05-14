# Tales of Phantasia: Narikiri Dungeon X — reverse engineering notes

Static recon against the **Japan release** EBOOT (decrypted via pspdecrypt). Loaded into IDA Pro, database persisted at `C:\dev\u4ick\psp\tales-re\EBOOT.elf.i64`.

The English fan-translation v0.04 patches text/script archives inside the ISO; the EBOOT itself is unchanged, so all addresses below apply to both releases.

## Input system (DONE)

The game wraps `sceCtrl` behind two public getters that the entire codebase (~80 call sites — menus, field, items, battle) funnels through. Everything is parameterizable for our 2P mod through these two functions, plus the struct they read from.

### Imports

| IDA addr | Symbol | NID |
|----------|--------|-----|
| `0x17f0b4` | `sceCtrlSetSamplingMode` | `0x1F4011E6` |
| `0x17f0bc` | `sceCtrlReadBufferPositive` | `0x1F803938` |
| `0x17f0c4` | `sceCtrlSetIdleCancelThreshold` | `0xA7144800` |

### Functions

| IDA addr | Symbol | Role |
|----------|--------|------|
| `0xe5208` | `input_init()` | Called once at boot. Sets analog mode + idle threshold + zeros state. |
| `0xe5290` | `input_read_frame(repeat_delay)` | Called per-VBlank. Reads pad → struct, computes edge masks + auto-repeat. |
| `0xe54d0` | `input_get_btn_make(use_analog)` | **Public API: "just pressed this frame"** (rising edge). |
| `0xe5500` | `input_get_btn_press(use_analog)` | **Public API: "pressed with auto-repeat"** (for menu navigation). |

### Global state struct

```c
struct PlayerInputState {       // sizeof = 0x130 = 304 bytes
    uint32_t timestamp;         // 0x00  raw from sceCtrl
    uint32_t buttons;           // 0x04  current held mask
    uint8_t  lx, ly, rx, ry;    // 0x08  analog (rx/ry unused on PSP-1000)
    uint8_t  pad_resv[4];       // 0x0C
    uint32_t prev_buttons;      // 0x10  for edge detection
    uint32_t btn_make;          // 0x14  rising edge
    uint32_t btn_press;         // 0x18  rising + repeat (menu nav)
    uint32_t btn_break;         // 0x1C  falling edge
    int32_t  btn_repeat[32];    // 0x20  per-bit repeat counters
    int32_t  analog_repeat[32]; // 0xA0  same for analog→D-pad
    uint32_t analog_buttons;    // 0x120 analog stick as virtual D-pad
    uint32_t analog_btn_make;   // 0x124
    uint32_t analog_btn_press;  // 0x128
    uint32_t analog_btn_break;  // 0x12C
};

PlayerInputState g_player_input;  // at 0x46303C in BSS
```

### Per-frame algorithm (input_read_frame)

```c
sceCtrlReadBufferPositive(&g_player_input, 1);
if (g_player_input.buttons & (CTRL_HOME | CTRL_HOLD))   // safety: ignore input
    g_player_input.buttons = 0, lx = ly = 0x80;
// Analog deadzone: snap [0x61..0x9F] → 0x80
btn_make  = (buttons ^ prev) & buttons;
btn_break = (buttons | prev) ^ buttons;
btn_press = btn_make + auto-repeat for held buttons
// Analog stick → virtual D-pad bits:
//   Lx > 0xC0 → CTRL_RIGHT (0x20),  Lx < 0x40 → CTRL_LEFT (0x80)
//   Ly > 0xC0 → CTRL_DOWN  (0x40),  Ly < 0x40 → CTRL_UP   (0x10)
// Same edge + repeat treatment, stored in analog_btn_*
```

## 2P mod strategy

**Strategy A (per-character input source)** — the chosen approach.

The 80+ callers of `input_get_btn_make` / `input_get_btn_press` should keep reading pad 0 (the real PSP pad — for menus, field, etc.). Only the **battle frame's per-character action driver** needs to route to pads 1-7 for characters 2-8.

The plan:

1. **Add per-pad parallels.** Write `input_get_btn_make_for_pad(int pad_idx, char use_analog)` that:
   - For `pad_idx == 0`: returns `g_player_input.btn_make` (current behaviour).
   - For `pad_idx > 0`: reads the corresponding `SceCtrlData` from `0x0E000000 + pad_idx*16`, computes a separate edge-mask state (cached per pad), returns its `btn_make`.

2. **Find the battle character action driver.** A function that runs once per character per battle frame, branches on "is player vs AI", reads input, dispatches action. Currently unknown — needs dynamic trace (see below).

3. **Patch only the in-battle call sites.** Each character-action-driver call to `input_get_btn_make(use_analog)` becomes `input_get_btn_make_for_pad(character_idx, use_analog)` (or whatever index variable the game tracks).

The other ~70 callers (`sub_FCBC` field menu, `sub_AD73C` shop UI, etc.) keep calling the pad-0 getter unchanged.

## Next session: dynamic trace plan

Static analysis stalled out at the battle module — Tales LMBS dispatches through function-pointer tables loaded from `battle/character/character.dat`, so the per-character action driver is hard to find without runtime tracing. PPSSPP's built-in CPU debugger makes this trivial:

1. Boot Tales of Phantasia X in our patched PPSSPP (`PPSSPPSDL` or Windows build).
2. Get into a battle (skip cutscenes; the very first encounter works).
3. Open Debug → Disassembly (`Ctrl+D`).
4. Set a breakpoint at `input_get_btn_make` (`0xe54d0` + `0x08804000` runtime base ≈ `0x08C674D0`).
5. When the game wants the player to pick a command, the breakpoint fires.
6. Open Debug → Call Stack. The frames above input_get_btn_make are:
   - `f0` = `input_get_btn_make` itself
   - `f1` = the immediate caller — almost certainly the **battle command UI** for the currently-active character
   - `f2` = the **per-character update function** — this is the hook target for strategy A
   - `f3` = the **battle main loop**

7. Note the `f2` address. Back in IDA, look at the parameter passed to `f2` — that's the character struct pointer. The character index is either a field of that struct or the loop variable in `f3`.

8. From there, modify `f2`'s input-reading branch to call our pad-N getter.

A debug session of maybe 10 minutes nets all four addresses and the character-struct field layout. Static analysis would take hours to converge on the same answers because the dispatch is data-driven.

## Database state

IDA db saved at `C:\dev\u4ick\psp\tales-re\EBOOT.elf.i64`. Has:

- 7 functions renamed (3 stubs, 4 input wrappers)
- 1 global named (`g_player_input`)
- 2 structs declared (`SceCtrlData`, `PlayerInputState`)
- Struct applied at `0x46303C`
- 5 explanatory comments

A fresh `BSS_runtime` segment was added covering `0x347c09–0x500000` because the PSP ELF loader didn't auto-map BSS — required to apply types to globals.

## Files

- `tales-recon.md` — this file
- `C:\dev\u4ick\psp\tales-re\EBOOT.elf.i64` — IDA database (not in repo; ~30 MB)
- `C:\dev\u4ick\psp\tales-re\EBOOT.elf` — decrypted MIPS-II ELF (not in repo; copyrighted)
