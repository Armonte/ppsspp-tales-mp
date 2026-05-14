#include "Core/ModularPatch.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Common.h"
#include "Common/Data/Format/JSONReader.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/JitCommon.h"

namespace {

// Set true on first successful ApplyForModule call; gates diag log code.
bool s_active = false;
std::string s_active_name;

// Default load base for user-RAM PSP EBOOTs.
constexpr u32 kDefaultLoadBase = 0x08804000;

// MIPS `jal target` encoding: top 6 bits = 0x000011 (0b000011 = opcode 3),
// low 26 bits = (target >> 2) & 0x03FFFFFF.
inline constexpr u32 EncodeJal(u32 target) {
	return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu);
}

// Parse `"0xHEX"` or `"DECIMAL"` to u32. Returns false on parse failure or
// out-of-range. We use strtoull so we accept full 32-bit hex values without
// signed-int truncation.
bool ParseU32(std::string_view str, u32 *out) {
	if (str.empty()) return false;
	std::string copy(str);
	const char *cstr = copy.c_str();
	char *end = nullptr;
	const int base = (str.size() >= 2 && (str[0] == '0') && (str[1] == 'x' || str[1] == 'X')) ? 16 : 10;
	unsigned long long v = strtoull(cstr, &end, base);
	if (end == cstr || v > 0xFFFFFFFFull) return false;
	*out = static_cast<u32>(v);
	return true;
}

// Parse a "site" string: either "+0xOFFSET" (load-base-relative) or
// "0xABSOLUTE" / "ABSOLUTE" (direct). Returns the resolved absolute address.
bool ParseSite(std::string_view str, u32 load_base, u32 *out) {
	if (str.empty()) return false;
	if (str[0] == '+') {
		u32 off;
		if (!ParseU32(str.substr(1), &off)) return false;
		*out = load_base + off;
		return true;
	}
	return ParseU32(str, out);
}

// Parse one entry in a payload's `instructions` array. Each entry is either:
//   "0xAABBCCDD"                    -> raw instruction word
//   "JAL_ABS:0xADDR"                -> jal opcode targeting ADDR
//   { "hex": "0xAABBCCDD", "asm": "..." }  -> raw with annotation
inline bool ParseInstruction(const JsonNode *node, u32 *out) {
	if (!node) return false;
	const JsonValue &v = node->value;
	std::string_view str;
	if (v.getTag() == JSON_STRING) {
		str = v.toString();
	} else if (v.getTag() == JSON_OBJECT) {
		const json::JsonGet obj(v);
		const char *hex = obj.getStringOrNull("hex");
		if (!hex) return false;
		str = hex;
	} else {
		return false;
	}
	if (str.size() > 8 && str.substr(0, 8) == "JAL_ABS:") {
		u32 target;
		if (!ParseU32(str.substr(8), &target)) return false;
		*out = EncodeJal(target);
		return true;
	}
	return ParseU32(str, out);
}

struct Payload {
	std::string name;
	u32 address;
	std::vector<u32> instructions;
};

enum class PatchType {
	Instr,    // write `value` verbatim
	Nop,      // write 0x00000000
	JalTo,    // write EncodeJal(target)
};

struct PatchSite {
	u32 address;
	PatchType type;
	u32 value;  // raw instr (Instr) or jal target (JalTo); unused for Nop
};

struct PatchFile {
	std::string module_name_match;
	std::string description;
	u32 load_base;
	std::vector<Payload> payloads;
	std::vector<PatchSite> patches;
};

bool ParsePayload(const JsonNode *payload_node, Payload *out) {
	if (!payload_node || payload_node->value.getTag() != JSON_OBJECT) return false;
	const json::JsonGet obj(payload_node->value);

	const char *name = obj.getStringOrNull("name");
	out->name = name ? name : "(unnamed)";

	const char *addr_str = obj.getStringOrNull("address");
	if (!addr_str || !ParseU32(addr_str, &out->address)) {
		ERROR_LOG(Log::Loader, "ModularPatch: payload '%s' missing/invalid 'address'", out->name.c_str());
		return false;
	}

	const JsonNode *instrs = obj.getArray("instructions");
	if (!instrs) {
		ERROR_LOG(Log::Loader, "ModularPatch: payload '%s' missing 'instructions' array", out->name.c_str());
		return false;
	}
	for (const JsonNode *it = instrs->value.toNode(); it; it = it->next) {
		u32 word = 0;
		if (!ParseInstruction(it, &word)) {
			ERROR_LOG(Log::Loader, "ModularPatch: payload '%s' has unparseable instruction at index %zu",
				out->name.c_str(), out->instructions.size());
			return false;
		}
		out->instructions.push_back(word);
	}
	return true;
}

