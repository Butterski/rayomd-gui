// SPDX-License-Identifier: Apache-2.0
// The toolkit core: icons, the display list and compositor, the immediate-mode context and the
// widgets, headless into owned images.
#include "runner.h"

#include "gfx/surface.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "text/glyphcache.h"
#include "ui/compositor.h"
#include "ui/displaylist.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/tokens.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace rgui;
using namespace rgui::ui;

namespace {

// The test font, loaded once: DejaVu Sans Mono on Linux, Consolas on Windows, else the first
// fixed-pitch face; nullptr when there is none (font-dependent tests skip).
const text::FontSet* TestFonts() {
    static text::FontIndex index;
    static text::FontSet fonts;
    static int state = 0;
    if (state == 0) {
#if defined(_WIN32)
        index.SetDirectories(text::FontIndex::DefaultDirectories());
#else
        index.SetDirectories({"/usr/share/fonts"});
#endif
        state = fonts.Load(index, {"DejaVu Sans Mono", "Consolas"}, {"DejaVu Sans"}) ? 1 : 2;
    }
    return state == 1 ? &fonts : nullptr;
}

Tokens TokyoNight() {
    const BundledTheme* theme = FindBundledTheme("tokyo-night");
    return BuildTokens(BundledPalette(*theme), TokenSources{});
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Harness {
    Context ctx;
    Tokens tokens = TokyoNight();
    double now = 10000;
    int width = 800;
    int height = 600;
    double scale = 1.0;
    std::string clipboard;

    explicit Harness(bool fonts = true) {
        ctx.SetTokens(&tokens);
        if (fonts) ctx.SetFonts(TestFonts());
        ctx.SetClipboard([this](std::string text) { clipboard = std::move(text); },
                         [this](std::function<void(std::string)> callback) { callback(clipboard); });
    }

    void Add(const InputEvent& e) { ctx.AddEvent(e, now); }
    void KeyDown(Key key, uint8_t mods = 0) {
        InputEvent e;
        e.type = InputEvent::Type::KeyDown;
        e.key = key;
        e.mods = mods;
        Add(e);
    }
    void Type(const std::string& text) {
        InputEvent e;
        e.type = InputEvent::Type::Text;
        e.text = text;
        Add(e);
    }
    void Move(float x, float y) {
        InputEvent e;
        e.type = InputEvent::Type::PointerMove;
        e.x = x;
        e.y = y;
        Add(e);
    }
    void Button(float x, float y, bool pressed, uint8_t mods = 0) {
        InputEvent e;
        e.type = InputEvent::Type::Button;
        e.x = x;
        e.y = y;
        e.pressed = pressed;
        e.mods = mods;
        Add(e);
    }
    void Click(float x, float y) {
        Move(x, y);
        Button(x, y, true);
        Button(x, y, false);
    }
    void Wheel(float x, float y, float notches, float pixels = 0) {
        InputEvent e;
        e.type = InputEvent::Type::Wheel;
        e.x = x;
        e.y = y;
        e.notchesY = notches;
        e.pixelsY = pixels;
        Add(e);
    }

    template <typename F>
    const FrameOutput& Frame(F&& build, double advanceMs = 16) {
        ctx.BeginFrame(static_cast<int>(width * scale), static_cast<int>(height * scale), scale, now);
        build();
        const FrameOutput& out = ctx.EndFrame();
        now += advanceMs;
        return out;
    }
};

struct Framebuffer {
    gfx::Image image;
    void Resize(int w, int h) {
        image.width = w;
        image.height = h;
        image.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    }
    gfx::Surface Surface() { return image.AsSurface(); }
};

// A panel of labels and controls, as the options panel draws them.
void BuildPanel(Context& ctx, const std::string& changing, int widgets) {
    RectF window = ctx.WindowRect();
    ctx.FillRect(window, ctx.GetTokens().colors.windowBackground);
    RectF panel = CutRight(window, 300);
    ctx.FillRect(panel, ctx.GetTokens().colors.previewBackground);
    panel = Inset(panel, 18);
    const float h = static_cast<float>(ctx.GetTokens().spacing.controlHeight);
    static const std::vector<std::string> items = {"Elegant", "Modern", "Tech"};
    for (int i = 0; i < widgets; i++) {
        ctx.PushId(static_cast<uint64_t>(i));
        RectF row = CutTop(panel, h);
        CutTop(panel, 8);
        switch (i % 4) {
        case 0: Label(ctx, i == 0 ? changing : "Page size and margins", row); break;
        case 1: {
            bool value = (i % 8) == 1;
            Checkbox(ctx, "Table of contents", value, row);
            break;
        }
        case 2: {
            int selected = 1;
            Dropdown(ctx, "style", selected, items, row);
            break;
        }
        default: Button(ctx, "Choose\xE2\x80\xA6", CutLeft(row, ButtonWidth(ctx, "Choose\xE2\x80\xA6"))); break;
        }
        ctx.PopId();
    }
}

} // namespace

// ---- Icons ----

RGUI_TEST("ui.icons_rasterize") {
    IconCache cache;
    for (int i = 0; i < kIconCount; i++) {
        const Icon icon = static_cast<Icon>(i);
        IconMask mask;
        CHECK(RasterizeIcon(icon, 16, mask));
        CHECK_EQ(mask.size, 16);
        size_t covered = 0, full = 0;
        for (uint8_t a : mask.alpha) {
            covered += a > 0;
            full += a == 255;
        }
        // A stroked outline: some pixels inked, far from all of them.
        if (!(covered > 8 && covered < 200)) std::printf("icon %s covers %zu px\n", IconData(icon).name, covered);
        CHECK(covered > 8);
        CHECK(covered < 200);
        CHECK(full + 1 < covered);   // antialiased edges
        const IconMask* cached = cache.Get(icon, 16);
        CHECK(cached != nullptr && cached->alpha == mask.alpha);
        CHECK(cache.Get(icon, 16) == cached);
    }
    CHECK_EQ(cache.Count(), static_cast<size_t>(kIconCount));
    IconMask none;
    CHECK(!RasterizeIcon(Icon::Save, 0, none));
    CHECK(none.alpha.empty());
    // The stroke scales with the size: the 2x mask has about 4x the ink.
    IconMask small, large;
    RasterizeIcon(Icon::Close, 16, small);
    RasterizeIcon(Icon::Close, 32, large);
    double inkSmall = 0, inkLarge = 0;
    for (uint8_t a : small.alpha) inkSmall += a;
    for (uint8_t a : large.alpha) inkLarge += a;
    CHECK(inkLarge > 3.5 * inkSmall && inkLarge < 4.5 * inkSmall);
}

// ---- Compositor ----

RGUI_TEST("ui.damage_merge") {
    std::vector<gfx::RectI> out;
    // 4 x 3 tiles of 64 over a 250 x 150 frame (the last column and row are partial).
    std::vector<uint8_t> dirty = {
        0, 1, 1, 0,
        0, 1, 1, 0,
        1, 0, 0, 1,
    };
    Compositor::MergeDamage(dirty, 4, 3, 64, 250, 150, out);
    CHECK_EQ(out.size(), 3u);
    if (out.size() == 3) {
        CHECK(out[0] == (gfx::RectI{64, 0, 192, 128}));   // equal runs of rows 0 and 1, merged
        CHECK(out[1] == (gfx::RectI{0, 128, 64, 150}));
        CHECK(out[2] == (gfx::RectI{192, 128, 250, 150}));
    }
    // Unequal runs stay apart.
    dirty = {1, 1, 0, 0,
             1, 1, 1, 0,
             0, 0, 0, 0};
    Compositor::MergeDamage(dirty, 4, 3, 64, 256, 192, out);
    CHECK_EQ(out.size(), 2u);
    // A checkerboard of 64 runs collapses to its bounding box.
    dirty.assign(16 * 8, 0);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 16; c++) dirty[static_cast<size_t>(r * 16 + c)] = ((r + c) % 2 == 0 && c > 0) ? 1 : 0;
    Compositor::MergeDamage(dirty, 16, 8, 64, 1024, 512, out);
    CHECK_EQ(out.size(), 1u);
    if (!out.empty()) CHECK(out[0] == (gfx::RectI{64, 0, 1024, 512}));
    // Exactly 32 rects stay 32.
    dirty.assign(16 * 4, 0);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 16; c += 2) dirty[static_cast<size_t>(r * 16 + c + (r % 2))] = 1;
    Compositor::MergeDamage(dirty, 16, 4, 64, 1024, 256, out);
    CHECK_EQ(out.size(), 32u);
}

