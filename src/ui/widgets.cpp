// SPDX-License-Identifier: Apache-2.0
#include "ui/widgets.h"

#include "common/exact_math.h"
#include "common/text_utils.h"
#include "text/unicode.h"

#include <algorithm>
#include <cstdio>

namespace rgui::ui {

namespace {

using RayoMd::ExactMath::Ceil;
using RayoMd::ExactMath::LlRound;

constexpr double kTooltipDelayMs = 400;
constexpr double kScrollbarLingerMs = 600;
constexpr Id kScrollStackId = 0x5c5c'0001'5c5c'0001ull;
constexpr Id kModalStackId = 0x5c5c'0002'5c5c'0002ull;

float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

const Tokens& T(Context& ctx) { return ctx.GetTokens(); }

float BodySize(Context& ctx) { return ctx.FontSize(T(ctx).font.body); }

void DrawControl(Context& ctx, RectF rect, const ControlState& state, Rgba fill) {
    const float radius = static_cast<float>(T(ctx).controlRadius);
    ctx.FillRoundRect(rect, radius, fill);
    if (state.borderWidth > 0 && state.border.a > 0) {
        ctx.StrokeRoundRect(rect, radius, static_cast<float>(state.borderWidth), state.border);
    }
}

// Offers each unconsumed KeyDown of the frame, in order, to `f`; consumes those it takes.
template <typename F>
void ForEachKey(Context& ctx, F&& f) {
    const std::vector<InputEvent>& events = ctx.Events();
    for (size_t i = 0; i < events.size(); i++) {
        if (!ctx.IsConsumed(i) && events[i].type == InputEvent::Type::KeyDown && f(events[i])) ctx.ConsumeEvent(i);
    }
}

// ---- UTF-8 ----

size_t NextCp(std::string_view s, size_t at) {
    if (at >= s.size()) return s.size();
    if (static_cast<unsigned char>(s[at]) < 0x80) return at + 1;
    uint32_t cp = 0;
    size_t length = 1;
    if (!RayoMd::Text::DecodeUtf8(s, at, cp, length) || length == 0) length = 1;
    return std::min(s.size(), at + length);
}

size_t PrevCp(std::string_view s, size_t at) {
    if (at == 0) return 0;
    size_t start = at - 1;
    // Back over continuation bytes (at most 3) to a lead byte that decodes up to `at`.
    for (size_t back = 1; back <= 4 && back <= at; back++) {
        const size_t candidate = at - back;
        if ((static_cast<unsigned char>(s[candidate]) & 0xC0) != 0x80) {
            if (NextCp(s, candidate) == at) start = candidate;
            break;
        }
    }
    return start;
}

uint32_t CpAt(std::string_view s, size_t at) {
    uint32_t cp = 0;
    size_t length = 1;
    if (at < s.size() && static_cast<unsigned char>(s[at]) < 0x80) return static_cast<unsigned char>(s[at]);
    if (at >= s.size() || !RayoMd::Text::DecodeUtf8(s, at, cp, length)) return 0xFFFD;
    return cp;
}

bool IsWordCp(uint32_t cp) {
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    return !text::IsWhiteSpace(cp);
}

size_t WordRight(std::string_view s, size_t at) {
    while (at < s.size() && !IsWordCp(CpAt(s, at))) at = NextCp(s, at);
    while (at < s.size() && IsWordCp(CpAt(s, at))) at = NextCp(s, at);
    return at;
}

size_t WordLeft(std::string_view s, size_t at) {
    while (at > 0 && !IsWordCp(CpAt(s, PrevCp(s, at)))) at = PrevCp(s, at);
    while (at > 0 && IsWordCp(CpAt(s, PrevCp(s, at)))) at = PrevCp(s, at);
    return at;
}

// ---- One-line editing, shared by TextField and NumberField ----

using CharFilter = bool (*)(uint32_t cp, std::string_view text);

struct LineEdit {
    size_t caret = 0;
    size_t anchor = 0;
    std::string undo;
    size_t undoCaret = 0;
    bool hasUndo = false;
    bool lastTyping = false;
    float scrollX = 0;
    std::string preedit;
    bool wasFocused = false;
    bool dragging = false;
};

bool HasSelection(const LineEdit& e) { return e.caret != e.anchor; }
size_t SelStart(const LineEdit& e) { return std::min(e.caret, e.anchor); }
size_t SelEnd(const LineEdit& e) { return std::max(e.caret, e.anchor); }

void ClampEdit(LineEdit& e, const std::string& text) {
    e.caret = std::min(e.caret, text.size());
    e.anchor = std::min(e.anchor, text.size());
}

// The pen x of the caret before byte `at` in `run` (logical px).
float CaretX(const text::GlyphRun& run, size_t at) {
    for (const text::PositionedGlyph& g : run.glyphs) {
        if (g.byteOffset >= at) return g.x;
    }
    return run.advance;
}

// The caret position nearest pen x `x`.
size_t HitTest(const text::GlyphRun& run, size_t textSize, float x) {
    for (const text::PositionedGlyph& g : run.glyphs) {
        if (x < g.x + g.advance / 2) return g.byteOffset;
    }
    return textSize;
}

class Editor {
public:
    Editor(Context& ctx, Id id, LineEdit& e, std::string& text, bool readOnly, CharFilter filter)
        : ctx_(ctx), id_(id), e_(e), text_(text), readOnly_(readOnly), filter_(filter) {}

    // Handles the frame's keys, text and clipboard in order; true when the text changed. Each
    // key goes to `other` first (true: it took the key), then to the line.
    template <typename F>
    bool Run(F&& other) {
        ClampEdit(e_, text_);
        std::string pasted;
        if (ctx_.TakeClipboard(id_, pasted) && !readOnly_) Insert(pasted, false);
        const auto& events = ctx_.Events();
        for (size_t i = 0; i < events.size(); i++) {
            if (ctx_.IsConsumed(i)) continue;
            const InputEvent& ev = events[i];
            if (ev.type == InputEvent::Type::Text) {
                if (!readOnly_) Insert(ev.text, true);
                e_.preedit.clear();
                ctx_.ConsumeEvent(i);
            } else if (ev.type == InputEvent::Type::Preedit) {
                e_.preedit = readOnly_ ? std::string() : ev.text;
                ctx_.ConsumeEvent(i);
            } else if (ev.type == InputEvent::Type::KeyDown) {
                if (other(ev) || Key(ev)) ctx_.ConsumeEvent(i);
            }
        }
        return changed_;
    }

private:
    void Snapshot(bool typing) {
        if (!(typing && e_.lastTyping)) {
            e_.undo = text_;
            e_.undoCaret = e_.caret;
            e_.hasUndo = true;
        }
        e_.lastTyping = typing;
    }

