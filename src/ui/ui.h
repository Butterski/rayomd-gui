// SPDX-License-Identifier: Apache-2.0
// The immediate-mode core: ids, hot / active / focused widgets, the frame's input, layers,
// per-widget state, timers, RectCut layout and drawing in logical pixels into a DisplayList.
//
// A frame: the platform's events go to AddEvent as they arrive; OnFrame calls BeginFrame, builds
// the UI (widgets read Events() and the pointer, draw through the helpers or List()), then
// EndFrame, whose output says what to tell the platform and whether another frame is wanted.
#pragma once

#include "app/platform.h"
#include "gfx/geometry.h"
#include "text/fontchoice.h"
#include "text/textlayout.h"
#include "ui/displaylist.h"
#include "ui/icons.h"
#include "ui/tokens.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rgui::ui {

using gfx::RectF;
using Id = uint64_t;

// ---- RectCut layout (logical pixels) ----
// Each Cut takes `amount` (clamped to what is there) off one side of `r` and returns that piece.
RectF CutTop(RectF& r, float amount);
RectF CutBottom(RectF& r, float amount);
RectF CutLeft(RectF& r, float amount);
RectF CutRight(RectF& r, float amount);
// `r` shrunk by `d` on every side (`dx` left and right, `dy` top and bottom), never below 0 size.
RectF Inset(RectF r, float d);
RectF Inset(RectF r, float dx, float dy);
// A `w` x `h` rectangle centred in `r`.
RectF Centered(RectF r, float w, float h);
// The part of a label before "##" (the rest only makes the id unique).
std::string_view VisibleLabel(std::string_view label);

// ---- Wheel scrolling with Omawrite's feel ----
// A notch moves `step` (120 logical px) towards a running target, eased with
// cubic-bezier(0.42, 0, 0.58, 1) over 200 ms for one pending notch, shortening linearly to
// 100 ms at 5 or more; each new notch restarts the curve from where it is. Pixel deltas
// (touchpads) move at once. Values are clamped to [0, max] and snapped to whole device pixels.
class SmoothScroll {
public:
    static constexpr float kNotchStep = 120.0f;

    void SetMax(float max);
    float Max() const { return max_; }
    void AddNotches(float notches, double nowMs, float step = kNotchStep);
    void AddPixels(float pixels);
    // Jumps without easing (a scrollbar drag, keeping a caret visible).
    void JumpTo(float value);
    // The position at `nowMs`, a whole number of device pixels at `scale`.
    float Value(double nowMs, double scale) const;
    // Where it is going.
    float Target() const { return target_; }
    bool Animating(double nowMs) const { return animating_ && nowMs < startMs_ + durationMs_; }

    // The eased fraction of cubic-bezier(0.42, 0, 0.58, 1) at time fraction `x` in [0, 1].
    static double Ease(double x);

private:
    float RawValue(double nowMs) const;

    float max_ = 0;
    float start_ = 0;
    float target_ = 0;
    double startMs_ = 0;
    double durationMs_ = 0;
    bool animating_ = false;
};

// ---- The frame's input ----
struct PointerInput {
    float x = -1e6f;        // logical pixels; far away until the pointer enters
    float y = -1e6f;
    bool inside = false;
    bool down[3] = {false, false, false};       // held now (MouseButton order)
    bool pressed[3] = {false, false, false};    // went down during the frame
    bool released[3] = {false, false, false};   // went up during the frame
    // 1, 2, 3...: presses of the left button within the double-click time and 4 logical px of
    // the previous one (the count of the last press this frame).
    int clickCount = 0;
    float pressX = 0;       // where the last left press of the frame happened
    float pressY = 0;
};

// What the platform should be told after a frame.
struct FrameOutput {
    const DisplayList* displayList = nullptr;
    Cursor cursor = Cursor::Arrow;
    bool textInputEnabled = false;
    gfx::RectI textInputRect;    // logical pixels (Platform::SetTextInputRect)
    // An animation runs: draw again as soon as the platform can.
    bool wantsFrame = false;
    // A timer is pending (a tooltip delay, a scrollbar fade): draw again at this time, in the
    // clock of BeginFrame's nowMs; < 0 when none.
    double nextDeadlineMs = -1;
    std::string title;
};

