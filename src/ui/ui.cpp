// SPDX-License-Identifier: Apache-2.0
#include "ui/ui.h"

#include "common/exact_math.h"

#include <algorithm>

namespace rgui::ui {

namespace {

using RayoMd::ExactMath::LlRound;

// Frames a state or shaped run may go unused before it is dropped.
constexpr uint64_t kStateFrames = 120;

int Round(double v) {
    const double limit = 1 << 28;
    if (!(v > -limit)) return -(1 << 28);
    if (!(v < limit)) return 1 << 28;
    return static_cast<int>(LlRound(v));
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

// ---- Layout ----

RectF CutTop(RectF& r, float amount) {
    const float a = Clamp(amount, 0, r.h);
    const RectF piece{r.x, r.y, r.w, a};
    r.y += a;
    r.h -= a;
    return piece;
}

RectF CutBottom(RectF& r, float amount) {
    const float a = Clamp(amount, 0, r.h);
    r.h -= a;
    return {r.x, r.y + r.h, r.w, a};
}

RectF CutLeft(RectF& r, float amount) {
    const float a = Clamp(amount, 0, r.w);
    const RectF piece{r.x, r.y, a, r.h};
    r.x += a;
    r.w -= a;
    return piece;
}

RectF CutRight(RectF& r, float amount) {
    const float a = Clamp(amount, 0, r.w);
    r.w -= a;
    return {r.x + r.w, r.y, a, r.h};
}

RectF Inset(RectF r, float d) { return Inset(r, d, d); }

RectF Inset(RectF r, float dx, float dy) {
    const float ix = std::min(dx, r.w / 2);
    const float iy = std::min(dy, r.h / 2);
    return {r.x + ix, r.y + iy, std::max(0.0f, r.w - 2 * ix), std::max(0.0f, r.h - 2 * iy)};
}

RectF Centered(RectF r, float w, float h) { return {r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h}; }

std::string_view VisibleLabel(std::string_view label) {
    const size_t at = label.find("##");
    return at == std::string_view::npos ? label : label.substr(0, at);
}

// ---- SmoothScroll ----

double SmoothScroll::Ease(double x) {
    if (!(x > 0)) return 0;
    if (!(x < 1)) return 1;
    // cubic-bezier(0.42, 0, 0.58, 1): x(s) is monotonic, so bisect s, then evaluate y(s).
    const double x1 = 0.42, x2 = 0.58;
    double lo = 0, hi = 1, s = x;
    for (int i = 0; i < 40; i++) {
        s = (lo + hi) / 2;
        const double u = 1 - s;
        const double xs = 3 * u * u * s * x1 + 3 * u * s * s * x2 + s * s * s;
        if (xs < x) lo = s; else hi = s;
    }
    const double u = 1 - s;
    return 3 * u * s * s + s * s * s;   // y1 = 0, y2 = 1
}

void SmoothScroll::SetMax(float max) {
    max_ = max > 0 ? max : 0;
    target_ = Clamp(target_, 0, max_);
    start_ = Clamp(start_, 0, max_);
}

float SmoothScroll::RawValue(double nowMs) const {
    if (!animating_ || !(nowMs < startMs_ + durationMs_)) return target_;
    const double t = (nowMs - startMs_) / durationMs_;
    return Clamp(static_cast<float>(start_ + (target_ - start_) * Ease(t)), 0, max_);
}

void SmoothScroll::AddNotches(float notches, double nowMs, float step) {
    if (notches == 0 || !(step > 0)) return;
    const float current = RawValue(nowMs);
    target_ = Clamp(target_ + notches * step, 0, max_);
    const float distance = target_ > current ? target_ - current : current - target_;
    if (distance < 0.01f) {
        JumpTo(target_);
        return;
    }
    const double pending = std::min(5.0, std::max(1.0, static_cast<double>(distance / step)));
    start_ = current;
    startMs_ = nowMs;
    durationMs_ = 200.0 - 25.0 * (pending - 1.0);
    animating_ = true;
}

void SmoothScroll::AddPixels(float pixels) { JumpTo(target_ + pixels); }

void SmoothScroll::EaseTo(float value, double nowMs) {
    const float current = RawValue(nowMs);
    target_ = Clamp(value, 0, max_);
    if ((target_ > current ? target_ - current : current - target_) < 0.01f) {
        JumpTo(target_);
        return;
    }
    start_ = current;
    startMs_ = nowMs;
    durationMs_ = 200.0;
    animating_ = true;
}

void SmoothScroll::JumpTo(float value) {
    target_ = Clamp(value, 0, max_);
    start_ = target_;
    animating_ = false;
}

float SmoothScroll::Value(double nowMs, double scale) const {
    const float v = RawValue(nowMs);
    if (!(scale > 0)) return v;
    return Clamp(static_cast<float>(Round(v * scale) / scale), 0, max_);
}

// ---- Context ----

Context::Context() : clipboardArrivals_(std::make_shared<std::unordered_map<Id, std::string>>()) {}
Context::~Context() = default;

void Context::SetFonts(const text::FontSet* fonts) {
    fonts_ = fonts;
    shapes_.clear();
}

void Context::SetTokens(const Tokens* tokens) { tokens_ = tokens; }

void Context::SetClipboard(std::function<void(std::string)> set,
                           std::function<void(std::function<void(std::string)>)> request) {
    clipboardSet_ = std::move(set);
    clipboardRequest_ = std::move(request);
}

void Context::AddEvent(const InputEvent& event, double nowMs) {
    int count = 0;
    if (event.type == InputEvent::Type::Button && event.pressed && event.button == MouseButton::Left) {
        const float dx = event.x - lastClickX_, dy = event.y - lastClickY_;
        if (nowMs - lastClickMs_ <= doubleClickMs_ && dx <= 4 && dx >= -4 && dy <= 4 && dy >= -4) {
            count = lastClickCount_ + 1;
        } else {
            count = 1;
        }
        lastClickMs_ = nowMs;
        lastClickX_ = event.x;
        lastClickY_ = event.y;
        lastClickCount_ = count;
    }
    pending_.push_back(event);
    pendingClickCounts_.push_back(count);
}

void Context::BeginFrame(int widthPx, int heightPx, double scale, double nowMs) {
    frame_++;
    now_ = nowMs;
    scale_ = scale > 0 ? scale : 1.0;
    widthPx_ = widthPx > 0 ? widthPx : 0;
    heightPx_ = heightPx > 0 ? heightPx : 0;
    width_ = static_cast<float>(widthPx_ / scale_);
    height_ = static_cast<float>(heightPx_ / scale_);

    events_.swap(pending_);
    pending_.clear();
    clickCounts_.swap(pendingClickCounts_);
    pendingClickCounts_.clear();
    consumed_.assign(events_.size(), 0);

    for (int b = 0; b < 3; b++) pointer_.pressed[b] = pointer_.released[b] = false;
    pointer_.clickCount = 0;
    for (size_t i = 0; i < events_.size(); i++) {
        const InputEvent& e = events_[i];
        switch (e.type) {
        case InputEvent::Type::PointerMove:
            pointer_.x = e.x;
            pointer_.y = e.y;
            pointer_.inside = true;
            break;
        case InputEvent::Type::PointerLeave:
            pointer_.inside = false;
            pointer_.x = pointer_.y = -1e6f;
            break;
        case InputEvent::Type::Button: {
            const int b = static_cast<int>(e.button);
            pointer_.x = e.x;
            pointer_.y = e.y;
            pointer_.inside = true;
            pointer_.down[b] = e.pressed;
            if (e.pressed) pointer_.pressed[b] = true; else pointer_.released[b] = true;
            if (e.pressed && b == 0) {
                pointer_.clickCount = clickCounts_[i];
                pointer_.pressX = e.x;
                pointer_.pressY = e.y;
            }
            mods_ = e.mods;
            break;
        }
        case InputEvent::Type::Wheel:
            pointer_.x = e.x;
            pointer_.y = e.y;
            pointer_.inside = true;
            mods_ = e.mods;
            break;
        case InputEvent::Type::KeyDown:
            mods_ = e.mods;
            break;
        case InputEvent::Type::FocusIn:
            windowFocused_ = true;
            break;
        case InputEvent::Type::FocusOut:
            windowFocused_ = false;
            for (bool& d : pointer_.down) d = false;
            mods_ = 0;
            break;
        default:
            break;
        }
    }

    lastOverlays_.swap(overlays_);
    overlays_.clear();
    lastModalLayer_ = modalLayer_;
    modalLayer_ = -1;
    hot_ = 0;
    activeSeen_ = false;
    anyPressTaken_ = false;
    focusOrder_.clear();
    idStack_.clear();
    layerStack_.assign(1, kLayerBase);
    opacity_.assign(1, 1.0f);
    disabledDepth_ = 0;
    disabledStack_.clear();
    for (DisplayList& list : lists_) list.Reset(widthPx_, heightPx_, scale_);
    cursor_ = Cursor::Arrow;
    textInputEnabled_ = false;
    wantsFrame_ = false;
    nextDeadline_ = -1;
}

const FrameOutput& Context::EndFrame() {
    const Id focusBefore = focused_;
    // Tab and Shift+Tab nobody took move focus through this frame's focusable widgets of the
    // top layer that holds the keyboard (a modal's, else all).
    const int minLayer = std::max(0, std::max(modalLayer_, lastModalLayer_));
    for (size_t i = 0; i < events_.size(); i++) {
        const InputEvent& e = events_[i];
        if (consumed_[i] || e.type != InputEvent::Type::KeyDown || e.key != Key::Tab) continue;
        if (e.mods != 0 && e.mods != kModShift) continue;
        consumed_[i] = 1;
        std::vector<Id> order;
        for (const auto& entry : focusOrder_) {
            if (entry.second >= minLayer) order.push_back(entry.first);
        }
        if (order.empty()) continue;
        const bool back = e.mods == kModShift;
        const auto at = std::find(order.begin(), order.end(), focused_);
        if (at == order.end()) {
            focused_ = back ? order.back() : order.front();
        } else {
            const size_t index = static_cast<size_t>(at - order.begin());
            focused_ = back ? order[(index + order.size() - 1) % order.size()] : order[(index + 1) % order.size()];
        }
    }
    // A press nothing took clears focus; an active widget that vanished lets go.
    if (pointer_.pressed[0] && !anyPressTaken_) focused_ = 0;
    if (active_ != 0 && !activeSeen_) active_ = 0;
    // Widgets learn of a focus change (focus rings, commits on focus loss) in the next frame.
    if (focused_ != focusBefore) wantsFrame_ = true;
    if (hot_ != lastHot_) {
        lastHot_ = hot_;
        hotSince_ = now_;
    }
    if (frame_ % 16 == 0) SweepState();

    joined_.Reset(widthPx_, heightPx_, scale_);
    for (const DisplayList& list : lists_) joined_.Append(list);

    output_.displayList = &joined_;
    output_.cursor = cursor_;
    output_.textInputEnabled = textInputEnabled_;
    output_.textInputRect = textInputEnabled_
        ? gfx::RectI{Round(textInputRect_.x), Round(textInputRect_.y), Round(textInputRect_.Right()),
                     Round(textInputRect_.Bottom())}
        : gfx::RectI{};
    output_.wantsFrame = wantsFrame_;
    output_.nextDeadlineMs = nextDeadline_;
    output_.title = title_;
    return output_;
}

void Context::SweepState() {
    for (auto it = states_.begin(); it != states_.end();) {
        if (frame_ - it->second.lastFrame > kStateFrames) it = states_.erase(it); else ++it;
    }
    for (auto it = shapes_.begin(); it != shapes_.end();) {
        if (frame_ - it->second.lastFrame > kStateFrames) it = shapes_.erase(it); else ++it;
    }
}

bool Context::ConsumeKey(Key key, uint8_t mods) {
    if (!KeysAllowed()) return false;
    for (size_t i = 0; i < events_.size(); i++) {
        const InputEvent& e = events_[i];
        if (!consumed_[i] && e.type == InputEvent::Type::KeyDown && e.key == key && e.mods == mods) {
            consumed_[i] = 1;
            return true;
        }
    }
    return false;
}

float Context::ConsumeWheel(RectF area, float& pixels, bool ctrl) {
    pixels = 0;
    float notches = 0;
    if (Disabled()) return 0;
    for (size_t i = 0; i < events_.size(); i++) {
        const InputEvent& e = events_[i];
        if (consumed_[i] || e.type != InputEvent::Type::Wheel) continue;
        if (((e.mods & kModCtrl) != 0) != ctrl) continue;
        if (e.notchesY == 0 && e.pixelsY == 0) continue;
        if (!area.Contains(e.x, e.y) || Blocked(e.x, e.y, CurrentLayer())) continue;
        consumed_[i] = 1;
        notches += e.notchesY;
        pixels += e.pixelsY;
    }
    return notches;
}

// ---- Ids ----

Id Context::MakeId(std::string_view label) const {
    uint64_t seed = idStack_.empty() ? kFnvOffset : idStack_.back();
    const Id id = Fnv1a64(label, seed);
    return id != 0 ? id : 1;
}

Id Context::MakeId(uint64_t value) const {
    uint64_t seed = idStack_.empty() ? kFnvOffset : idStack_.back();
    const Id id = Fnv1a64(&value, sizeof(value), seed ^ 0x9e3779b97f4a7c15ull);
    return id != 0 ? id : 1;
}

void Context::PushId(std::string_view label) { idStack_.push_back(MakeId(label)); }
void Context::PushId(uint64_t value) { idStack_.push_back(MakeId(value)); }
void Context::PopId() {
    if (!idStack_.empty()) idStack_.pop_back();
}

// ---- Interaction ----

bool Context::Blocked(float x, float y, int layer) const {
    for (const std::vector<Overlay>* list : {&lastOverlays_, &overlays_}) {
        for (const Overlay& o : *list) {
            if (o.layer > layer && (o.all || o.rect.Contains(x, y))) return true;
        }
    }
    return false;
}

bool Context::Hovered(RectF rect) const {
    return pointer_.inside && rect.Contains(pointer_.x, pointer_.y) &&
           !Blocked(pointer_.x, pointer_.y, CurrentLayer());
}

Interaction Context::Interact(Id id, RectF rect, uint32_t flags) {
    Interaction r;
    if (Disabled() || id == 0) {
        if (active_ == id) active_ = 0;
        return r;
    }
    if (active_ == id) activeSeen_ = true;
    if ((active_ == 0 || active_ == id) && Hovered(rect)) {
        r.hovered = true;
        hot_ = id;
    }
    if (r.hovered && pointer_.pressed[0] && !anyPressTaken_ &&
        rect.Contains(pointer_.pressX, pointer_.pressY)) {
        anyPressTaken_ = true;
        active_ = id;
        activeSeen_ = true;
        r.pressed = true;
        r.clickCount = pointer_.clickCount;
        if ((flags & kInteractFocusable) && !(flags & kInteractNoFocusOnPress)) focused_ = id;
    }
    if (active_ == id) {
        if (!pointer_.down[0]) {
            r.released = true;
            r.clicked = rect.Contains(pointer_.x, pointer_.y) && !Blocked(pointer_.x, pointer_.y, CurrentLayer());
            active_ = 0;
        } else {
            r.held = true;
        }
    }
    return r;
}

bool Context::FocusBehavior(Id id) {
    if (Disabled()) {
        if (focused_ == id) focused_ = 0;
        return false;
    }
    focusOrder_.push_back({id, CurrentLayer()});
    return IsFocused(id) && KeysAllowed();
}

bool Context::KeysAllowed() const { return std::max(modalLayer_, lastModalLayer_) <= CurrentLayer(); }

double Context::HotMilliseconds(Id id) const {
    if (id == 0 || id != hot_ || id != lastHot_) return 0;
    return now_ - hotSince_;
}

// ---- Layers ----

void Context::PushLayer(int layer) {
    layerStack_.push_back(std::min(std::max(layer, 0), kLayerCount - 1));
}

void Context::PopLayer() {
    if (layerStack_.size() > 1) layerStack_.pop_back();
}

void Context::Block(RectF rect) { overlays_.push_back({rect, CurrentLayer(), false}); }

void Context::BlockAll() {
    overlays_.push_back({WindowRect(), CurrentLayer(), true});
    modalLayer_ = std::max(modalLayer_, CurrentLayer());
}

// ---- Disabled and opacity ----

void Context::BeginDisabled(bool disabled) {
    disabledStack_.push_back(disabled);
    if (disabled) {
        disabledDepth_++;
        PushOpacity(0.4f);
    }
}

void Context::EndDisabled() {
    if (disabledStack_.empty()) return;
    if (disabledStack_.back()) {
        disabledDepth_--;
        PopOpacity();
    }
    disabledStack_.pop_back();
}

void Context::PushOpacity(float opacity) { opacity_.push_back(Opacity() * Clamp(opacity, 0, 1)); }

void Context::PopOpacity() {
    if (opacity_.size() > 1) opacity_.pop_back();
}

// ---- Time and output ----

void Context::WakeAt(double ms) {
    if (nextDeadline_ < 0 || ms < nextDeadline_) nextDeadline_ = ms;
}

void Context::SetTextInputRect(RectF rect) {
    textInputEnabled_ = true;
    textInputRect_ = rect;
}

// ---- Clipboard ----

void Context::SetClipboardText(std::string text) {
    if (clipboardSet_) clipboardSet_(std::move(text));
}

void Context::RequestClipboard(Id id) {
    if (!clipboardRequest_) return;
    std::weak_ptr<std::unordered_map<Id, std::string>> arrivals = clipboardArrivals_;
    std::function<void()> requestFrame = requestFrame_;
    clipboardRequest_([arrivals, id, requestFrame](std::string text) {
        if (auto map = arrivals.lock()) {
            (*map)[id] = std::move(text);
            if (requestFrame) requestFrame();
        }
    });
}

bool Context::TakeClipboard(Id id, std::string& text) {
    const auto found = clipboardArrivals_->find(id);
    if (found == clipboardArrivals_->end()) return false;
    text = std::move(found->second);
    clipboardArrivals_->erase(found);
    return true;
}

// ---- Drawing ----

gfx::RectI Context::ToDevice(RectF rect) const {
    return {Round(rect.x * scale_), Round(rect.y * scale_), Round((static_cast<double>(rect.x) + rect.w) * scale_),
            Round((static_cast<double>(rect.y) + rect.h) * scale_)};
}

float Context::ToDeviceX(float x) const { return static_cast<float>(x * scale_); }

float Context::SnapToDevice(float v) const { return static_cast<float>(Round(v * scale_) / scale_); }

uint32_t Context::Color(Rgba color) const {
    const float o = Opacity();
    return o < 1.0f ? color.WithAlpha(o).Premultiplied() : color.Premultiplied();
}

void Context::FillRect(RectF rect, Rgba color) { List().FillRect(ToDevice(rect), Color(color)); }

void Context::StrokeRect(RectF rect, float width, Rgba color) {
    List().StrokeRect(ToDevice(rect), std::max(1, Round(width * scale_)), Color(color));
}

namespace {
gfx::RectF ToF(const gfx::RectI& r) {
    return {static_cast<float>(r.x0), static_cast<float>(r.y0), static_cast<float>(r.Width()),
            static_cast<float>(r.Height())};
}
} // namespace

void Context::FillRoundRect(RectF rect, float radius, Rgba color) {
    const gfx::RectI d = ToDevice(rect);
    if (!(radius > 0)) {
        List().FillRect(d, Color(color));
        return;
    }
    List().FillRoundRect(ToF(d), static_cast<float>(radius * scale_), Color(color));
}

void Context::StrokeRoundRect(RectF rect, float radius, float width, Rgba color) {
    const gfx::RectI d = ToDevice(rect);
    const int w = std::max(1, Round(width * scale_));
    if (!(radius > 0)) {
        List().StrokeRect(d, w, Color(color));
        return;
    }
    List().StrokeRoundRect(ToF(d), static_cast<float>(radius * scale_), static_cast<float>(w), Color(color));
}

void Context::DrawIcon(Icon icon, RectF square, Rgba color) {
    const int x0 = Round(square.x * scale_), y0 = Round(square.y * scale_);
    const int size = std::max(1, Round(square.w * scale_));
    List().DrawIcon(icon, {x0, y0, x0 + size, y0 + size}, Color(color));
}

void Context::DrawImage(const gfx::Image& image, RectF dst, float opacity) {
    const float o = Clamp(opacity, 0, 1) * Opacity();
    List().DrawImage(image, ToF(ToDevice(dst)), static_cast<uint8_t>(Round(o * 255.0)));
}

void Context::PushClip(RectF rect) { List().PushClip(ToDevice(rect)); }
void Context::PopClip() { List().PopClip(); }

// ---- Text ----

FontMetrics Context::Metrics(float size, bool bold) const {
    FontMetrics m;
    if (!fonts_ || !fonts_->IsLoaded()) return m;
    const text::FontFace& face = fonts_->Face(bold);
    const float upm = face.UnitsPerEm();
    if (upm > 0 && face.Ascender() > 0) {
        m.ascent = face.Ascender() / upm * size;
        m.descent = -face.Descender() / upm * size;
    } else {
        m.ascent = 0.8f * size;
        m.descent = 0.2f * size;
    }
    m.lineHeight = m.ascent + m.descent;
    return m;
}

const text::GlyphRun& Context::Shape(std::string_view textValue, float size, bool bold) {
    static const text::GlyphRun kEmpty;
    if (!fonts_ || !fonts_->IsLoaded() || textValue.empty()) return kEmpty;
    const uint32_t size64 = static_cast<uint32_t>(Round(size * 64.0));
    uint64_t key = Fnv1a64(textValue);
    key = Fnv1a64(&size64, sizeof(size64), key);
    key = (key ^ (bold ? 1u : 0u)) * kFnvPrime;
    ShapeEntry& entry = shapes_[key];
    if (entry.lastFrame == 0 || entry.size64 != size64 || entry.bold != bold || entry.text != textValue) {
        entry.text.assign(textValue.data(), textValue.size());
        entry.size64 = size64;
        entry.bold = bold;
        text::ShapeRun(textValue, *fonts_, size64 / 64.0f, bold, entry.run);
    }
    entry.lastFrame = frame_ ? frame_ : 1;
    return entry.run;
}

void Context::DrawText(std::string_view textValue, float x, float baseline, float size, Rgba color, bool bold) {
    const text::GlyphRun& run = Shape(textValue, size, bold);
    if (run.glyphs.empty()) return;
    const float ox = static_cast<float>(Round(x * scale_));
    const float by = static_cast<float>(Round(baseline * scale_));
    const float s = static_cast<float>(scale_);
    glyphScratch_.clear();
    for (const text::PositionedGlyph& g : run.glyphs) {
        if (!g.face) continue;
        glyphScratch_.push_back({g.face, g.glyph, ox + g.x * s, by});
    }
    if (glyphScratch_.empty()) return;
    List().Glyphs(glyphScratch_.data(), glyphScratch_.size(), size * s, Color(color));
}

float Context::CenteredBaseline(float top, float height, float size, bool bold) const {
    const FontMetrics m = Metrics(size, bold);
    return SnapToDevice(top + (height - m.lineHeight) / 2 + m.ascent);
}

float Context::DrawTextInRect(std::string_view textValue, RectF rect, float size, Rgba color, Align align,
                              bool bold) {
    const float width = TextWidth(textValue, size, bold);
    float x = rect.x;
    if (align == Align::Center) x = rect.x + (rect.w - width) / 2;
    else if (align == Align::Right) x = rect.Right() - width;
    const float baseline = CenteredBaseline(rect.y, rect.h, size, bold);
    const bool clip = width > rect.w + 0.01f;
    if (clip) PushClip(rect);
    DrawText(textValue, x, baseline, size, color, bold);
    if (clip) PopClip();
    return width;
}

} // namespace rgui::ui
