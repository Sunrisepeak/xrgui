// svg-outline -- a stroked SVG icon becomes a filled one.
//
//   svg-outline <in.svg> -o <out.svg>       write the filled SVG
//   svg-outline <in.svg> --embed <out.h>    write its bytes as a comma-separated
//                                           list, to be #include'd inside a
//                                           `char[] = { ... }` initializer
//   [--tolerance <px>]                      curve flattening tolerance (0.15)
//
// WHAT IT COMPUTES, AND WHY NOT MORE.
//
// xrgui's MSDF loader (src/graphic/msdf.cpp) hands nanosvg's paths to msdfgen
// and ignores strokes. The icons are drawn with strokes (width 3, round caps,
// round joins), so something has to turn a stroke into an outline first.
//
// A stroke is the union of the stroke of each of its segments, and the stroke
// of a straight segment with round caps is a CAPSULE: a rectangle of half the
// stroke width on each side, closed by a semicircle at each end. So each path
// is flattened into segments and every segment becomes one capsule contour, all
// with the same orientation. Nothing is unioned here: msdfgen's generator runs
// with overlapSupport (the default GeneratorConfig, which is what xrgui calls),
// and under nonzero winding a pile of same-oriented overlapping capsules IS the
// union. Round joins come for free -- the caps of adjacent capsules overlap at
// every joint -- which is why the tool only has to be exact for round caps and
// joins, the only kind these icons use. Another cap style is reported once and
// rendered round.
//
// Filled paths are copied through untouched, curves and all.
//
// Coordinates: nanosvg resolves the viewBox and units into pixel space, so the
// output's viewBox is the image size and the numbers are what nanosvg reports.
// The one header: nanosvg is a C library. Everything else is `import std;`.
#include <nanosvg.h>

import std;

namespace {

struct pt { float x, y; };

// Distance of a point from the chord p0-p3, for the flatness test.
float chord_error(pt p0, pt p1, pt p2, pt p3) {
    const float dx = p3.x - p0.x, dy = p3.y - p0.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9f) {
        return std::max(std::hypot(p1.x - p0.x, p1.y - p0.y), std::hypot(p2.x - p0.x, p2.y - p0.y));
    }
    const auto d = [&](pt p) { return std::abs((p.x - p0.x) * dy - (p.y - p0.y) * dx) / len; };
    return std::max(d(p1), d(p2));
}