RGUI_TEST("ui.compositor_incremental") {
    if (!TestFonts()) {
        std::printf("skip ui.compositor_incremental: no font\n");
        return;
    }
    Harness h;
    Framebuffer fb;
    fb.Resize(800, 600);
    text::GlyphCache glyphs;
    IconCache icons;
    Compositor compositor(2);
    const uint32_t bg = h.tokens.colors.windowBackground.Premultiplied();

    const FrameOutput& first = h.Frame([&] { BuildPanel(h.ctx, "Document", 20); });
    std::vector<gfx::RectI> damage = compositor.Render(*first.displayList, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), compositor.TileCount());
    CHECK_EQ(damage.size(), 1u);
    if (!damage.empty()) CHECK(damage[0] == (gfx::RectI{0, 0, 800, 600}));

    // The same frame again: nothing to draw.
    const FrameOutput& same = h.Frame([&] { BuildPanel(h.ctx, "Document", 20); });
    damage = compositor.Render(*same.displayList, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), 0u);
    CHECK(damage.empty());

    // One label changes: only the tiles its old and new glyphs touch.
    const std::vector<uint32_t> before = fb.image.pixels;
    const FrameOutput& changed = h.Frame([&] { BuildPanel(h.ctx, "Contents", 20); });
    gfx::RectI labelBounds;
    for (const Command& c : changed.displayList->Commands()) {
        if (c.type == CommandType::Glyphs) {
            labelBounds = c.bounds;   // the first text drawn is the changing label
            break;
        }
    }
    damage = compositor.Render(*changed.displayList, bg, fb.Surface(), glyphs, icons);
    const int t = Compositor::kTileSize;
    const size_t expected = static_cast<size_t>(((labelBounds.x1 - 1) / t - labelBounds.x0 / t + 1) *
                                                ((labelBounds.y1 - 1) / t - labelBounds.y0 / t + 1));
    CHECK(compositor.DirtyTileCount() >= 1);
    CHECK_EQ(compositor.DirtyTileCount(), expected);
    // Every changed pixel lies in the damage.
    size_t changedPixels = 0;
    for (int y = 0; y < 600; y++) {
        for (int x = 0; x < 800; x++) {
            const size_t i = static_cast<size_t>(y * 800 + x);
            if (fb.image.pixels[i] == before[i]) continue;
            changedPixels++;
            bool inside = false;
            for (const gfx::RectI& r : damage) inside |= r.Contains(x, y);
            CHECK(inside);
            if (!inside) y = 600;
        }
    }
    CHECK(changedPixels > 20);

    // The incremental result equals a full render on another compositor (one thread).
    Framebuffer full;
    full.Resize(800, 600);
    Compositor fresh(1);
    text::GlyphCache glyphs2;
    IconCache icons2;
    fresh.Render(*changed.displayList, bg, full.Surface(), glyphs2, icons2);
    CHECK(full.image.pixels == fb.image.pixels);

    // A resize redraws everything.
    h.width = 700;
    fb.Resize(700, 600);
    const FrameOutput& resized = h.Frame([&] { BuildPanel(h.ctx, "Contents", 20); });
    damage = compositor.Render(*resized.displayList, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), compositor.TileCount());
    CHECK_EQ(compositor.TileCount(), static_cast<size_t>(11 * 10));
    // And so does a new scale at the same size.
    const FrameOutput& again = h.Frame([&] { BuildPanel(h.ctx, "Contents", 20); });
    compositor.Render(*again.displayList, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), 0u);
    DisplayList blank;
    blank.Reset(700, 600, 1.0);
    compositor.Render(blank, bg, fb.Surface(), glyphs, icons);
    compositor.Render(blank, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), 0u);
    blank.Reset(700, 600, 2.0);   // the same (empty) content at another scale
    compositor.Render(blank, bg, fb.Surface(), glyphs, icons);
    CHECK_EQ(compositor.DirtyTileCount(), compositor.TileCount());
}

