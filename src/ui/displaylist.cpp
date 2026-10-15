// SPDX-License-Identifier: Apache-2.0
#include "ui/displaylist.h"

#include "common/exact_math.h"

#include <cstring>

namespace rgui::ui {

namespace {

using RayoMd::ExactMath::Ceil;
using RayoMd::ExactMath::Floor;

template <typename T>
uint64_t Mix(uint64_t hash, const T& value) {
    return Fnv1a64(&value, sizeof(value), hash);
}

uint64_t MixRect(uint64_t hash, const gfx::RectF& r) {
    hash = Mix(hash, r.x);
    hash = Mix(hash, r.y);
    hash = Mix(hash, r.w);
    return Mix(hash, r.h);
}

uint64_t MixRect(uint64_t hash, const gfx::RectI& r) {
    hash = Mix(hash, r.x0);
    hash = Mix(hash, r.y0);
    hash = Mix(hash, r.x1);
    return Mix(hash, r.y1);
}

// The whole pixels a float rectangle touches.
gfx::RectI Cover(const gfx::RectF& r) {
    if (!(r.w > 0 && r.h > 0)) return {};
    const double limit = 1 << 28;
    auto clampD = [&](double v) { return v < -limit ? -limit : (v > limit ? limit : v); };
    return {static_cast<int>(Floor(clampD(r.x))), static_cast<int>(Floor(clampD(r.y))),
            static_cast<int>(Ceil(clampD(static_cast<double>(r.x) + r.w))),
            static_cast<int>(Ceil(clampD(static_cast<double>(r.y) + r.h)))};
}

gfx::RectF ToF(const gfx::RectI& r) {
    return {static_cast<float>(r.x0), static_cast<float>(r.y0), static_cast<float>(r.Width()),
            static_cast<float>(r.Height())};
}

} // namespace

void DisplayList::Reset(int width, int height, double scale) {
    commands_.clear();
    glyphs_.clear();
    clips_.clear();
    width_ = width < 0 ? 0 : width;
    height_ = height < 0 ? 0 : height;
    scale_ = scale;
    clips_.push_back({0, 0, width_, height_});
}

void DisplayList::Add(Command& command, gfx::RectI rawBounds) {
    command.clip = clips_.back();
    command.bounds = rawBounds.Intersect(command.clip);
    if (command.bounds.Empty()) return;
    uint64_t h = kFnvOffset;
    h = Mix(h, static_cast<uint8_t>(command.type));
    h = Mix(h, static_cast<uint8_t>(command.icon));
    h = Mix(h, command.opacity);
    h = Mix(h, command.color);
    h = MixRect(h, command.rect);
    h = Mix(h, command.radius);
    h = Mix(h, command.width);
    h = Mix(h, command.size);
    h = MixRect(h, command.clip);
    if (command.type == CommandType::Glyphs) {
        for (uint32_t i = 0; i < command.glyphCount; i++) {
            const GlyphInstance& g = glyphs_[command.glyphBegin + i];
            h = Mix(h, g.source ? g.source->SourceId() : 0u);
            h = Mix(h, g.glyph);
            h = Mix(h, g.x);
            h = Mix(h, g.y);
        }
    } else if (command.type == CommandType::Image && command.image) {
        h = Mix(h, command.image->id);
        h = Mix(h, command.image->version);
    }
    command.hash = h;
    commands_.push_back(command);
}

void DisplayList::FillRect(gfx::RectI rect, uint32_t color) {
    if (rect.Empty() || color == 0) return;
    Command c;
    c.type = CommandType::Rect;
    c.rect = ToF(rect);
    c.color = color;
    Add(c, rect);
}

void DisplayList::StrokeRect(gfx::RectI rect, int width, uint32_t color) {
    if (rect.Empty() || width <= 0 || color == 0) return;
    Command c;
    c.type = CommandType::RectStroke;
    c.rect = ToF(rect);
    c.width = static_cast<float>(width);
    c.color = color;
    Add(c, rect);
}

void DisplayList::FillRoundRect(gfx::RectF rect, float radius, uint32_t color) {
    if (color == 0) return;
    Command c;
    c.type = CommandType::RoundRect;
    c.rect = rect;
    c.radius = radius > 0 ? radius : 0;
    c.color = color;
    Add(c, Cover(rect));
}

void DisplayList::StrokeRoundRect(gfx::RectF rect, float radius, float width, uint32_t color) {
    if (!(width > 0) || color == 0) return;
    Command c;
    c.type = CommandType::RoundRectStroke;
    c.rect = rect;
    c.radius = radius > 0 ? radius : 0;
    c.width = width;
    c.color = color;
    Add(c, Cover(rect));
}

void DisplayList::Glyphs(const GlyphInstance* glyphs, size_t count, float sizePx, uint32_t color,
                         const gfx::RectI* ink) {
    if (count == 0 || !(sizePx > 0) || color == 0) return;
    gfx::RectI bounds;
    if (ink) {
        bounds = *ink;
    } else {
        float minX = glyphs[0].x, maxX = glyphs[0].x, minY = glyphs[0].y, maxY = glyphs[0].y;
        for (size_t i = 1; i < count; i++) {
            minX = std::min(minX, glyphs[i].x);
            maxX = std::max(maxX, glyphs[i].x);
            minY = std::min(minY, glyphs[i].y);
            maxY = std::max(maxY, glyphs[i].y);
        }
        const float x0 = minX - 0.3f * sizePx;
        const float y0 = minY - 1.25f * sizePx;
        bounds = Cover({x0, y0, maxX + 1.3f * sizePx - x0, maxY + 0.5f * sizePx - y0});
    }
    // Skip the copy when nothing of it can show.
    if (bounds.Intersect(clips_.back()).Empty()) return;
    Command c;
    c.type = CommandType::Glyphs;
    c.size = sizePx;
    c.color = color;
    c.glyphBegin = static_cast<uint32_t>(glyphs_.size());
    c.glyphCount = static_cast<uint32_t>(count);
    glyphs_.insert(glyphs_.end(), glyphs, glyphs + count);
    const size_t before = commands_.size();
    Add(c, bounds);
    if (commands_.size() == before) glyphs_.resize(c.glyphBegin);
}

void DisplayList::DrawImage(const gfx::Image& image, gfx::RectF dst, uint8_t opacity) {
    if (opacity == 0 || image.width <= 0 || image.height <= 0) return;
    Command c;
    c.type = CommandType::Image;
    c.rect = dst;
    c.opacity = opacity;
    c.image = &image;
    Add(c, Cover(dst));
}

void DisplayList::DrawIcon(Icon icon, gfx::RectI rect, uint32_t color) {
    if (rect.Empty() || color == 0) return;
    Command c;
    c.type = CommandType::Icon;
    c.icon = icon;
    c.rect = ToF(rect);
    c.color = color;
    Add(c, {rect.x0, rect.y0, rect.x0 + rect.Width(), rect.y0 + rect.Width()});
}

void DisplayList::PushClip(gfx::RectI rect) {
    gfx::RectI clip = rect.Intersect(clips_.back());
    if (clip.Empty()) clip = {clip.x0, clip.y0, clip.x0, clip.y0};
    Command c;
    c.type = CommandType::ClipPush;
    c.rect = ToF(rect);
    c.clip = clips_.back();
    c.bounds = clip;
    c.hash = MixRect(Mix(kFnvOffset, static_cast<uint8_t>(c.type)), clip);
    commands_.push_back(c);
    clips_.push_back(clip);
}

void DisplayList::PopClip() {
    if (clips_.size() <= 1) return;
    clips_.pop_back();
    Command c;
    c.type = CommandType::ClipPop;
    c.clip = clips_.back();
    c.hash = Mix(kFnvOffset, static_cast<uint8_t>(c.type));
    commands_.push_back(c);
}

void DisplayList::Append(const DisplayList& other) {
    const uint32_t shift = static_cast<uint32_t>(glyphs_.size());
    glyphs_.insert(glyphs_.end(), other.glyphs_.begin(), other.glyphs_.end());
    const size_t first = commands_.size();
    commands_.insert(commands_.end(), other.commands_.begin(), other.commands_.end());
    for (size_t i = first; i < commands_.size(); i++) {
        if (commands_[i].type == CommandType::Glyphs) commands_[i].glyphBegin += shift;
    }
}

} // namespace rgui::ui
