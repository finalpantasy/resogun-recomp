// SPDX-License-Identifier: GPL-2.0-or-later
// Everybody's Golf miss-shot flash. When a swing is judged a miss the game draws one full-screen
// quad (pixel shader 0x76f7a64f, blend registers 0x61610009: ONE_MINUS_DST_COLOR, ZERO) for 30 game
// ticks, which inverts the whole frame, HUD included. settings.ini miss_shot_flash: 0 (default)
// hides it, 1 shows it as the game draws it. The same shader also draws with other blends; those
// stay hidden either way (see SkipImpactInvert).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ResogunMissFlash {

constexpr uint64_t ImpactShaderHash = 0x76f7a64full;
/// The first blend control register (BlendControl, packed) of the miss-shot quad.
constexpr uint32_t MissFlashBlend = 0x61610009u;

/// "1", "true", "yes" or "on" (any case) turn the flash on; anything else, empty included, is off.
inline bool ParseMissShotFlash(std::string_view value) {
    auto equals = [&](std::string_view word) {
        if (value.size() != word.size()) return false;
        for (size_t i = 0; i < word.size(); ++i) {
            const char c = value[i] >= 'A' && value[i] <= 'Z' ? char(value[i] - 'A' + 'a') : value[i];
            if (c != word[i]) return false;
        }
        return true;
    };
    return equals("1") || equals("true") || equals("yes") || equals("on");
}

/// Whether a draw is skipped: every draw of the impact shader, except the miss-shot quad when the
/// flash is on.
inline bool SkipImpactInvert(uint64_t fragment_hash, uint32_t blend0, bool flash_on) {
    if (fragment_hash != ImpactShaderHash) return false;
    return !(flash_on && blend0 == MissFlashBlend);
}

} // namespace ResogunMissFlash
