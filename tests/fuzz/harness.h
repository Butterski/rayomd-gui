// SPDX-License-Identifier: Apache-2.0
// The fuzz harnesses: one function per reader of outside data, each taking any bytes. Built two
// ways: as libFuzzer targets (RGUI_FUZZ=ON, tests/fuzz/CMakeLists.txt: fuzz_main.cpp calls one
// harness per executable), and into rayomd-gui-tests, whose fuzz.regressions test replays every
// input under tests/fuzz/regressions/<harness>/ through the same function.
#pragma once

#include <cstddef>
#include <cstdint>

struct rgui_wl_interface;

namespace rgui::fuzz {

int FuzzPdf(const uint8_t* data, size_t size);
int FuzzJpeg(const uint8_t* data, size_t size);
int FuzzFont(const uint8_t* data, size_t size);
int FuzzTheme(const uint8_t* data, size_t size);
int FuzzFontconf(const uint8_t* data, size_t size);
int FuzzSettings(const uint8_t* data, size_t size);
int FuzzMarkdown(const uint8_t* data, size_t size);
int FuzzHighlight(const uint8_t* data, size_t size);
#if defined(__linux__)
int FuzzXkb(const uint8_t* data, size_t size);
int FuzzCompose(const uint8_t* data, size_t size);
int FuzzDbus(const uint8_t* data, size_t size);
int FuzzWlWire(const uint8_t* data, size_t size);
// The objects FuzzWlWire creates, in order: objects[i] has id i + 2 (1 is wl_display).
size_t WlWireObjects(const rgui_wl_interface* const** objects);
#endif

// A broken invariant: prints it and aborts, so libFuzzer (and the replay test) see a crash.
[[noreturn]] void Fail(const char* what);

// A private directory for harnesses that must go through a file, made once per process.
const char* TempDir();

} // namespace rgui::fuzz
