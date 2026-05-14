# Hooking strategy on PSP (MIPS R4000 / Allegrex)

PSP is MIPS little-endian. No safetyhook, no Detours, no x86 SEH guarantees. But hooking is still mechanical — the catch is doing it without crashing the JIT.

## Constraints

1. **Fixed addresses.** PSP user-RAM EBOOTs load at `0x08804000` typically. All function addresses are absolute and known. No ASLR.
2. **MIPS instructions are 4-byte fixed-width.** Every patch is a multiple of 4 bytes. No partial overwrites.
3. **Branch delay slot.** Every `j` / `jal` / `beqz` / etc. is followed by a slot instruction that *also executes*. Hooks must preserve the instruction in the delay slot.
4. **JIT cache invalidation.** PPSSPP JIT-compiles MIPS to host code in blocks. If we patch MIPS bytes after JIT translation, the cache must be flushed for that range or the old translation keeps running.
5. **Instruction-cache flush.** On real PSP hardware, `sceKernelDcacheWritebackInvalidateAll()` + `sceKernelIcacheInvalidateAll()` must run after self-modifying code or the CPU runs stale instructions. PPSSPP simulates the same — our hook installer must trigger an icache invalidate.

## Three hook approaches

### A. Per-call-site JAL rewrite (most surgical)

For each `jal target` we want to redirect, overwrite the 4-byte instruction with `jal new_target`.

**`jal` encoding:** `0x0C000000 | ((target >> 2) & 0x03FFFFFF)`. Target must be in the same 256 MB region (PSP user-RAM is far smaller than that, so always safe).

**Example** — at `0x105AC` in `battle_input_dispatch`:
```asm
0x105AC:  0x0C039574    ; jal input_get_btn_make (0xE54D0)
0x105B0:  0x00002021    ; move $a0, $zero (delay slot: use_analog = 0)
```

To redirect to our hook at, say, `0x09F00000` (in EXTRA1_RAM region we already mapped):
```asm
0x105AC:  0x0C7C0000    ; jal 0x09F00000
0x105B0:  0x00002021    ; (unchanged — same delay slot)
```

That's a single 4-byte write. The delay-slot instruction stays in place — our hook just needs to accept the same args (`$a0 = use_analog`).

**Pros:**
- Smallest possible patch
- Multiple sites can be redirected independently
- Easy to revert (write the original 4 bytes back)

**Cons:**
- Need to find every call site (we have a list — 19+ in `battle_input_dispatch`)
- Each patch independent; if we miss one, that path bypasses the hook

### B. Function-entry trampoline (catch-all)

Patch the entry of `input_get_btn_make` itself so all callers go through us:

```asm
; Before, at 0xE54D0:
0xE54D0:  beqz $a0, .skip_analog     ; if (use_analog == 0) goto skip
0xE54D4:  lw   $v0, 0x14(g_player_input)   ; v0 = btn_make
...

; After:
0xE54D0:  j   trampoline_addr        ; absolute jump
0xE54D4:  nop                         ; safe delay slot

; trampoline (at our chosen address):
; 1. Detect if caller is in battle context (e.g., $ra in [battle_input_dispatch_start, battle_input_dispatch_end))
; 2. If yes, look up current character index, route to per-pad btn_make
; 3. Else, execute original two instructions and return to 0xE54D8
```

**`j` encoding:** `0x08000000 | ((target >> 2) & 0x03FFFFFF)` — same restriction.

**Pros:**
- Single patch covers every caller
- No need to enumerate 19+ sites

**Cons:**
- Need to walk the return address to disambiguate "called from battle" vs "called from menu". Adds branches per call.
- The original instructions at `0xE54D0/0xE54D4` are not naked NOPs — they're the start of real logic (`beqz $a0, ...`). The trampoline must preserve them, including the branch's relative offset (which would now be wrong if blindly copied — `beqz` uses PC-relative immediate). Branch-instruction relocation is the most error-prone part of trampoline hooking.

### C. PPSSPP-side ROM patch (we own the emulator)

Since we maintain this PPSSPP fork, we can detect "Tales of Phantasia: Narikiri Dungeon X" by disc ID (`NPJH50231`) at game load and apply patches automatically:

