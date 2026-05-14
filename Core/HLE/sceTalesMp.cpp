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

	// First-cut: don't actually mutate code yet. Just enumerate what we WOULD
	// patch so we can verify the loop runs without crashing. Once we have
	// the MIPS hook payload assembled and placed in unused PSP RAM, this loop
	// will replace each `jal input_get_btn_make` with `jal <hook_addr>` and
	// then call jit->InvalidateCacheAt() to flush the JIT translation.
	int matched = 0, missed = 0;
	for (u32 site : BTN_MAKE_CALLSITES) {
		const u32 runtime_addr = PSP_LOAD_BASE + site;
		const u32 instr = Memory::Read_U32(runtime_addr);
		if (instr == expected_jal) {
			matched++;
			DEBUG_LOG(Log::Loader, "TalesMp:  site %08x ok (jal input_get_btn_make)", runtime_addr);
		} else {
			missed++;
			WARN_LOG(Log::Loader, "TalesMp:  site %08x got %08x, expected %08x",
				runtime_addr, instr, expected_jal);
		}
	}
	INFO_LOG(Log::Loader, "TalesMp: pre-flight done. %d/%d sites verified%s",
		matched, (int)ARRAY_SIZE(BTN_MAKE_CALLSITES),
		missed ? " (some misses — investigate before patching)" : "");

	// TODO(next iteration):
	//   1. Allocate or stake-claim a small region in PSP RAM for our hook
	//      payload (suggest 0x09FF0000, in the EXTRA1 view we already added).
	//   2. Write assembled MIPS bytes for hook_btn_make() into that region.
	//   3. Loop and Memory::Write_U32(EncodeJal(hook_addr), site).
	//   4. MIPSComp::jit->InvalidateCacheAt(first_site, last_site - first_site + 4).
	//   5. Same for input_get_btn_press / input_get_btn_held sites.
	return matched > 0;
}

}  // namespace TalesMp
