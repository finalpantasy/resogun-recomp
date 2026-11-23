// SPDX-License-Identifier: GPL-2.0-or-later
// resogun: which permutation's stage hash a shader permutation's pipelines use.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Vulkan {

/// resogun: one other permutation of a program, as ChooseKeyPermutation sees it.
struct PermutationCandidate {
    size_t target;   ///< the index its pipeline keys use (its alias, else itself)
    bool same_state; ///< equal runtime info and fetch shader
};

struct KeyPermutationChoice {
    size_t index;    ///< the permutation whose stage hash pipelines of `self` use
    bool same_spirv; ///< another permutation had identical SPIR-V (aliased or not)
};

/// resogun: the lowest permutation index whose pipelines can stand in for `self`'s: identical
/// SPIR-V (`content_of`, called only for candidates that could lower the result; nullopt when
/// unknown) and identical runtime info and fetch shader. `candidate(j)` is nullopt for empty
/// slots. Pure, so the rule is tested without a device.
template <typename Candidate, typename ContentOf>
KeyPermutationChoice ChooseKeyPermutation(size_t self, size_t count, std::uint64_t content,
                                          Candidate&& candidate, ContentOf&& content_of) {
    KeyPermutationChoice choice{self, false};
    for (size_t j = 0; j < count; ++j) {
        if (j == self) {
            continue;
        }
        const std::optional<PermutationCandidate> other = candidate(j);
        if (!other || other->target >= choice.index) {
            continue;
        }
        const std::optional<std::uint64_t> other_content = content_of(j);
        if (!other_content || *other_content != content) {
            continue;
        }
        choice.same_spirv = true;
        if (other->same_state) {
            choice.index = other->target;
        }
    }
    return choice;
}

} // namespace Vulkan
