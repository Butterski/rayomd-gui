// SPDX-License-Identifier: Apache-2.0
#include "pdf/renderer.h"

#include "common/exact_math.h"
#include "gfx/canvas.h"
#include "pdf/content.h"
#include "text/glyphcache.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace rgui::pdf {

void RenderStats::AddUnknown(std::string_view what) {
    for (const std::string& known : unknown) {
        if (known == what) return;
    }
    unknown.emplace_back(what);
}

namespace {

using gfx::Matrix;
using RayoMd::ExactMath::Floor;
using RayoMd::ExactMath::LRound;

constexpr size_t kMaxSaves = 64;
constexpr int kMaxFormDepth = 16;
// Operators one page may run, its forms' included: a page of any content ends in bounded time.
constexpr uint32_t kMaxOperators = 1u << 21;

bool Finite(double v) {
    return v == v && v - v == 0;
}

// A user-space coordinate as a float, kept in float's range.
float ToFloat(double v) {
    if (v > 1e30) return 1e30f;
    if (v < -1e30) return -1e30f;
    return static_cast<float>(v);
}

uint8_t ToByte(double v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    return static_cast<uint8_t>(LRound(v * 255.0));
}

enum class Space : uint8_t { Gray, Rgb, Cmyk, Unsupported };

int Components(Space space) {
    return space == Space::Gray ? 1 : space == Space::Rgb ? 3 : space == Space::Cmyk ? 4 : 0;
}

// Colour components of `space` as RGB, each 0..1.
void ToRgb(Space space, const double* c, double* rgb) {
    switch (space) {
        case Space::Gray: rgb[0] = rgb[1] = rgb[2] = c[0]; break;
        case Space::Rgb: rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2]; break;
        case Space::Cmyk:
            for (int i = 0; i < 3; i++) {
                const double ink = std::clamp(c[i], 0.0, 1.0), k = std::clamp(c[3], 0.0, 1.0);
                rgb[i] = (1 - ink) * (1 - k);
            }
            break;
        case Space::Unsupported: rgb[0] = rgb[1] = rgb[2] = 0; break;
    }
}

uint32_t ColorOf(const double* rgb, double alpha) {
    return gfx::Premultiply(ToByte(rgb[0]), ToByte(rgb[1]), ToByte(rgb[2]), ToByte(alpha));
}

struct GState {
    Matrix ctm;
    Space fillSpace = Space::Gray;
    Space strokeSpace = Space::Gray;
    double fill[3] = {0, 0, 0};   // RGB
    double stroke[3] = {0, 0, 0};
    bool fillPaintable = true;    // false: a colour we do not draw (a pattern)
    bool strokePaintable = true;
    double fillAlpha = 1;
    double strokeAlpha = 1;
    gfx::StrokeStyle line;
    size_t clips = 0;             // canvas clips in effect
    // Text state.
    const PdfFont* font = nullptr;
    double fontSize = 0;
    double charSpacing = 0;
    double wordSpacing = 0;
    double hscale = 1;
    double leading = 0;
    double rise = 0;
    int mode = 0;
};

// What each rendering thread keeps from page to page.
struct Scratch {
    text::GlyphCache glyphs;
    gfx::Canvas canvas;
    gfx::Path glyphPath;
    gfx::Path userPath;
    gfx::Path region;
};

Scratch& ThreadScratch() {
    thread_local Scratch scratch;
    return scratch;
}

class Interpreter {
    using RectI = gfx::RectI;

public:
    Interpreter(const PdfFile& file, const RenderEnvironment& env, RenderStats& stats, Scratch& scratch,
                const Matrix& base)
        : file_(file), env_(env), stats_(stats), s_(scratch), canvas_(scratch.canvas) {
        gs_.ctm = base;
    }

    void Run(std::string_view content, const PdfValue& resources, int depth) {
        ContentLexer lexer(content);
        std::string_view op;
        std::vector<PdfValue> operands;
        while (!stopped_ && lexer.Next(op, operands)) {
            if (++stats_.operators > kMaxOperators) {
                stats_.AddUnknown("operators");
                stopped_ = true;
                break;
            }
            Operator(op, operands, resources, depth);
        }
    }

private:
    // ---- operands ------------------------------------------------------------------------

    // Exactly `n` finite numbers.
    static bool Numbers(const std::vector<PdfValue>& ops, size_t n, double* out) {
        if (ops.size() != n) return false;
        for (size_t i = 0; i < n; i++) {
            if (!ops[i].IsNumber()) return false;
            out[i] = ops[i].Number();
            if (!Finite(out[i])) return false;
        }
        return true;
    }

    // The entry `name` of the resource category `category`, resolved, and its entry as written
    // (a reference: what caches hash); null when absent.
    const PdfValue& Resource(const PdfValue& resources, std::string_view category, std::string_view name,
                             const PdfValue** entry = nullptr) const {
        const PdfValue& dict = file_.Get(resources, category);
        const PdfValue* found = dict.IsDict() ? dict.Find(name) : nullptr;
        if (entry) *entry = found;
        return found ? file_.Resolve(*found) : file_.Get(dict, name);
    }

    // ---- graphics state ------------------------------------------------------------------

    void Save() {
        if (stack_.size() >= kMaxSaves) {
            ignoredSaves_++;
            return;
        }
        stack_.push_back(gs_);
    }

    void Restore() {
        if (ignoredSaves_ > 0) {
            ignoredSaves_--;
            return;
        }
        if (stack_.size() <= floor_) return;  // unbalanced, or below the form being run
        gs_ = stack_.back();
        stack_.pop_back();
        PopClipsTo(gs_.clips);
    }