    void Erase(size_t from, size_t to) {
        text_.erase(from, to - from);
        e_.caret = e_.anchor = from;
        changed_ = true;
    }

    void Insert(std::string_view s, bool typing) {
        std::string clean;
        for (size_t at = 0; at < s.size();) {
            const size_t next = NextCp(s, at);
            const uint32_t cp = CpAt(s, at);
            if (cp >= 0x20 && cp != 0x7F && (!filter_ || filter_(cp, Remaining(clean)))) {
                clean.append(s.substr(at, next - at));
            }
            at = next;
        }
        if (clean.empty() && !HasSelection(e_)) return;
        Snapshot(typing);
        if (HasSelection(e_)) Erase(SelStart(e_), SelEnd(e_));
        text_.insert(e_.caret, clean);
        e_.caret += clean.size();
        e_.anchor = e_.caret;
        changed_ = true;
    }

    // The text as it will be with `pending` inserted at the caret (for filters).
    std::string Remaining(const std::string& pending) const {
        std::string t = text_;
        if (HasSelection(e_)) t.erase(SelStart(e_), SelEnd(e_) - SelStart(e_));
        t.insert(std::min(SelStart(e_), t.size()), pending);
        return t;
    }

    void Move(size_t to, bool extend) {
        e_.caret = to;
        if (!extend) e_.anchor = to;
        e_.lastTyping = false;
    }

    bool Key(const InputEvent& ev) {
        const uint8_t m = ev.mods & (kModCtrl | kModShift | kModAlt | kModSuper);
        if (m & (kModAlt | kModSuper)) return false;
        const bool ctrl = (m & kModCtrl) != 0;
        const bool shift = (m & kModShift) != 0;
        switch (ev.key) {
        case Key::Left:
            if (!shift && !ctrl && HasSelection(e_)) Move(SelStart(e_), false);
            else Move(ctrl ? WordLeft(text_, e_.caret) : PrevCp(text_, e_.caret), shift);
            return true;
        case Key::Right:
            if (!shift && !ctrl && HasSelection(e_)) Move(SelEnd(e_), false);
            else Move(ctrl ? WordRight(text_, e_.caret) : NextCp(text_, e_.caret), shift);
            return true;
        case Key::Home:
            Move(0, shift);
            return true;
        case Key::End:
            Move(text_.size(), shift);
            return true;
        case Key::Backspace:
        case Key::Delete: {
            if (readOnly_ || shift) return false;
            if (HasSelection(e_)) {
                Snapshot(false);
                Erase(SelStart(e_), SelEnd(e_));
                return true;
            }
            size_t from = e_.caret, to = e_.caret;
            if (ev.key == Key::Backspace) from = ctrl ? WordLeft(text_, e_.caret) : PrevCp(text_, e_.caret);
            else to = ctrl ? WordRight(text_, e_.caret) : NextCp(text_, e_.caret);
            if (from == to) return true;
            Snapshot(!ctrl);
            Erase(from, to);
            return true;
        }
        case Key::A:
            if (m != kModCtrl) return false;
            e_.anchor = 0;
            e_.caret = text_.size();
            return true;
        case Key::C:
        case Key::X:
            if (m != kModCtrl) return false;
            if (HasSelection(e_)) {
                ctx_.SetClipboardText(text_.substr(SelStart(e_), SelEnd(e_) - SelStart(e_)));
                if (ev.key == Key::X && !readOnly_) {
                    Snapshot(false);
                    Erase(SelStart(e_), SelEnd(e_));
                }
            }
            return true;
        case Key::V:
            if (m != kModCtrl) return false;
            if (!readOnly_) ctx_.RequestClipboard(id_);
            return true;
        case Key::Insert:
            if (m != kModShift) return false;
            if (!readOnly_) ctx_.RequestClipboard(id_);
            return true;
        case Key::Z:
            if (m != kModCtrl || readOnly_) return false;
            if (e_.hasUndo) {
                std::swap(e_.undo, text_);
                std::swap(e_.undoCaret, e_.caret);
                e_.caret = std::min(e_.caret, text_.size());
                e_.anchor = e_.caret;
                e_.undoCaret = std::min(e_.undoCaret, e_.undo.size());
                e_.lastTyping = false;
                changed_ = true;
            }
            return true;
        default:
            return false;
        }
    }

