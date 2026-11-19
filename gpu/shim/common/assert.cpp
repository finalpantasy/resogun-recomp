// resogun: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#ifdef _WIN32
#include <cinttypes>
#include <windows.h>
#endif

void assert_fail_impl() {
    std::fflush(stdout);
#ifdef _WIN32
    // Return addresses relative to the executable survive ASLR and can be symbolized offline:
    // llvm-symbolizer --relative-address --obj=out/resogun.exe 0x...
    void* frames[16];
    const auto depth = CaptureStackBackTrace(0, 16, frames, nullptr);
    const auto exe = GetModuleHandleW(nullptr);
    for (unsigned i = 0; exe && i < depth; ++i) {
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]), &module) &&
            module == exe) {
            const auto rva = reinterpret_cast<uintptr_t>(frames[i]) -
                             reinterpret_cast<uintptr_t>(exe);
            // A caller's return address is just after its call instruction.
            std::fprintf(stderr, "GPU assert: executable RVA 0x%" PRIxPTR "\n",
                         i && rva ? rva - 1 : rva);
        }
    }
#endif
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::fflush(stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