    void PopClipsTo(size_t clips) {
        while (clipDepth_ > clips) {
            canvas_.PopClip();
            clipDepth_--;
        }
    }

    // Intersects the clip with `path` (user space) under the CTM.
    void Clip(const gfx::Path& path, gfx::FillRule rule) {
        RectI rect;
        if (PixelRect(path, rect)) {
            canvas_.PushClipRect(rect);
        } else {
            canvas_.PushClipPath(path, gs_.ctm, rule);
        }
        clipDepth_++;
        gs_.clips = clipDepth_;
    }

    // A rectangle path whose device corners fall on whole pixels: a rectangle clip, no mask.
    bool PixelRect(const gfx::Path& path, RectI& out) const {
        const auto& verbs = path.Verbs();
        const auto& pts = path.Points();
        if (verbs.size() != 5 || verbs[0] != gfx::Verb::MoveTo || verbs[4] != gfx::Verb::Close) return false;
        for (int i = 1; i < 4; i++) {
            if (verbs[static_cast<size_t>(i)] != gfx::Verb::LineTo) return false;
        }
        double x[4], y[4];
        for (int i = 0; i < 4; i++) {
            gs_.ctm.Apply(pts[static_cast<size_t>(i)].x, pts[static_cast<size_t>(i)].y, x[i], y[i]);
            if (!(x[i] > -1e7 && x[i] < 1e7 && y[i] > -1e7 && y[i] < 1e7)) return false;
            if (x[i] != Floor(x[i]) || y[i] != Floor(y[i])) return false;
        }
        // Axis-aligned: consecutive corners share an x or a y, alternately.
        const bool startsHorizontal = y[0] == y[1];
        for (int i = 0; i < 4; i++) {
            const int j = (i + 1) & 3;
            const bool horizontal = ((i & 1) == 0) == startsHorizontal;
            if (horizontal ? y[i] != y[j] : x[i] != x[j]) return false;
        }
        out = {static_cast<int>(std::min({x[0], x[1], x[2], x[3]})), static_cast<int>(std::min({y[0], y[1], y[2], y[3]})),
               static_cast<int>(std::max({x[0], x[1], x[2], x[3]})), static_cast<int>(std::max({y[0], y[1], y[2], y[3]}))};
        return true;
    }

    gfx::Paint FillPaint() const { return gfx::Paint::Solid(ColorOf(gs_.fill, gs_.fillAlpha)); }
    gfx::Paint StrokePaint() const { return gfx::Paint::Solid(ColorOf(gs_.stroke, gs_.strokeAlpha)); }
    bool CanFill() const { return gs_.fillPaintable && gs_.fillAlpha > 0; }
    bool CanStroke() const { return gs_.strokePaintable && gs_.strokeAlpha > 0; }

    void SetDash(const PdfValue& array, double phase) {
        gs_.line.dash.clear();
        const auto& items = file_.Resolve(array).Array();
        if (items.size() > 64) return;  // solid
        for (const PdfValue& item : items) {
            double v = 0;
            if (!file_.Number(item, v) || !Finite(v)) {
                gs_.line.dash.clear();
                return;
            }
            gs_.line.dash.push_back(ToFloat(v));
        }
        gs_.line.dashPhase = Finite(phase) ? ToFloat(phase) : 0;
    }

    void SetLineWidth(double w) { gs_.line.width = ToFloat(w < 0 ? -w : w); }
    void SetCap(int64_t v) { gs_.line.cap = v == 1 ? gfx::LineCap::Round : v == 2 ? gfx::LineCap::Square : gfx::LineCap::Butt; }
    void SetJoin(int64_t v) {
        gs_.line.join = v == 1 ? gfx::LineJoin::Round : v == 2 ? gfx::LineJoin::Bevel : gfx::LineJoin::Miter;
    }
    void SetMiter(double v) { gs_.line.miterLimit = ToFloat(v < 1 ? 1 : v); }

    void ExtGState(const PdfValue& dict) {
        if (!dict.IsDict()) {
            stats_.AddUnknown("gs:missing");
            return;
        }
        double v = 0;
        if (file_.Number(file_.Get(dict, "ca"), v) && Finite(v)) gs_.fillAlpha = std::clamp(v, 0.0, 1.0);
        if (file_.Number(file_.Get(dict, "CA"), v) && Finite(v)) gs_.strokeAlpha = std::clamp(v, 0.0, 1.0);
        if (file_.Number(file_.Get(dict, "LW"), v) && Finite(v)) SetLineWidth(v);
        if (file_.Number(file_.Get(dict, "ML"), v) && Finite(v)) SetMiter(v);
        const PdfValue& cap = file_.Get(dict, "LC");
        if (cap.IsNumber()) SetCap(cap.Int());
        const PdfValue& join = file_.Get(dict, "LJ");
        if (join.IsNumber()) SetJoin(join.Int());
        const PdfValue& dash = file_.Get(dict, "D");
        if (dash.IsArray() && dash.items.size() == 2) {
            double phase = 0;
            file_.Number(dash.items[1], phase);
            SetDash(dash.items[0], phase);
        }
        const PdfValue& mask = file_.Get(dict, "SMask");
        if (!mask.IsNull() && !mask.IsName("None")) stats_.AddUnknown("gs:SMask");
        const PdfValue& bm = file_.Get(dict, "BM");
        const PdfValue& mode = bm.IsArray() && !bm.items.empty() ? file_.Resolve(bm.items[0]) : bm;
        if (!mode.IsNull() && !mode.IsName("Normal") && !mode.IsName("Compatible")) stats_.AddUnknown("gs:BM");
    }

