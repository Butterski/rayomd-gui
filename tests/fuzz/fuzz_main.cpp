// SPDX-License-Identifier: Apache-2.0
// libFuzzer's entry for one harness, named by RGUI_FUZZ_HARNESS (tests/fuzz/CMakeLists.txt).
#include "harness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return rgui::fuzz::RGUI_FUZZ_HARNESS(data, size);
}

// GCC 16's libstdc++ headers, compiled by Clang, pair the nothrow operator new of
// std::get_temporary_buffer (std::stable_sort, std::inplace_merge) with a deallocation ASan sees
// as free(): a false alloc-dealloc-mismatch in the toolchain, not in our code.
extern "C" const char* __asan_default_options() { return "alloc_dealloc_mismatch=0"; }
