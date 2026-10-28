#pragma once

#include <cstdint>

// Tekken: Dark Resurrection (ULUS-10139): both netplay players on one emulated PSP, through the game's hidden local
// two-player path.
//
// The game reads player input through one handler, called per player. Two of its call sites (0x08805AC0,
// 0x08805AD0) are pointed at a small thunk (0x08A6B2D4 / 0x08A6B308) that calls the handler for player 1 as before
// and then again as player 2 with an input word the host writes to 0x09B108D0. In versus, the host also writes
// player 2's button word (0x08B812A8) and stick (0x08B812B0/2). Same layout and addresses as the libretro rollback core
//, so recordings from it play back the same way.

namespace Rollback {

bool IsTekkenDR();
// Applies player 2's input for the frame about to run. buttons: PSP CTRL_* bits; analog: 0..255, 128 = center.
void TekkenDRApplyPlayer2(uint32_t buttons, uint8_t analogX, uint8_t analogY);

}  // namespace Rollback