RGUI_TEST("ui.compositor_draws") {
    // Each command type lands where it should, clips hold, and tile threads agree with one.
    gfx::Image photo;
    photo.width = 2;
    photo.height = 2;
    photo.pixels = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    photo.id = 7;
    photo.version = 1;
    DisplayList list;
    list.Reset(300, 200, 1.0);
    list.FillRect({10, 10, 50, 50}, 0xFFFF0000u);
    list.StrokeRect({60, 10, 100, 50}, 2, 0xFF00FF00u);
    list.FillRoundRect({110, 10, 40, 40}, 8, 0xFF0000FFu);
    list.StrokeRoundRect({160, 10, 40, 40}, 8, 2, 0xFFFFFFFFu);
    list.PushClip({10, 100, 40, 140});
    list.FillRect({0, 90, 100, 160}, 0xFF00FFFFu);   // clipped to 30 x 40
    list.PopClip();
    list.DrawImage(photo, {200, 100, 40, 40}, 255);
    list.DrawIcon(Icon::Close, {250, 100, 266, 116}, 0xFFFFFFFFu);

    text::GlyphCache glyphs;
    IconCache icons;
    Framebuffer a, b;
    a.Resize(300, 200);
    b.Resize(300, 200);
    Compositor one(1), four(4);
    one.Render(list, 0xFF000000u, a.Surface(), glyphs, icons);
    four.Render(list, 0xFF000000u, b.Surface(), glyphs, icons);
    CHECK(a.image.pixels == b.image.pixels);
    auto px = [&](int x, int y) { return a.image.pixels[static_cast<size_t>(y * 300 + x)]; };
    CHECK_EQ(px(30, 30), 0xFFFF0000u);
    CHECK_EQ(px(9, 30), 0xFF000000u);
    CHECK_EQ(px(61, 30), 0xFF00FF00u);     // the stroke, inside the rect
    CHECK_EQ(px(80, 30), 0xFF000000u);     // the hole
    CHECK_EQ(px(130, 30), 0xFF0000FFu);
    CHECK_EQ(px(110, 10), 0xFF000000u);    // a rounded-off corner
    CHECK_EQ(px(180, 30), 0xFF000000u);
    CHECK_EQ(px(180, 11), 0xFFFFFFFFu);
    CHECK_EQ(px(20, 120), 0xFF00FFFFu);
    CHECK_EQ(px(5, 120), 0xFF000000u);     // outside the clip
    CHECK_EQ(px(45, 120), 0xFF000000u);
    CHECK_EQ(px(202, 102), 0xFFFF0000u);   // the image's top-left texel, smooth but clamped
    CHECK_EQ(px(238, 138), 0xFFFFFFFFu);
    CHECK(px(258, 108) != 0xFF000000u);    // the cross's centre
    CHECK_EQ(px(250, 108), 0xFF000000u);   // left of the cross's arms
}