    // ---- colour --------------------------------------------------------------------------

    Space SpaceOf(const PdfValue& name, const PdfValue& resources) {
        const std::string_view n = name.Name();
        if (n == "DeviceGray" || n == "G") return Space::Gray;
        if (n == "DeviceRGB" || n == "RGB") return Space::Rgb;
        if (n == "DeviceCMYK" || n == "CMYK") return Space::Cmyk;
        const PdfValue& cs = n.empty() ? name : Resource(resources, "ColorSpace", n);
        std::string_view family = cs.Name();
        if (cs.IsArray() && !cs.items.empty()) family = file_.Resolve(cs.items[0]).Name();
        if (family == "DeviceGray" || family == "CalGray") return Space::Gray;
        if (family == "DeviceRGB" || family == "CalRGB") return Space::Rgb;
        if (family == "DeviceCMYK") return Space::Cmyk;
        if (family == "ICCBased" && cs.items.size() >= 2) {
            const int64_t comps = file_.Get(cs.items[1], "N").Int(0);
            if (comps == 1) return Space::Gray;
            if (comps == 3) return Space::Rgb;
            if (comps == 4) return Space::Cmyk;
        }
        stats_.AddUnknown("colorspace:" + std::string(family.empty() ? (n.empty() ? "?" : n) : family));
        return Space::Unsupported;
    }

    void SetColorSpace(bool stroke, const PdfValue& name, const PdfValue& resources) {
        const Space space = SpaceOf(name, resources);
        const double black[4] = {0, 0, 0, 1};
        double* rgb = stroke ? gs_.stroke : gs_.fill;
        ToRgb(space, black, rgb);
        (stroke ? gs_.strokeSpace : gs_.fillSpace) = space;
        (stroke ? gs_.strokePaintable : gs_.fillPaintable) = space != Space::Unsupported;
    }

    void SetColor(bool stroke, Space space, const double* c) {
        ToRgb(space, c, stroke ? gs_.stroke : gs_.fill);
        (stroke ? gs_.strokeSpace : gs_.fillSpace) = space;
        (stroke ? gs_.strokePaintable : gs_.fillPaintable) = true;
    }

    // sc, scn, SC, SCN: components of the current space; a pattern name is not drawn.
    void SetColorComponents(bool stroke, const std::vector<PdfValue>& ops) {
        const Space space = stroke ? gs_.strokeSpace : gs_.fillSpace;
        if (!ops.empty() && ops.back().IsName()) {
            stats_.AddUnknown("pattern");
            (stroke ? gs_.strokePaintable : gs_.fillPaintable) = false;
            return;
        }
        const int n = Components(space);
        double c[4];
        if (n == 0 || !Numbers(ops, static_cast<size_t>(n), c)) return;
        SetColor(stroke, space, c);
    }

    // ---- paths ---------------------------------------------------------------------------

    void Paint(bool close, bool fill, gfx::FillRule rule, bool stroke) {
        if (close) path_.Close();
        if (fill && CanFill()) canvas_.FillPath(path_, gs_.ctm, rule, FillPaint());
        if (stroke && CanStroke()) canvas_.StrokePath(path_, gs_.ctm, gs_.line, StrokePaint());
        if (pendingClip_) Clip(path_, clipRule_);
        pendingClip_ = false;
        path_.Clear();
        hasCurrent_ = false;
    }

    void PathOperator(char op, const std::vector<PdfValue>& ops) {
        double v[6];
        switch (op) {
            case 'm':
                if (!Numbers(ops, 2, v)) return;
                path_.MoveTo(ToFloat(v[0]), ToFloat(v[1]));
                SetCurrent(v[0], v[1], true);
                return;
            case 'l':
                if (!Numbers(ops, 2, v) || !hasCurrent_) return;
                path_.LineTo(ToFloat(v[0]), ToFloat(v[1]));
                SetCurrent(v[0], v[1], false);
                return;
            case 'c':
                if (!Numbers(ops, 6, v) || !hasCurrent_) return;
                path_.CubicTo(ToFloat(v[0]), ToFloat(v[1]), ToFloat(v[2]), ToFloat(v[3]), ToFloat(v[4]), ToFloat(v[5]));
                SetCurrent(v[4], v[5], false);
                return;
            case 'v':
                if (!Numbers(ops, 4, v) || !hasCurrent_) return;
                path_.CubicTo(ToFloat(curX_), ToFloat(curY_), ToFloat(v[0]), ToFloat(v[1]), ToFloat(v[2]), ToFloat(v[3]));
                SetCurrent(v[2], v[3], false);
                return;
            case 'y':
                if (!Numbers(ops, 4, v) || !hasCurrent_) return;
                path_.CubicTo(ToFloat(v[0]), ToFloat(v[1]), ToFloat(v[2]), ToFloat(v[3]), ToFloat(v[2]), ToFloat(v[3]));
                SetCurrent(v[2], v[3], false);
                return;
            case 'h':
                if (!ops.empty() || !hasCurrent_) return;
                path_.Close();
                curX_ = startX_;
                curY_ = startY_;
                return;
            case 'r':  // re
                if (!Numbers(ops, 4, v)) return;
                path_.AddRect(ToFloat(v[0]), ToFloat(v[1]), ToFloat(v[2]), ToFloat(v[3]));
                SetCurrent(v[0], v[1], true);
                return;
            default: return;
        }
    }