// Flatten one cubic by subdivision (de Casteljau) until the control points sit
// within `tol` of the chord. Appends the end point only; the caller seeded the
// start point.
void flatten(pt p0, pt p1, pt p2, pt p3, float tol, std::vector<pt>& out, int depth = 0) {
    if (depth > 16 || chord_error(p0, p1, p2, p3) <= tol) {
        out.push_back(p3);
        return;
    }
    const pt p01{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2}, p12{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2},
             p23{(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
    const pt p012{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2}, p123{(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
    const pt mid{(p012.x + p123.x) / 2, (p012.y + p123.y) / 2};
    flatten(p0, p01, p012, mid, tol, out, depth + 1);
    flatten(mid, p123, p23, p3, tol, out, depth + 1);
}

std::string num(float v) {
    std::string s = std::format("{:.3f}", v);
    // "1.500" -> "1.5", "2.000" -> "2": smaller file, same geometry.
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

// A quarter-circle arc from angle a0 to a1 around `c` (radius r), as one
// cubic: kappa is the standard 4/3 * tan(pi/8). The tangent at each end points
// the way the angle moves, so a sweep in decreasing angle flips it -- left
// unflipped, every cap grew a notch where the control points pulled the curve
// the wrong way.
void arc_quarter(std::string& d, pt c, float r, float a0, float a1) {
    constexpr float kappa = 0.5522847498f;
    const float k = (a1 > a0 ? 1.0f : -1.0f) * r * kappa;
    const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
    const pt p0{c.x + r * c0, c.y + r * s0}, p3{c.x + r * c1, c.y + r * s1};
    const pt p1{p0.x - k * s0, p0.y + k * c0};
    const pt p2{p3.x + k * s1, p3.y - k * c1};
    d += " C " + num(p1.x) + " " + num(p1.y) + " " + num(p2.x) + " " + num(p2.y) + " " + num(p3.x) + " " + num(p3.y);
}

// A semicircle around `c` from angle a0 to a1 (|a1 - a0| == pi), two quarters,
// swept in the direction the two angles give.
void arc_half(std::string& d, pt c, float r, float a0, float a1) {
    const float mid = (a0 + a1) / 2;
    arc_quarter(d, c, r, a0, mid);
    arc_quarter(d, c, r, mid, a1);
}

// One capsule around segment a->b with half-width h. Orientation is the same
// for every capsule: along the left side to b, around b's FRONT to the right
// side, back to a, around a's BACK to the left side. The end caps must sweep
// through the outside of the segment; swept through the inside they cross the
// rectangle and the contour intersects itself -- measured as a 60% mismatch
// against the rendered stroke before this was written down.
void capsule(std::string& d, pt a, pt b, float h) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float ux = dx / len, uy = dy / len;      // along
    const float nx = -uy, ny = ux;                 // left normal, at angle ang + pi/2
    const float ang = std::atan2(uy, ux);
    constexpr float pi = 3.14159265358979f;
    d += " M " + num(a.x + nx * h) + " " + num(a.y + ny * h);
    d += " L " + num(b.x + nx * h) + " " + num(b.y + ny * h);
    arc_half(d, b, h, ang + pi / 2, ang - pi / 2);          // through ang: the front of b
    d += " L " + num(a.x - nx * h) + " " + num(a.y - ny * h);
    arc_half(d, a, h, ang - pi / 2, ang - 3 * pi / 2);      // through ang - pi: the back of a
    d += " Z";
}

void circle(std::string& d, pt c, float r) {
    constexpr float pi = 3.14159265358979f;
    d += " M " + num(c.x + r) + " " + num(c.y);
    for (int q = 0; q < 4; ++q) arc_quarter(d, c, r, q * pi / 2, (q + 1) * pi / 2);
    d += " Z";
}

int usage(const char* argv0) {
    std::println(std::cerr, "usage: {} <in.svg> (-o <out.svg> | --embed <out.h>) [--tolerance <px>]", argv0);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string in, out_svg, out_h;
    // 0.15 units on a 48-unit icon is a third of a pixel where these render;
    // 0.05 tripled the output for no visible difference (14 KB against 5 KB).
    float tol = 0.15f;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-o" && i + 1 < argc)               out_svg = argv[++i];
        else if (a == "--embed" && i + 1 < argc)     out_h = argv[++i];
        else if (a == "--tolerance" && i + 1 < argc) tol = std::stof(argv[++i]);
        else if (!a.starts_with("-") && in.empty()) in = a;
        else return usage(argv[0]);
    }
    if (in.empty() || (out_svg.empty() && out_h.empty())) return usage(argv[0]);

    NSVGimage* image = nsvgParseFromFile(in.c_str(), "px", 96.0f);
    if (image == nullptr) {
        std::println(std::cerr, "svg-outline: cannot parse {}", in);
        return 1;
    }

    std::string d;
    bool warned_cap = false;
    std::size_t shapes = 0;
    std::vector<pt> poly;
    for (NSVGshape* shape = image->shapes; shape != nullptr; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        const bool filled  = shape->fill.type != NSVG_PAINT_NONE;
        const bool stroked = shape->stroke.type != NSVG_PAINT_NONE && shape->strokeWidth > 0.0f;
        if (!filled && !stroked) continue;
        ++shapes;
        if (stroked && !warned_cap && (shape->strokeLineCap != NSVG_CAP_ROUND || shape->strokeLineJoin != NSVG_JOIN_ROUND)) {
            std::println(std::cerr, "svg-outline: {}: a stroke with a non-round cap or join is rendered round", in);
            warned_cap = true;
        }
        for (NSVGpath* path = shape->paths; path != nullptr; path = path->next) {
            if (path->npts < 1) continue;
            const auto P = [&](int i) { return pt{path->pts[i * 2], path->pts[i * 2 + 1]}; };

            if (filled) {
                // Copied through: nanosvg already made it cubics.
                d += " M " + num(P(0).x) + " " + num(P(0).y);
                for (int i = 1; i + 2 < path->npts; i += 3)
                    d += " C " + num(P(i).x) + " " + num(P(i).y) + " " + num(P(i + 1).x) + " " + num(P(i + 1).y)
                       + " " + num(P(i + 2).x) + " " + num(P(i + 2).y);
                d += " Z";
            }
            if (stroked) {
                poly.clear();
                poly.push_back(P(0));
                for (int i = 1; i + 2 < path->npts; i += 3) flatten(P(i - 1), P(i), P(i + 1), P(i + 2), tol, poly);
                if (path->closed && (poly.front().x != poly.back().x || poly.front().y != poly.back().y))
                    poly.push_back(poly.front());
                const float h = shape->strokeWidth / 2.0f;
                std::size_t segments = 0;
                for (std::size_t i = 1; i < poly.size(); ++i) {
                    const float len = std::hypot(poly[i].x - poly[i - 1].x, poly[i].y - poly[i - 1].y);
                    if (len < 1e-4f) continue;
                    capsule(d, poly[i - 1], poly[i], h);
                    ++segments;
                }
                // A dot: a path whose every segment is degenerate is a round cap
                // and nothing else.
                if (segments == 0) circle(d, poly.front(), h);
            }
        }
    }
    const float w = image->width, hgt = image->height;
    nsvgDelete(image);
    if (shapes == 0) {
        std::println(std::cerr, "svg-outline: {}: no visible filled or stroked shape", in);
        return 1;
    }

    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(w) + "\" height=\"" + num(hgt)
                    + "\" viewBox=\"0 0 " + num(w) + " " + num(hgt) + "\"><path fill=\"#000\" fill-rule=\"nonzero\" d=\""
                    + d.substr(1) + "\"/></svg>\n";

    const auto write = [&](const std::string& path, const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) { std::println(std::cerr, "svg-outline: cannot write {}", path); return false; }
        return true;
    };
    if (!out_svg.empty() && !write(out_svg, svg)) return 1;
    if (!out_h.empty()) {
        // The same shape build.mcpp's bin2c wrote: a comma-separated byte list,
        // sixteen per line, no braces -- it is #include'd inside an initializer.
        std::string bytes;
        for (std::size_t n = 0; n < svg.size(); ++n) {
            if (n != 0) bytes += ',';
            if (n % 16 == 0) bytes += '\n';
            bytes += std::to_string(static_cast<int>(static_cast<unsigned char>(svg[n])));
        }
        bytes += '\n';
        if (!write(out_h, bytes)) return 1;
    }
    return 0;
}