// Layers stack drawing and input: a higher layer draws above every lower one and, where it
// blocked (Block), takes the pointer from them. Keyboard focus stays in the highest layer that
// blocked the whole window (a modal).
enum Layer : int {
    kLayerBase = 0,
    kLayerPopup = 1,
    kLayerModal = 2,
    kLayerModalPopup = 3,
    kLayerTooltip = 4,
    kLayerCount = 5,
};

// What Interact saw of a widget this frame.
struct Interaction {
    bool hovered = false;   // the pointer is over it and nothing above blocks it
    bool pressed = false;   // the left button went down on it this frame
    bool held = false;      // it is active (pressed on it and not yet released)
    bool released = false;  // the left button was released while it was active
    bool clicked = false;   // released over it after a press on it
    int clickCount = 0;     // with pressed: 1, 2, 3...
};

enum InteractFlags : uint32_t {
    kInteractFocusable = 1,     // takes keyboard focus (Tab order, focus on press)
    kInteractNoFocusOnPress = 2 // focusable by Tab only
};

enum class Align : uint8_t { Left, Center, Right };

struct FontMetrics {
    float ascent = 0;       // above the baseline, logical px
    float descent = 0;      // below the baseline, positive
    float lineHeight = 0;   // ascent + descent
};

class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // ---- Setup ----
    // The interface font (owned by the app, outliving the Context) and tokens. Without tokens a
    // default Tokens{} is used; without fonts text draws nothing and measures 0.
    void SetFonts(const text::FontSet* fonts);
    const text::FontSet* Fonts() const { return fonts_; }
    void SetTokens(const Tokens* tokens);
    const Tokens& GetTokens() const { return tokens_ ? *tokens_ : defaultTokens_; }
    // The double-click time (300 ms by default; Windows passes GetDoubleClickTime()).
    void SetDoubleClickTime(int ms) { doubleClickMs_ = ms; }
    // Clipboard access for text fields (the app passes Platform's), and the platform's
    // RequestFrame, called when a clipboard read arrives between frames.
    void SetClipboard(std::function<void(std::string)> set,
                      std::function<void(std::function<void(std::string)>)> request);
    void SetRequestFrame(std::function<void()> requestFrame) { requestFrame_ = std::move(requestFrame); }

    // ---- Input ----
    // Queues an event for the next frame; `nowMs` is when it arrived (click counting).
    void AddEvent(const InputEvent& event, double nowMs);

    // ---- Frame ----
    // Starts a frame of `widthPx` x `heightPx` device pixels at `scale`; `nowMs` is a monotonic
    // clock in milliseconds.
    void BeginFrame(int widthPx, int heightPx, double scale, double nowMs);
    // Ends it: Tab moves focus if no widget consumed it, unused state is dropped, the layers
    // are joined into one display list.
    const FrameOutput& EndFrame();
    uint64_t FrameNumber() const { return frame_; }
    double NowMs() const { return now_; }
    double Scale() const { return scale_; }
    float Width() const { return width_; }    // logical px
    float Height() const { return height_; }
    RectF WindowRect() const { return {0, 0, width_, height_}; }

    // The frame's events in arrival order. Widgets handle the ones meant for them and consume
    // them; a consumed Tab does not move focus.
    const std::vector<InputEvent>& Events() const { return events_; }
    bool IsConsumed(size_t index) const { return consumed_[index] != 0; }
    void ConsumeEvent(size_t index) { consumed_[index] = 1; }
    // For a left-button press event: its click count (1, 2, 3...), else 0.
    int ClickCount(size_t index) const { return clickCounts_[index]; }
    // The first unconsumed KeyDown of `key` with exactly `mods` (Ctrl, Shift, Alt, Super);
    // consumes it.
    bool ConsumeKey(Key key, uint8_t mods = 0);
    // The vertical wheel over `area` (unblocked at the current layer) of events whose Ctrl
    // matches `ctrl`: consumes them and returns the notches; `pixels` gets the pixel deltas.
    float ConsumeWheel(RectF area, float& pixels, bool ctrl = false);
    const PointerInput& Pointer() const { return pointer_; }
    // The modifiers of the latest key, button or wheel event.
    uint8_t Mods() const { return mods_; }
    bool WindowFocused() const { return windowFocused_; }

    // ---- Ids ----
    // FNV-1a-64 of `label` mixed with the id stack. In a label, "##" starts a part that counts
    // for the id but is not shown ("Choose##theme").
    Id MakeId(std::string_view label) const;
    Id MakeId(uint64_t value) const;
    void PushId(std::string_view label);
    void PushId(uint64_t value);
    void PopId();

    // ---- Interaction ----
    // The pointer is over `rect` and nothing at a higher layer blocks it.
    bool Hovered(RectF rect) const;
    // The standard behaviour of a clickable widget: hover (hot), press (active, focus with
    // kInteractFocusable), release, click. Disabled widgets get nothing.
    Interaction Interact(Id id, RectF rect, uint32_t flags = 0);
    // Registers `id` in the Tab order (not when disabled) and says whether it has focus.
    bool FocusBehavior(Id id);
    bool IsFocused(Id id) const { return id != 0 && focused_ == id; }
    Id FocusedId() const { return focused_; }
    void SetFocus(Id id) { focused_ = id; }
    void ClearFocus() { focused_ = 0; }
    // True when a widget of this layer may take keys: no modal sits above it.
    bool KeysAllowed() const;
    bool IsHot(Id id) const { return id != 0 && hot_ == id; }
    bool IsActive(Id id) const { return id != 0 && active_ == id; }
    Id HotId() const { return hot_; }
    Id ActiveId() const { return active_; }
    void SetHot(Id id) { hot_ = id; }
    void SetActive(Id id) { active_ = id; }
    void ClearActive() { active_ = 0; }
    // How long `id` has been hot, in ms (0 when it is not).
    double HotMilliseconds(Id id) const;

    // ---- Layers ----
    void PushLayer(int layer);
    void PopLayer();
    int CurrentLayer() const { return layerStack_.back(); }
    // Takes the pointer within `rect` from every lower layer (from the next frame on, and for
    // widgets after this call in this frame).
    void Block(RectF rect);
    // Takes the whole window and the keyboard from every lower layer (a modal).
    void BlockAll();

    // ---- Disabled widgets ----
    // Widgets inside draw at 40 % opacity and take no input or focus.
    void BeginDisabled(bool disabled = true);
    void EndDisabled();
    bool Disabled() const { return disabledDepth_ > 0; }
    // Multiplies the alpha of everything drawn until the matching Pop.
    void PushOpacity(float opacity);
    void PopOpacity();
    float Opacity() const { return opacity_.back(); }

    // ---- Per-widget state ----
    // The state of `id` as a T, created on first use (value-initialised); a state not asked for
    // during 120 frames is erased. Asking for another type replaces it.
    template <typename T>
    T& State(Id id);
    bool HasState(Id id) const { return states_.count(id) != 0; }

    // ---- Time ----
    // Another frame as soon as possible (an animation is running).
    void RequestAnimation() { wantsFrame_ = true; }
    // Another frame no later than `ms` (BeginFrame's clock).
    void WakeAt(double ms);

    // ---- Output ----
    void SetCursor(Cursor cursor) { cursor_ = cursor; }
    void SetTextInputRect(RectF rect);   // enables text input for this frame
    void SetTitle(std::string title) { title_ = std::move(title); }

    // ---- Clipboard ----
    void SetClipboardText(std::string text);
    // Asks for the clipboard's text for `id`; it arrives later through TakeClipboard(id).
    void RequestClipboard(Id id);
    // The clipboard text that arrived for `id`, if any (once).
    bool TakeClipboard(Id id, std::string& text);

    // ---- Drawing, logical pixels ----
    DisplayList& List() { return lists_[static_cast<size_t>(CurrentLayer())]; }
    // Device pixels: each edge rounded on its own (round(x * scale)).
    gfx::RectI ToDevice(RectF rect) const;
    float ToDeviceX(float x) const;
    float SnapToDevice(float v) const;   // to a whole number of device pixels, in logical px
    // A colour as premultiplied, with the opacity stack applied.
    uint32_t Color(Rgba color) const;

    void FillRect(RectF rect, Rgba color);
    void StrokeRect(RectF rect, float width, Rgba color);
    void FillRoundRect(RectF rect, float radius, Rgba color);
    void StrokeRoundRect(RectF rect, float radius, float width, Rgba color);
    void DrawIcon(Icon icon, RectF square, Rgba color);
    void DrawImage(const gfx::Image& image, RectF dst, float opacity = 1.0f);
    void PushClip(RectF rect);
    void PopClip();

    // ---- Text ----
    FontMetrics Metrics(float size, bool bold = false) const;
    // The run of `text` at `size` logical px per em, shaped once and kept while it is used
    // (pen positions in logical px).
    const text::GlyphRun& Shape(std::string_view text, float size, bool bold = false);
    float TextWidth(std::string_view text, float size, bool bold = false) { return Shape(text, size, bold).advance; }
    // Draws `text` with its pen starting at x (snapped to device pixels) on `baseline`.
    void DrawText(std::string_view text, float x, float baseline, float size, Rgba color, bool bold = false);
    // Draws `text` vertically centred in `rect`, aligned horizontally, clipped to `rect` when it
    // does not fit. Returns its width.
    float DrawTextInRect(std::string_view text, RectF rect, float size, Rgba color, Align align = Align::Left,
                         bool bold = false);
    // The baseline that centres a line of `size` in a box from `top` of `height`.
    float CenteredBaseline(float top, float height, float size, bool bold = false) const;

