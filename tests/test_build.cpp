// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "app/version.h"

#include <cstdio>
#include <string>

RGUI_TEST("build.version") {
    std::string text;
    if (FILE* file = std::fopen((std::string(rgui::test::SourceDir()) + "/VERSION").c_str(), "rb")) {
        char buffer[64];
        const size_t size = std::fread(buffer, 1, sizeof(buffer), file);
        std::fclose(file);
        text.assign(buffer, size);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    CHECK(!text.empty());
    CHECK_EQ(std::string(RAYOMD_GUI_VERSION), text);
    CHECK_EQ(std::string(rgui::Version()), text);
}