    Context& ctx_;
    Id id_;
    LineEdit& e_;
    std::string& text_;
    bool readOnly_;
    CharFilter filter_;
    bool changed_ = false;
};

// Mouse on a field: press places the caret (Shift extends), a double click selects a word,
// dragging selects. `inner` is the text's box.
void FieldMouse(Context& ctx, const Interaction& in, LineEdit& e, const std::string& text, RectF inner, float size) {
    const text::GlyphRun& run = ctx.Shape(text, size);
    const float x = ctx.Pointer().x - inner.x + e.scrollX;
    if (in.pressed) {
        const size_t at = HitTest(run, text.size(), x);
        e.lastTyping = false;
        if (in.clickCount >= 3) {
            e.anchor = 0;
            e.caret = text.size();
        } else if (in.clickCount == 2) {
            size_t a = at, b = at;
            const bool word = at < text.size() && IsWordCp(CpAt(text, at));
            while (a > 0 && IsWordCp(CpAt(text, PrevCp(text, a))) == word) a = PrevCp(text, a);
            while (b < text.size() && IsWordCp(CpAt(text, b)) == word) b = NextCp(text, b);
            e.anchor = a;
            e.caret = b;
        } else {
            e.caret = at;
            if (!(ctx.Mods() & kModShift)) e.anchor = at;
        }
        e.dragging = in.clickCount <= 1;
    } else if (in.held && e.dragging) {
        e.caret = HitTest(run, text.size(), x);
    }
    if (!in.held) e.dragging = false;
}

// Draws a field's text, selection, preedit and caret inside `inner`.
void FieldDraw(Context& ctx, LineEdit& e, const std::string& text, RectF inner, bool focused,
               std::string_view placeholder, float size, Rgba color) {
    const Tokens& t = T(ctx);
    const FontMetrics fm = ctx.Metrics(size);
    const float baseline = ctx.CenteredBaseline(inner.y, inner.h, size);
    const float lineTop = baseline - fm.ascent;
    ctx.PushClip(inner);
    if (text.empty() && e.preedit.empty()) {
        if (!placeholder.empty()) ctx.DrawText(placeholder, inner.x, baseline, size, t.colors.mutedText);
        e.scrollX = 0;
    } else {
        std::string shown;
        const std::string* drawn = &text;
        if (focused && !e.preedit.empty()) {
            shown = text;
            shown.insert(std::min(e.caret, shown.size()), e.preedit);
            drawn = &shown;
        }
        const text::GlyphRun& run = ctx.Shape(*drawn, size);
        float caretX = CaretX(run, e.caret + (drawn == &shown ? e.preedit.size() : 0));
        // Keep the caret in view.
        if (focused) {
            if (caretX - e.scrollX > inner.w - 1) e.scrollX = caretX - inner.w + 1;
            if (caretX - e.scrollX < 0) e.scrollX = caretX;
        }
        if (run.advance - e.scrollX < inner.w - 1) e.scrollX = std::max(0.0f, run.advance - inner.w + 1);
        e.scrollX = ctx.SnapToDevice(e.scrollX);
        const float ox = inner.x - e.scrollX;
        if (focused && HasSelection(e) && drawn == &text) {
            const float a = CaretX(run, SelStart(e)), b = CaretX(run, SelEnd(e));
            ctx.FillRect({ox + a, lineTop, b - a, fm.lineHeight}, t.colors.selection);
        }
        ctx.DrawText(*drawn, ox, baseline, size, color);
        if (drawn == &shown) {
            const float a = CaretX(run, e.caret), b = CaretX(run, e.caret + e.preedit.size());
            ctx.FillRect({ox + a, baseline + 1, b - a, 1}, color);
        }
        caretX += ox;
        if (focused) {
            ctx.FillRect({ctx.SnapToDevice(caretX), lineTop, 1, fm.lineHeight}, t.colors.caret);
            ctx.SetTextInputRect({caretX, lineTop, 1, fm.lineHeight});
        }
    }
    if (focused && text.empty() && e.preedit.empty()) {
        ctx.FillRect({ctx.SnapToDevice(inner.x), lineTop, 1, fm.lineHeight}, t.colors.caret);
        ctx.SetTextInputRect({inner.x, lineTop, 1, fm.lineHeight});
    }
    ctx.PopClip();
}

// The chrome of a field in its state.
void FieldChrome(Context& ctx, RectF rect, bool focused, bool hovered) {
    const Tokens& t = T(ctx);
    const ControlState& s = focused ? t.focus : (hovered ? t.hoverCursor : t.normal);
    DrawControl(ctx, rect, s, s.Fill());
}

// ---- Numbers ----

bool NumberChar(uint32_t cp, std::string_view text) {
    if (cp >= '0' && cp <= '9') return true;
    if (cp == '.') return std::count(text.begin(), text.end(), '.') == 0;
    return false;
}

// Digits with at most one '.', at least one digit.
bool ParseNumber(std::string_view s, double& out) {
    double value = 0, scale = 1;
    bool dot = false, digit = false;
    for (const char c : s) {
        if (c >= '0' && c <= '9') {
            digit = true;
            if (dot) {
                scale /= 10;
                value += (c - '0') * scale;
            } else {
                value = value * 10 + (c - '0');
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digit || !(value < 1e15)) return false;
    out = value;
    return true;
}

double RoundTo(double v, int decimals) {
    double p = 1;
    for (int i = 0; i < decimals && i < 9; i++) p *= 10;
    if (!(v < 1e15 && v > -1e15)) return v;
    return static_cast<double>(LlRound(v * p)) / p;
}

std::string FormatNumber(double v, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::min(std::max(decimals, 0), 9), v);
    return buffer;
}

double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- Stacks of Begin / End widgets ----

struct ScrollAreaState {
    SmoothScroll scroll;
    float content = 0;
    double lastActivity = -1e18;
    float dragGrab = 0;
};

struct ScrollFrame {
    Id id = 0;
    RectF viewport;
    float offset = 0;
};

struct ScrollStack {
    std::vector<ScrollFrame> frames;
};

struct ModalFrame {
    Id id = 0;
    RectF card;
    RectF body;
};

struct ModalStack {
    std::vector<ModalFrame> frames;
};

struct DropdownState {
    bool open = false;
    int highlight = 0;
    SmoothScroll scroll;
    bool keyboard = false;   // the highlight moved by key: keep it in view
};

struct PopupListState {
    int highlight = 0;
    bool moved = false;      // Up / Down moved the highlight since the reset
    bool keyboard = false;   // the highlight moved by key: keep it in view
    SmoothScroll scroll;
};

constexpr int kPopupListRows = 10;
constexpr float kPopupListWidth = 280;

} // namespace

// ---- IconButton and Tooltip ----

void Tooltip(Context& ctx, Id owner, RectF anchor, std::string_view textValue) {
    if (textValue.empty() || !ctx.IsHot(owner) || ctx.IsActive(owner)) return;
    const double ms = ctx.HotMilliseconds(owner);
    if (ms < kTooltipDelayMs) {
        ctx.WakeAt(ctx.NowMs() + (kTooltipDelayMs - ms));
        return;
    }
    const Tokens& t = T(ctx);
    const float size = ctx.FontSize(t.font.bodySmall);
    const FontMetrics fm = ctx.Metrics(size);
    const float padX = static_cast<float>(t.spacing.md), padY = static_cast<float>(t.spacing.sm);
    const float w = ctx.SnapToDevice(ctx.TextWidth(textValue, size) + 2 * padX + 2);
    const float h = ctx.SnapToDevice(static_cast<float>(Ceil(fm.lineHeight)) + 2 * padY + 2);
    const float gap = static_cast<float>(t.spacing.sm);
    float x = anchor.x + (anchor.w - w) / 2;
    x = Clampf(x, 0, std::max(0.0f, ctx.Width() - w));
    float y = anchor.y - gap - h;
    if (y < 0) y = anchor.Bottom() + gap;
    const RectF box{ctx.SnapToDevice(x), ctx.SnapToDevice(y), w, h};
    ctx.PushLayer(kLayerTooltip);
    const float radius = static_cast<float>(t.controlRadius);
    ctx.FillRoundRect(box, radius, t.tooltip.background);
    ctx.StrokeRoundRect(box, radius, static_cast<float>(std::max(1, t.tooltip.borderWidth)), t.tooltip.border);
    ctx.DrawTextInRect(textValue, Inset(box, padX + 1, padY + 1), size, t.tooltip.text, Align::Center);
    ctx.PopLayer();
}

bool IconButton(Context& ctx, std::string_view idLabel, Icon icon, RectF rect, std::string_view tooltip,
                IconStyle style) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    const Interaction in = ctx.Interact(id, rect);
    const bool hot = in.hovered || in.held;
    if (hot) {
        ctx.SetCursor(Cursor::Hand);
        ctx.FillRoundRect(rect, static_cast<float>(t.controlRadius), in.held && in.hovered ? t.pressedFill
                                                                                           : t.hoverCursor.Fill());
    }
    Rgba color;
    switch (style) {
    case IconStyle::Muted:
        color = hot ? t.colors.mutedText : t.colors.mutedText.WithAlpha(0.55);
        break;
    case IconStyle::Accent:
        color = hot ? t.colors.accentHover : t.colors.accent;
        break;
    case IconStyle::Text:
        color = t.colors.text;
        break;
    }
    const float iconSize = 16;
    ctx.DrawIcon(icon, Centered(rect, iconSize, iconSize), color);
    Tooltip(ctx, id, rect, tooltip);
    return in.clicked;
}

// ---- Button ----

float ButtonWidth(Context& ctx, std::string_view label) {
    return ctx.SnapToDevice(ctx.TextWidth(VisibleLabel(label), BodySize(ctx)) +
                            2 * static_cast<float>(T(ctx).spacing.controlPaddingX));
}

bool Button(Context& ctx, std::string_view label, RectF rect, ButtonStyle style) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(label);
    const Interaction in = ctx.Interact(id, rect, kInteractFocusable);
    const bool focused = ctx.FocusBehavior(id);
    bool activated = in.clicked;
    if (focused && (ctx.ConsumeKey(Key::Enter) || ctx.ConsumeKey(Key::Space))) activated = true;
    if (in.hovered || in.held) ctx.SetCursor(Cursor::Hand);
    const bool pressed = in.held && in.hovered;
    Rgba textColor = t.colors.text;
    if (style == ButtonStyle::Primary) {
        const Rgba fill = pressed ? t.colors.accentPressed : (in.hovered || focused ? t.colors.accentHover : t.colors.accent);
        ctx.FillRoundRect(rect, static_cast<float>(t.controlRadius), fill);
        textColor = t.colors.textOnAccent;
    } else if (pressed) {
        DrawControl(ctx, rect, t.hoverCursor, t.pressedFill);
    } else if (in.hovered) {
        DrawControl(ctx, rect, t.hoverCursor, t.hoverCursor.Fill());
    } else if (focused) {
        DrawControl(ctx, rect, t.focus, t.focus.Fill());
    } else {
        DrawControl(ctx, rect, t.normal, t.normal.Fill());
    }
    ctx.DrawTextInRect(VisibleLabel(label), Inset(rect, static_cast<float>(t.spacing.controlPaddingX), 0), BodySize(ctx),
                       textColor, Align::Center);
    return activated;
}

