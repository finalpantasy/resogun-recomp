// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};
    u32 user_data{};

    auto operator<=>(const Bindings&) const = default;
};

/// resogun: with graphics pipeline libraries (RESOGUN_PIPELINE_LIBRARY=1) the fragment shader's
/// resources stay in descriptor set 0 and the other graphics stages' move to set 1, binding
/// numbers unchanged, so a fragment library's layout depends on its own shader only. Set once by
/// the pipeline cache before any shader compiles.
inline bool& SplitStageSets() {
    static bool split = false;
    return split;
}

} // namespace Shader::Backend
