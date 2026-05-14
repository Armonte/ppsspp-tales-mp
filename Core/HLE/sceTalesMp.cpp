// Tales of Phantasia: Narikiri Dungeon X — multiplayer patch installer.
//
// PSP user-RAM EBOOTs load at 0x08804000 by default. Static IDA addresses
// here are file-relative (load base 0), so all runtime targets are
// IDA_ADDR + PSP_LOAD_BASE. See hooking.md for the full design.

#include "Core/HLE/sceTalesMp.h"

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/Config.h"

namespace TalesMp {

// prev_buttons cache used by the hook (8 pads × 4 bytes, indexed by current_char_idx).
// Reading any pad's cache slot is enough to see whether the hook ever fired
// (non-zero means at least one input_get_btn_make call hit the hook for that pad).
static constexpr u32 HOOK_PREV_BUTTONS_CACHE = 0x09F00200;
static bool g_patches_applied = false;

// Where the EBOOT lives at runtime. The PSP loader applies relocations to
// this base, so every address from our IDA database needs PSP_LOAD_BASE
// added to it before we can poke it via Memory::Write_U32.
static constexpr u32 PSP_LOAD_BASE = 0x08804000;

// 23 confirmed call sites to input_get_btn_make (0xE54D0) inside
// battle_input_dispatch (0xFCBC). Static addresses from the IDA recon.
// See tales-recon.md for how these were enumerated.
static constexpr u32 BTN_MAKE_CALLSITES[] = {
	0x0105ac, 0x010670, 0x0106a4, 0x010aec, 0x010b04, 0x010b4c,
	0x010e10, 0x010ee0, 0x010f28, 0x011050, 0x0110a0, 0x011190,
	0x0112b8, 0x011310, 0x0113f4, 0x01144c, 0x011538, 0x0116ac,
	0x011710, 0x01177c, 0x0117d4, 0x011960, 0x0119bc,
};

// Two `jal battle_char_update_per_frame` sites in the dispatch loop's
// per-char tail (0x12694..0x128A0). Replaced with `jal char_update_wrapper`
// so we can swap g_player_input per-char when coop is active.
static constexpr u32 CHAR_UPDATE_CALLSITES[] = {
	0x012784, 0x0128a0,
};

// Address of input_get_btn_make in the EBOOT (IDA-relative).
static constexpr u32 INPUT_GET_BTN_MAKE_ADDR = 0xE54D0;
// Address of battle_char_update_per_frame in the EBOOT (IDA-relative).
static constexpr u32 CHAR_UPDATE_ADDR = 0x42C18;

bool IsSupportedDiscId(std::string_view disc_id) {
	// Tales of Phantasia: Narikiri Dungeon X
	//   ULJS00293 — Japan, original retail UMD (verified).
	//   NPJH50231 — Japan, PSN digital re-release (likely; not yet confirmed).
	// English fan-translation v0.04 patches text archives only, EBOOT unchanged.
	return disc_id == "ULJS00293" || disc_id == "NPJH50231";
}

// Computes the encoded `jal target` MIPS instruction word.
// Format: 0x0C000000 | ((target >> 2) & 0x03FFFFFF)
static constexpr u32 EncodeJal(u32 target) {
	return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu);
}