    void SetCurrent(double x, double y, bool start) {
        curX_ = x;
        curY_ = y;
        if (start) {
            startX_ = x;
            startY_ = y;
        }
        hasCurrent_ = true;
    }

    // ---- text ----------------------------------------------------------------------------

    const PdfFont* FontOf(const PdfValue& resources, std::string_view name) {
        const PdfValue* entry = nullptr;
        const PdfValue& dict = Resource(resources, "Font", name, &entry);
        if (!entry || !dict.IsDict()) {
            stats_.AddUnknown("font:missing");
            return nullptr;
        }
        auto found = fonts_.find(&dict);
        if (found == fonts_.end()) {
            std::shared_ptr<const PdfFont> font = env_.fonts ? env_.fonts->Load(file_, *entry, env_.fontIndex)
                                                             : LoadFont(file_, dict, env_.fontIndex);
            found = fonts_.emplace(&dict, std::move(font)).first;
        }
        return found->second.get();
    }

    static bool Upright(const Matrix& m) {
        if (!(m.a > 0 && m.d < 0 && Finite(m.a) && Finite(m.d) && Finite(m.e) && Finite(m.f))) return false;
        const double eps = 1e-6 * m.a;
        return (m.b < 0 ? -m.b : m.b) <= eps && (m.c < 0 ? -m.c : m.c) <= eps && (m.a + m.d < 0 ? -(m.a + m.d) : m.a + m.d) <= eps;
    }

    void DrawGlyph(const text::GlyphSource& source, uint16_t glyph, const Matrix& fontToText, bool fill, bool stroke) {
        const Matrix trm = fontToText.Multiply(tm_).Multiply(gs_.ctm);
        stats_.glyphs++;
        if (fill && !stroke && Upright(trm) && trm.a <= text::GlyphCache::kMaxSize && trm.e > -1e8 && trm.e < 1e8 &&
            trm.f > -1e8 && trm.f < 1e8) {
            const text::GlyphKey key = text::GlyphCache::KeyFor(source, glyph, static_cast<float>(trm.a), static_cast<float>(trm.e));
            const text::GlyphBitmap* bitmap = s_.glyphs.Get(source, key);
            if (bitmap && bitmap->width > 0 && bitmap->height > 0) {
                const int x0 = static_cast<int>(Floor(trm.e)) + bitmap->left;
                const int y0 = static_cast<int>(LRound(trm.f)) + bitmap->top;
                canvas_.DrawMask(bitmap->alpha.data(), bitmap->width, {x0, y0, x0 + bitmap->width, y0 + bitmap->height},
                                 ColorOf(gs_.fill, gs_.fillAlpha));
            }
            return;
        }
        stats_.glyphOutlines++;
        if (!source.GlyphPath(glyph, s_.glyphPath) || s_.glyphPath.Empty()) return;
        const Matrix toUser = source.UnitsToEm(glyph).Multiply(fontToText).Multiply(tm_);
        if (fill) canvas_.FillPath(s_.glyphPath, toUser.Multiply(gs_.ctm), gfx::FillRule::NonZero, FillPaint());
        if (stroke) {
            s_.userPath = s_.glyphPath.Transformed(toUser);
            canvas_.StrokePath(s_.userPath, gs_.ctm, gs_.line, StrokePaint());
        }
    }

    void ShowText(std::string_view bytes) {
        const PdfFont* font = gs_.font;
        if (!font) return;
        if (!font->Unsupported().empty()) {
            stats_.AddUnknown(font->Unsupported());
            return;
        }
        const text::GlyphSource* source = font->Source();
        const double size = gs_.fontSize, th = gs_.hscale;
        const Matrix fontToText{size * th, 0, 0, size, 0, gs_.rise};
        const int mode = gs_.mode & 3;
        const bool fill = (mode == 0 || mode == 2) && CanFill();
        const bool stroke = (mode == 1 || mode == 2) && CanStroke();
        size_t at = 0;
        PdfGlyph g;
        while (font->Next(bytes, at, g)) {
            if ((fill || stroke) && source) DrawGlyph(*source, g.glyph, fontToText, fill, stroke);
            const double tx = (g.width / 1000.0 * size + gs_.charSpacing + (g.wordSpace ? gs_.wordSpacing : 0)) * th;
            tm_ = Matrix::Translate(tx, 0).Multiply(tm_);
        }
    }

    void MoveLine(double tx, double ty) {
        tlm_ = Matrix::Translate(tx, ty).Multiply(tlm_);
        tm_ = tlm_;
    }

