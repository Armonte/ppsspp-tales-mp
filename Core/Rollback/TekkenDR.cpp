#include "ppsspp_config.h"

#include "Core/Rollback/TekkenDR.h"

#include <string>

#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"

namespace Rollback {

namespace {

// Call sites of the per-player input handler, and what they call originally.
constexpr uint32_t kCallSite1 = 0x08805AD0;
constexpr uint32_t kCallSite2 = 0x08805AC0;
constexpr uint32_t kCallSite1Original = 0x0E201600;  // jal 0x08805800
constexpr uint32_t kCallSite2Original = 0x0E2015CC;  // jal 0x08805730
constexpr uint32_t kCallSite1Patched = 0x0E29ACB5;   // jal 0x08A6B2D4
constexpr uint32_t kCallSite2Patched = 0x0E29ACC2;   // jal 0x08A6B308

// Thunks, one per call site: run the original call (a1 = 0, player 1), then if a1 was 0 call it again with
// a0 = the host's player 2 input word and a1 = 1. Placed in an unused stretch of the game's data.
constexpr uint32_t kCave = 0x08A6B2D4;
constexpr uint32_t kCaveWords[] = {
	0x27bdfff0, 0xafbf000c, 0x14a00007, 0x00000000, 0x0e201600, 0x00000000, 0x3c0409b1, 0x8c8408d0,
	0x0e201600, 0x24050001, 0x8fbf000c, 0x03e00008, 0x27bd0010,
	0x27bdfff0, 0xafbf000c, 0x14a00007, 0x00000000, 0x0e2015cc, 0x00000000, 0x3c0409b1, 0x8c8408d0,
	0x0e2015cc, 0x24050001, 0x8fbf000c, 0x03e00008, 0x27bd0010,
};

// Player 2's input word as the handler reads it (its own bit order, see RawWord).
constexpr uint32_t kPlayer2Raw = 0x09B108D0;
// Versus: player 2's button word (CTRL_* bits) and stick (s16 each, -32767..32767).
constexpr uint32_t kPlayer2Buttons = 0x08B812A8;
constexpr uint32_t kPlayer2StickX = 0x08B812B0;
constexpr uint32_t kPlayer2StickY = 0x08B812B2;
// Game mode words: player 2 plays when both are 3 (versus), or the first is 34/35 and the second 3.
constexpr uint32_t kMode1 = 0x08B7519C;
constexpr uint32_t kMode2 = 0x08B756FC;

bool Player2Plays() {
	const uint32_t mode1 = Memory::ReadUnchecked_U32(kMode1);
	const uint32_t mode2 = Memory::ReadUnchecked_U32(kMode2);
	return mode2 == 3 && (mode1 == 3 || mode1 == 34 || mode1 == 35);
}

void InstallThunks() {
	if (Memory::ReadUnchecked_U32(kCallSite1) == kCallSite1Patched)
		return;
	for (size_t i = 0; i < sizeof(kCaveWords) / sizeof(kCaveWords[0]); i++)
		Memory::WriteUnchecked_U32(kCaveWords[i], kCave + (uint32_t)i * 4);
	Memory::WriteUnchecked_U32(kCallSite1Patched, kCallSite1);
	Memory::WriteUnchecked_U32(kCallSite2Patched, kCallSite2);
	currentMIPS->InvalidateICacheRangeImmediate(kCave, sizeof(kCaveWords));
	currentMIPS->InvalidateICacheRangeImmediate(kCallSite1, 4);
	currentMIPS->InvalidateICacheRangeImmediate(kCallSite2, 4);
}

// CTRL_* bits to the handler's input word.
uint32_t RawWord(uint32_t buttons) {
	static const struct { uint32_t ctrl, raw; } map[] = {
		{ CTRL_SELECT, 0x0100 }, { CTRL_START, 0x0800 }, { CTRL_UP, 0x1000 }, { CTRL_RIGHT, 0x2000 },
		{ CTRL_DOWN, 0x4000 }, { CTRL_LEFT, 0x8000 }, { CTRL_LTRIGGER, 0x0004 }, { CTRL_RTRIGGER, 0x0008 },
		{ CTRL_TRIANGLE, 0x0010 }, { CTRL_CIRCLE, 0x0020 }, { CTRL_CROSS, 0x0040 }, { CTRL_SQUARE, 0x0080 },
	};
	uint32_t raw = 0;
	for (const auto &m : map) {
		if (buttons & m.ctrl)
			raw |= m.raw;
	}
	return raw;
}

}  // namespace

bool IsTekkenDR() {
	return g_paramSFO.GetDiscID() == "ULUS10139";
}

void TekkenDRApplyPlayer2(uint32_t buttons, uint8_t analogX, uint8_t analogY) {
	// All addresses are in user RAM, which every TDR boot has.
	if (!Memory::IsValidRange(kCallSite2, 4) || !Memory::IsValidRange(kPlayer2Raw, 4) || !Player2Plays())
		return;

	buttons &= CTRL_SELECT | CTRL_START | CTRL_UP | CTRL_RIGHT | CTRL_DOWN | CTRL_LEFT | CTRL_LTRIGGER |
		CTRL_RTRIGGER | CTRL_TRIANGLE | CTRL_CIRCLE | CTRL_CROSS | CTRL_SQUARE;
	// The stick doubles as the d-pad when no direction is held (half deflection).
	const uint32_t dpad = CTRL_UP | CTRL_RIGHT | CTRL_DOWN | CTRL_LEFT;
	if (!(buttons & dpad)) {
		if (analogX >= 192)
			buttons |= CTRL_RIGHT;
		else if (analogX <= 63)
			buttons |= CTRL_LEFT;
		if (analogY <= 63)
			buttons |= CTRL_UP;
		else if (analogY >= 192)
			buttons |= CTRL_DOWN;
	}
	auto stick = [](uint8_t v) {
		const float f = ((float)v - 127.5f) / 127.5f;
		return (uint16_t)(int16_t)(f * 32767.0f);
	};

	Memory::WriteUnchecked_U32(buttons, kPlayer2Buttons);
	Memory::WriteUnchecked_U16(stick(analogX), kPlayer2StickX);
	Memory::WriteUnchecked_U16(stick(analogY), kPlayer2StickY);
	InstallThunks();
	Memory::WriteUnchecked_U32(RawWord(buttons), kPlayer2Raw);
}

}  // namespace Rollback
