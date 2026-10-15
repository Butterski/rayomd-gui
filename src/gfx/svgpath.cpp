// SPDX-License-Identifier: Apache-2.0
// SVG path data for icons (plan Phase 4 step 8): absolute M, L, H, V, C and Z.
#include "gfx/svgpath.h"

#include <cstdint>

namespace rgui::gfx {

namespace {

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    void SkipSeparators() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r' ||
                                  s_[i_] == '\f' || s_[i_] == ',')) {
            ++i_;
        }
    }
    bool AtEnd() {
        SkipSeparators();
        return i_ >= s_.size();
    }
    // The next character after separators, 0 at the end.
    char Peek() {
        SkipSeparators();
        return i_ < s_.size() ? s_[i_] : 0;
    }
    void Advance() { ++i_; }
    bool NumberNext() {
        const char c = Peek();
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
    }

    // An SVG number: [sign] digits [. digits] [e [sign] digits], at least one mantissa digit.
    bool Number(float& out) {
        SkipSeparators();
        bool negative = false;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) negative = s_[i_++] == '-';
        uint64_t mantissa = 0;
        int exponent = 0;
        int digits = 0;
        auto digit = [&](char c) {
            if (mantissa < 100000000000000000ull) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
                return 0;
            }
            return 1;  // past 17 significant digits: dropped
        };
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
            exponent += digit(s_[i_++]);
            ++digits;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
                if (digit(s_[i_++]) == 0) --exponent;
                ++digits;
            }
        }
        if (digits == 0) return false;
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            bool expNegative = false;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) expNegative = s_[i_++] == '-';
            int e = 0;
            int expDigits = 0;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
                if (e < 1000) e = e * 10 + (s_[i_] - '0');
                ++i_;
                ++expDigits;
            }
            if (expDigits == 0) return false;
            exponent += expNegative ? -e : e;
        }
        if (exponent > 60 || exponent < -60) return false;  // icons live in small grids
        double value = static_cast<double>(mantissa);
        double scale = 1;
        for (int k = exponent < 0 ? -exponent : exponent; k > 0; --k) scale *= 10;
        value = exponent < 0 ? value / scale : value * scale;
        if (value > 1e30) return false;
        out = static_cast<float>(negative ? -value : value);
        return true;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
};

} // namespace

bool ParseSvgPath(std::string_view data, Path& path) {
    Reader r(data);
    float cx = 0, cy = 0;
    bool first = true;
    while (!r.AtEnd()) {
        const char command = r.Peek();
        if (first && command != 'M') return false;
        first = false;
        r.Advance();
        switch (command) {
        case 'M':
        case 'L': {
            bool move = command == 'M';
            do {
                float x, y;
                if (!r.Number(x) || !r.Number(y)) return false;
                if (move) path.MoveTo(x, y);
                else path.LineTo(x, y);
                move = false;
                cx = x, cy = y;
            } while (r.NumberNext());
            break;
        }
        case 'H':
            do {
                if (!r.Number(cx)) return false;
                path.LineTo(cx, cy);
            } while (r.NumberNext());
            break;
        case 'V':
            do {
                if (!r.Number(cy)) return false;
                path.LineTo(cx, cy);
            } while (r.NumberNext());
            break;
        case 'C':
            do {
                float x1, y1, x2, y2, x, y;
                if (!r.Number(x1) || !r.Number(y1) || !r.Number(x2) || !r.Number(y2) || !r.Number(x) ||
                    !r.Number(y)) {
                    return false;
                }
                path.CubicTo(x1, y1, x2, y2, x, y);
                cx = x, cy = y;
            } while (r.NumberNext());
            break;
        case 'Z': {
            path.Close();
            // The current point returns to the subpath's start.
            const std::vector<Verb>& verbs = path.Verbs();
            const std::vector<PointF>& points = path.Points();
            size_t pointIndex = points.size();
            for (size_t v = verbs.size(); v-- > 0;) {
                const Verb verb = verbs[v];
                const size_t n = verb == Verb::MoveTo || verb == Verb::LineTo ? 1
                               : verb == Verb::QuadTo                         ? 2
                               : verb == Verb::CubicTo                        ? 3
                                                                              : 0;
                pointIndex -= n;
                if (verb == Verb::MoveTo) {
                    cx = points[pointIndex].x;
                    cy = points[pointIndex].y;
                    break;
                }
            }
            if (r.NumberNext()) return false;
            break;
        }
        default:
            return false;  // relative commands, arcs, quadratics and anything else
        }
    }
    return !first;
}

} // namespace rgui::gfx