bool ParsePatchSite(const JsonNode *patch_node, u32 load_base, PatchSite *out) {
	if (!patch_node || patch_node->value.getTag() != JSON_OBJECT) return false;
	const json::JsonGet obj(patch_node->value);

	const char *site_str = obj.getStringOrNull("site");
	if (!site_str || !ParseSite(site_str, load_base, &out->address)) {
		ERROR_LOG(Log::Loader, "ModularPatch: patch missing/invalid 'site'");
		return false;
	}

	const char *type_str = obj.getStringOrNull("type");
	if (!type_str) {
		ERROR_LOG(Log::Loader, "ModularPatch: patch @ 0x%08x missing 'type'", out->address);
		return false;
	}
	std::string_view tv(type_str);
	if (tv == "nop") {
		out->type = PatchType::Nop;
		out->value = 0;
	} else if (tv == "instr") {
		out->type = PatchType::Instr;
		const char *val_str = obj.getStringOrNull("value");
		if (!val_str || !ParseU32(val_str, &out->value)) {
			ERROR_LOG(Log::Loader, "ModularPatch: patch @ 0x%08x type=instr missing/invalid 'value'", out->address);
			return false;
		}
	} else if (tv == "jal_to") {
		out->type = PatchType::JalTo;
		const char *tgt_str = obj.getStringOrNull("target");
		if (!tgt_str || !ParseU32(tgt_str, &out->value)) {
			ERROR_LOG(Log::Loader, "ModularPatch: patch @ 0x%08x type=jal_to missing/invalid 'target'", out->address);
			return false;
		}
	} else {
		ERROR_LOG(Log::Loader, "ModularPatch: patch @ 0x%08x unknown type '%s'", out->address, type_str);
		return false;
	}
	return true;
}

// Read and parse a single patch JSON file from VFS.
bool LoadPatchFile(const std::string &vfs_path, PatchFile *out) {
	size_t size = 0;
	uint8_t *data = g_VFS.ReadFile(vfs_path, &size);
	if (!data) {
		WARN_LOG(Log::Loader, "ModularPatch: failed to read %s", vfs_path.c_str());
		return false;
	}
	json::JsonReader reader((const char *)data, size);
	delete[] data;
	if (!reader.ok()) {
		ERROR_LOG(Log::Loader, "ModularPatch: %s is not valid JSON", vfs_path.c_str());
		return false;
	}
	const json::JsonGet root = reader.root();

	const char *match = root.getStringOrNull("module_name_match");
	if (!match) {
		ERROR_LOG(Log::Loader, "ModularPatch: %s missing 'module_name_match'", vfs_path.c_str());
		return false;
	}
	out->module_name_match = match;
	const char *desc = root.getStringOrNull("description");
	out->description = desc ? desc : "";

	out->load_base = kDefaultLoadBase;
	const char *lb_str = root.getStringOrNull("load_base");
	if (lb_str) {
		if (!ParseU32(lb_str, &out->load_base)) {
			ERROR_LOG(Log::Loader, "ModularPatch: %s has invalid 'load_base'", vfs_path.c_str());
			return false;
		}
	}

	const JsonNode *payloads = root.getArray("payloads");
	if (payloads) {
		for (const JsonNode *it = payloads->value.toNode(); it; it = it->next) {
			Payload p;
			if (!ParsePayload(it, &p)) return false;
			out->payloads.push_back(std::move(p));
		}
	}
	const JsonNode *patches = root.getArray("patches");
	if (patches) {
		for (const JsonNode *it = patches->value.toNode(); it; it = it->next) {
			PatchSite s;
			if (!ParsePatchSite(it, out->load_base, &s)) return false;
			out->patches.push_back(s);
		}
	}
	return true;
}

