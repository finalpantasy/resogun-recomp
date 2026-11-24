// SPDX-License-Identifier: GPL-2.0-or-later
// Everybody's Golf miss-shot flash. When a swing is judged a miss the game draws one full-screen
// quad (pixel shader 0x76f7a64f, blend registers 0x61610009: ONE_MINUS_DST_COLOR, ZERO) for 30 game
// ticks, which inverts the whole frame, HUD included. settings.ini miss_shot_flash: 0 (default)
// hides it, 1 shows it as the game draws it. The same shader also draws the screen fades (normal
// alpha blend, black with a fading alpha); those always draw. Any other blend stays hidden (see
// SkipImpactInvert).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ResogunMissFlash {

constexpr uint64_t ImpactShaderHash = 0x76f7a64full;
/// The first blend control register (BlendControl, packed) of the miss-shot quad.
constexpr uint32_t MissFlashBlend = 0x61610009u;
/// The blend of the screen fades (title and menu fade to and from black): SRC_ALPHA,
/// ONE_MINUS_SRC_ALPHA, drawn with a black vertex color whose alpha runs 0xfe to 0.
constexpr uint32_t FadeBlend = 0x61610504u;

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

/// Whether a draw is skipped. The miss-shot quad is skipped unless the flash is on; the fade draws
/// are never skipped; any other draw of the impact shader stays skipped.
inline bool SkipImpactInvert(uint64_t fragment_hash, uint32_t blend0, bool flash_on) {
    if (fragment_hash != ImpactShaderHash) return false;
    if (blend0 == MissFlashBlend) return !flash_on;
    if (blend0 == FadeBlend) return false;
    return true;
}

} // namespace ResogunMissFlash