RGUI_TEST("ui.display_list_bounds") {
    DisplayList list;
    list.Reset(200, 100, 1.0);
    list.FillRect({-10, -10, 20, 20}, 0xFFFFFFFFu);
    list.FillRect({300, 0, 310, 10}, 0xFFFFFFFFu);   // outside: dropped
    list.PushClip({50, 50, 60, 60});
    list.FillRect({0, 0, 200, 100}, 0xFFFFFFFFu);
    list.PopClip();
    list.PopClip();   // unmatched: ignored
    CHECK_EQ(list.Commands().size(), 4u);
    CHECK(list.Commands()[0].bounds == (gfx::RectI{0, 0, 20, 20}));
    CHECK(list.Commands()[2].bounds == (gfx::RectI{50, 50, 60, 60}));
    CHECK(list.CurrentClip() == (gfx::RectI{0, 0, 200, 100}));
    // Equal commands hash equal; any field changes the hash.
    DisplayList other;
    other.Reset(200, 100, 1.0);
    other.FillRect({-10, -10, 20, 20}, 0xFFFFFFFFu);
    other.FillRect({-10, -10, 20, 20}, 0xFFFFFFFEu);
    CHECK_EQ(other.Commands()[0].hash, list.Commands()[0].hash);
    CHECK(other.Commands()[1].hash != list.Commands()[0].hash);
    // Images hash their id and version, not their pixels.
    gfx::Image image;
    image.width = image.height = 1;
    image.pixels = {0xFF000000u};
    image.id = 3;
    DisplayList i1, i2;
    i1.Reset(10, 10, 1.0);
    i2.Reset(10, 10, 1.0);
    i1.DrawImage(image, {0, 0, 5, 5}, 255);
    image.pixels[0] = 0xFFFFFFFFu;
    i2.DrawImage(image, {0, 0, 5, 5}, 255);
    CHECK_EQ(i1.Commands()[0].hash, i2.Commands()[0].hash);
    image.version++;
    i2.DrawImage(image, {0, 0, 5, 5}, 255);
    CHECK(i1.Commands()[0].hash != i2.Commands()[1].hash);
}

// ---- Context ----

RGUI_TEST("ui.click_count") {
    Harness h(false);
    int counts[4] = {0, 0, 0, 0};
    auto frame = [&](int slot) {
        h.Frame([&] {
            const Interaction in = h.ctx.Interact(h.ctx.MakeId("area"), {0, 0, 400, 400});
            if (in.pressed) counts[slot] = in.clickCount;
        }, 100);
    };
    h.Click(100, 100);
    frame(0);
    h.Click(103, 101);   // 100 ms later, 3 px away
    frame(1);
    h.Click(103, 101);
    frame(2);
    CHECK_EQ(counts[0], 1);
    CHECK_EQ(counts[1], 2);
    CHECK_EQ(counts[2], 3);
    h.Click(110, 101);   // 7 px away: a new click
    frame(3);
    CHECK_EQ(counts[3], 1);
    h.now += 400;        // past the double-click time
    h.Click(110, 101);
    frame(3);
    CHECK_EQ(counts[3], 1);
    h.ctx.SetDoubleClickTime(500);
    h.now += 400;
    h.Click(110, 101);
    frame(3);
    CHECK_EQ(counts[3], 2);
}

RGUI_TEST("ui.focus_order") {
    Harness h(false);
    bool disabledMiddle = false;
    auto build = [&] {
        Button(h.ctx, "One", {10, 10, 80, 28});
        h.ctx.BeginDisabled(disabledMiddle);
        Button(h.ctx, "Two", {10, 50, 80, 28});
        h.ctx.EndDisabled();
        Button(h.ctx, "Three", {10, 90, 80, 28});
    };
    h.Frame(build);
    const Id one = h.ctx.MakeId("One"), two = h.ctx.MakeId("Two"), three = h.ctx.MakeId("Three");
    CHECK_EQ(h.ctx.FocusedId(), 0u);
    h.KeyDown(Key::Tab);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), one);
    h.KeyDown(Key::Tab);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), two);
    h.KeyDown(Key::Tab);
    h.KeyDown(Key::Tab);   // two in one frame: three, then around to one
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), one);
    h.KeyDown(Key::Tab, kModShift);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), three);
    // A disabled widget is skipped.
    disabledMiddle = true;
    h.KeyDown(Key::Tab, kModShift);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), one);
    h.KeyDown(Key::Tab);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), three);
    // A widget that consumes Tab keeps focus (the editor inserts a tab).
    h.KeyDown(Key::Tab);
    h.Frame([&] {
        build();
        h.ctx.ConsumeKey(Key::Tab);
    });
    CHECK_EQ(h.ctx.FocusedId(), three);
    // A press on nothing clears focus.
    h.Click(500, 500);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), 0u);
}