// ---- Checkbox ----

bool Checkbox(Context& ctx, std::string_view label, bool& value, RectF row) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(label);
    const Interaction in = ctx.Interact(id, row, kInteractFocusable);
    const bool focused = ctx.FocusBehavior(id);
    bool toggled = in.clicked;
    if (focused && ctx.ConsumeKey(Key::Space)) toggled = true;
    if (toggled) value = !value;
    if (in.hovered || in.held) ctx.SetCursor(Cursor::Hand);
    const float boxSize = 14;
    const RectF box{row.x, ctx.SnapToDevice(row.y + (row.h - boxSize) / 2), boxSize, boxSize};
    const float radius = std::min(static_cast<float>(t.controlRadius), 3.0f);
    if (value) {
        ctx.FillRoundRect(box, radius, in.hovered ? t.colors.accentHover : t.colors.accent);
        ctx.DrawIcon(Icon::Check, box, t.colors.textOnAccent);
        if (focused) ctx.StrokeRoundRect(Inset(box, -2), radius + 2, 1, t.focus.border.a ? t.focus.border : t.colors.accent);
    } else {
        const ControlState& s = focused ? t.focus : (in.hovered ? t.hoverCursor : t.normal);
        ctx.FillRoundRect(box, radius, s.Fill());
        Rgba border = s.border.a ? s.border : t.normal.border;
        ctx.StrokeRoundRect(box, radius, static_cast<float>(std::max(1, s.borderWidth)), border);
    }
    RectF rest = row;
    CutLeft(rest, boxSize + static_cast<float>(t.spacing.controlGap));
    ctx.DrawTextInRect(VisibleLabel(label), rest, BodySize(ctx), t.colors.text);
    return toggled;
}

// ---- Dropdown ----

