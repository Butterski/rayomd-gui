// SPDX-License-Identifier: Apache-2.0
// The settings file: Load of any bytes, the typed getters on every key, and two invariants:
// whatever Load keeps, SetString accepts; Save then Load gives the same values back.
#include "harness.h"

#include "common/file_io.h"
#include "ui/settingsfile.h"

#include <filesystem>
#include <string>

namespace rgui::fuzz {

int FuzzSettings(const uint8_t* data, size_t size) {
    const std::filesystem::path dir = TempDir();
    const std::filesystem::path input = dir / "settings.ini";
    if (!RayoMd::File::WriteAll(input, std::string(reinterpret_cast<const char*>(data), size))) {
        Fail("cannot write the input file");
    }
    ui::SettingsFile loaded;
    if (!loaded.Load(input)) return 0;

    ui::SettingsFile copy;
    for (const auto& [key, value] : loaded.Values()) {
        (void)loaded.GetInt(key, 0);
        (void)loaded.GetDouble(key, 0.0);
        (void)loaded.GetBool(key, false);
        if (!copy.SetString(key, value)) Fail("Load kept a key or value SetString refuses");
    }
    const std::filesystem::path saved = dir / "saved" / "settings.ini";
    if (!copy.Save(saved)) Fail("Save failed");
    ui::SettingsFile reloaded;
    if (!reloaded.Load(saved)) Fail("Load of a saved file failed");
    if (reloaded.Values() != loaded.Values()) Fail("Save then Load changed the values");
    return 0;
}

} // namespace rgui::fuzz