RGUI_TEST("ui.disabled_ignores_input") {
    Harness h(false);
    bool value = false;
    bool clicked = false;
    auto disabledFrame = [&] {
        h.Frame([&] {
            h.ctx.BeginDisabled();
            clicked = Button(h.ctx, "Go", {10, 10, 80, 28}) || clicked;
            Checkbox(h.ctx, "Check", value, {10, 60, 200, 28});
            h.ctx.EndDisabled();
        });
    };
    h.Click(20, 20);
    disabledFrame();
    h.Click(20, 70);
    h.KeyDown(Key::Tab);
    disabledFrame();
    CHECK(!clicked);
    CHECK(!value);
    CHECK_EQ(h.ctx.FocusedId(), 0u);
    // Enabled, the same input works, and the opacity of disabled drawing is 40 %.
    auto enabledFrame = [&] {
        h.Frame([&] {
            clicked = Button(h.ctx, "Go", {10, 10, 80, 28}) || clicked;
            Checkbox(h.ctx, "Check", value, {10, 60, 200, 28});
            h.ctx.BeginDisabled();
            CHECK_NEAR(h.ctx.Opacity(), 0.4, 1e-6);
            h.ctx.EndDisabled();
            CHECK_NEAR(h.ctx.Opacity(), 1.0, 1e-6);
        });
    };
    h.Click(20, 20);
    enabledFrame();
    h.Click(20, 70);
    enabledFrame();
    CHECK(clicked);
    CHECK(value);
}

RGUI_TEST("ui.layout_and_state") {
    RectF r{0, 0, 100, 50};
    const RectF top = CutTop(r, 10);
    CHECK(top.h == 10 && r.y == 10 && r.h == 40);
    const RectF right = CutRight(r, 200);   // clamped to what is there
    CHECK(right.w == 100 && r.w == 0);
    const RectF in = Inset({0, 0, 10, 10}, 8);
    CHECK(in.w == 0 && in.h == 0);
    CHECK_EQ(std::string(VisibleLabel("Choose##theme")), std::string("Choose"));

    Harness h(false);
    h.scale = 1.25;
    h.Frame([&] {
        const gfx::RectI d = h.ctx.ToDevice({10.2f, 0.4f, 3.0f, 2.0f});
        CHECK(d == (gfx::RectI{13, 1, 16, 3}));
        CHECK_NEAR(h.ctx.SnapToDevice(1.0f), 0.8, 1e-6);
        CHECK(h.ctx.MakeId("a") != h.ctx.MakeId("b"));
        const Id outer = h.ctx.MakeId("a");
        h.ctx.PushId("row");
        CHECK(h.ctx.MakeId("a") != outer);
        h.ctx.PopId();
        CHECK_EQ(h.ctx.MakeId("a"), outer);
        h.ctx.State<int>(42) = 7;
    });
    // State lives while used, and goes after 120 unused frames.
    h.Frame([&] { CHECK_EQ(h.ctx.State<int>(42), 7); });
    for (int i = 0; i < 140; i++) h.Frame([] {});
    CHECK(!h.ctx.HasState(42));
}

// ---- Widgets ----

