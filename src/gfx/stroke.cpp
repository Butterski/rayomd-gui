// SPDX-License-Identifier: Apache-2.0
// The stroker (plan Phase 4 step 6). The path is flattened in user space, dashed by arc length,
// and every segment, join and cap becomes its own closed polygon, all wound the same way, so a
// NonZero fill of the outline is their union.
#include "gfx/stroke.h"

#include "common/exact_math.h"

namespace rgui::gfx {

namespace {

namespace ExactMath = RayoMd::ExactMath;

constexpr double kPi = 3.14159265358979323846;

struct V {
    double x;
    double y;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
inline V operator-(V a) { return {-a.x, -a.y}; }
inline V operator*(V a, double k) { return {a.x * k, a.y * k}; }
inline double Dot(V a, V b) { return a.x * b.x + a.y * b.y; }
inline double Cross(V a, V b) { return a.x * b.y - a.y * b.x; }
inline V LeftNormal(V d) { return {-d.y, d.x}; }

// kStep[n - 1] = {cos(pi / n), sin(pi / n)}: one step of an arc cut into n per half turn.
struct Step {
    double c;
    double s;
};
constexpr Step kStep[64] = {
    {-1, 0},
    {0, 1},
    {0.50000000000000011, 0.8660254037844386},
    {0.70710678118654757, 0.70710678118654746},
    {0.80901699437494745, 0.58778525229247314},
    {0.86602540378443871, 0.49999999999999994},
    {0.90096886790241915, 0.43388373911755812},
    {0.92387953251128674, 0.38268343236508978},
    {0.93969262078590843, 0.34202014332566871},
    {0.95105651629515353, 0.3090169943749474},
    {0.95949297361449737, 0.28173255684142967},
    {0.96592582628906831, 0.25881904510252074},
    {0.97094181742605201, 0.23931566428755774},
    {0.97492791218182362, 0.22252093395631439},
    {0.97814760073380569, 0.20791169081775931},
    {0.98078528040323043, 0.19509032201612825},
    {0.98297309968390179, 0.18374951781657034},
    {0.98480775301220802, 0.17364817766693033},
    {0.98636130340272232, 0.16459459028073389},
    {0.98768834059513777, 0.15643446504023087},
    {0.98883082622512852, 0.14904226617617444},
    {0.98982144188093268, 0.14231483827328514},
    {0.99068594603633076, 0.13616664909624659},
    {0.99144486137381038, 0.13052619222005157},
    {0.99211470131447788, 0.12533323356430426},
    {0.99270887409805397, 0.12053668025532305},
    {0.99323835774194302, 0.11609291412523023},
    {0.9937122098932426, 0.11196447610330786},
    {0.9941379571543596, 0.10811901842394177},
    {0.99452189536827329, 0.10452846326765346},
    {0.99486932339189516, 0.10116832198743217},
    {0.99518472667219693, 0.098017140329560604},
    {0.99547192257308459, 0.095056043304182658},
    {0.99573417629503447, 0.092268359463301988},
    {0.99597429399523907, 0.089639308903433496},
    {0.99619469809174555, 0.087155742747658166},
    {0.99639748854252652, 0.084805924475509192},
    {0.99658449300666985, 0.082579345472332324},
    {0.99675730813421004, 0.080466568716725875},
    {0.99691733373312796, 0.078459095727844944},
    {0.99706580118374044, 0.076549252836495649},
    {0.99720379718118013, 0.074730093586424254},
    {0.99733228366355164, 0.072995314660907529},
    {0.99745211461025352, 0.071339183199232339},
    {0.9975640502598242, 0.069756473744125302},
    {0.99766876919053915, 0.068242413364670976},
    {0.99776687862315316, 0.066792633745121552},
    {0.99785892323860348, 0.065403129230143062},
    {0.99794539275033634, 0.064070219980712911},
    {0.99802672842827156, 0.062790519529313374},
    {0.9981033287370441, 0.061560906133942835},
    {0.9981755542233175, 0.06037849742228605},
    {0.99824373176432146, 0.059240627893714287},
    {0.99830815827126818, 0.058144828910475829},
    {0.99836910392613565, 0.057088810862767979},
    {0.9984268150178166, 0.056070447237191789},
    {0.99848151643331617, 0.055087760355865441},
    {0.99853341385112382, 0.054138908585417526},
    {0.99858269567676194, 0.05322217484217865},
    {0.99862953475457383, 0.052335956242943828},
    {0.99867408988483053, 0.051478754770346531},
    {0.99871650717105276, 0.050649168838712712},
    {0.99875692121892234, 0.049845885660697163},
    {0.99879545620517241, 0.049067674327418015},
};

// The most dashes one stroke makes; a pattern that would make more is drawn solid.
constexpr double kMaxDashes = 100000;

class Stroker final : public PolylineSink {
public:
    Stroker(const StrokeStyle& style, double halfWidth, int roundSteps, Path& out)
        : style_(style), hw_(halfWidth), steps_(roundSteps), out_(out) {
        step_ = kStep[steps_ - 1];
        // Inner arc points sit on the radius that gives the polygon the circle's area:
        // n r'^2 sin(pi / n) = pi r^2 per half turn.
        arcScale_ = ExactMath::Sqrt((kPi / steps_) / step_.s);
    }