```cpp
// In Core/HLE/sceCtrl.cpp or a new tales-mp.cpp module:
void ApplyTalesMpPatches() {
    if (g_disc_id != "NPJH50231") return;
    // Patch 1: redirect all input_get_btn_make calls in battle_input_dispatch
    static const u32 callsites[] = {
        0x105ac, 0x10670, 0x106a4, 0x10aec, 0x10b04, 0x10b4c,
        0x10e10, 0x10ee0, 0x10f28, 0x11050, 0x110a0, 0x11190,
        0x112b8, 0x11310, 0x113f4, 0x1144c, 0x11538, 0x116ac,
        0x11710, 0x1177c, 0x117d4, 0x11960, 0x119bc,
    };
    const u32 hook_addr = HOOK_BTN_MAKE_PSP_ADDR;
    const u32 new_jal = 0x0C000000 | ((hook_addr >> 2) & 0x03FFFFFF);
    for (u32 site : callsites) {
        Memory::Write_U32(new_jal, PSP_RAM_BASE + site);
    }
    // Invalidate JIT cache for the patched region
    MIPSComp::jit->InvalidateCacheAt(PSP_RAM_BASE + 0x105ac, 0x119bc - 0x105ac + 4);
    // Inject the hook payload at HOOK_BTN_MAKE_PSP_ADDR
    Memory::WriteStruct(HOOK_BTN_MAKE_PSP_ADDR, hook_bytes_btn_make, sizeof(hook_bytes_btn_make));
}
```

**Pros:**
- No PRX plugin needed
- User just installs our PPSSPP, drops in the ROM, it works
- Game-version-specific patches are versioned with our PPSSPP binary
- We can ship pre-assembled hook payloads (run pspsdk once, embed bytes)