RGUI_TEST("ui.smooth_scroll") {
    CHECK_NEAR(SmoothScroll::Ease(0), 0, 1e-12);
    CHECK_NEAR(SmoothScroll::Ease(1), 1, 1e-12);
    CHECK_NEAR(SmoothScroll::Ease(0.5), 0.5, 1e-9);
    CHECK(SmoothScroll::Ease(0.1) < 0.1);   // ease in
    CHECK(SmoothScroll::Ease(0.9) > 0.9);   // ease out
    SmoothScroll s;
    s.SetMax(1000);
    s.AddNotches(1, 0);
    CHECK_EQ(s.Value(0, 1.0), 0.0f);
    CHECK_EQ(s.Value(100, 1.0), 60.0f);
    CHECK(s.Animating(199));
    CHECK_EQ(s.Value(200, 1.0), 120.0f);
    CHECK(!s.Animating(200));
    // Five pending notches take 100 ms.
    s.AddNotches(5, 200);
    CHECK(s.Animating(299));
    CHECK_EQ(s.Value(300, 1.0), 720.0f);
    // Clamped at the end, and snapped to device pixels at 1.5x.
    s.AddNotches(10, 300);
    CHECK_EQ(s.Target(), 1000.0f);
    for (double t = 300; t <= 500; t += 7) {
        const float v = s.Value(t, 1.5);
        const float device = v * 1.5f;
        CHECK_NEAR(device, static_cast<float>(static_cast<int>(device + 0.5f)), 1e-3);
        CHECK(v >= 720.0f && v <= 1000.0f);
    }
    s.AddPixels(-2000);
    CHECK_EQ(s.Value(600, 1.0), 0.0f);
    CHECK(!s.Animating(600));
    // EaseTo: the curve of one notch over 200 ms, to a target clamped at the end.
    SmoothScroll e;
    e.SetMax(1000);
    e.EaseTo(500, 0);
    CHECK_EQ(e.Value(100, 1.0), 250.0f);
    CHECK_EQ(e.Value(200, 1.0), 500.0f);
    CHECK(!e.Animating(200));
    e.EaseTo(2000, 300);
    CHECK_EQ(e.Target(), 1000.0f);
}

RGUI_TEST("ui.scroll_area") {
    Harness h(false);
    float offset = 0;
    auto build = [&] {
        offset = BeginScrollArea(h.ctx, "pane", {0, 0, 400, 300});
        EndScrollArea(h.ctx, 1000);
    };
    h.Frame(build);
    CHECK_EQ(offset, 0.0f);
    h.Wheel(100, 100, 1);
    const FrameOutput& out = h.Frame(build, 100);
    CHECK(out.wantsFrame);
    h.Frame(build, 100);
    CHECK(offset > 0 && offset < 120);
    h.Frame(build, 100);
    CHECK_EQ(offset, 120.0f);
    // Pixel deltas move at once; the end clamps to content - viewport.
    h.Wheel(100, 100, 0, 1000);
    h.Frame(build);
    h.Frame(build);
    CHECK_EQ(offset, 700.0f);
    // A wheel outside does nothing.
    h.Wheel(500, 100, -3);
    h.Frame(build);
    h.Frame(build, 300);
    CHECK_EQ(offset, 700.0f);
}

RGUI_TEST("ui.splitter") {
    Harness h(false);
    float ratio = 0.95f;
    bool changed = false;
    auto build = [&] { Splitter(h.ctx, "split", {0, 0, 800, 600}, ratio, &changed); };
    h.Frame(build);
    CHECK_EQ(ratio, 0.8f);
    // Drag the hairline at x = 640 to x = 50: clamped to 0.2.
    h.Move(640, 300);
    h.Button(640, 300, true);
    h.Frame(build);
    h.Move(50, 300);
    h.Frame(build);
    CHECK_NEAR(ratio, 0.2, 1e-6);
    CHECK(changed);
    h.Move(400, 300);
    h.Frame(build);
    CHECK_NEAR(ratio, 0.5, 1e-6);
    const FrameOutput& out = h.Frame(build);
    CHECK(out.cursor == Cursor::ResizeH);
    h.Button(400, 300, false);
    h.Move(700, 300);
    h.Frame(build);
    CHECK_NEAR(ratio, 0.5, 1e-6);
}

RGUI_TEST("ui.number_field") {
    Harness h(false);
    double value = 1.0;
    auto build = [&] { NumberField(h.ctx, "margin", value, 0.25, 2.0, 0.05, 2, " in", {10, 10, 120, 28}); };
    h.Frame(build);
    // Focus by click: the text is selected, typing replaces it; letters and a second '.' drop.
    h.Click(50, 20);
    h.Frame(build);
    h.Type("1a.5.7");
    h.KeyDown(Key::Enter);
    h.Frame(build);
    CHECK_NEAR(value, 1.57, 1e-9);
    // Out of range clamps.
    h.KeyDown(Key::A, kModCtrl);
    h.Type("9");
    h.KeyDown(Key::Enter);
    h.Frame(build);
    CHECK_NEAR(value, 2.0, 1e-9);
    // Up and Down step, clamped.
    h.KeyDown(Key::Up);
    h.Frame(build);
    CHECK_NEAR(value, 2.0, 1e-9);
    h.KeyDown(Key::Down);
    h.KeyDown(Key::Down);
    h.Frame(build);
    CHECK_NEAR(value, 1.9, 1e-9);
    // Unparsable text reverts on focus loss.
    h.KeyDown(Key::A, kModCtrl);
    h.KeyDown(Key::Backspace);
    h.Type(".");
    h.Frame(build);
    h.Click(500, 500);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId(), 0u);
    CHECK_NEAR(value, 1.9, 1e-9);
    // A valid edit commits on focus loss too; the wheel steps without focus.
    h.Click(50, 20);
    h.Frame(build);
    h.Type("0.1");
    h.Frame(build);
    h.Click(500, 500);
    const FrameOutput& lost = h.Frame(build);
    CHECK(lost.wantsFrame);   // the field commits in the frame that follows
    h.Frame(build);
    CHECK_NEAR(value, 0.25, 1e-9);
    h.Wheel(50, 20, -2);
    h.Frame(build);
    CHECK_NEAR(value, 0.35, 1e-9);
}