bool Dropdown(Context& ctx, std::string_view idLabel, int& selected, const std::vector<std::string>& items, RectF rect) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    DropdownState& st = ctx.State<DropdownState>(id);
    const int count = static_cast<int>(items.size());
    const int before = selected;
    if (count == 0) selected = -1;
    else selected = std::min(std::max(selected, 0), count - 1);

    const Interaction in = ctx.Interact(id, rect, kInteractFocusable);
    const bool focused = ctx.FocusBehavior(id);
    if (ctx.Disabled()) st.open = false;
    if (in.clicked && count > 0) {
        st.open = !st.open;
        st.highlight = selected;
        st.keyboard = true;
    }
    if (focused && count > 0) {
        ForEachKey(ctx, [&](const InputEvent& ev) {
            if (ev.mods != 0) return false;
            if (st.open) {
                switch (ev.key) {
                case Key::Up: st.highlight = std::max(0, st.highlight - 1); st.keyboard = true; return true;
                case Key::Down: st.highlight = std::min(count - 1, st.highlight + 1); st.keyboard = true; return true;
                case Key::Enter:
                    selected = std::min(std::max(st.highlight, 0), count - 1);
                    st.open = false;
                    return true;
                case Key::Escape: st.open = false; return true;
                default: return false;
                }
            }
            switch (ev.key) {
            case Key::Enter:
            case Key::Space:
                st.open = true;
                st.highlight = selected;
                st.keyboard = true;
                return true;
            case Key::Up: selected = std::max(0, selected - 1); return true;
            case Key::Down: selected = std::min(count - 1, selected + 1); return true;
            default: return false;
            }
        });
    } else if (st.open && ctx.FocusedId() != id) {
        st.open = false;   // focus went elsewhere (Tab, a click)
    }

    // The button.
    if (in.hovered || in.held) ctx.SetCursor(Cursor::Hand);
    if (in.held && in.hovered) DrawControl(ctx, rect, t.hoverCursor, t.pressedFill);
    else if (in.hovered) DrawControl(ctx, rect, t.hoverCursor, t.hoverCursor.Fill());
    else if (focused || st.open) DrawControl(ctx, rect, t.focus, t.focus.Fill());
    else DrawControl(ctx, rect, t.normal, t.normal.Fill());
    RectF inner = Inset(rect, static_cast<float>(t.spacing.controlPaddingX), 0);
    const RectF chevron = CutRight(inner, 16);
    ctx.DrawIcon(Icon::ChevronDown, Centered(chevron, 16, 16), t.colors.mutedText);
    CutRight(inner, static_cast<float>(t.spacing.sm));
    if (selected >= 0) ctx.DrawTextInRect(items[static_cast<size_t>(selected)], inner, BodySize(ctx), t.colors.text);

    // The popup.
    if (st.open && count > 0) {
        const float rowH = static_cast<float>(t.spacing.popupRowHeight);
        const float border = static_cast<float>(std::max(0, t.popups.borderWidth));
        const float gap = static_cast<float>(t.spacing.xxs);
        const float below = ctx.Height() - (rect.Bottom() + gap) - border * 2;
        const float above = rect.y - gap - border * 2;
        int rows = std::min(count, 12);
        const bool down = below >= rows * rowH || below >= above;
        const float room = down ? below : above;
        rows = std::max(1, std::min(rows, static_cast<int>(room / rowH)));
        const float h = rows * rowH + 2 * border;
        const RectF popup{rect.x, down ? rect.Bottom() + gap : rect.y - gap - h, rect.w, h};
        const RectF list = Inset(popup, border);

        ctx.PushLayer(ctx.CurrentLayer() + 1);
        ctx.PushId(id);
        ctx.Block(ctx.WindowRect());
        st.scroll.SetMax(count * rowH - list.h);
        float pixels = 0;
        const float notches = ctx.ConsumeWheel(list, pixels);
        if (notches != 0) st.scroll.AddNotches(notches, ctx.NowMs());
        if (pixels != 0) st.scroll.AddPixels(pixels);
        if (st.keyboard) {
            const float top = st.highlight * rowH;
            const float target = st.scroll.Target();
            if (top < target) st.scroll.JumpTo(top);
            else if (top + rowH > target + list.h) st.scroll.JumpTo(top + rowH - list.h);
            st.keyboard = false;
        }
        if (st.scroll.Animating(ctx.NowMs())) ctx.RequestAnimation();
        const float offset = st.scroll.Value(ctx.NowMs(), ctx.Scale());
        // Hover moves the highlight only when the pointer moves, so keys work under a still one.
        bool pointerMoved = false;
        for (const InputEvent& ev : ctx.Events()) pointerMoved |= ev.type == InputEvent::Type::PointerMove;

        const float radius = static_cast<float>(t.radius);
        ctx.FillRoundRect(popup, radius, t.popups.background);
        ctx.PushClip(list);
        const int first = std::max(0, static_cast<int>(offset / rowH));
        for (int i = first; i < count; i++) {
            const RectF r{list.x, list.y + i * rowH - offset, list.w, rowH};
            if (r.y >= list.Bottom()) break;
            const Interaction rowIn = ctx.Interact(ctx.MakeId(static_cast<uint64_t>(i)), r);
            if (rowIn.hovered) {
                if (pointerMoved) st.highlight = i;
                ctx.SetCursor(Cursor::Hand);
            }
            if (rowIn.clicked) {
                selected = i;
                st.open = false;
            }
            if (i == st.highlight) ctx.FillRect(r, t.menu.selectedBackground);
            const Rgba color = i == selected ? t.menu.selectedText : t.popups.text;
            ctx.DrawTextInRect(items[static_cast<size_t>(i)], Inset(r, static_cast<float>(t.spacing.controlPaddingX), 0),
                               BodySize(ctx), color);
        }
        ctx.PopClip();
        if (border > 0) ctx.StrokeRoundRect(popup, radius, border, t.popups.border);
        // A press anywhere else closes it (and goes nowhere else).
        const Interaction outside = ctx.Interact(ctx.MakeId("outside"), ctx.WindowRect());
        if (outside.pressed) st.open = false;
        ctx.PopId();
        ctx.PopLayer();
    }
    return selected != before;
}

// ---- PopupList ----

PopupEvent PopupListKeys(Context& ctx, std::string_view id, int count, bool enterPicks) {
    PopupListState& st = ctx.State<PopupListState>(ctx.MakeId(id));
    st.highlight = count > 0 ? std::min(std::max(st.highlight, 0), count - 1) : 0;
    PopupEvent event;
    ForEachKey(ctx, [&](const InputEvent& ev) {
        if (ev.mods != 0 || event.action != PopupAction::None) return false;
        switch (ev.key) {
        case Key::Up:
        case Key::Down:
            if (count > 0) {
                st.highlight = ev.key == Key::Up ? (st.highlight + count - 1) % count : (st.highlight + 1) % count;
                st.moved = true;
                st.keyboard = true;
            }
            return true;
        case Key::Escape:
            event.action = PopupAction::Close;
            return true;
        case Key::Enter:
            if (count == 0 || !(enterPicks || st.moved)) return false;
            event.action = PopupAction::Pick;
            event.index = st.highlight;
            return true;
        default:
            return false;
        }
    });
    return event;
}

void ResetPopupList(Context& ctx, std::string_view id) {
    PopupListState& st = ctx.State<PopupListState>(ctx.MakeId(id));
    st.highlight = 0;
    st.moved = false;
    st.keyboard = true;
}

