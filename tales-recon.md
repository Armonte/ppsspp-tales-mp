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

## Scene & battle architecture (DONE — static)

The main game runs in `scene_main_loop` (`0xAB0`) with a `while(1) switch(current_scene)`:

| Scene | Function | Purpose |
|-------|----------|---------|
| 0 | `scene_logo` (0x0) | Bandai/Tales logos |
| 1 | `scene_title` (0x4A4) | Title menu (6 options) |
| 2 | `field_update_frame` (0x11A870) | Overworld + dungeons |
| 3 | `sub_1081D4` | (unknown — maybe minigame / shop?) |
| 4 | `battle_tick` + `battle_main_loop` | **Battle** |
| 5,6 | misc | reboot, etc. |

**Battle scene 4 flow:**
```
case 4:
    battle_tick()              // generic per-frame object update over 10 slots × 728B each
    ...                        // session counters
    state = battle_pick_outcome_state(&battle_state)  // runs ENTIRE battle
    battle_exit_with_result(state)                    // post-battle cleanup
```

`battle_main_loop` (`0x8D14`) is the per-battle `while(1)` — runs ~60Hz until end:

```
input_read_frame(state.vsync_count)
battle_input_dispatch(state)     ; 12 KB state machine, where pad input dispatches actions
battle_effects_update(state)
battle_render()
sceDisplayWaitVblank
battle_character_action_run(state) ; small handler, only fires in state 24
battle_state_update(state)         ; small handler, only fires in state 19
break if state.end_flag set
```

### BattleState struct (partial)

`battle_state` lives at runtime address (passed via pointer). Known fields:

| Offset | Field | Meaning |
|--------|-------|---------|
| +1196  | `u32 vsync_count` | frames between updates |
| +1216  | `u32 action_result` | last command's result |
| +1220  | `u32 outcome` | victory/defeat code returned to caller |
| +1516  | `u8 end_flag` | set to break the main loop |
| +1517  | `u8 main_state` | state machine: 15=idle, 17=action, 19=cmd-select, 24=transition, 26=item |
| +1524  | `u8 num_chars` | count of active party members |
| +1528  | `u8 current_char_idx` | **who's being controlled this turn** ← key field for 2P |
| +1534  | `u8 cmd_substate` | 1=normal, 2=item, 3=tech, 4=special |
| +1538  | `u8 cmd_target` | target character/enemy index |
| +1603  | `u8 cmd_action_target` | action's per-character flag |
| +4032  | `Char* char_ptrs[]` | array of pointers to character structs |
| +4132  | `void* action_obj` | currently executing action |

### Where strategy A goes

The 2P mod hook lives in **`battle_input_dispatch`** (`0xFCBC`, 12 KB). Every call inside that function to `input_get_btn_make` / `input_get_btn_press` / `input_get_btn_held` currently reads pad 0 via `g_player_input`. The fix is:

1. Walk the 19+ input-read sites in `battle_input_dispatch`.
2. At each site, determine which character this input is going to (likely a function-scope local set from `state->current_char_idx`, or an outer loop variable).
3. Replace `input_get_btn_*(use_analog)` with `input_get_btn_*_for_pad(char_idx, use_analog)`, where the new getters read `(SceCtrlData *)(0x0E000000 + char_idx * 16)` and maintain per-pad edge masks.
4. The state machine in `battle_input_dispatch` must be modified so it processes ALL active characters per frame instead of just `current_char_idx` — otherwise pads 2-8 only fire when their character has the turn. This is the hard part — the state machine likely assumes single-character-at-a-time.

The state-machine refactor is the actual work. The input plumbing is mechanical.

## Renamed in IDA (persisted)

| Addr | Symbol |
|------|--------|
| `0x0` | `scene_logo` |
| `0x4A4` | `scene_title` |
| `0xAB0` | `scene_main_loop` |
| `0x15C4` | `scene_transition_to` |
| `0x15D0` | `scene_get_current` |
| `0x15DC` | `scene_finish_frame` |
| `0x2B40` | `battle_tick` |
| `0x8D14` | `battle_main_loop` |
| `0x909C` | `battle_character_action_run` |
| `0xE54A0` | `input_get_btn_held` |
| `0xE54D0` | `input_get_btn_make` |
| `0xE5500` | `input_get_btn_press` |
| `0xE5208` | `input_init` |
| `0xE5290` | `input_read_frame` |
| `0xFCBC` | `battle_input_dispatch` |
| `0x11A870` | `field_update_frame` |
| `0x11FA40` | `battle_exit_with_result` |
| `0x12C94` | `battle_state_update` |
| `0x14040` | `battle_effects_update` |
| `0xF8B54` | `battle_render` |
| `0x46303C` | `g_player_input` (struct applied) |

Database persisted at `C:\dev\u4ick\psp\tales-re\EBOOT.elf.i64`.

## Next session (dynamic trace, when ready)

To confirm the static analysis and find any remaining gaps, run the game in our patched PPSSPP with the CPU debugger:

1. Get into a battle.
2. Set a breakpoint at `0xE54D0` (`input_get_btn_make`) — converted to runtime address: `0x08800000 + 0xE54D0 = 0x088E54D0`. (Or use PPSSPP's symbol search if it picks up our renamed symbols.)
3. When the player goes to the command menu, the breakpoint fires.
4. Open Call Stack. Confirm the path: `input_get_btn_make` ← `battle_input_dispatch` ← `battle_main_loop`. Look at the value of register `s0`/`s1` at the call site — it should be a pointer into `battle_state` or one of its char_ptrs[N].
5. Step through `battle_input_dispatch` to find the actual line that reads `state.current_char_idx` and gates input to the corresponding character. That's the surgical patch point.

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