private:
    struct StateSlot {
        uint64_t lastFrame = 0;
        const void* type = nullptr;
        std::unique_ptr<void, void (*)(void*)> data{nullptr, nullptr};
    };
    struct Overlay {
        RectF rect;
        int layer = 0;
        bool all = false;
    };
    struct ShapeEntry {
        std::string text;
        uint32_t size64 = 0;
        bool bold = false;
        uint64_t lastFrame = 0;
        text::GlyphRun run;
    };
    template <typename T>
    static const void* TypeTag() {
        static const char tag = 0;
        return &tag;
    }

    bool Blocked(float x, float y, int layer) const;
    void SweepState();

    // Setup.
    const text::FontSet* fonts_ = nullptr;
    const Tokens* tokens_ = nullptr;
    Tokens defaultTokens_;
    int doubleClickMs_ = 300;
    std::function<void(std::string)> clipboardSet_;
    std::function<void(std::function<void(std::string)>)> clipboardRequest_;
    std::function<void()> requestFrame_;

    // Input between frames and of the frame.
    std::vector<InputEvent> pending_;
    std::vector<InputEvent> events_;
    std::vector<uint8_t> consumed_;
    std::vector<int> pendingClickCounts_;   // per pending event
    std::vector<int> clickCounts_;          // per frame event
    PointerInput pointer_;
    uint8_t mods_ = 0;
    bool windowFocused_ = true;
    double lastClickMs_ = -1e18;
    float lastClickX_ = 0;
    float lastClickY_ = 0;
    int lastClickCount_ = 0;

    // Frame.
    uint64_t frame_ = 0;
    double now_ = 0;
    double scale_ = 1;
    int widthPx_ = 0;
    int heightPx_ = 0;
    float width_ = 0;
    float height_ = 0;

    // Ids and interaction.
    std::vector<Id> idStack_;
    Id hot_ = 0;
    Id lastHot_ = 0;
    bool activeSeen_ = false;
    double hotSince_ = 0;
    Id active_ = 0;
    Id focused_ = 0;
    std::vector<std::pair<Id, int>> focusOrder_;   // (id, layer) in declaration order
    bool anyPressTaken_ = false;

    // Layers.
    std::vector<int> layerStack_{kLayerBase};
    std::vector<Overlay> overlays_;       // this frame's
    std::vector<Overlay> lastOverlays_;   // the previous frame's
    int modalLayer_ = -1;                 // this frame's highest BlockAll layer
    int lastModalLayer_ = -1;
    int disabledDepth_ = 0;
    std::vector<bool> disabledStack_;
    std::vector<float> opacity_{1.0f};

    // State, text and clipboard.
    std::unordered_map<Id, StateSlot> states_;
    std::unordered_map<uint64_t, ShapeEntry> shapes_;
    std::shared_ptr<std::unordered_map<Id, std::string>> clipboardArrivals_;
    std::vector<GlyphInstance> glyphScratch_;

    // Output.
    DisplayList lists_[kLayerCount];
    DisplayList joined_;
    FrameOutput output_;
    Cursor cursor_ = Cursor::Arrow;
    bool textInputEnabled_ = false;
    RectF textInputRect_;
    std::string title_;
    bool wantsFrame_ = false;
    double nextDeadline_ = -1;
};

template <typename T>
T& Context::State(Id id) {
    StateSlot& slot = states_[id];
    slot.lastFrame = frame_;
    if (slot.type != TypeTag<T>() || !slot.data) {
        slot.data = std::unique_ptr<void, void (*)(void*)>(new T(), [](void* p) { delete static_cast<T*>(p); });
        slot.type = TypeTag<T>();
    }
    return *static_cast<T*>(slot.data.get());
}

} // namespace rgui::ui