PopupEvent PopupList(Context& ctx, std::string_view idLabel, const std::vector<std::string>& items, RectF anchor,
                     std::string_view emptyText) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    PopupListState& st = ctx.State<PopupListState>(id);
    const int count = static_cast<int>(items.size());
    st.highlight = count > 0 ? std::min(std::max(st.highlight, 0), count - 1) : 0;
    PopupEvent event;
    if (count == 0 && emptyText.empty()) return event;

    const float rowH = static_cast<float>(t.spacing.popupRowHeight);
    const float border = static_cast<float>(std::max(0, t.popups.borderWidth));
    const float gap = static_cast<float>(t.spacing.xxs);
    const int shown = std::max(count, 1);
    const float below = ctx.Height() - (anchor.Bottom() + gap) - border * 2;
    const float above = anchor.y - gap - border * 2;
    int rows = std::min(shown, kPopupListRows);
    const bool down = below >= rows * rowH || below >= above;
    rows = std::max(1, std::min(rows, static_cast<int>((down ? below : above) / rowH)));
    const float h = rows * rowH + 2 * border;
    const float w = std::min(kPopupListWidth, ctx.Width());
    const float x = Clampf(anchor.x, 0, std::max(0.0f, ctx.Width() - w));
    const RectF popup{x, down ? anchor.Bottom() + gap : anchor.y - gap - h, w, h};
    const RectF list = Inset(popup, border);

    ctx.PushLayer(ctx.CurrentLayer() + 1);
    ctx.PushId(id);
    ctx.Block(ctx.WindowRect());
    st.scroll.SetMax(shown * rowH - list.h);
    float pixels = 0;
    const float notches = ctx.ConsumeWheel(list, pixels);
    if (notches != 0) st.scroll.AddNotches(notches, ctx.NowMs());
    if (pixels != 0) st.scroll.AddPixels(pixels);
    if (st.keyboard) {
        const float top = st.highlight * rowH;
        const float target = st.scroll.Target();
        if (top < target) st.scroll.JumpTo(top);
        else if (top + rowH > target + list.h) st.scroll.JumpTo(top + rowH - list.h);
        st.keyboard = false;
    }
    if (st.scroll.Animating(ctx.NowMs())) ctx.RequestAnimation();
    const float offset = st.scroll.Value(ctx.NowMs(), ctx.Scale());
    // Hover moves the highlight only when the pointer moves, so keys work under a still one.
    bool pointerMoved = false;
    for (const InputEvent& ev : ctx.Events()) pointerMoved |= ev.type == InputEvent::Type::PointerMove;

    const float radius = static_cast<float>(t.radius);
    const float padX = static_cast<float>(t.spacing.controlPaddingX);
    ctx.FillRoundRect(popup, radius, t.popups.background);
    ctx.PushClip(list);
    if (count == 0) {
        ctx.DrawTextInRect(emptyText, Inset(RectF{list.x, list.y, list.w, rowH}, padX, 0), BodySize(ctx),
                           t.colors.mutedText);
    }
    const int first = std::max(0, static_cast<int>(offset / rowH));
    for (int i = first; i < count; i++) {
        const RectF r{list.x, list.y + i * rowH - offset, list.w, rowH};
        if (r.y >= list.Bottom()) break;
        const Interaction rowIn = ctx.Interact(ctx.MakeId(static_cast<uint64_t>(i)), r);
        if (rowIn.hovered) {
            if (pointerMoved) st.highlight = i;
            ctx.SetCursor(Cursor::Hand);
        }
        if (rowIn.clicked) {
            event.action = PopupAction::Pick;
            event.index = i;
        }
        if (i == st.highlight) ctx.FillRect(r, t.menu.selectedBackground);
        const Rgba color = i == st.highlight ? t.menu.selectedText : t.popups.text;
        ctx.DrawTextInRect(items[static_cast<size_t>(i)], Inset(r, padX, 0), BodySize(ctx), color);
    }
    ctx.PopClip();
    if (border > 0) ctx.StrokeRoundRect(popup, radius, border, t.popups.border);
    const Interaction outside = ctx.Interact(ctx.MakeId("outside"), ctx.WindowRect());
    if (outside.pressed && event.action == PopupAction::None) event.action = PopupAction::Close;
    ctx.PopId();
    ctx.PopLayer();
    return event;
}

// ---- Text fields ----

float TextFieldHeight(Context& ctx) {
    const Tokens& t = T(ctx);
    const float line = static_cast<float>(Ceil(ctx.Metrics(BodySize(ctx)).lineHeight));
    return std::max(static_cast<float>(t.spacing.controlHeight), line + 2 * static_cast<float>(t.spacing.inputPaddingY));
}

bool TextField(Context& ctx, std::string_view idLabel, std::string& text, RectF rect, std::string_view placeholder,
               bool readOnly) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    LineEdit& e = ctx.State<LineEdit>(id);
    const Interaction in = ctx.Interact(id, rect, kInteractFocusable);
    const bool focused = ctx.FocusBehavior(id);
    if (in.hovered || in.held) ctx.SetCursor(Cursor::IBeam);
    const RectF inner = Inset(rect, static_cast<float>(t.spacing.controlPaddingX), static_cast<float>(t.spacing.inputPaddingY));
    ClampEdit(e, text);
    FieldMouse(ctx, in, e, text, inner, BodySize(ctx));
    bool changed = false;
    if (focused) {
        if (!e.wasFocused && !in.pressed) {
            e.anchor = 0;
            e.caret = text.size();
        }
        changed = Editor(ctx, id, e, text, readOnly, nullptr).Run([](const InputEvent&) { return false; });
    } else {
        e.preedit.clear();
        e.anchor = e.caret;
    }
    e.wasFocused = focused;
    FieldChrome(ctx, rect, focused, in.hovered);
    FieldDraw(ctx, e, text, inner, focused, placeholder, BodySize(ctx), t.colors.text);
    return changed;
}

namespace {
struct NumberState {
    LineEdit edit;
    std::string text;
};
} // namespace

bool NumberField(Context& ctx, std::string_view idLabel, double& value, double min, double max, double step,
                 int decimals, std::string_view suffix, RectF rect) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    NumberState& st = ctx.State<NumberState>(id);
    LineEdit& e = st.edit;
    const double before = value;
    auto commit = [&] {
        double parsed = 0;
        if (ParseNumber(st.text, parsed)) value = RoundTo(ClampD(parsed, min, max), decimals);
        st.text = FormatNumber(value, decimals);
        ClampEdit(e, st.text);
    };
    auto stepBy = [&](double n) {
        double parsed = 0;
        double base = ParseNumber(st.text, parsed) && ctx.IsFocused(id) ? parsed : value;
        value = RoundTo(ClampD(base + n * step, min, max), decimals);
        st.text = FormatNumber(value, decimals);
        e.caret = e.anchor = st.text.size();
    };

    const Interaction in = ctx.Interact(id, rect, kInteractFocusable);
    const bool focused = ctx.FocusBehavior(id);
    if (in.hovered || in.held) ctx.SetCursor(Cursor::IBeam);
    const RectF inner = Inset(rect, static_cast<float>(t.spacing.controlPaddingX), static_cast<float>(t.spacing.inputPaddingY));

    if (focused && !e.wasFocused) {
        st.text = FormatNumber(value, decimals);
        e.anchor = 0;
        e.caret = st.text.size();
    } else if (!focused && e.wasFocused) {
        commit();
    }
    if (!focused) st.text = FormatNumber(value, decimals);

    float pixels = 0;
    const float notches = ctx.ConsumeWheel(rect, pixels);
    if (notches != 0) stepBy(-notches);   // rolling up (negative notches) increases

    if (focused) {
        // Focus by a press selects the whole number, as Tab does; later presses place the caret.
        if (!(in.pressed && !e.wasFocused)) FieldMouse(ctx, in, e, st.text, inner, BodySize(ctx));
        Editor(ctx, id, e, st.text, false, &NumberChar).Run([&](const InputEvent& ev) {
            if (ev.mods != 0) return false;
            switch (ev.key) {
            case Key::Up: stepBy(1); return true;
            case Key::Down: stepBy(-1); return true;
            case Key::Enter:
                commit();
                e.anchor = 0;
                e.caret = st.text.size();
                return true;
            default: return false;
            }
        });
    }
    e.wasFocused = focused;

    FieldChrome(ctx, rect, focused, in.hovered);
    if (focused) {
        FieldDraw(ctx, e, st.text, inner, true, {}, BodySize(ctx), t.colors.text);
    } else {
        std::string shown = st.text;
        shown.append(suffix);
        e.scrollX = 0;
        ctx.DrawTextInRect(shown, inner, BodySize(ctx), t.colors.text);
    }
    return value != before;
}

