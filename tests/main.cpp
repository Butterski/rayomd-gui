// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace rgui::test {
int RunTests(int argc, char** argv);
}

int main(int argc, char** argv) {
#ifndef _WIN32
    // The engine reads RAYOMD_FONT once per process: name DejaVu Sans before any export, so
    // PDFs come out the same on every machine that has it.
    static const char kDejaVu[] = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    if (!std::getenv("RAYOMD_FONT") && access(kDejaVu, R_OK) == 0) setenv("RAYOMD_FONT", kDejaVu, 0);
#endif
    return rgui::test::RunTests(argc, argv);
}