    void TextOperator(std::string_view op, const std::vector<PdfValue>& ops, const PdfValue& resources) {
        double v[6];
        if (op == "BT") {
            tm_ = tlm_ = Matrix();
        } else if (op == "ET") {
        } else if (op == "Tc") {
            if (Numbers(ops, 1, v)) gs_.charSpacing = v[0];
        } else if (op == "Tw") {
            if (Numbers(ops, 1, v)) gs_.wordSpacing = v[0];
        } else if (op == "Tz") {
            if (Numbers(ops, 1, v)) gs_.hscale = v[0] / 100.0;
        } else if (op == "TL") {
            if (Numbers(ops, 1, v)) gs_.leading = v[0];
        } else if (op == "Ts") {
            if (Numbers(ops, 1, v)) gs_.rise = v[0];
        } else if (op == "Tr") {
            if (Numbers(ops, 1, v) && v[0] >= 0 && v[0] <= 7) gs_.mode = static_cast<int>(v[0]);
        } else if (op == "Tf") {
            if (ops.size() == 2 && ops[0].IsName() && ops[1].IsNumber() && Finite(ops[1].Number())) {
                gs_.font = FontOf(resources, ops[0].Name());
                gs_.fontSize = ops[1].Number();
            }
        } else if (op == "Td") {
            if (Numbers(ops, 2, v)) MoveLine(v[0], v[1]);
        } else if (op == "TD") {
            if (Numbers(ops, 2, v)) {
                gs_.leading = -v[1];
                MoveLine(v[0], v[1]);
            }
        } else if (op == "Tm") {
            if (Numbers(ops, 6, v)) tm_ = tlm_ = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
        } else if (op == "T*") {
            if (ops.empty()) MoveLine(0, -gs_.leading);
        } else if (op == "Tj") {
            if (ops.size() == 1 && ops[0].IsString()) ShowText(ops[0].String());
        } else if (op == "TJ") {
            if (ops.size() != 1 || !ops[0].IsArray()) return;
            for (const PdfValue& item : ops[0].items) {
                if (item.IsString()) {
                    ShowText(item.String());
                } else if (item.IsNumber() && Finite(item.Number())) {
                    const double tx = -item.Number() / 1000.0 * gs_.fontSize * gs_.hscale;
                    tm_ = Matrix::Translate(tx, 0).Multiply(tm_);
                }
            }
        } else if (op == "'") {
            if (ops.size() != 1 || !ops[0].IsString()) return;
            MoveLine(0, -gs_.leading);
            ShowText(ops[0].String());
        } else if (op == "\"") {
            if (ops.size() != 3 || !ops[0].IsNumber() || !ops[1].IsNumber() || !ops[2].IsString()) return;
            if (!Finite(ops[0].Number()) || !Finite(ops[1].Number())) return;
            gs_.wordSpacing = ops[0].Number();
            gs_.charSpacing = ops[1].Number();
            MoveLine(0, -gs_.leading);
            ShowText(ops[2].String());
        }
    }

    // ---- XObjects ------------------------------------------------------------------------

    void DrawImage(const PdfValue& image, const PdfValue& entry) {
        auto found = images_.find(&image);
        if (found == images_.end()) {
            std::string reason;
            std::shared_ptr<const gfx::Image> decoded;
            if (env_.images) {
                decoded = env_.images->Load(file_, entry, &reason);
            } else {
                auto own = std::make_shared<gfx::Image>();
                if (DecodeImage(file_, image, *own, &reason)) decoded = std::move(own);
            }
            if (!decoded) stats_.AddUnknown("image:" + (reason.empty() ? std::string("unreadable") : reason));
            found = images_.emplace(&image, std::move(decoded)).first;
        }
        if (!found->second || !(gs_.fillAlpha > 0)) return;
        // Image space: row 0 at the top of the unit square, (0, 1) in user space.
        const Matrix flip{1, 0, 0, -1, 0, 1};
        canvas_.DrawImage(*found->second, flip.Multiply(gs_.ctm), ToByte(gs_.fillAlpha), true);
        stats_.images++;
    }

