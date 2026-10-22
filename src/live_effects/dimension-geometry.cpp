// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure dimension model: anchors, per-kind geometry and label formatting.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/dimension-geometry.h"

#include <algorithm>
#include <cmath>
#include <iomanip>

#include <glib.h>

#include <2geom/elliptical-arc.h>

#include "util-string/ustring-format.h"
#include "util/units.h"

namespace Inkscape::LivePathEffect::Dimension {

namespace {

/// Below this (user units / degrees) lengths and angles count as degenerate.
constexpr double EPS = 1e-9;

/// Arrowheads flip to the outside when the dimension line is shorter than this multiple of the arrow length.
constexpr double ARROWS_OUTSIDE_FACTOR = 2.5;

/// Arrowhead wing half-width as a fraction of the arrow length.
constexpr double ARROW_WING_FRACTION = 1.0 / 3.0;

/// The 9 bbox handles, in tl..br order.
constexpr char const *HANDLE_NAMES[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};

bool is_finite(Geom::Point const &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

/// Left of the travel direction u: visually left on screen when y is down,
/// visually left in math orientation when y is up.
Geom::Point left_perp(Geom::Point const &u, bool y_down)
{
    return y_down ? Geom::Point(u.y(), -u.x()) : Geom::Point(-u.y(), u.x());
}

/// Perpendicular of u that points visually up (used for Above/Below label shifts).
Geom::Point visual_up(Geom::Point const &u, bool y_down)
{
    Geom::Point n = left_perp(u, y_down);
    double const up_component = y_down ? -n.y() : n.y();
    if (up_component < -EPS) {
        n = -n;
    }
    return n;
}

/// Counter-clockwise perpendicular (positive angle direction in the given coordinates).
Geom::Point ccw_perp(Geom::Point const &u)
{
    return Geom::Point(-u.y(), u.x());
}

/// Upright-normalise a text angle: a right-to-left line reads left-to-right.
double normalise_label_angle_deg(double deg)
{
    if (deg > 90.0) {
        deg -= 180.0;
    } else if (deg < -90.0) {
        deg += 180.0;
    }
    return deg;
}

/// One closed arrowhead triangle: tip at @a tip, pointing along @a dir, tail behind it.
void append_arrow(Geom::PathVector &pv, Geom::Point const &tip, Geom::Point const &dir, double length)
{
    Geom::Point const u = dir / dir.length();
    Geom::Point const tail = tip - u * length;
    Geom::Point const wing = ccw_perp(u) * (length * ARROW_WING_FRACTION);

    Geom::Path path;
    path.start(tip);
    path.appendNew<Geom::LineSegment>(tail + wing);
    path.appendNew<Geom::LineSegment>(tail - wing);
    path.close();
    pv.push_back(std::move(path));
}

/// Arrow direction at a dimension line end: outward normally, flipped to
/// point inward (body beyond the extension line) when the line is too short.
Geom::Point arrow_dir(bool flip_outside, Geom::Point const &u, bool at_start)
{
    Geom::Point outward = at_start ? -u : u;
    return flip_outside ? -outward : outward;
}

/// Extension line for one measured point: from gap-distance toward the dim
/// line, past it by the overshoot. Skipped when the line sits on the point.
void append_ext_line(Geom::PathVector &pv, Geom::Point const &p, Geom::Point const &side_unit,
                     double dist_to_line, double gap, double overshoot)
{
    if (dist_to_line <= EPS) {
        return;
    }
    double const start_dist = std::min(gap, dist_to_line);
    double const end_dist = dist_to_line + std::max(0.0, overshoot);
    Geom::Path path;
    path.start(p + side_unit * start_dist);
    path.appendNew<Geom::LineSegment>(p + side_unit * end_dist);
    pv.push_back(std::move(path));
}

/// Shift from the line/arc midpoint for a text position (positive = away from the geometry).
double label_shift(TextPosition position, double gap)
{
    switch (position) {
        case TextPosition::Above:
            return gap;
        case TextPosition::Inline:
            return 0.0;
        case TextPosition::Below:
            return -gap;
    }
    return 0.0;
}

Result build_linear(Kind kind, Geom::Point const &p1, Geom::Point const &p2, Style const &style, bool y_down)
{
    Result res;

    double const offset = style.offset;
    double dim_len = 0.0;
    Geom::Point u;      ///< dimension line direction (D1 -> D2)
    Geom::Point d1, d2; ///< dimension line ends, at the measured points' feet

    if (kind == Kind::Aligned || kind == Kind::Segment) {
        Geom::Point const d = p2 - p1;
        dim_len = d.length();
        if (dim_len <= EPS) {
            return res; // coincident points
        }
        u = d / dim_len;
        Geom::Point const side = left_perp(u, y_down) * (offset >= 0.0 ? 1.0 : -1.0);
        double const dist = std::abs(offset);
        d1 = p1 + side * dist;
        d2 = p2 + side * dist;
        append_ext_line(res.path, p1, side, dist, style.ext_gap, style.ext_overshoot);
        append_ext_line(res.path, p2, side, dist, style.ext_gap, style.ext_overshoot);
    } else if (kind == Kind::Horizontal) {
        dim_len = std::abs(p2.x() - p1.x());
        if (dim_len <= EPS) {
            return res;
        }
        u = Geom::Point(p2.x() >= p1.x() ? 1.0 : -1.0, 0.0);
        double const dist = std::abs(offset);
        double const y_line = offset >= 0.0 ? std::min(p1.y(), p2.y()) - dist : std::max(p1.y(), p2.y()) + dist;
        double const side_y = offset >= 0.0 ? -1.0 : 1.0; // y direction from the points toward the line
        d1 = Geom::Point(p1.x(), y_line);
        d2 = Geom::Point(p2.x(), y_line);
        Geom::Point const side(0.0, side_y);
        append_ext_line(res.path, p1, side, std::abs(p1.y() - y_line), style.ext_gap, style.ext_overshoot);
        append_ext_line(res.path, p2, side, std::abs(p2.y() - y_line), style.ext_gap, style.ext_overshoot);
    } else { // Vertical
        dim_len = std::abs(p2.y() - p1.y());
        if (dim_len <= EPS) {
            return res;
        }
        u = Geom::Point(0.0, p2.y() >= p1.y() ? 1.0 : -1.0);
        double const dist = std::abs(offset);
        double const x_line = offset >= 0.0 ? std::max(p1.x(), p2.x()) + dist : std::min(p1.x(), p2.x()) - dist;
        double const side_x = offset >= 0.0 ? 1.0 : -1.0; // x direction from the points toward the line
        d1 = Geom::Point(x_line, p1.y());
        d2 = Geom::Point(x_line, p2.y());
        Geom::Point const side(side_x, 0.0);
        append_ext_line(res.path, p1, side, std::abs(p1.x() - x_line), style.ext_gap, style.ext_overshoot);
        append_ext_line(res.path, p2, side, std::abs(p2.x() - x_line), style.ext_gap, style.ext_overshoot);
    }

    // Dimension line.
    {
        Geom::Path path;
        path.start(d1);
        path.appendNew<Geom::LineSegment>(d2);
        res.path.push_back(std::move(path));
    }

    // Arrowheads: outward-to-extension, flipped outside for short lines.
    bool const outside = dim_len < ARROWS_OUTSIDE_FACTOR * style.arrow_length;
    append_arrow(res.path, d1, arrow_dir(outside, u, true), style.arrow_length);
    append_arrow(res.path, d2, arrow_dir(outside, u, false), style.arrow_length);

    // Label on the midpoint, shifted per text position.
    Geom::Point const mid = (d1 + d2) / 2.0;
    res.label_pos = mid + visual_up(u, y_down) * label_shift(style.text_position, style.label_gap);
    res.label_angle_deg = style.text_horizontal ? 0.0 : normalise_label_angle_deg(Geom::deg_from_rad(std::atan2(u.y(), u.x())));
    res.value_uu_or_deg = dim_len;
    res.valid = true;
    return res;
}

Result build_angular(Geom::Point const &arm_a, Geom::Point const &vertex, Geom::Point const &arm_b,
                     Style const &style, bool y_down)
{
    (void)y_down; // angular geometry is rotation-symmetric; only linear offset sides depend on y orientation
    Result res;

    Geom::Point const va = arm_a - vertex;
    Geom::Point const vb = arm_b - vertex;
    double const len_a = va.length();
    double const len_b = vb.length();
    if (len_a <= EPS || len_b <= EPS) {
        return res; // zero arm
    }

    double const ang_a = std::atan2(va.y(), va.x());
    double const ang_b = std::atan2(vb.y(), vb.x());
    double delta = ang_b - ang_a;
    while (delta <= -M_PI) {
        delta += 2.0 * M_PI;
    }
    while (delta > M_PI) {
        delta -= 2.0 * M_PI;
    }
    double const value_deg = std::abs(Geom::deg_from_rad(delta));
    if (value_deg <= EPS) {
        return res; // coincident arms: no angle in (0, 180]
    }
    bool const positive = delta > 0.0;

    double const radius = style.offset > EPS ? style.offset : 0.5 * std::min(len_a, len_b);
    if (radius <= EPS) {
        return res;
    }

    Geom::Point const start = vertex + (va / len_a) * radius;
    Geom::Point const end = vertex + (vb / len_b) * radius;

    // Arc + reversed arc as ONE closed subpath: filling it encloses zero area.
    {
        Geom::Path ring;
        ring.start(start);
        ring.appendNew<Geom::EllipticalArc>(radius, radius, 0.0, false, positive, end);
        ring.append(ring.back().reverse());
        ring.close();
        res.path.push_back(std::move(ring));
    }

    // Arrowheads at the arc ends, tangent to the arc, pointing outward (tips
    // at the arc ends, bodies back along the arc); flipped outside when the
    // arc is too short, exactly like a short dimension line.
    // Each arrow is tangent to the arc at its own end.
    Geom::Point const travel_start = ccw_perp(va / len_a) * (positive ? 1.0 : -1.0);
    Geom::Point const travel_end = ccw_perp(vb / len_b) * (positive ? 1.0 : -1.0);
    bool const outside = radius * value_deg * M_PI / 180.0 < ARROWS_OUTSIDE_FACTOR * style.arrow_length;
    append_arrow(res.path, start, outside ? travel_start : -travel_start, style.arrow_length);
    append_arrow(res.path, end, outside ? -travel_end : travel_end, style.arrow_length);

    // Label on the arc midpoint direction, shifted per text position
    // (Above: outside the arc, Below: inside).
    double const mid_angle = ang_a + delta / 2.0;
    Geom::Point const mid_dir(std::cos(mid_angle), std::sin(mid_angle));
    double const label_dist = radius + label_shift(style.text_position, style.label_gap);
    res.label_pos = vertex + mid_dir * label_dist;

    Geom::Point const travel_mid = ccw_perp(mid_dir) * (positive ? 1.0 : -1.0);
    res.label_angle_deg = style.text_horizontal ? 0.0 : normalise_label_angle_deg(Geom::deg_from_rad(std::atan2(travel_mid.y(), travel_mid.x())));
    res.value_uu_or_deg = value_deg;
    res.valid = true;
    return res;
}

} // namespace

std::optional<Kind> parse_kind(std::string_view key)
{
    if (key == "aligned") {
        return Kind::Aligned;
    }
    if (key == "horizontal") {
        return Kind::Horizontal;
    }
    if (key == "vertical") {
        return Kind::Vertical;
    }
    if (key == "angular") {
        return Kind::Angular;
    }
    if (key == "segment") {
        return Kind::Segment;
    }
    return std::nullopt; // "auto" is not a stored kind
}

char const *kind_key(Kind kind)
{
    switch (kind) {
        case Kind::Aligned:
            return "aligned";
        case Kind::Horizontal:
            return "horizontal";
        case Kind::Vertical:
            return "vertical";
        case Kind::Angular:
            return "angular";
        case Kind::Segment:
            return "segment";
    }
    return "";
}

Kind resolve_auto(Geom::Point p1, Geom::Point p2)
{
    return std::abs(p2.x() - p1.x()) >= std::abs(p2.y() - p1.y()) ? Kind::Horizontal : Kind::Vertical;
}

std::optional<Anchor> parse_anchor(std::string_view spec)
{
    auto const colon = spec.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view const prefix = spec.substr(0, colon);
    std::string_view const value = spec.substr(colon + 1);

    Anchor anchor;
    if (prefix == "bbox") {
        for (int i = 0; i < 9; ++i) {
            if (value == HANDLE_NAMES[i]) {
                anchor.type = Anchor::BBox;
                anchor.handle = i;
                return anchor;
            }
        }
        return std::nullopt;
    }
    if (prefix == "node") {
        // Strict non-negative decimal integer; rejects "", signs, garbage.
        if (value.empty() || value.size() > 9) {
            return std::nullopt;
        }
        int k = 0;
        for (char const c : value) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            k = k * 10 + (c - '0');
        }
        anchor.type = Anchor::Node;
        anchor.node = k;
        return anchor;
    }
    if (prefix == "rel") {
        auto const slash = value.find('/');
        if (slash == std::string_view::npos || slash == 0 || slash + 1 >= value.size()) {
            return std::nullopt;
        }
        std::string_view const u_str = value.substr(0, slash);
        std::string_view const v_str = value.substr(slash + 1);
        if (u_str.empty() || v_str.empty() || v_str.find('/') != std::string_view::npos) {
            return std::nullopt;
        }
        std::string const u_buf(u_str);
        std::string const v_buf(v_str);
        char *end = nullptr;
        double const u = g_ascii_strtod(u_buf.c_str(), &end);
        if (end != u_buf.c_str() + u_buf.size()) {
            return std::nullopt;
        }
        end = nullptr;
        double const v = g_ascii_strtod(v_buf.c_str(), &end);
        if (end != v_buf.c_str() + v_buf.size()) {
            return std::nullopt;
        }
        if (!std::isfinite(u) || !std::isfinite(v)) {
            return std::nullopt; // rejects NaN and inf
        }
        anchor.type = Anchor::Rel;
        anchor.u = u;
        anchor.v = v;
        return anchor;
    }
    return std::nullopt;
}

std::string format_anchor(Anchor const &anchor)
{
    switch (anchor.type) {
        case Anchor::BBox:
            if (anchor.handle >= 0 && anchor.handle < 9) {
                return "bbox:" + std::string(HANDLE_NAMES[anchor.handle]);
            }
            break;
        case Anchor::Node:
            if (anchor.node >= 0) {
                return "node:" + std::to_string(anchor.node);
            }
            break;
        case Anchor::Rel:
            char buf[G_ASCII_DTOSTR_BUF_SIZE];
            std::string out = "rel:";
            out += g_ascii_dtostr(buf, sizeof(buf), anchor.u);
            out += '/';
            out += g_ascii_dtostr(buf, sizeof(buf), anchor.v);
            return out;
    }
    return {};
}

std::vector<Geom::Point> curve_nodes(Geom::PathVector const &curve_doc)
{
    std::vector<Geom::Point> nodes;
    for (auto const &path : curve_doc) {
        if (path.empty()) {
            continue;
        }
        nodes.push_back(path.initialPoint());
        // Every segment end point; a closed subpath's wrap-around end (the
        // closing segment's end, or the last real segment ending on the
        // start point) is not counted twice.
        size_t segments = path.size();
        if (path.closed() && segments > 0 && Geom::are_near(path.curveAt(segments - 1).finalPoint(), path.initialPoint())) {
            --segments;
        }
        for (size_t i = 0; i < segments; ++i) {
            nodes.push_back(path.curveAt(i).finalPoint());
        }
    }
    return nodes;
}

std::optional<Geom::Point> resolve_anchor(Anchor const &anchor, Geom::OptRect const &geom_bbox_doc,
                                          Geom::PathVector const &curve_doc)
{
    switch (anchor.type) {
        case Anchor::BBox:
            if (!geom_bbox_doc || anchor.handle < 0 || anchor.handle > 8) {
                return std::nullopt;
            }
            {
                double const xs[3] = {geom_bbox_doc->left(), geom_bbox_doc->midpoint().x(), geom_bbox_doc->right()};
                double const ys[3] = {geom_bbox_doc->top(), geom_bbox_doc->midpoint().y(), geom_bbox_doc->bottom()};
                return Geom::Point(xs[anchor.handle % 3], ys[anchor.handle / 3]);
            }
        case Anchor::Node: {
            auto const nodes = curve_nodes(curve_doc);
            if (anchor.node < 0 || static_cast<size_t>(anchor.node) >= nodes.size()) {
                return std::nullopt;
            }
            return nodes[anchor.node];
        }
        case Anchor::Rel:
            if (!geom_bbox_doc) {
                return std::nullopt;
            }
            return Geom::Point(geom_bbox_doc->left() + anchor.u * geom_bbox_doc->width(),
                               geom_bbox_doc->top() + anchor.v * geom_bbox_doc->height());
    }
    return std::nullopt;
}

Result build(Kind kind, std::span<Geom::Point const> pts, Style const &style, bool y_down)
{
    for (auto const &p : pts) {
        if (!is_finite(p)) {
            return {};
        }
    }

    if (kind == Kind::Angular) {
        if (pts.size() != 3) {
            return {};
        }
        return build_angular(pts[0], pts[1], pts[2], style, y_down);
    }
    if (pts.size() != 2) {
        return {};
    }
    return build_linear(kind, pts[0], pts[1], style, y_down);
}

Glib::ustring format_label(double value, int precision, Glib::ustring const &unit_abbr, bool show_units,
                           Glib::ustring const &prefix, Glib::ustring const &suffix, bool angular)
{
    precision = std::clamp(precision, 0, 6);

    Glib::ustring num = Inkscape::ustring::format_classic(std::fixed, std::setprecision(precision), value);
    // Normalise "-0.00" (any negative all-zero rendering) to positive.
    if (num.size() > 1 && num[0] == '-') {
        bool zero = true;
        for (char const c : num.substr(1)) {
            if (c != '0' && c != '.') {
                zero = false;
                break;
            }
        }
        if (zero) {
            num.erase(0, 1);
        }
    }

    Glib::ustring out = prefix;
    out += num;
    if (show_units) {
        // "°" is two UTF-8 bytes; build it explicitly to stay encoding-safe.
        Glib::ustring const degree = Glib::ustring("\xC2\xB0");
        Glib::ustring const shown = (angular && unit_abbr.empty()) ? degree : unit_abbr;
        if (!shown.empty()) {
            if (!angular) {
                out += ' ';
            }
            out += shown;
        }
    }
    out += suffix;
    return out;
}

double to_unit(double length_uu, double doc_scale_uu_to_px, Glib::ustring const &unit, double drawing_scale)
{
    return Inkscape::Util::Quantity::convert(length_uu * doc_scale_uu_to_px, "px", unit) * drawing_scale;
}

} // namespace Inkscape::LivePathEffect::Dimension

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4