    void SetDash(std::vector<double> dash, double phase) {
        dash_ = std::move(dash);
        phase_ = phase;
    }

    void Polyline(const PointF* points, size_t count, bool closed) override {
        pts_.clear();
        for (size_t i = 0; i < count; ++i) {
            const V p{points[i].x, points[i].y};
            if (!(p.x > -1e300 && p.x < 1e300 && p.y > -1e300 && p.y < 1e300)) return;  // NaN: drop the subpath
            if (!pts_.empty()) {
                const V d = p - pts_.back();
                if (d.x * d.x + d.y * d.y == 0) continue;
            }
            pts_.push_back(p);
        }
        if (pts_.empty()) return;
        if (closed && pts_.size() > 1) {
            const V d = pts_.back() - pts_.front();
            if (d.x * d.x + d.y * d.y == 0) pts_.pop_back();
        }
        if (pts_.size() == 1) {
            // A lone MoveTo draws nothing; a closed point or a run of equal points is a dot.
            if (count >= 2 || closed) PointCap(pts_[0], {1, 0});
            return;
        }
        if (dash_.empty()) {
            Run(pts_.data(), pts_.size(), closed);
        } else {
            Dash(closed);
        }
    }

private:
    const StrokeStyle& style_;
    double hw_;
    int steps_;
    Step step_{};
    double arcScale_ = 1;
    Path& out_;
    std::vector<double> dash_;
    double phase_ = 0;
    std::vector<V> pts_;
    std::vector<V> piece_;
    std::vector<V> dirs_;
    std::vector<V> poly_;

    // Emits poly_ as a closed polygon wound positively; nothing when it has no area.
    void Emit() {
        const size_t n = poly_.size();
        if (n < 3) return;
        double twice = 0;
        for (size_t i = 0; i < n; ++i) twice += Cross(poly_[i], poly_[(i + 1) % n]);
        if (!(twice > 0 || twice < 0)) return;
        auto at = [&](size_t i) { return twice > 0 ? poly_[i] : poly_[n - 1 - i]; };
        out_.MoveTo(static_cast<float>(at(0).x), static_cast<float>(at(0).y));
        for (size_t i = 1; i < n; ++i) out_.LineTo(static_cast<float>(at(i).x), static_cast<float>(at(i).y));
        out_.Close();
    }

    // Appends the arc around `c` from c + v0 to c + v1, turning by the sign of `dir`, at most
    // `maxSteps` steps; the end lands exactly on v1.
    void Arc(V c, V v0, V v1, double dir, int maxSteps) {
        const double sn = dir < 0 ? -step_.s : step_.s;
        V v = v0;
        poly_.push_back(c + v0);
        for (int i = 0; i < maxSteps; ++i) {
            v = {v.x * step_.c - v.y * sn, v.x * sn + v.y * step_.c};
            if (dir * Cross(v, v1) <= 0) break;
            poly_.push_back(c + v * arcScale_);
        }
        poly_.push_back(c + v1);
    }