    void DrawForm(const PdfValue& form, const PdfValue& resources, int depth) {
        if (depth >= kMaxFormDepth) {
            stats_.AddUnknown("form-depth");
            return;
        }
        std::string_view content;
        std::string error;
        if (!file_.StreamData(form, content, &error)) {
            stats_.AddUnknown(error.empty() ? "contents" : error);
            return;
        }
        // The form runs in a state of its own that it cannot restore past.
        stack_.push_back(gs_);
        const size_t floor = floor_;
        const size_t ignored = ignoredSaves_;
        floor_ = stack_.size();
        ignoredSaves_ = 0;
        const Matrix tm = tm_, tlm = tlm_;
        double m[6] = {1, 0, 0, 1, 0, 0};
        const auto& matrix = file_.Get(form, "Matrix").Array();
        if (matrix.size() == 6) {
            double read[6];
            bool ok = true;
            for (size_t i = 0; i < 6; i++) ok = ok && file_.Number(matrix[i], read[i]) && Finite(read[i]);
            if (ok) std::copy(read, read + 6, m);
        }
        gs_.ctm = Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}.Multiply(gs_.ctm);
        PdfRect box;
        if (file_.Rect(file_.Get(form, "BBox"), box)) {
            gfx::Path clip;
            clip.AddRect(ToFloat(box.x0), ToFloat(box.y0), ToFloat(box.Width()), ToFloat(box.Height()));
            Clip(clip, gfx::FillRule::NonZero);
        }
        const PdfValue& own = file_.Get(form, "Resources");
        gfx::Path path;
        std::swap(path, path_);
        const bool pendingClip = pendingClip_;
        pendingClip_ = false;
        Run(content, own.IsDict() ? own : resources, depth + 1);
        std::swap(path, path_);
        pendingClip_ = pendingClip;
        // Back to the state before the form: past every save the form left open, then its own.
        while (stack_.size() >= floor_) {
            gs_ = stack_.back();
            stack_.pop_back();
        }
        floor_ = floor;
        ignoredSaves_ = ignored;
        PopClipsTo(gs_.clips);
        tm_ = tm;
        tlm_ = tlm;
    }

    void Do(std::string_view name, const PdfValue& resources, int depth) {
        const PdfValue* entry = nullptr;
        const PdfValue& object = Resource(resources, "XObject", name, &entry);
        const std::string_view subtype = file_.Get(object, "Subtype").Name();
        if (!entry || !object.IsStream()) {
            stats_.AddUnknown("xobject:missing");
        } else if (subtype == "Image") {
            DrawImage(object, *entry);
        } else if (subtype == "Form") {
            DrawForm(object, resources, depth);
        } else {
            stats_.AddUnknown("xobject:" + std::string(subtype.empty() ? "?" : subtype));
        }
    }

    // ---- shadings ------------------------------------------------------------------------

    struct Stop {
        double x;        // function input
        double rgb[3];
    };

    // A type-2 function at input x, as RGB.
    bool Exponential(const PdfValue& fn, double x, Space space, double* rgb) {
        double n = 1;
        file_.Number(file_.Get(fn, "N"), n);
        if (n != 1) stats_.AddUnknown("shading-exponent");
        const int comps = Components(space);
        double c0[4] = {0, 0, 0, 0}, c1[4] = {1, 1, 1, 1};
        const auto& a0 = file_.Get(fn, "C0").Array();
        const auto& a1 = file_.Get(fn, "C1").Array();
        if ((!a0.empty() && a0.size() != static_cast<size_t>(comps)) || (!a1.empty() && a1.size() != static_cast<size_t>(comps))) {
            stats_.AddUnknown("shading:function");
            return false;
        }
        for (size_t i = 0; i < a0.size(); i++) file_.Number(a0[i], c0[i]);
        for (size_t i = 0; i < a1.size(); i++) file_.Number(a1[i], c1[i]);
        double d0 = 0, d1 = 1;
        const auto& domain = file_.Get(fn, "Domain").Array();
        if (domain.size() == 2) {
            file_.Number(domain[0], d0);
            file_.Number(domain[1], d1);
        }
        x = std::clamp(x, std::min(d0, d1), std::max(d0, d1));
        double c[4];
        for (int i = 0; i < comps; i++) c[i] = c0[i] + x * (c1[i] - c0[i]);
        ToRgb(space, c, rgb);
        return true;
    }

    // Stops of `fn` (type 2, or type 3 of type-2 functions) at function inputs.
    bool FunctionStops(const PdfValue& fn, double t0, double t1, Space space, std::vector<Stop>& stops) {
        const int64_t type = file_.Get(fn, "FunctionType").Int(-1);
        Stop a{}, b{};
        if (type == 2) {
            a.x = t0;
            b.x = t1;
            if (!Exponential(fn, t0, space, a.rgb) || !Exponential(fn, t1, space, b.rgb)) return false;
            stops.push_back(a);
            stops.push_back(b);
            return true;
        }
        if (type != 3) {
            stats_.AddUnknown("shading:function");
            return false;
        }
        const auto& functions = file_.Get(fn, "Functions").Array();
        const auto& bounds = file_.Get(fn, "Bounds").Array();
        const auto& encode = file_.Get(fn, "Encode").Array();
        const auto& domain = file_.Get(fn, "Domain").Array();
        const size_t k = functions.size();
        double d0 = 0, d1 = 1;
        if (k == 0 || k > 256 || bounds.size() != k - 1 || encode.size() != 2 * k || domain.size() != 2 ||
            !file_.Number(domain[0], d0) || !file_.Number(domain[1], d1) || !(d1 > d0)) {
            stats_.AddUnknown("shading:function");
            return false;
        }
        for (size_t i = 0; i < k; i++) {
            double lo = d0, hi = d1, e0 = 0, e1 = 1;
            if (i > 0) file_.Number(bounds[i - 1], lo);
            if (i + 1 < k) file_.Number(bounds[i], hi);
            file_.Number(encode[2 * i], e0);
            file_.Number(encode[2 * i + 1], e1);
            const PdfValue& sub = file_.Resolve(functions[i]);
            if (file_.Get(sub, "FunctionType").Int(-1) != 2) {
                stats_.AddUnknown("shading:function");
                return false;
            }
            a.x = lo;
            b.x = hi;
            if (!Exponential(sub, e0, space, a.rgb) || !Exponential(sub, e1, space, b.rgb)) return false;
            stops.push_back(a);
            stops.push_back(b);
        }
        return true;
    }

    void Shade(const PdfValue& shading, const PdfValue& resources) {
        if (!shading.IsDict()) {
            stats_.AddUnknown("shading:missing");
            return;
        }
        const int64_t type = file_.Get(shading, "ShadingType").Int(0);
        if (type != 2) {
            stats_.AddUnknown("shading:" + std::to_string(type));
            return;
        }
        if (!CanFill()) return;
        const auto& coords = file_.Get(shading, "Coords").Array();
        double c[4];
        if (coords.size() != 4) {
            stats_.AddUnknown("shading:coords");
            return;
        }
        for (int i = 0; i < 4; i++) {
            if (!file_.Number(coords[static_cast<size_t>(i)], c[i]) || !Finite(c[i])) {
                stats_.AddUnknown("shading:coords");
                return;
            }
        }
        const Space space = SpaceOf(file_.Get(shading, "ColorSpace"), resources);
        if (space == Space::Unsupported) return;
        double t0 = 0, t1 = 1;
        const auto& domain = file_.Get(shading, "Domain").Array();
        if (domain.size() == 2) {
            file_.Number(domain[0], t0);
            file_.Number(domain[1], t1);
        }
        if (!(t1 != t0) || !Finite(t0) || !Finite(t1)) return;
        const PdfValue& fn = file_.Get(shading, "Function");
        if (!fn.IsDict()) {
            stats_.AddUnknown("shading:function");
            return;
        }
        std::vector<Stop> stops;
        if (!FunctionStops(fn, t0, t1, space, stops)) return;
        auto gradient = std::make_shared<gfx::LinearGradient>();
        gradient->p0 = {ToFloat(c[0]), ToFloat(c[1])};
        gradient->p1 = {ToFloat(c[2]), ToFloat(c[3])};
        const auto& extend = file_.Get(shading, "Extend").Array();
        gradient->extendStart = extend.size() == 2 && file_.Resolve(extend[0]).Bool(false);
        gradient->extendEnd = extend.size() == 2 && file_.Resolve(extend[1]).Bool(false);
        gradient->alpha = ToByte(gs_.fillAlpha);
        float last = 0;
        for (const Stop& stop : stops) {
            double offset = (stop.x - t0) / (t1 - t0);
            if (!Finite(offset)) offset = 0;
            const float o = std::max(last, static_cast<float>(std::clamp(offset, 0.0, 1.0)));
            last = o;
            gradient->stops.push_back({o, ToByte(stop.rgb[0]), ToByte(stop.rgb[1]), ToByte(stop.rgb[2])});
        }
        // The region: the clip's bounds, brought back to user space.
        Matrix inverse;
        if (!gs_.ctm.Invert(inverse)) return;
        const RectI clip = canvas_.ClipBounds();
        if (clip.Empty()) return;
        const double xs[4] = {static_cast<double>(clip.x0), static_cast<double>(clip.x1), static_cast<double>(clip.x1),
                              static_cast<double>(clip.x0)};
        const double ys[4] = {static_cast<double>(clip.y0), static_cast<double>(clip.y0), static_cast<double>(clip.y1),
                              static_cast<double>(clip.y1)};
        gfx::Path& region = s_.region;
        region.Clear();
        for (int i = 0; i < 4; i++) {
            double x = 0, y = 0;
            inverse.Apply(xs[i], ys[i], x, y);
            if (i == 0) {
                region.MoveTo(ToFloat(x), ToFloat(y));
            } else {
                region.LineTo(ToFloat(x), ToFloat(y));
            }
        }
        region.Close();
        canvas_.FillPath(region, gs_.ctm, gfx::FillRule::NonZero, gfx::Paint::Gradient(std::move(gradient)));
    }

    // ---- dispatch ------------------------------------------------------------------------

    void Operator(std::string_view op, const std::vector<PdfValue>& ops, const PdfValue& resources, int depth) {
        double v[6];
        if (op.empty()) return;
        switch (op[0]) {
            case 'q':
                if (op == "q") return Save();
                break;
            case 'Q':
                if (op == "Q") return Restore();
                break;
            case 'c':
                if (op == "cm") {
                    if (Numbers(ops, 6, v)) gs_.ctm = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]}.Multiply(gs_.ctm);
                    return;
                }
                if (op == "c") return PathOperator('c', ops);
                if (op == "cs") {
                    if (ops.size() == 1 && ops[0].IsName()) SetColorSpace(false, ops[0], resources);
                    return;
                }
                break;
            case 'w':
                if (op == "w") {
                    if (Numbers(ops, 1, v)) SetLineWidth(v[0]);
                    return;
                }
                break;
            case 'J':
                if (op == "J") {
                    if (Numbers(ops, 1, v)) SetCap(static_cast<int64_t>(v[0]));
                    return;
                }
                break;
            case 'j':
                if (op == "j") {
                    if (Numbers(ops, 1, v)) SetJoin(static_cast<int64_t>(v[0]));
                    return;
                }
                break;
            case 'M':
                if (op == "M") {
                    if (Numbers(ops, 1, v)) SetMiter(v[0]);
                    return;
                }
                if (op == "MP") return;
                break;
            case 'd':
                if (op == "d") {
                    if (ops.size() == 2 && ops[0].IsArray() && ops[1].IsNumber()) SetDash(ops[0], ops[1].Number());
                    return;
                }
                break;
            case 'r':
                if (op == "ri") return;
                if (op == "re") return PathOperator('r', ops);
                if (op == "rg") {
                    if (Numbers(ops, 3, v)) SetColor(false, Space::Rgb, v);
                    return;
                }
                break;
            case 'R':
                if (op == "RG") {
                    if (Numbers(ops, 3, v)) SetColor(true, Space::Rgb, v);
                    return;
                }
                break;
            case 'i':
                if (op == "i") return;
                break;
            case 'g':
                if (op == "g") {
                    if (Numbers(ops, 1, v)) SetColor(false, Space::Gray, v);
                    return;
                }
                if (op == "gs") {
                    if (ops.size() == 1 && ops[0].IsName()) ExtGState(Resource(resources, "ExtGState", ops[0].Name()));
                    return;
                }
                break;
            case 'G':
                if (op == "G") {
                    if (Numbers(ops, 1, v)) SetColor(true, Space::Gray, v);
                    return;
                }
                break;
            case 'k':
                if (op == "k") {
                    if (Numbers(ops, 4, v)) SetColor(false, Space::Cmyk, v);
                    return;
                }
                break;
            case 'K':
                if (op == "K") {
                    if (Numbers(ops, 4, v)) SetColor(true, Space::Cmyk, v);
                    return;
                }
                break;
            case 'C':
                if (op == "CS") {
                    if (ops.size() == 1 && ops[0].IsName()) SetColorSpace(true, ops[0], resources);
                    return;
                }
                break;
            case 's':
                if (op == "sc" || op == "scn") return SetColorComponents(false, ops);
                if (op == "s") return Paint(true, false, gfx::FillRule::NonZero, true);
                if (op == "sh") {
                    if (ops.size() == 1 && ops[0].IsName()) Shade(Resource(resources, "Shading", ops[0].Name()), resources);
                    return;
                }
                break;
            case 'S':
                if (op == "SC" || op == "SCN") return SetColorComponents(true, ops);
                if (op == "S") return Paint(false, false, gfx::FillRule::NonZero, true);
                break;
            case 'm':
            case 'l':
            case 'v':
            case 'y':
            case 'h':
                if (op.size() == 1) return PathOperator(op[0], ops);
                break;
            case 'f':
                if (op == "f") return Paint(false, true, gfx::FillRule::NonZero, false);
                if (op == "f*") return Paint(false, true, gfx::FillRule::EvenOdd, false);
                break;
            case 'F':
                if (op == "F") return Paint(false, true, gfx::FillRule::NonZero, false);
                break;
            case 'B':
                if (op == "B") return Paint(false, true, gfx::FillRule::NonZero, true);
                if (op == "B*") return Paint(false, true, gfx::FillRule::EvenOdd, true);
                if (op == "BT") return TextOperator(op, ops, resources);
                if (op == "BDC" || op == "BMC" || op == "BX" || op == "BI") return;
                break;
            case 'b':
                if (op == "b") return Paint(true, true, gfx::FillRule::NonZero, true);
                if (op == "b*") return Paint(true, true, gfx::FillRule::EvenOdd, true);
                break;
            case 'n':
                if (op == "n") return Paint(false, false, gfx::FillRule::NonZero, false);
                break;
            case 'W':
                if (op == "W" || op == "W*") {
                    pendingClip_ = true;
                    clipRule_ = op == "W" ? gfx::FillRule::NonZero : gfx::FillRule::EvenOdd;
                    return;
                }
                break;
            case 'E':
                if (op == "ET") return TextOperator(op, ops, resources);
                if (op == "EMC" || op == "EX") return;
                break;
            case 'D':
                if (op == "Do") {
                    if (ops.size() == 1 && ops[0].IsName()) Do(ops[0].Name(), resources, depth);
                    return;
                }
                if (op == "DP") return;
                break;
            case 'T':
            case '\'':
            case '"':
                if (op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" || op == "Tf" || op == "Tr" || op == "Ts" ||
                    op == "Td" || op == "TD" || op == "Tm" || op == "T*" || op == "Tj" || op == "TJ" || op == "'" ||
                    op == "\"") {
                    return TextOperator(op, ops, resources);
                }
                break;
            default: break;
        }
        stats_.AddUnknown("op:" + std::string(op));
    }

    const PdfFile& file_;
    const RenderEnvironment& env_;
    RenderStats& stats_;
    Scratch& s_;
    gfx::Canvas& canvas_;
    GState gs_;
    std::vector<GState> stack_;
    size_t floor_ = 0;          // saves a Q may not restore past (the running form's)
    size_t ignoredSaves_ = 0;   // q beyond kMaxSaves, matched by Qs that do nothing
    size_t clipDepth_ = 0;
    bool stopped_ = false;
    gfx::Path path_;
    bool hasCurrent_ = false;
    double curX_ = 0, curY_ = 0, startX_ = 0, startY_ = 0;
    bool pendingClip_ = false;
    gfx::FillRule clipRule_ = gfx::FillRule::NonZero;
    Matrix tm_;
    Matrix tlm_;
    std::unordered_map<const PdfValue*, std::shared_ptr<const PdfFont>> fonts_;
    std::unordered_map<const PdfValue*, std::shared_ptr<const gfx::Image>> images_;
};

} // namespace