// ---- Text ----

void Label(Context& ctx, std::string_view textValue, RectF rect, std::optional<Rgba> color, Align align, float size) {
    ctx.DrawTextInRect(textValue, rect, size > 0 ? size : BodySize(ctx), color ? *color : T(ctx).colors.text, align);
}

float SectionHeaderHeight(Context& ctx, bool first) {
    const Tokens& t = T(ctx);
    const float row = static_cast<float>(Ceil(ctx.Metrics(ctx.FontSize(t.font.subtitle)).lineHeight)) +
                      static_cast<float>(t.spacing.sm);
    return (first ? 0.0f : static_cast<float>(t.spacing.panelGap)) + row;
}

void SectionHeader(Context& ctx, std::string_view textValue, RectF& column, bool first) {
    const Tokens& t = T(ctx);
    if (!first) CutTop(column, static_cast<float>(t.spacing.panelGap));
    const RectF row = CutTop(column, SectionHeaderHeight(ctx, true));
    ctx.DrawTextInRect(textValue, row, ctx.FontSize(t.font.subtitle), t.colors.mutedText);
}

std::vector<std::string_view> WrapText(Context& ctx, std::string_view textValue, float size, float width) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < textValue.size()) {
        const size_t newline = textValue.find('\n', start);
        const size_t end = newline == std::string_view::npos ? textValue.size() : newline;
        std::string_view rest = textValue.substr(start, end - start);
        while (!rest.empty()) {
            const text::GlyphRun& run = ctx.Shape(rest, size);
            if (run.advance <= width) {
                lines.push_back(rest);
                break;
            }
            // The last space whose text before it fits, else the last code point that fits.
            size_t cut = 0, lastSpace = std::string_view::npos;
            for (const text::PositionedGlyph& g : run.glyphs) {
                if (g.x + g.advance > width) break;
                cut = NextCp(rest, g.byteOffset);
                if (rest[g.byteOffset] == ' ') lastSpace = g.byteOffset;
            }
            if (lastSpace != std::string_view::npos && lastSpace > 0) {
                lines.push_back(rest.substr(0, lastSpace));
                rest = rest.substr(lastSpace + 1);
            } else {
                if (cut == 0) cut = NextCp(rest, 0);
                lines.push_back(rest.substr(0, cut));
                rest = rest.substr(cut);
            }
        }
        if (newline == std::string_view::npos) break;
        if (rest.empty() && end == start) lines.push_back({});
        start = end + 1;
    }
    return lines;
}

// ---- ScrollArea ----

SmoothScroll& ScrollAreaScroll(Context& ctx, std::string_view idLabel) {
    return ctx.State<ScrollAreaState>(ctx.MakeId(idLabel)).scroll;
}

float BeginScrollArea(Context& ctx, std::string_view idLabel, RectF viewport, float contentHeight) {
    const Id id = ctx.MakeId(idLabel);
    ScrollAreaState& st = ctx.State<ScrollAreaState>(id);
    if (contentHeight >= 0) st.content = contentHeight;
    st.scroll.SetMax(st.content - viewport.h);
    const double now = ctx.NowMs();
    if (st.scroll.Animating(now)) {
        ctx.RequestAnimation();
        st.lastActivity = now;
    }
    const float offset = st.scroll.Value(now, ctx.Scale());
    ctx.State<ScrollStack>(kScrollStackId).frames.push_back({id, viewport, offset});
    ctx.PushId(id);
    ctx.PushClip(viewport);
    return offset;
}

void EndScrollArea(Context& ctx, float contentHeight) {
    ScrollStack& stack = ctx.State<ScrollStack>(kScrollStackId);
    if (stack.frames.empty()) return;
    const ScrollFrame frame = stack.frames.back();
    stack.frames.pop_back();
    ctx.PopClip();
    ctx.PopId();
    const Tokens& t = T(ctx);
    ScrollAreaState& st = ctx.State<ScrollAreaState>(frame.id);
    st.content = std::max(0.0f, contentHeight);
    const RectF vp = frame.viewport;
    st.scroll.SetMax(st.content - vp.h);
    const double now = ctx.NowMs();
    // The wheel after the content, so widgets inside that step with it (NumberField) come first.
    float pixels = 0;
    const float notches = ctx.ConsumeWheel(vp, pixels);
    if (notches != 0) st.scroll.AddNotches(notches, now);
    if (pixels != 0) st.scroll.AddPixels(pixels);
    if (notches != 0 || pixels != 0) {
        st.lastActivity = now;
        ctx.RequestAnimation();
    }
    const float max = st.scroll.Max();
    if (!(max > 0)) return;

    const float zone = 10;
    const RectF track{vp.Right() - zone, vp.y, zone, vp.h};
    const float thumbH = std::max(24.0f, vp.h * vp.h / st.content);
    const float travel = vp.h - thumbH;
    const float offset = frame.offset;
    const float thumbY = vp.y + (travel > 0 ? travel * offset / max : 0);

    ctx.PushId(frame.id);
    const Id thumbId = ctx.MakeId("thumb");
    ctx.PopId();
    const Interaction in = ctx.Interact(thumbId, track);
    if (in.pressed) {
        const float py = ctx.Pointer().y;
        if (py >= thumbY && py < thumbY + thumbH) {
            st.dragGrab = py - thumbY;
        } else {
            st.dragGrab = thumbH / 2;   // a press on the track jumps there
        }
    }
    if (in.held && travel > 0) {
        const float y = ctx.Pointer().y - st.dragGrab - vp.y;
        st.scroll.JumpTo(Clampf(y / travel, 0, 1) * max);
        ctx.RequestAnimation();
    }
    const bool hoverArea = ctx.Hovered(vp);
    const bool active = in.hovered || in.held;
    if (hoverArea || active) st.lastActivity = now;
    const double since = now - st.lastActivity;
    if (since > kScrollbarLingerMs) return;
    if (!hoverArea && !active) ctx.WakeAt(st.lastActivity + kScrollbarLingerMs + 1);
    const float w = active ? 8.0f : 6.0f;
    const RectF thumb{vp.Right() - w - 2, ctx.SnapToDevice(thumbY), w, ctx.SnapToDevice(thumbH)};
    ctx.FillRoundRect(thumb, w / 2, active ? t.colors.scrollbarThumbActive : t.colors.scrollbarThumb);
}