bool ApplyPatches() {
	INFO_LOG(Log::Loader, "TalesMp: applying patches for Tales of Phantasia: NDX");
	INFO_LOG(Log::Loader, "TalesMp: extra-pads state: bEnableExtraPads=%d m_pExtraPadMMIO=%p",
		(int)g_Config.bEnableExtraPads, (void*)Memory::m_pExtraPadMMIO);

	// Accept two states at the first site:
	//  - original JAL to input_get_btn_make: fresh boot, we need to install everything.
	//  - our hook JAL: re-apply path (after savestate load). We need to rewrite the
	//    hook payload (BSS region got wiped by the state restore) and re-invalidate JIT.
	const u32 first_site_runtime = PSP_LOAD_BASE + BTN_MAKE_CALLSITES[0];
	const u32 expected_jal = EncodeJal(PSP_LOAD_BASE + INPUT_GET_BTN_MAKE_ADDR);
	const u32 expected_jal_to_hook = EncodeJal(0x09F00000);
	const u32 actual = Memory::Read_U32(first_site_runtime);
	INFO_LOG(Log::Loader, "TalesMp: site[0]=%08x got=%08x  (expected_orig=%08x expected_hook=%08x)",
		first_site_runtime, actual, expected_jal, expected_jal_to_hook);

	if (actual != expected_jal && actual != expected_jal_to_hook) {
		ERROR_LOG(Log::Loader, "TalesMp: instruction at first patch site doesn't match either "
			"the original JAL (%08x) or our hook JAL (%08x). Aborting patcher.",
			expected_jal, expected_jal_to_hook);
		return false;
	}
	const bool re_apply = (actual == expected_jal_to_hook);
	if (re_apply) {
		INFO_LOG(Log::Loader, "TalesMp: site[0] already hooked — re-applying after savestate load");
	}

	int matched = 0, missed = 0;
	for (u32 site : BTN_MAKE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		const u32 instr = Memory::Read_U32(runtime_addr);
		if (instr == expected_jal || instr == expected_jal_to_hook) {
			matched++;
		} else {
			missed++;
			WARN_LOG(Log::Loader, "TalesMp:  site %08x got %08x, expected %08x or %08x",
				runtime_addr, instr, expected_jal, expected_jal_to_hook);
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: pre-flight ok %d/%d sites%s",
		matched, (int)ARRAY_SIZE(BTN_MAKE_CALLSITES),
		missed ? " (some misses — aborting patch)" : "");
	if (missed > 0) {
		return false;
	}

	// Pre-flight for the Phase 2 char_update_wrapper sites.
	static constexpr u32 WRAPPER_ADDR = 0x09F00080;
	const u32 expected_char_update_jal = EncodeJal(PSP_LOAD_BASE + CHAR_UPDATE_ADDR);
	const u32 expected_wrapper_jal     = EncodeJal(WRAPPER_ADDR);
	int wm = 0, wmiss = 0;
	for (u32 site : CHAR_UPDATE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		const u32 instr = Memory::Read_U32(runtime_addr);
		if (instr == expected_char_update_jal || instr == expected_wrapper_jal) {
			wm++;
		} else {
			wmiss++;
			WARN_LOG(Log::Loader, "TalesMp:  char_update site %08x got %08x, expected %08x or %08x",
				runtime_addr, instr, expected_char_update_jal, expected_wrapper_jal);
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: char_update pre-flight ok %d/%d sites%s",
		wm, (int)ARRAY_SIZE(CHAR_UPDATE_CALLSITES),
		wmiss ? " (some misses — aborting patch)" : "");
	if (wmiss > 0) {
		return false;
	}

	// MIPS hook payload — coop-flag forcer.
	//
	// IDA recon revealed Tales NDX has built-in 2-character co-op gated by
	// bs->coop_flag (+0x627). The per-frame dispatch loop in battle_input_dispatch
	// at 0x12694..0x128A0 iterates over all party chars and, when coop_flag is
	// set, treats char_ptrs[coop_char1 (+0x629)] AND char_ptrs[coop_char2 (+0x62A)]
	// as human-controlled (clears their +0x3E1 AI flag). With this enabled, both
	// chars run as humans through battle_char_update_per_frame.
	//
	// $s2 = BattleState pointer at every JAL site inside battle_input_dispatch.
	// We force coop_flag=1, coop_char1=0 (Cless/Mel), coop_char2=1 (Mint) every
	// frame, then tail-call the original input_get_btn_make so the rest of the
	// function executes normally. Diag counter at 0x09F00104 still ticks.
	//
	// NOTE: This is the EXPERIMENT to confirm coop_flag enables 2-char human
	// dispatch. g_player_input is still single-source — pad 1 input routing
	// is a follow-up step once we confirm the dual-char dispatch fires.
	static constexpr u32 HOOK_ADDR = 0x09F00000;
	static const u32 hook_payload[] = {
		// 00: addiu $t9, $0, 1                 ; t9 = 1
		0x24190001,
		// 04: sb    $t9, 0x627($s2)            ; bs->coop_flag = 1
		0xA2590627,
		// 08: sb    $0,  0x629($s2)            ; bs->coop_char1_idx = 0 (Cless)
		0xA2400629,
		// 0c: sb    $t9, 0x62A($s2)            ; bs->coop_char2_idx = 1 (Mint)
		0xA259062A,
		// 10: lui   $t9, 0x09F0                ; diag scratch base
		0x3C1909F0,
		// 14: lw    $t8, 0x400($t9)            ; t8 = invocation counter
		0x8F380400,
		// 18: addiu $t8, $t8, 1                ; ++counter
		0x27180001,
		// 1c: sw    $t8, 0x400($t9)            ; counter store
		0xAF380400,
		// 20: lbu   $t7, 0x5F8($s2)            ; t7 = current_char_idx (post-write)
		0x924F05F8,
		// 24: sb    $t7, 0x404($t9)            ; diag: char_idx
		0xA32F0404,
		// 28: j     0x088E94D0                 ; tail-call original input_get_btn_make
		0x0A23A534,
		// 2c: nop                              ; delay slot
		0x00000000,
	};

	// On fresh boot the hook region should be all zeros. On re-apply (after
	// savestate load), it may contain either our previous payload OR garbage
	// from the state — either way we'll just overwrite it.
	if (!re_apply) {
		for (size_t i = 0; i < ARRAY_SIZE(hook_payload); ++i) {
			const u32 a = HOOK_ADDR + (u32)(i * 4);
			const u32 v = Memory::Read_U32(a);
			if (v != 0) {
				ERROR_LOG(Log::Loader, "TalesMp: hook region @ %08x not zero (got %08x). Aborting.", a, v);
				return false;
			}
		}
		INFO_LOG(Log::Loader, "TalesMp: hook region clean (%zu instructions = %zu bytes at %08x)",
			ARRAY_SIZE(hook_payload), ARRAY_SIZE(hook_payload) * 4, HOOK_ADDR);
	} else {
		INFO_LOG(Log::Loader, "TalesMp: re-apply path, skipping hook-region zero check");
	}

	// Write hook payload to PSP RAM.
	for (size_t i = 0; i < ARRAY_SIZE(hook_payload); ++i) {
		Memory::Write_U32(hook_payload[i], HOOK_ADDR + (u32)(i * 4));
	}
	// Verify by reading back.
	for (size_t i = 0; i < ARRAY_SIZE(hook_payload); ++i) {
		const u32 v = Memory::Read_U32(HOOK_ADDR + (u32)(i * 4));
		if (v != hook_payload[i]) {
			ERROR_LOG(Log::Loader, "TalesMp: writeback verify failed at %08x: got %08x, expected %08x",
				HOOK_ADDR + (u32)(i * 4), v, hook_payload[i]);
			return false;
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: hook payload written + verified at %08x (%zu instructions, %zu bytes)",
		HOOK_ADDR, ARRAY_SIZE(hook_payload), ARRAY_SIZE(hook_payload) * 4);

	// Phase 2: char_update_wrapper at 0x09F00080.
	//
	// Replaces the 2 `jal battle_char_update_per_frame` sites inside the
	// dispatch loop tail of battle_input_dispatch (0x12784, 0x128A0). When
	// the dispatch loop is processing slot 1 (Mint) AND coop_flag is set:
	//   (a) Save bs->current_char_idx, then set it to char->+5 (Mint's id)
	//       so that char_advance_state_buffer's `cur_idx == char_id` gate
	//       passes and input reads apply to her.
	//   (b) Set char->+0x194 |= 2 to suppress her AI tick.
	//   (c) Overwrite g_player_input @ 0x46303C with pad-1's data (from
	//       MMIO @ 0x0E000014).
	//   (d) jal the real battle_char_update_per_frame.
	//   (e) Restore bs->current_char_idx so the rest of the frame sees the
	//       original active char.
	//
	// Pad-1 prev_buttons cache at 0x09F00304, saved cur_idx at 0x09F00308.
	//
	// Inputs: $a0 = char ptr (set by caller's delay slot); $s1 = party slot
	// idx (preserved across JAL by MIPS convention); $s2 = BattleState ptr.
	static const u32 wrapper_payload[] = {
		// 00: addiu $sp, $sp, -16
		0x27BDFFF0,
		// 04: sw    $ra, 0($sp)
		0xAFBF0000,
		// --- Wrapper entry diag: increment counter @ 0x09F00500 ---
		// 08: lui   $t8, 0x09F0
		0x3C1809F0,
		// 0c: lw    $t9, 0x500($t8)
		0x8F190500,
		// 10: addiu $t9, $t9, 1
		0x27390001,
		// 14: sw    $t9, 0x500($t8)         ; total wrapper invocations
		0xAF190500,
		// 18: sb    $s1, 0x520($t8)         ; last seen $s1 (party slot idx)
		0xA3110520,
		// --- check $s1 == 1 ---
		// 1c: addiu $t9, $0, 1
		0x24190001,
		// 20: bne   $s1, $t9, .normal_call  (offset 0x27 → instr 48)
		0x16390027,
		// 24: nop
		0x00000000,
		// --- check coop_flag ---
		// 28: lb    $t9, 0x627($s2)
		0x82590627,
		// --- $s1==1 diag: bump counter @ 0x09F00504 ---
		// 2c: lui   $t8, 0x09F0
		0x3C1809F0,
		// 30: lw    $t7, 0x504($t8)
		0x8F0F0504,
		// 34: addiu $t7, $t7, 1
		0x25EF0001,
		// 38: sw    $t7, 0x504($t8)         ; slot-1 wrapper hits
		0xAF0F0504,
		// 3c: beq   $t9, $0, .normal_call   (offset 0x20 → instr 48)
		0x13200020,
		// 40: nop
		0x00000000,
		// --- Mint slot + coop active: swap state, do tick, restore ---
		// 20: lb    $t9, 0x5F8($s2)         ; t9 = saved current_char_idx
		0x825905F8,
		// 24: lui   $t8, 0x09F0
		0x3C1809F0,
		// 28: sb    $t9, 0x308($t8)         ; *(0x09F00308) = saved cur_idx
		0xA3190308,
		// 2c: lb    $t9, 5($a0)             ; t9 = char->+5 (char_id, e.g. Mint=1)
		0x80990005,
		// 30: sb    $t9, 0x5F8($s2)         ; bs->current_char_idx = char_id
		0xA25905F8,
		// 34: lw    $t8, 0x194($a0)         ; char->status flags
		0x8C980194,
		// 38: ori   $t8, $t8, 2             ; set "AI bypass" bit
		0x37180002,
		// 3c: sw    $t8, 0x194($a0)
		0xAC980194,
		// 40: lui   $t8, 0x0E00             ; MMIO base
		0x3C180E00,
		// 44: lw    $t9, 0x14($t8)          ; pad-1 buttons (MMIO+0x14)
		0x8F190014,
		// 48: lui   $t7, 0xFFFC             ; mask high
		0x3C0FFFFC,
		// 4c: ori   $t7, $t7, 0xFFFF        ; mask = 0xFFFCFFFF
		0x35EFFFFF,
		// 50: and   $t9, $t9, $t7
		0x032FC824,
		// 54: lui   $t7, 0x09F0
		0x3C0F09F0,
		// 58: lw    $t6, 0x304($t7)         ; pad-1 prev buttons
		0x8DEE0304,
		// 5c: xor   $t5, $t9, $t6
		0x032E6826,
		// 60: and   $t5, $t5, $t9           ; btn_make
		0x01B96824,
		// 64: sw    $t9, 0x304($t7)         ; update prev
		0xADF90304,
		// 68: lui   $t8, 0x0046             ; g_player_input base
		0x3C180046,
		// 6c: ori   $t8, $t8, 0x303C
		0x3718303C,
		// 70: sw    $t5, 0($t8)             ; btn_make
		0xAF0D0000,
		// 74: sw    $t5, 4($t8)             ; btn_press
		0xAF0D0004,
		// 78: sw    $t9, 8($t8)             ; prev_buttons (held)
		0xAF190008,
		// 7c: jal   0x08846C18              ; sub_42C18
		0x0E211B06,
		// 80: nop                            ; delay slot
		0x00000000,
		// --- Restore current_char_idx ---
		// 84: lui   $t8, 0x09F0
		0x3C1809F0,
		// 88: lb    $t9, 0x308($t8)         ; load saved cur_idx
		0x83190308,
		// 8c: sb    $t9, 0x5F8($s2)         ; bs->current_char_idx = saved
		0xA25905F8,
		// 90: lw    $ra, 0($sp)
		0x8FBF0000,
		// 94: jr    $ra
		0x03E00008,
		// 98: addiu $sp, $sp, 16             ; delay slot
		0x27BD0010,
		// --- .normal_call: no swap, just forward ---
		// 9c: jal   0x08846C18
		0x0E211B06,
		// a0: nop
		0x00000000,
		// a4: lw    $ra, 0($sp)
		0x8FBF0000,
		// a8: jr    $ra
		0x03E00008,
		// ac: addiu $sp, $sp, 16
		0x27BD0010,
	};
	static_assert(ARRAY_SIZE(wrapper_payload) == 53, "wrapper_payload size changed; recompute branch offsets");

	// Write wrapper payload to PSP RAM.
	for (size_t i = 0; i < ARRAY_SIZE(wrapper_payload); ++i) {
		Memory::Write_U32(wrapper_payload[i], WRAPPER_ADDR + (u32)(i * 4));
	}
	for (size_t i = 0; i < ARRAY_SIZE(wrapper_payload); ++i) {
		const u32 v = Memory::Read_U32(WRAPPER_ADDR + (u32)(i * 4));
		if (v != wrapper_payload[i]) {
			ERROR_LOG(Log::Loader, "TalesMp: wrapper writeback verify failed at %08x: got %08x, expected %08x",
				WRAPPER_ADDR + (u32)(i * 4), v, wrapper_payload[i]);
			return false;
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: char_update_wrapper written + verified at %08x (%zu instructions, %zu bytes)",
		WRAPPER_ADDR, ARRAY_SIZE(wrapper_payload), ARRAY_SIZE(wrapper_payload) * 4);

	// Redirect the 2 char_update call sites.
	u32 cu_first = 0xFFFFFFFFu, cu_last = 0u;
	for (u32 site : CHAR_UPDATE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		Memory::Write_U32(expected_wrapper_jal, runtime_addr);
		if (runtime_addr < cu_first) cu_first = runtime_addr;
		if (runtime_addr > cu_last)  cu_last  = runtime_addr;
	}
	if (MIPSComp::jit) {
		MIPSComp::jit->InvalidateCacheAt(cu_first, (cu_last - cu_first) + 4);
		INFO_LOG(Log::Loader, "TalesMp: JIT invalidated char_update %08x..%08x", cu_first, cu_last + 4);
	}
	INFO_LOG(Log::Loader, "TalesMp: replaced %u char_update sites with jal %08x",
		(unsigned)ARRAY_SIZE(CHAR_UPDATE_CALLSITES), WRAPPER_ADDR);

	// Now redirect all 23 call sites.
	const u32 new_jal = EncodeJal(HOOK_ADDR);
	INFO_LOG(Log::Loader, "TalesMp: replacing %u call sites with jal %08x (encoded %08x)",
		(unsigned)ARRAY_SIZE(BTN_MAKE_CALLSITES), HOOK_ADDR, new_jal);
	u32 first_site = 0xFFFFFFFFu;
	u32 last_site  = 0u;
	for (u32 site : BTN_MAKE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		Memory::Write_U32(new_jal, runtime_addr);
		if (runtime_addr < first_site) first_site = runtime_addr;
		if (runtime_addr > last_site)  last_site  = runtime_addr;
	}
	// JIT cache invalidate the patched range. PPSSPP's JIT will re-translate
	// these blocks on next execution; until we do this, the old (pre-patch)
	// host code keeps running.
	if (MIPSComp::jit) {
		const u32 inv_start = first_site;
		const u32 inv_len = (last_site - first_site) + 4;
		MIPSComp::jit->InvalidateCacheAt(inv_start, inv_len);
		INFO_LOG(Log::Loader, "TalesMp: JIT invalidated %08x..%08x (%u bytes)",
			inv_start, inv_start + inv_len, inv_len);
	} else {
		WARN_LOG(Log::Loader, "TalesMp: no JIT instance? skipping cache invalidate.");
	}

	INFO_LOG(Log::Loader, "TalesMp: patch installed. Per-pad routing active "
		"(char_idx=0 -> original; char_idx 1..7 -> MMIO @ 0x0E0000_N0)");
	g_patches_applied = true;
	return true;
}

u32 ReadHookCounter() {
	// Now returns "did the hook ever fire for char_idx 1..7" — sum of non-zero
	// prev_buttons cache entries. Cleaner than a single counter that would only
	// tick on the pads-1..7 path anyway.
	if (!g_patches_applied) return 0;
	u32 total = 0;
	for (int i = 1; i < 8; ++i) {
		const u32 a = HOOK_PREV_BUTTONS_CACHE + (u32)(i * 4);
		if (Memory::IsValidAddress(a) && Memory::Read_U32(a) != 0) {
			total++;
		}
	}
	return total;
}

void LogHookStatus() {
	if (!g_patches_applied) {
		INFO_LOG(Log::Loader, "TalesMp: status = patches not applied (game not matched)");
		return;
	}
	// Definitive "did the hook fire even once?" check — our payload increments
	// a counter at 0x09F00400 on EVERY invocation (before any branching).
	// (Moved from 0x09F00104 in v2 to avoid colliding with the char_update
	// wrapper code region at 0x09F00080..0x09F0012F.)
	if (Memory::IsValidAddress(0x09F00400)) {
		const u32 hook_calls = Memory::Read_U32(0x09F00400);
		const u32 last_idx = Memory::Read_U8(0x09F00404);
		INFO_LOG(Log::Loader, "TalesMp: HOOK INVOCATIONS = %u  (last char_idx = %u)",
			hook_calls, last_idx);
	}
	// prev_buttons cache snapshot — only ticks NON-ZERO when pad N had any
	// buttons pressed at the moment the hook last fired for that pad. The
	// cache slots can be 0 even when the hook has fired thousands of times,
	// if the bound input was idle. So this isn't a reliable "fired" check —
	// see the JIT block check below for that.
	for (int i = 0; i < 8; ++i) {
		const u32 a = HOOK_PREV_BUTTONS_CACHE + (u32)(i * 4);
		INFO_LOG(Log::Loader, "TalesMp: prev_buttons[%d] @ %08x = %08x",
			i, a, Memory::IsValidAddress(a) ? Memory::Read_U32(a) : 0);
	}
	// MMIO pad slots — what __CtrlUpdateLatch is publishing for each pad.
	// If pad N's buttons here are 0 while the user is pressing input bound to
	// "Pad N+1 (virtual)" in the UI, the binding plumbing is broken.
	for (int i = 0; i < 8; ++i) {
		const u32 a = 0x0E000000 + (u32)(i * 16);  // CtrlData layout: u32 timestamp, u32 buttons, ...
		if (Memory::IsValidAddress(a)) {
			INFO_LOG(Log::Loader, "TalesMp: mmio_pad[%d] @ %08x  timestamp=%08x buttons=%08x",
				i, a, Memory::Read_U32(a), Memory::Read_U32(a + 4));
		}
	}
	// JIT-replacement check — PPSSPP's JIT writes a RUNBLOCK opcode
	// (0x68xxxxxx) over the first instruction of each compiled block. If our
	// patched JAL site has been replaced by 0x68... then PPSSPP DID compile
	// our patched code into a JIT block (block id in low bits). When that
	// block executes, host code runs the JIT translation of our patched JAL,
	// which calls our hook. So a 0x68... readback here is GOOD.
	for (size_t s = 0; s < ARRAY_SIZE(BTN_MAKE_CALLSITES); ++s) {
		const u32 a = PSP_LOAD_BASE + BTN_MAKE_CALLSITES[s];
		const u32 instr = Memory::IsValidAddress(a) ? Memory::Read_U32(a) : 0;
		const u32 expected_jal_to_hook = EncodeJal(0x09F00000);
		const char *what =
			instr == expected_jal_to_hook                 ? "our JAL, not JIT'd yet" :
			(instr & 0xFC000000u) == 0x68000000u           ? "JIT block (RUNBLOCK)" :
			                                                "UNEXPECTED";
		// Only log if not the expected steady state, OR for the first site.
		if (s == 0 || what[0] == 'U') {
			INFO_LOG(Log::Loader, "TalesMp: site[%zu] @ %08x = %08x (%s)", s, a, instr, what);
		}
	}
}

}  // namespace TalesMp