// Write payloads + patches to PSP RAM and invalidate JIT.
bool ApplyPatchFile(const PatchFile &pf) {
	INFO_LOG(Log::Loader, "ModularPatch: applying '%s' (%zu payloads, %zu patches): %s",
		pf.module_name_match.c_str(), pf.payloads.size(), pf.patches.size(),
		pf.description.empty() ? "(no description)" : pf.description.c_str());

	for (const Payload &p : pf.payloads) {
		INFO_LOG(Log::Loader, "ModularPatch: payload '%s' @ 0x%08x (%zu instrs)",
			p.name.c_str(), p.address, p.instructions.size());
		for (size_t i = 0; i < p.instructions.size(); ++i) {
			const u32 a = p.address + static_cast<u32>(i * 4);
			Memory::Write_U32(p.instructions[i], a);
		}
		// Read-back verify to catch RAM-not-yet-mapped or partial writes.
		for (size_t i = 0; i < p.instructions.size(); ++i) {
			const u32 a = p.address + static_cast<u32>(i * 4);
			const u32 v = Memory::Read_U32(a);
			if (v != p.instructions[i]) {
				ERROR_LOG(Log::Loader, "ModularPatch: writeback verify failed for '%s' @ 0x%08x: got 0x%08x, expected 0x%08x",
					p.name.c_str(), a, v, p.instructions[i]);
				return false;
			}
		}
		// Invalidate JIT cache for the payload region. After savestate load,
		// PPSSPP may have JIT-compiled blocks that cached the OLD wrapper
		// bytes; without an explicit invalidate they will re-execute stale
		// host code that doesn't match the freshly-written MIPS payload. The
		// pre-refactor sceTalesMp didn't invalidate either, but its pre-flight
		// abort masked this: if state restored unexpected bytes, patches were
		// skipped, so the JIT had nothing new to be stale about. We patch
		// unconditionally now, so we must invalidate explicitly.
		if (MIPSComp::jit && !p.instructions.empty()) {
			MIPSComp::jit->InvalidateCacheAt(p.address,
				static_cast<int>(p.instructions.size() * 4));
		}
	}

	for (const PatchSite &s : pf.patches) {
		u32 word = 0;
		switch (s.type) {
			case PatchType::Nop:    word = 0x00000000u; break;
			case PatchType::Instr:  word = s.value; break;
			case PatchType::JalTo:  word = EncodeJal(s.value); break;
		}
		Memory::Write_U32(word, s.address);
		if (MIPSComp::jit) {
			MIPSComp::jit->InvalidateCacheAt(s.address, 4);
		}
	}
	INFO_LOG(Log::Loader, "ModularPatch: '%s' applied successfully", pf.module_name_match.c_str());
	return true;
}

}  // namespace

namespace ModularPatch {

bool ApplyForModule(std::string_view module_name) {
	INFO_LOG(Log::Loader, "ModularPatch: ApplyForModule('%.*s')", (int)module_name.size(), module_name.data());
	// Find candidate JSON files under assets/patches/. We don't have a
	// directory-listing helper here that's both VFS-aware and reliable on
	// every backend, so we use the static well-known location and try the
	// `<module_name>.json` lookup directly first, then fall back to the
	// canonical "tales of phantasia ndx" name kept here as the seed entry.
	// Future games: drop their JSON file in assets/patches/ named exactly
	// after their module name (matched via substring on `module_name_match`).
	const std::vector<std::string> candidates = {
		std::string("patches/") + std::string(module_name) + ".json",
		"patches/TOP_NARIKIRI_DUNGEON_R.json",
	};
	for (const std::string &path : candidates) {
		if (!g_VFS.Exists(path)) {
			continue;
		}
		PatchFile pf;
		if (!LoadPatchFile(path, &pf)) {
			continue;
		}
		if (module_name.find(pf.module_name_match) == std::string_view::npos) {
			// File exists but its match string doesn't apply to this module.
			continue;
		}
		if (!ApplyPatchFile(pf)) {
			continue;
		}
		s_active = true;
		s_active_name = pf.module_name_match;
		return true;
	}
	return false;
}

bool IsActive() { return s_active; }
std::string ActiveName() { return s_active_name; }

}  // namespace ModularPatch
