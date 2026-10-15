// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace rgui::test {
namespace {

struct Test {
    const char* name;
    TestFunction function;
};

std::vector<Test>& Tests() {
    static std::vector<Test> tests;
    return tests;
}

const char* g_current = "";
bool g_currentFailed = false;

bool Selected(std::string_view name, int argc, char** argv) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; i++) {
        if (name.substr(0, std::string_view(argv[i]).size()) == argv[i]) return true;
    }
    return false;
}

} // namespace

Registrar::Registrar(const char* name, TestFunction function) {
    Tests().push_back({name, function});
}

void Fail(const char* file, int line, const std::string& expression) {
    g_currentFailed = true;
    std::printf("FAIL %s %s:%d: %s\n", g_current, file, line, expression.c_str());
    std::fflush(stdout);
}

const char* SourceDir() {
    return RGUI_SOURCE_DIR;
}

const char* BuildDir() {
    return RGUI_BUILD_DIR;
}

// Runs the tests whose names begin with one of the arguments, or all of them without any.
int RunTests(int argc, char** argv) {
    int run = 0;
    int failed = 0;
    for (const Test& test : Tests()) {
        if (!Selected(test.name, argc, argv)) continue;
        g_current = test.name;
        g_currentFailed = false;
        const auto start = std::chrono::steady_clock::now();
        test.function();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        run++;
        if (g_currentFailed) {
            failed++;
            std::printf("failed %s (%.1f ms)\n", test.name, ms);
        } else {
            std::printf("ok %s (%.1f ms)\n", test.name, ms);
        }
        std::fflush(stdout);
    }
    if (run == 0) {
        std::printf("no test matches the given names\n");
        return 1;
    }
    std::printf("%d test(s), %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace rgui::test