**Cons:**
- Tightly couples mod to our PPSSPP fork (can't easily share with mainline PPSSPP users)
- Need to maintain hook payloads as MIPS byte arrays in C++ source
- JIT cache invalidation hook needed (see existing patches in `Core/Debugger/Breakpoints.cpp` for how PPSSPP handles self-modifying-code already — breakpoints are exactly this pattern)

### D. PRX plugin (most portable)

Write a `tales_2p.prx` using pspsdk. Drop into `memstick/seplugins/`, enable via PPSSPP UI. On module load, the PRX:

1. Checks game ID via `sceKernelGetCompiledSdkVersion` / `sceKernelGetGameInfo`.
2. Reads MIPS code from the EBOOT to verify expected instruction bytes at known offsets (anti-version-mismatch sanity check).
3. Patches JAL targets in-place.
4. Provides the hook function bodies from within the PRX's own loaded address space.
5. Calls `sceKernelIcacheInvalidateAll()`.

PPSSPP already supports user-supplied PRX plugins via the same `seplugins/` flow real PSP-CFW uses. This is the most "respectful" approach.

**Pros:**
- Portable: works on any PPSSPP, and on real PSP CFW too
- Mod is auditable as standalone source
- Easy to distribute alongside the game patch

**Cons:**
- pspsdk build pipeline (already set up — see `tests/extra-pads/Makefile`)
- PRX plugins for PSP have historically been finicky in PPSSPP (mostly worked, some quirks)
- Slightly larger surface area for bugs

## Architectural reality check

The simple "redirect input reads to pad N" hook only does half the job, because **Tales of Phantasia: NDX is single-active-character LMBS:**

- `battle_state.current_char_idx (+1528)` holds the ONE character currently taking input.
- `battle_get_active_char(bs)` returns `char_ptrs[current_char_idx]`.
- The state machine (`battle_input_dispatch`) processes commands for THAT one character only.
- Other party members run on AI in the background, NOT through the state machine.
- Player can switch which character is "active" via L+arrow → triggers `battle_switch_active_char` (`0x139D8`) which re-initializes the new active character's per-frame state.

For real 2-player co-op, the state machine needs to process **multiple characters per frame**, each with their own pad. Two routes:

### Route A: "Take over" mode (easy v1)

Each pad's L+arrow can switch ITS OWN assigned character independently. P1 controls whoever `current_char_idx_p[0]` points to; P2 controls whoever `current_char_idx_p[1]` points to. They share command-select bandwidth — only one can be in the action menu at a time, but they can both control real-time movement/attacks of their assigned character.

Requires:
- Per-pad `current_char_idx_p[8]` global
- `battle_get_active_char` becomes `battle_get_active_char_for_pad(bs, pad_idx)` (with pad 0 reading current_char_idx_p[0], etc.)
- All ~6 callers updated to pass their pad context
- L+arrow handler in battle_input_dispatch reads which pad pressed L → updates that pad's current_char_idx_p
- The state machine still runs once per frame for "active commander" (pad 0 by default), so command menus serialize

### Route B: True parallel control (real v2)

Each pad runs an independent copy of the state machine for its character. Requires:
- Cloning the relevant battle_state fields per pad (main_state, cmd_substate, cmd_target, etc.)
- Running `battle_input_dispatch` N times per frame, each with a different "viewing" battle_state
- Resolving simultaneous-action conflicts (two players targeting the same enemy slot, etc.)

This is significantly more invasive. Route A first; Route B if there's demand.

## v1 implementation plan (Route A + approach C)

1. **Add `g_tales_mp.current_char_idx[NUM_VIRTUAL_PADS]` in our PPSSPP source** — sized off our `NUM_VIRTUAL_PADS = 8`.
2. **Write a small MIPS payload** (assembled offline via psp-as into a byte array):
   ```c
   // hook_btn_make: called instead of input_get_btn_make from battle sites.
   // Reads current_char_idx_p[caller_pad], returns btn_make for that pad.
   unsigned int hook_btn_make(int use_analog) {
       int pad = derive_caller_pad();  // see below
       SceCtrlData *p = (SceCtrlData *)(0x0E000000 + pad * 16);
       static unsigned int prev[8] = {0};
       unsigned int btn = p->Buttons & ~0x30000;  // strip HOME|HOLD
       unsigned int make = (btn ^ prev[pad]) & btn;
       prev[pad] = btn;
       return make;
   }
   ```
   Embed assembled bytes in C++.
3. **Inject payload at fixed unused address** — e.g., `0x09FF0000` in the EXTRA1 region we already added. Or allocate from the kernel via PPSSPP's HLE.
4. **`derive_caller_pad()` strategy** — at every patched JAL site, we know which character index that site corresponds to (it's all about `current_char_idx` — same one). So **all** sites in `battle_input_dispatch` are for `current_char_idx_p[X]` where X is the pad whose "turn" it is to command. Initially X=0 (P1), but pressing L+arrow on pad N temporarily sets X=N during that command session.
5. **L+arrow detection** — hook the existing L-arrow handler in `battle_input_dispatch` to also note which pad pressed L. Easy: an additional patched JAL site that calls our hook BEFORE the existing L+arrow logic, and our hook records `pending_pad = caller_pad_from_input_state()`.
6. **JIT invalidation** — call `MIPSComp::jit->InvalidateCacheAt(start, len)` for the patched range. Already exposed in our codebase.

This delivers:
- P1 commands character X normally, character X follows pad 0
- P2 holds L on pad 1 → claims a character; from then on pad 1 controls THAT character
- Both pads can independently move + attack their characters in real-time
- Command menus (TP, items) serialize: whoever opens the menu first holds the cmd machine

## Estimated work

| Task | Effort |
|------|--------|
| MIPS payload assembly + byte-array embedding | 1-2 hr |
| PPSSPP patch installer (Memory::Write_U32 + JIT invalidate) | 2-3 hr |
| Game-ID gate + safety checks | 1 hr |
| L+arrow ownership tracking | 2-3 hr |
| **Initial testing in actual battle** | 4-6 hr (iteration on edge cases) |
| Documenting binding-screen UX | 1 hr |
| **v1 total** | **~2 days** of focused work |

Compared to building this on real-PSP-CFW (which would need a PRX, sceKernel HLE compatibility, save-state handling), the PPSSPP-side approach is faster and ships immediately to anyone who downloads our PPSSPP-tales-mp binary.

## Self-modifying code in PPSSPP — JIT safety reference

PPSSPP already handles SMC in two places we can model from:
- `Core/Debugger/Breakpoints.cpp` — writes `0x0000000D` (`break`) into MIPS code at breakpoint addresses, invalidates JIT for that block. See `BreakPoints::AddBreakPoint`.
- `Core/MemMap.cpp` (in our fork) — view setup for `0x0E000000` already involves `g_arena.CreateView` with the same memory region the JIT reads from.

The JIT invalidation API is `MIPSComp::jit->InvalidateCacheAt(start, len)` (per `Core/MIPS/JitCommon/JitCommon.h`). Calling it after every `Memory::Write_U32` to a code address is sufficient.

## Status of recon

- Input system: fully decoded (`PlayerInputState` struct, getters, edge detection algorithm).
- Scene dispatcher: identified (`scene_main_loop` + state-machine cases).
- Battle main loop: identified (`battle_main_loop` running `input_dispatch` + state machine).
- Battle state struct: partial (`current_char_idx`, `main_state`, command sub-fields, `char_ptrs[]` array).
- Active-character lookup: identified (`battle_get_active_char`).
- Active-character switch: identified (`battle_switch_active_char`).
- 19+ input-call sites in `battle_input_dispatch`: enumerated with concrete addresses.

Remaining for v1 implementation:
- Locate the L+arrow handler inside `battle_input_dispatch`
- Confirm whether action animations are per-character or shared via the state machine
- Decide between approach C (PPSSPP-side) and approach D (PRX) — recommendation is C
