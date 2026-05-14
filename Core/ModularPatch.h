// Modular patch system for PSP game-specific runtime patches.
//
// At __KernelLoadExec time, the kernel calls ModularPatch::ApplyForModule(name)
// with the PSP module name (e.g. "TOP_NARIKIRI_DUNGEON_R"). The patch system
// scans assets/patches/*.json for files whose `module_name_match` substring
// occurs in `name`, parses each match, and applies its payloads and patches
// to PSP RAM.
//
// This keeps game-specific MIPS payloads + hook addresses out of PPSSPP source.
// Adding support for another game means dropping in another JSON file.
//
// JSON schema (one file per game):
// {
//   "module_name_match": "TOP_NARIKIRI_DUNGEON_R",
//   "description":       "free-form prose",
//   "load_base":         "0x08804000",
//   "payloads": [
//     {
//       "name":         "main_hook",
//       "address":      "0x09F00000",
//       "instructions": [
//         "0x27BDFFF0",
//         { "hex": "0xAFBF0000", "asm": "sw $ra, 0($sp)" },
//         "JAL_ABS:0x088E94D0"
//       ]
//     }
//   ],
//   "patches": [
//     { "site": "+0x0105AC",  "type": "jal_to", "target": "0x09F00000" },
//     { "site": "+0x044478",  "type": "nop" },
//     { "site": "0x088E94D0", "type": "instr",  "value": "0x00000000" }
//   ]
// }
//
// Sites prefixed with "+" are interpreted as load_base-relative offsets.
// Instruction shortcuts in payloads: "JAL_ABS:0xADDR" emits the MIPS jal
// opcode for the absolute target address.

#pragma once

#include <string>
#include <string_view>

namespace ModularPatch {

// Scan assets/patches/*.json, parse each, and apply the first one whose
// `module_name_match` is a substring of `module_name`. Returns true if a
// patch file was found AND applied without errors.
//
// Safe to call from __KernelLoadExec. No-op if no matching patch file
// exists (this is the normal case for non-modded games).
bool ApplyForModule(std::string_view module_name);

// True if a patch was successfully applied during the current emulation
// session. Used by HLE diag code to gate per-game log readouts.
bool IsActive();

// Name of the currently-active patch (the `module_name_match` field of the
// JSON), or empty if no patch is active. Lets diag code namespace its logs.
std::string ActiveName();

}  // namespace ModularPatch