    // A zero-length dash or subpath at `p` in direction `d`: a dot or a square for those caps.
    void PointCap(V p, V d) {
        if (style_.cap == LineCap::Butt) return;
        poly_.clear();
        if (style_.cap == LineCap::Square) {
            const V u = d * hw_;
            const V o = LeftNormal(d) * hw_;
            poly_ = {p - u + o, p + u + o, p + u - o, p - u - o};
        } else {
            V v{hw_ * arcScale_, 0};
            for (int i = 0; i < 2 * steps_; ++i) {
                poly_.push_back(p + v);
                v = {v.x * step_.c - v.y * step_.s, v.x * step_.s + v.y * step_.c};
            }
        }
        Emit();
    }

    // The cap at `p` of a line whose outward direction is `d`.
    void Cap(V p, V d) {
        const V o = LeftNormal(d) * hw_;
        poly_.clear();
        if (style_.cap == LineCap::Square) {
            const V u = d * hw_;
            poly_ = {p + o, p + o + u, p - o + u, p - o};
        } else if (style_.cap == LineCap::Round) {
            poly_.push_back(p);
            Arc(p, o, -o, -1, steps_ + 1);  // through p + d * hw
        } else {
            return;
        }
        Emit();
    }

    // The join at `p` from direction d0 to d1, on the outer side of the turn.
    void Join(V p, V d0, V d1) {
        const double cross = Cross(d0, d1);
        const double dot = Dot(d0, d1);
        if (cross == 0 && dot > 0) return;
        const double side = cross > 0 ? -1 : 1;
        const V o0 = LeftNormal(d0) * (side * hw_);
        const V o1 = LeftNormal(d1) * (side * hw_);
        poly_.clear();
        poly_.push_back(p);
        switch (style_.join) {
        case LineJoin::Round:
            Arc(p, o0, o1, -side, steps_ + 1);
            break;
        case LineJoin::Miter: {
            const double ml = style_.miterLimit;
            if (1 + dot > 1e-12 && 2 / (1 + dot) <= ml * ml) {
                poly_.push_back(p + o0);
                poly_.push_back(p + (o0 + o1) * (1 / (1 + dot)));
                poly_.push_back(p + o1);
                break;
            }
            [[fallthrough]];
        }
        case LineJoin::Bevel:
            poly_.push_back(p + o0);
            poly_.push_back(p + o1);
            break;
        }
        Emit();
    }

    // Strokes the polyline p[0..n), n >= 2 with no two consecutive points equal.
    void Run(const V* p, size_t n, bool closed) {
        const size_t segments = closed ? n : n - 1;
        dirs_.resize(segments);
        for (size_t i = 0; i < segments; ++i) {
            const V a = p[i];
            const V b = p[(i + 1) % n];
            const V d = b - a;
            const V u = d * (1 / ExactMath::Sqrt(d.x * d.x + d.y * d.y));
            dirs_[i] = u;
            const V o = LeftNormal(u) * hw_;
            poly_ = {a + o, b + o, b - o, a - o};
            Emit();
        }
        if (closed) {
            for (size_t i = 0; i < n; ++i) Join(p[i], dirs_[(i + n - 1) % n], dirs_[i]);
        } else {
            for (size_t i = 1; i + 1 < n; ++i) Join(p[i], dirs_[i - 1], dirs_[i]);
            Cap(p[0], -dirs_[0]);
            Cap(p[n - 1], dirs_[segments - 1]);
        }
    }

    // Ends the dash in piece_ (direction `d` where it has no length).
    void FlushPiece(V d) {
        size_t w = 0;
        for (size_t i = 0; i < piece_.size(); ++i) {
            if (w > 0) {
                const V e = piece_[i] - piece_[w - 1];
                if (e.x * e.x + e.y * e.y == 0) continue;
            }
            piece_[w++] = piece_[i];
        }
        piece_.resize(w);
        if (w >= 2) {
            Run(piece_.data(), w, false);
        } else if (w == 1) {
            PointCap(piece_[0], d);
        }
        piece_.clear();
    }