RGUI_TEST("ui.dropdown_keys") {
    Harness h(false);
    const std::vector<std::string> items = {"A4", "A3", "A5", "Letter", "Legal", "Custom"};
    int selected = 0;
    bool changed = false;
    auto build = [&] { changed = Dropdown(h.ctx, "page", selected, items, {10, 10, 200, 28}) || changed; };
    h.Frame(build);
    h.KeyDown(Key::Tab);
    h.Frame(build);
    h.KeyDown(Key::Enter);   // opens
    h.Frame(build);
    h.KeyDown(Key::Down);
    h.KeyDown(Key::Down);
    h.KeyDown(Key::Down);
    h.Frame(build);
    CHECK_EQ(selected, 0);
    CHECK(!changed);
    h.KeyDown(Key::Enter);   // picks Letter, closes
    h.Frame(build);
    CHECK_EQ(selected, 3);
    CHECK(changed);
    changed = false;
    h.KeyDown(Key::Enter);   // opens again
    h.Frame(build);
    h.KeyDown(Key::Up);
    h.KeyDown(Key::Escape);  // closes, no change
    h.Frame(build);
    CHECK_EQ(selected, 3);
    CHECK(!changed);
    // Closed: Up / Down step the choice, clamped.
    h.KeyDown(Key::Down);
    h.KeyDown(Key::Down);
    h.KeyDown(Key::Down);
    h.Frame(build);
    CHECK_EQ(selected, 5);
    // By pointer: open with a click, pick the second row (rows below the button).
    h.Click(50, 20);
    h.Frame(build);
    const float rowH = static_cast<float>(h.tokens.spacing.popupRowHeight);
    const float rowY = 10 + 28 + static_cast<float>(h.tokens.spacing.xxs) + h.tokens.popups.borderWidth + rowH * 1.5f;
    h.Click(50, rowY);
    h.Frame(build);
    h.Frame(build);
    CHECK_EQ(selected, 1);
    // A press outside an open popup closes it without reaching the widget below.
    bool behind = false;
    h.Click(50, 20);
    h.Frame(build);
    h.Click(500, 500);
    h.Frame([&] {
        behind = Button(h.ctx, "Behind", {400, 400, 200, 200});
        build();
    });
    h.Frame(build);
    CHECK(!behind);
    CHECK_EQ(selected, 1);
}

RGUI_TEST("ui.text_field") {
    Harness h(false);
    std::string text = "hello";
    auto build = [&] { TextField(h.ctx, "name", text, {10, 10, 300, 28}); };
    h.Frame(build);
    h.KeyDown(Key::Tab);   // focus selects all
    h.Frame(build);
    h.KeyDown(Key::End);
    h.Type(" world");
    h.Frame(build);
    CHECK_EQ(text, std::string("hello world"));
    h.KeyDown(Key::Backspace, kModCtrl);
    h.Frame(build);
    CHECK_EQ(text, std::string("hello "));
    h.KeyDown(Key::Z, kModCtrl);   // undo the last edit
    h.Frame(build);
    CHECK_EQ(text, std::string("hello world"));
    h.KeyDown(Key::Z, kModCtrl);   // and again: redo
    h.Frame(build);
    CHECK_EQ(text, std::string("hello "));
    // Select with Shift, copy, paste at the end; newlines from the clipboard are dropped.
    h.KeyDown(Key::Home);
    h.KeyDown(Key::Right, kModCtrl | kModShift);
    h.KeyDown(Key::C, kModCtrl);
    h.Frame(build);
    CHECK_EQ(h.clipboard, std::string("hello"));
    h.KeyDown(Key::End);
    h.KeyDown(Key::V, kModCtrl);
    h.Frame(build);
    h.Frame(build);
    CHECK_EQ(text, std::string("hello hello"));
    h.clipboard = "a\nb";
    h.KeyDown(Key::Insert, kModShift);
    h.Frame(build);
    h.Frame(build);
    CHECK_EQ(text, std::string("hello helloab"));
    // Multi-byte text moves by code point.
    text = "z\xC5\xBC";   // "zż"
    h.KeyDown(Key::End);
    h.KeyDown(Key::Backspace);
    h.Frame(build);
    CHECK_EQ(text, std::string("z"));
    // Read-only: no edits.
    std::string fixed = "theme.txt";
    h.KeyDown(Key::Tab);
    h.Frame([&] {
        build();
        TextField(h.ctx, "fixed", fixed, {10, 50, 300, 28}, {}, true);
    });
    h.Type("x");
    h.KeyDown(Key::Backspace);
    h.Frame([&] {
        build();
        TextField(h.ctx, "fixed", fixed, {10, 50, 300, 28}, {}, true);
    });
    CHECK_EQ(fixed, std::string("theme.txt"));
}

