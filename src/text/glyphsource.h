// SPDX-License-Identifier: Apache-2.0
// What the glyph cache rasterizes: a font with a process-unique id and glyph outlines.
#pragma once

#include "gfx/geometry.h"
#include "gfx/path.h"

#include <cstdint>

namespace rgui::text {

class GlyphSource {
public:
    // Unique within the process for every opened font (an atomic counter, never 0), so glyph
    // caches can key on it.
    virtual uint32_t SourceId() const = 0;
    // The glyph's outline in the font's own units, y up, appended to `out` after clearing it.
    // False, with `out` empty, when the glyph does not exist or its data is malformed.
    virtual bool GlyphPath(uint16_t glyph, gfx::Path& out) const = 0;
    // The transform from those units to em units (1 em = 1, y up).
    virtual gfx::Matrix UnitsToEm(uint16_t glyph) const = 0;

protected:
    ~GlyphSource() = default;
};

// A fresh source id from the process-wide counter.
uint32_t NewGlyphSourceId();

} // namespace rgui::text
