// Tales of Phantasia: Narikiri Dungeon X — multiplayer patch installer.
//
// PSP user-RAM EBOOTs load at 0x08804000 by default. Static IDA addresses
// here are file-relative (load base 0), so all runtime targets are
// IDA_ADDR + PSP_LOAD_BASE. See hooking.md for the full design.

#include "Core/HLE/sceTalesMp.h"

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/JitCommon.h"

namespace TalesMp {

// Constants exported for ReadHookCounter() — see ApplyPatches() for details.
static constexpr u32 HOOK_COUNTER_ADDR = 0x09F00100;
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

	// Sanity check the load base by reading the first JAL we expect at site 0.
	// Expected: jal 0xE54D0 == 0x0C039574 (relative to PSP_LOAD_BASE=0x08804000,
	// runtime target = 0x088E94D0; jal encoding uses (target>>2)&0x03FFFFFF
	// so 0x088E94D0 >> 2 = 0x02239574, masked = 0x02239574, ored with 0x0C000000
	// = 0x0E239574). We're checking against the EBOOT loaded at PSP_LOAD_BASE.
	const u32 first_site_runtime = PSP_LOAD_BASE + BTN_MAKE_CALLSITES[0];
	const u32 expected_jal = EncodeJal(PSP_LOAD_BASE + INPUT_GET_BTN_MAKE_ADDR);
	const u32 actual = Memory::Read_U32(first_site_runtime);
	INFO_LOG(Log::Loader, "TalesMp: site[0]=%08x got=%08x expected=%08x",
		first_site_runtime, actual, expected_jal);

	if (actual != expected_jal) {
		ERROR_LOG(Log::Loader, "TalesMp: instruction mismatch at first patch site. "
			"Either the EBOOT isn't loaded yet, the load base is different, or "
			"this is a different game version. Aborting patcher.");
		return false;
	}

	int matched = 0, missed = 0;
	for (u32 site : BTN_MAKE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		const u32 instr = Memory::Read_U32(runtime_addr);
		if (instr == expected_jal) {
			matched++;
		} else {
			missed++;
			WARN_LOG(Log::Loader, "TalesMp:  site %08x got %08x, expected %08x",
				runtime_addr, instr, expected_jal);
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: pre-flight ok %d/%d sites%s",
		matched, (int)ARRAY_SIZE(BTN_MAKE_CALLSITES),
		missed ? " (some misses — aborting patch)" : "");
	if (missed > 0) {
		return false;
	}

	// MIPS hook payload — see encoding rationale at the top of this file.
	// 6 instructions: increment a counter at HOOK_COUNTER_ADDR, then tail-call
	// input_get_btn_make so existing semantics are preserved. The counter
	// proves the JIT invalidation worked: if zero after a run, the JIT is
	// still executing pre-patch translation.
	static constexpr u32 HOOK_ADDR = 0x09F00000;            // top of free user RAM
	// HOOK_COUNTER_ADDR is file-scope so ReadHookCounter() can see it.
	static const u32 hook_payload[] = {
		0x3C0809F0,  // lui   $t0, 0x09F0           ; t0 = 0x09F00000
		0x8D090100,  // lw    $t1, 0x100($t0)       ; t1 = *(0x09F00100)
		0x25290001,  // addiu $t1, $t1, 1           ; ++counter
		0xAD090100,  // sw    $t1, 0x100($t0)       ; *(0x09F00100) = t1
		0x0A23A534,  // j     0x088E94D0            ; tail-call input_get_btn_make
		0x00000000,  // nop                          ; delay slot
	};

	// Safety: hook region must be empty (read as zero). If not, we'd be
	// overwriting something the game is using.
	for (size_t i = 0; i < ARRAY_SIZE(hook_payload) + 1; ++i) {  // +1 for counter slot
		const u32 a = HOOK_ADDR + (u32)(i * 4);
		const u32 v = Memory::Read_U32(a);
		if (v != 0) {
			ERROR_LOG(Log::Loader, "TalesMp: hook region @ %08x not zero (got %08x). Aborting.", a, v);
			return false;
		}
	}
	const u32 counter_before = Memory::Read_U32(HOOK_COUNTER_ADDR);
	INFO_LOG(Log::Loader, "TalesMp: hook region clean. Counter @ %08x = %u (pre-patch)",
		HOOK_COUNTER_ADDR, counter_before);

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

	INFO_LOG(Log::Loader, "TalesMp: patch installed. Counter at %08x ticks per input_get_btn_make call.",
		HOOK_COUNTER_ADDR);
	g_patches_applied = true;
	return true;
}

u32 ReadHookCounter() {
	if (!g_patches_applied) return 0;
	if (!Memory::IsValidAddress(HOOK_COUNTER_ADDR)) return 0;
	return Memory::Read_U32(HOOK_COUNTER_ADDR);
}

void LogHookStatus() {
	if (!g_patches_applied) {
		INFO_LOG(Log::Loader, "TalesMp: status = patches not applied (game not matched)");
		return;
	}
	const u32 counter = ReadHookCounter();
	INFO_LOG(Log::Loader, "TalesMp: status = patches installed; hook fired %u times this session%s",
		counter, counter == 0 ? " (never reached battle code)" : "");
	// Verify the first patch site still holds our redirected JAL so we know
	// nobody overwrote it during play. Helps catch JIT-invalidation issues.
	if (Memory::IsValidAddress(PSP_LOAD_BASE + BTN_MAKE_CALLSITES[0])) {
		const u32 site_now = Memory::Read_U32(PSP_LOAD_BASE + BTN_MAKE_CALLSITES[0]);
		const u32 expected_jal_to_hook = EncodeJal(0x09F00000);
		INFO_LOG(Log::Loader, "TalesMp: post-run check: site[0] = %08x (%s)", site_now,
			site_now == expected_jal_to_hook ? "hooked, OK" : "OVERWRITTEN!");
	}
}

}  // namespace TalesMp
