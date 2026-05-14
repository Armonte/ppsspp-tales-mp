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

// 19 confirmed call sites to input_get_btn_make (0xE54D0) inside
// battle_input_dispatch (0xFCBC). Static addresses from the IDA recon.
// See tales-recon.md for how these were enumerated.
static constexpr u32 BTN_MAKE_CALLSITES[] = {
	0x0105ac, 0x010670, 0x0106a4, 0x010aec, 0x010b04, 0x010b4c,
	0x010e10, 0x010ee0, 0x010f28, 0x011050, 0x0110a0, 0x011190,
	0x0112b8, 0x011310, 0x0113f4, 0x01144c, 0x011538, 0x0116ac,
	0x011710, 0x01177c, 0x0117d4, 0x011960, 0x0119bc,
};

// Address of input_get_btn_make in the EBOOT (IDA-relative).
static constexpr u32 INPUT_GET_BTN_MAKE_ADDR = 0xE54D0;

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

	// MIPS hook payload — per-pad input router.
	//
	// $s2 holds battle_state pointer at every JAL site inside battle_input_dispatch.
	// We read current_char_idx (BattleState +0x5F8). Pad 0 tail-calls the original
	// input_get_btn_make (exact P1 semantics preserved). Pads 1-7 read raw buttons
	// from the MMIO mirror at 0x0E000000 + N*16, compute btn_make = (cur^prev)&cur
	// using a per-pad prev_buttons cache at 0x09F00200..0x09F0021F (8 pads x 4B).
	// Pad index >= 8 returns 0 (safety).
	//
	// Counter at 0x09F00100 keeps incrementing for diagnostic use, but only on
	// the pad-1..7 path; pad 0 tail-call skips it. To avoid that asymmetry, we
	// drop the counter entirely from this revision — ReadHookCounter still works
	// but now reads the prev_buttons cache for pad 0 (4 bytes).
	static constexpr u32 HOOK_ADDR = 0x09F00000;
	static const u32 hook_payload[] = {
		// 00: lbu   $t0, 0x5F8($s2)          ; t0 = current_char_idx (u8)
		0x924805F8,
		// 04: lui   $t9, 0x09F0                ; t9 = 0x09F00000 (scratch base)
		0x3C1909F0,
		// 08: sb    $t0, 0x100($t9)            ; *(u8*)(0x09F00100) = current_char_idx
		0xA3280100,
		// 0c: lw    $t9, 0x104($t9)            ; t9 = invocation counter
		0x8F390104,
		// 10: addiu $t9, $t9, 1                ; ++counter
		0x27390001,
		// 14: lui   $t1, 0x09F0
		0x3C0909F0,
		// 18: sw    $t9, 0x104($t1)            ; *(u32*)(0x09F00104) = counter
		0xAD390104,
		// 1c: beqz  $t0, .pad0  (offset = +22 instructions from delay-slot PC = 0x20; target 0x78)
		0x11000016,
		// 20: nop                              ; delay slot for beqz
		0x00000000,
		// 0c: sltiu $t1, $t0, 8                ; t1 = (char_idx < 8)
		0x2D090008,
		// 10: bnez  $t1, .valid_pad (offset +3)
		0x15200003,
		// 14: nop                              ; delay slot
		0x00000000,
		// 18: jr    $ra                        ; out-of-range pad → return 0
		0x03E00008,
		// 1c: or    $v0, $0, $0                ; delay slot: v0 = 0
		0x00001025,
		// 20: sll   $t1, $t0, 4                ; .valid_pad: t1 = idx * 16
		0x00084900,
		// 24: lui   $t2, 0x0E00                ; t2 = 0x0E000000
		0x3C0A0E00,
		// 28: addu  $t1, $t2, $t1              ; t1 = 0x0E000000 + idx*16
		0x01494821,
		// 2c: lw    $t2, 4($t1)                ; t2 = pad->Buttons
		0x8D2A0004,
		// 30: lui   $t3, 0xFFFC                ; t3 = 0xFFFC0000
		0x3C0BFFFC,
		// 34: ori   $t3, $t3, 0xFFFF           ; t3 = 0xFFFCFFFF (~HOME|HOLD)
		0x356BFFFF,
		// 38: and   $t2, $t2, $t3              ; t2 = buttons & ~0x30000
		0x014B5024,
		// 3c: lui   $t3, 0x09F0                ; t3 = 0x09F00000 (cache base)
		0x3C0B09F0,
		// 40: sll   $t4, $t0, 2                ; t4 = idx * 4
		0x00086080,
		// 44: addu  $t3, $t3, $t4              ; t3 = cache_base + idx*4
		0x016C5821,
		// 48: lw    $t4, 0x200($t3)            ; t4 = prev_buttons[idx]
		0x8D6C0200,
		// 4c: xor   $t5, $t2, $t4              ; t5 = cur ^ prev
		0x014C6826,
		// 50: and   $t5, $t5, $t2              ; t5 = btn_make = (cur^prev) & cur
		0x01AA6824,
		// 54: sw    $t2, 0x200($t3)            ; prev_buttons[idx] = cur
		0xAD6A0200,
		// 58: jr    $ra
		0x03E00008,
		// 5c: or    $v0, $t5, $0               ; delay slot: v0 = btn_make
		0x01A01025,
		// 60: j     0x088E94D0                 ; .pad0: tail-call original
		0x0A23A534,
		// 64: nop                              ; delay slot
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
	// a counter at 0x09F00104 on EVERY invocation (before any branching).
	// 0 = hook never ran. Any non-zero value = JIT did translate our patched
	// JAL and route through our hook.
	if (Memory::IsValidAddress(0x09F00104)) {
		const u32 hook_calls = Memory::Read_U32(0x09F00104);
		const u32 last_idx = Memory::Read_U8(0x09F00100);
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
