// SPDX-License-Identifier: GPL-2.0-or-later
// The miss-shot flash setting (gpu/shim/resogun_miss_flash.h): how settings.ini miss_shot_flash
// reads, and which impact-shader draws are skipped for each value (the fades never).
#include <cassert>
#include <cstdio>

#include "../gpu/shim/resogun_miss_flash.h"

using namespace ResogunMissFlash;

int main() {
    // Off is the default and anything unrecognized.
    assert(!ParseMissShotFlash("") && !ParseMissShotFlash("0") && !ParseMissShotFlash("off"));
    assert(!ParseMissShotFlash("2") && !ParseMissShotFlash("10") && !ParseMissShotFlash("maybe"));
    assert(ParseMissShotFlash("1") && ParseMissShotFlash("true") && ParseMissShotFlash("YES") &&
           ParseMissShotFlash("On"));

    // Flash off: the miss-shot quad is hidden; the screen fades (normal alpha blend) always draw.
    assert(SkipImpactInvert(ImpactShaderHash, MissFlashBlend, false));
    assert(!SkipImpactInvert(ImpactShaderHash, FadeBlend, false));
    assert(FadeBlend == 0x61610504u);
    // Flash on: the miss-shot quad draws too.
    assert(!SkipImpactInvert(ImpactShaderHash, MissFlashBlend, true));
    assert(!SkipImpactInvert(ImpactShaderHash, FadeBlend, true));
    // Any other blend of the shader (blending disabled included) stays hidden either way.
    assert(SkipImpactInvert(ImpactShaderHash, 0u, false));
    assert(SkipImpactInvert(ImpactShaderHash, 0u, true));
    assert(SkipImpactInvert(ImpactShaderHash, 0x12345678u, true));
    // Other shaders are never touched.
    assert(!SkipImpactInvert(0x1234ull, MissFlashBlend, false));
    assert(!SkipImpactInvert(0x1234ull, MissFlashBlend, true));
    std::puts("miss flash ok");
    return 0;
}