RGUI_TEST("ui.modal") {
    Harness h(false);
    const ModalButton buttons[] = {
        {"Save", true, Key::S, true, false},
        {"Discard", false, Key::D, false, false},
        {"Cancel", false, Key::C, false, true},
    };
    int result = -2;
    bool behind = false;
    auto build = [&] {
        behind = Button(h.ctx, "Behind", {0, 0, 800, 600}) || behind;
        result = MessageModal(h.ctx, "unsaved", "Unsaved changes", "Save changes to notes.md before closing?", buttons, 3);
    };
    h.Frame(build);
    CHECK_EQ(result, -1);
    h.KeyDown(Key::Escape);
    h.Frame(build);
    CHECK_EQ(result, 2);
    h.KeyDown(Key::D, kModAlt);
    h.Frame(build);
    CHECK_EQ(result, 1);
    h.KeyDown(Key::Enter);
    h.Frame(build);
    CHECK_EQ(result, 0);
    h.Click(5, 5);   // the scrim: nothing below gets it
    h.Frame(build);
    h.Frame(build);
    CHECK(!behind);
    CHECK_EQ(result, -1);
    // Tab stays among the modal's buttons.
    h.KeyDown(Key::Tab);
    h.Frame(build);
    CHECK_EQ(h.ctx.FocusedId() != 0, true);
    CHECK(h.ctx.FocusedId() != h.ctx.MakeId("Behind"));
}

RGUI_TEST("ui.tooltip_delay") {
    if (!TestFonts()) {
        std::printf("skip ui.tooltip_delay: no font\n");
        return;
    }
    Harness h;
    auto build = [&] { IconButton(h.ctx, "save", Icon::Save, {12, 560, 28, 28}, "Save (Ctrl+S)"); };
    auto tooltipShown = [](const FrameOutput& out) {
        for (const Command& c : out.displayList->Commands()) {
            if (c.type == CommandType::Glyphs) return true;
        }
        return false;
    };
    h.Move(26, 574);
    const FrameOutput& a = h.Frame(build, 100);
    CHECK(!tooltipShown(a));
    CHECK(a.nextDeadlineMs > 0);
    CHECK_EQ(a.cursor, Cursor::Hand);
    const double deadline = a.nextDeadlineMs;
    h.now = deadline;
    const FrameOutput& b = h.Frame(build);
    CHECK(tooltipShown(b));
    // Kept inside the window: its text starts at x >= 0 though the button sits at the edge.
    for (const Command& c : b.displayList->Commands()) {
        if (c.type == CommandType::Glyphs) CHECK(c.bounds.x0 >= 0 && c.bounds.y1 <= 560);
    }
}

RGUI_TEST("ui.panel_frame_timing") {
    if (!TestFonts()) {
        std::printf("skip ui.panel_frame_timing: no font\n");
        return;
    }
    Harness h;
    h.width = 1920;
    h.height = 1080;
    Framebuffer fb;
    fb.Resize(1920, 1080);
    text::GlyphCache glyphs;
    IconCache icons;
    Compositor compositor;
    const uint32_t bg = h.tokens.colors.windowBackground.Premultiplied();
    double t0 = NowMs();
    const FrameOutput& first = h.Frame([&] { BuildPanel(h.ctx, "Document", 40); });
    const double ui1 = NowMs() - t0;
    t0 = NowMs();
    compositor.Render(*first.displayList, bg, fb.Surface(), glyphs, icons);
    const double raster1 = NowMs() - t0;
    double uiSame = 0, rasterSame = 0;
    for (int i = 0; i < 10; i++) {
        t0 = NowMs();
        const FrameOutput& same = h.Frame([&] { BuildPanel(h.ctx, "Document", 40); });
        uiSame += NowMs() - t0;
        t0 = NowMs();
        const auto& damage = compositor.Render(*same.displayList, bg, fb.Surface(), glyphs, icons);
        rasterSame += NowMs() - t0;
        CHECK(damage.empty());
    }
    // A full-screen redraw with warm caches.
    double full = 0;
    for (int i = 0; i < 5; i++) {
        compositor.InvalidateAll();
        const FrameOutput& again = h.Frame([&] { BuildPanel(h.ctx, "Document", 40); });
        t0 = NowMs();
        compositor.Render(*again.displayList, bg, fb.Surface(), glyphs, icons);
        full += NowMs() - t0;
    }
    std::printf("ui panel 1920x1080, 40 widgets, %d tile threads: first frame ui %.2f ms + raster %.2f ms; "
                "unchanged frame ui %.3f ms + compose %.3f ms; full-screen raster %.2f ms\n",
                compositor.Threads(), ui1, raster1, uiSame / 10, rasterSame / 10, full / 5);
}
