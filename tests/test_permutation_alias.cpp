// Pipeline keys of shader permutations with identical SPIR-V (vk_permutation_alias.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>
#include "video_core/renderer_vulkan/vk_permutation_alias.h"

using namespace Vulkan;

struct Slot {
    bool valid;
    size_t target;
    bool same_state;
    std::uint64_t content;
};

static KeyPermutationChoice Choose(size_t self, std::uint64_t content, const std::vector<Slot>& slots,
                                   int* reads = nullptr) {
    return ChooseKeyPermutation(
        self, slots.size(), content,
        [&](size_t j) -> std::optional<PermutationCandidate> {
            if (!slots[j].valid) return std::nullopt;
            return PermutationCandidate{slots[j].target, slots[j].same_state};
        },
        [&](size_t j) -> std::optional<std::uint64_t> {
            if (reads) ++*reads;
            if (!slots[j].content) return std::nullopt;
            return slots[j].content;
        });
}

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // No other permutation: itself.
    auto c = Choose(0, 7, {{true, 0, true, 7}});
    assert(c.index == 0 && !c.same_spirv);

    // Identical SPIR-V and state at a lower index: that index.
    c = Choose(2, 7, {{true, 0, true, 5}, {true, 1, true, 7}, {true, 2, true, 7}});
    assert(c.index == 1 && c.same_spirv);

    // The lowest of several matches, through an earlier alias.
    c = Choose(3, 7, {{true, 0, true, 7}, {true, 0, true, 7}, {true, 2, true, 9}, {true, 3, true, 7}});
    assert(c.index == 0);
    c = Choose(3, 7, {{true, 0, true, 5}, {true, 0, true, 7}, {false, 0, false, 0}, {true, 3, true, 7}});
    assert(c.index == 0); // slot 1 already stands for 0

    // Same SPIR-V with another runtime info or fetch shader: kept apart, but reported.
    c = Choose(1, 7, {{true, 0, false, 7}, {true, 1, true, 7}});
    assert(c.index == 1 && c.same_spirv);

    // A higher index never becomes the key; empty slots and unknown SPIR-V are skipped.
    c = Choose(0, 7, {{true, 0, true, 7}, {true, 1, true, 7}});
    assert(c.index == 0);
    c = Choose(2, 7, {{false, 0, true, 7}, {true, 1, true, 0}, {true, 2, true, 7}});
    assert(c.index == 2 && !c.same_spirv);

    // Contents are only read for candidates that could lower the result.
    int reads = 0;
    c = Choose(3, 7, {{true, 0, true, 7}, {true, 1, true, 7}, {true, 2, true, 7}, {true, 3, true, 7}},
               &reads);
    assert(c.index == 0 && reads == 1);

    std::puts("permutation-alias-test: ok");
    return 0;
}