// ---- Splitter ----

float Splitter(Context& ctx, std::string_view idLabel, RectF area, float& ratio, bool* changed) {
    const Tokens& t = T(ctx);
    const float before = ratio;
    if (!(ratio >= 0.2f)) ratio = 0.2f;   // NaN too
    if (ratio > 0.8f) ratio = 0.8f;
    const Id id = ctx.MakeId(idLabel);
    float x = ctx.SnapToDevice(area.x + area.w * ratio);
    const Interaction in = ctx.Interact(id, {x - 3, area.y, 6, area.h});
    if (in.held && area.w > 0) {
        ratio = Clampf((ctx.Pointer().x - area.x) / area.w, 0.2f, 0.8f);
        x = ctx.SnapToDevice(area.x + area.w * ratio);
    }
    if (in.hovered || in.held) ctx.SetCursor(Cursor::ResizeH);
    ctx.FillRect({x, area.y, 1, area.h}, t.colors.hairline);
    if (changed) *changed = ratio != before;
    return x;
}

// ---- Modal ----

float ModalBodyWidth(Context& ctx) { return std::min(420.0f, ctx.Width() - 48) - 40; }

RectF BeginModal(Context& ctx, std::string_view idLabel, std::string_view title, float bodyHeight) {
    const Tokens& t = T(ctx);
    const Id id = ctx.MakeId(idLabel);
    ctx.PushLayer(kLayerModal);
    ctx.PushId(id);
    ctx.BlockAll();
    ctx.FillRect(ctx.WindowRect(), t.colors.scrim);
    const float pad = 20;
    const float headingSize = ctx.FontSize(t.font.heading);
    const float titleH = static_cast<float>(Ceil(ctx.Metrics(headingSize, true).lineHeight));
    const float gap = static_cast<float>(t.spacing.xxl);
    const float w = std::max(0.0f, std::min(420.0f, ctx.Width() - 48));
    const float h = pad + titleH + gap + bodyHeight + pad + static_cast<float>(t.spacing.controlHeight) + pad;
    const RectF card{ctx.SnapToDevice((ctx.Width() - w) / 2), ctx.SnapToDevice(std::max(0.0f, (ctx.Height() - h) / 2)), w, h};
    const float radius = static_cast<float>(t.radius);
    ctx.FillRoundRect(card, radius, t.popups.background);
    if (t.popups.borderWidth > 0) ctx.StrokeRoundRect(card, radius, static_cast<float>(t.popups.borderWidth), t.popups.border);
    RectF inner = Inset(card, pad);
    ctx.DrawTextInRect(title, CutTop(inner, titleH), headingSize, t.popups.text, Align::Left, true);
    CutTop(inner, gap);
    const RectF body = CutTop(inner, bodyHeight);
    ctx.State<ModalStack>(kModalStackId).frames.push_back({id, card, body});
    return body;
}

int EndModal(Context& ctx, const ModalButton* buttons, size_t count) {
    ModalStack& stack = ctx.State<ModalStack>(kModalStackId);
    if (stack.frames.empty()) return -1;
    const ModalFrame frame = stack.frames.back();
    stack.frames.pop_back();
    const Tokens& t = T(ctx);
    int result = -1;
    // Keys first: Enter / Esc / Alt+key, unless a widget of the body took them.
    for (size_t i = 0; i < count && result < 0; i++) {
        if (buttons[i].altKey != Key::None && ctx.ConsumeKey(buttons[i].altKey, kModAlt)) result = static_cast<int>(i);
    }
    for (size_t i = 0; i < count && result < 0; i++) {
        if (buttons[i].enter && ctx.ConsumeKey(Key::Enter)) result = static_cast<int>(i);
    }
    for (size_t i = 0; i < count && result < 0; i++) {
        if (buttons[i].escape && ctx.ConsumeKey(Key::Escape)) result = static_cast<int>(i);
    }
    const float pad = 20;
    const float h = static_cast<float>(t.spacing.controlHeight);
    RectF row{frame.card.x + pad, frame.card.Bottom() - pad - h, frame.card.w - 2 * pad, h};
    std::vector<RectF> rects(count);
    for (size_t k = count; k-- > 0;) {
        rects[k] = CutRight(row, ButtonWidth(ctx, buttons[k].label));
        CutRight(row, static_cast<float>(t.spacing.controlGap));
    }
    for (size_t i = 0; i < count; i++) {
        if (Button(ctx, buttons[i].label, rects[i], buttons[i].primary ? ButtonStyle::Primary : ButtonStyle::Normal) &&
            result < 0) {
            result = static_cast<int>(i);
        }
    }
    // Presses on the scrim go nowhere.
    ctx.Interact(ctx.MakeId("scrim"), ctx.WindowRect());
    ctx.PopId();
    ctx.PopLayer();
    return result;
}

int MessageModal(Context& ctx, std::string_view id, std::string_view title, std::string_view body,
                 const ModalButton* buttons, size_t count) {
    const float size = BodySize(ctx);
    const float lineH = static_cast<float>(Ceil(1.4 * size));
    const std::vector<std::string_view> lines = WrapText(ctx, body, size, ModalBodyWidth(ctx));
    const RectF area = BeginModal(ctx, id, title, lineH * static_cast<float>(lines.size()));
    const Rgba color = ctx.GetTokens().popups.text;
    for (size_t i = 0; i < lines.size(); i++) {
        ctx.DrawTextInRect(lines[i], {area.x, area.y + lineH * static_cast<float>(i), area.w, lineH}, size, color);
    }
    return EndModal(ctx, buttons, count);
}

} // namespace rgui::ui
