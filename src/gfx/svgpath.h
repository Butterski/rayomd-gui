// SPDX-License-Identifier: Apache-2.0
// Icon paths: SVG path data with absolute M, L, H, V, C and Z only (implicit repeats allowed).
// Anything else, arcs and relative commands included, is refused.
#pragma once

#include "gfx/path.h"

#include <string_view>

namespace rgui::gfx {

// Appends the commands of `data` to `path`; false, with `path` unspecified, on anything else.
bool ParseSvgPath(std::string_view data, Path& path);

} // namespace rgui::gfx