const RenderEnvironment& DefaultRenderEnvironment() {
    static text::FontIndex index;
    static const RenderEnvironment environment = [] {
        index.Start();
        RenderEnvironment env;
        env.fontIndex = &index;
        env.images = &SharedImageCache();
        env.fonts = &SharedFontCache();
        return env;
    }();
    return environment;
}

bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats,
                const RenderEnvironment& environment) {
    RenderStats local;
    RenderStats& st = stats ? *stats : local;
    st = RenderStats();
    if (page < 0 || page >= file.PageCount() || widthPx <= 0 || widthPx > kMaxRenderSide) return false;
    const PdfRect box = file.PageMediaBox(page);
    if (!(box.Width() > 0) || !(box.Height() > 0) || !Finite(box.Width()) || !Finite(box.Height())) return false;
    const double s = widthPx / box.Width();
    const double h = box.Height() * s;
    if (!(h < kMaxRenderSide + 0.5)) return false;
    const int heightPx = std::max(1, static_cast<int>(LRound(h)));
    if (heightPx > kMaxRenderSide ||
        static_cast<uint64_t>(widthPx) * static_cast<uint64_t>(heightPx) > kMaxRenderPixels) {
        return false;
    }
    std::vector<uint32_t> pixels(static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx), 0xFFFFFFFFu);
    Scratch& scratch = ThreadScratch();
    scratch.glyphs.BeginFrame();
    scratch.canvas.SetTarget(gfx::Surface{pixels.data(), widthPx, heightPx, widthPx});
    {
        Interpreter interpreter(file, environment, st, scratch, Matrix{s, 0, 0, -s, -box.x0 * s, box.y1 * s});
        std::string_view content;
        std::string error;
        if (file.PageContent(page, content, &error)) {
            interpreter.Run(content, file.PageResources(page), 0);
        } else {
            st.AddUnknown(error.empty() ? "contents" : error);
        }
    }
    scratch.canvas.SetTarget(gfx::Surface{});
    out.width = widthPx;
    out.height = heightPx;
    out.pixels = std::move(pixels);
    out.opaque = true;
    return true;
}

bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats) {
    return RenderPage(file, page, widthPx, out, stats, DefaultRenderEnvironment());
}

} // namespace rgui::pdf