    void Dash(bool closed) {
        const size_t n = pts_.size();
        const size_t k = dash_.size();
        double period = 0;
        for (double v : dash_) period += v;
        // Where the phase falls in the pattern.
        double phase = ExactMath::Fmod(phase_, period);
        if (phase < 0) phase += period;
        size_t idx = 0;
        bool on = true;
        while (phase > 0 && phase >= dash_[idx]) {
            phase -= dash_[idx];
            idx = (idx + 1) % k;
            on = !on;
        }
        double remain = dash_[idx] - phase;
        piece_.clear();
        if (on) piece_.push_back(pts_[0]);
        const size_t segments = closed ? n : n - 1;
        V d{1, 0};
        for (size_t i = 0; i < segments; ++i) {
            const V a = pts_[i];
            const V b = pts_[(i + 1) % n];
            const V e = b - a;
            const double len = ExactMath::Sqrt(e.x * e.x + e.y * e.y);
            d = e * (1 / len);
            double pos = 0;
            while (len - pos > remain) {
                pos += remain;
                const V q = a + d * pos;
                if (on) {
                    piece_.push_back(q);
                    FlushPiece(d);
                } else {
                    piece_.clear();
                    piece_.push_back(q);
                }
                on = !on;
                idx = (idx + 1) % k;
                remain = dash_[idx];
            }
            remain -= len - pos;
            if (on) piece_.push_back(b);
        }
        if (on) FlushPiece(d);
    }
};

} // namespace

void StrokeOutline(const Path& path, const Matrix& m, const StrokeStyle& style, Path& outline) {
    outline.Clear();
    double s = ExactMath::Sqrt(m.a * m.d - m.b * m.c > 0 ? m.a * m.d - m.b * m.c : m.b * m.c - m.a * m.d);
    if (!(s >= 1e-6)) s = 1e-6;  // NaN too
    if (!(s < 1e12)) return;
    const double width = style.width;
    if (!(width < 1e12)) return;  // NaN and absurd widths draw nothing
    const double hw = width > 0 ? width / 2 : 0.5 / s;
    const double radiusPx = hw * s;
    int steps = static_cast<int>(ExactMath::Ceil(ExactMath::Sqrt(radiusPx) * 4));
    if (!(steps >= 4)) steps = 4;
    if (steps > 64) steps = 64;
    const float tolerance = static_cast<float>(kFlattenTolerance / s);

    Stroker stroker(style, hw, steps, outline);

    // The dash pattern: an odd count repeats twice; negative, non-finite or all-zero ones are solid.
    if (!style.dash.empty()) {
        std::vector<double> dash(style.dash.begin(), style.dash.end());
        if (dash.size() % 2 == 1) dash.insert(dash.end(), style.dash.begin(), style.dash.end());
        double period = 0;
        bool valid = true;
        for (double v : dash) {
            if (!(v >= 0 && v < 1e15)) valid = false;
            period += v;
        }
        const double phase = style.dashPhase;
        if (valid && period > 0 && phase > -1e15 && phase < 1e15) {
            // Too many dashes to draw one by one: solid. The length is measured on the flattened path.
            struct Measure final : PolylineSink {
                double length = 0;
                void Polyline(const PointF* p, size_t n, bool closed) override {
                    for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); ++i) {
                        const PointF a = p[i], b = p[(i + 1) % n];
                        const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
                        length += ExactMath::Sqrt(dx * dx + dy * dy);
                    }
                }
            } measure;
            std::vector<PointF> scratch;
            FlattenPolylines(path, {}, tolerance, scratch, measure);
            if (measure.length / period <= kMaxDashes) stroker.SetDash(std::move(dash), phase);
        }
    }

    std::vector<PointF> scratch;
    FlattenPolylines(path, {}, tolerance, scratch, stroker);
}

} // namespace rgui::gfx
