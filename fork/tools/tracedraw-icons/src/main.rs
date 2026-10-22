// SPDX-License-Identifier: GPL-2.0-or-later
//! Render TraceDraw's painter icons headlessly and write them as SVG.
#![allow(dead_code)]
mod i18n;
mod icons;
mod theme;
mod tools;

use egui::epaint::{Shape, StrokeKind};
use egui::{Color32, Pos2, Rect, Vec2};
use std::fmt::Write;

const PX: f32 = 64.0; // render size; output viewBox is 16x16

fn col(c: Color32) -> String {
    if c.a() == 255 {
        format!("#{:02x}{:02x}{:02x}", c.r(), c.g(), c.b())
    } else {
        // egui colours are premultiplied
        let a = c.a() as f32 / 255.0;
        let un = |v: u8| ((v as f32 / a).round().min(255.0)) as u8;
        format!("#{:02x}{:02x}{:02x}\" fill-opacity=\"{:.3}\" stroke-opacity=\"{:.3}", un(c.r()), un(c.g()), un(c.b()), a, a)
    }
}
fn pt(p: Pos2, o: Pos2) -> (f32, f32) {
    ((p.x - o.x) * 16.0 / PX, (p.y - o.y) * 16.0 / PX)
}
fn cm(c: &egui::epaint::ColorMode) -> Color32 {
    match c { egui::epaint::ColorMode::Solid(c) => *c, _ => panic!("textured stroke") }
}
fn w(v: f32) -> f32 { v * 16.0 / PX }

fn emit(out: &mut String, s: &Shape, o: Pos2) {
    match s {
        Shape::Noop => {}
        Shape::Vec(v) => for x in v { emit(out, x, o) },
        Shape::LineSegment { points, stroke } => {
            let (a, b) = (pt(points[0], o), pt(points[1], o));
            let _ = writeln!(out, r#"<line x1="{:.3}" y1="{:.3}" x2="{:.3}" y2="{:.3}" stroke="{}" stroke-width="{:.3}" stroke-linecap="round"/>"#, a.0, a.1, b.0, b.1, col(stroke.color), w(stroke.width));
        }
        Shape::Path(p) => {
            let mut d = String::new();
            for (i, q) in p.points.iter().enumerate() {
                let q = pt(*q, o);
                let _ = write!(d, "{}{:.3},{:.3} ", if i == 0 { "M" } else { "L" }, q.0, q.1);
            }
            if p.closed { d.push('Z'); }
            let fill = if p.fill.a() > 0 { col(p.fill) } else { "none".into() };
            let stroke = if p.stroke.width > 0.0 && cm(&p.stroke.color).a() > 0 {
                format!(r#" stroke="{}" stroke-width="{:.3}" stroke-linejoin="round" stroke-linecap="round""#, col(cm(&p.stroke.color)), w(p.stroke.width))
            } else { String::new() };
            let _ = writeln!(out, r#"<path d="{}" fill="{}"{}/>"#, d.trim(), fill, stroke);
        }
        Shape::Circle(c) => {
            let q = pt(c.center, o);
            let fill = if c.fill.a() > 0 { col(c.fill) } else { "none".into() };
            let stroke = if c.stroke.width > 0.0 { format!(r#" stroke="{}" stroke-width="{:.3}""#, col(c.stroke.color), w(c.stroke.width)) } else { String::new() };
            let _ = writeln!(out, r#"<circle cx="{:.3}" cy="{:.3}" r="{:.3}" fill="{}"{}/>"#, q.0, q.1, w(c.radius), fill, stroke);
        }
        Shape::Rect(r) => {
            let mut rect = r.rect;
            if r.stroke.width > 0.0 {
                match r.stroke_kind {
                    StrokeKind::Inside => rect = rect.shrink(r.stroke.width / 2.0),
                    StrokeKind::Outside => rect = rect.expand(r.stroke.width / 2.0),
                    StrokeKind::Middle => {}
                }
            }
            let a = pt(rect.min, o);
            let fill = if r.fill.a() > 0 { col(r.fill) } else { "none".into() };
            let stroke = if r.stroke.width > 0.0 { format!(r#" stroke="{}" stroke-width="{:.3}""#, col(r.stroke.color), w(r.stroke.width)) } else { String::new() };
            let _ = writeln!(out, r#"<rect x="{:.3}" y="{:.3}" width="{:.3}" height="{:.3}" rx="{:.3}" fill="{}"{}/>"#, a.0, a.1, w(rect.width()), w(rect.height()), w(r.corner_radius.nw as f32), fill, stroke);
        }
        Shape::Ellipse(e) => {
            let q = pt(e.center, o);
            let fill = if e.fill.a() > 0 { col(e.fill) } else { "none".into() };
            let stroke = if e.stroke.width > 0.0 { format!(r#" stroke="{}" stroke-width="{:.3}""#, col(e.stroke.color), w(e.stroke.width)) } else { String::new() };
            let _ = writeln!(out, r#"<ellipse cx="{:.3}" cy="{:.3}" rx="{:.3}" ry="{:.3}" fill="{}"{}/>"#, q.0, q.1, w(e.radius.x), w(e.radius.y), fill, stroke);
        }
        Shape::QuadraticBezier(b) => {
            let p: Vec<_> = b.points.iter().map(|q| pt(*q, o)).collect();
            let fill = if b.fill.a() > 0 { col(b.fill) } else { "none".into() };
            let _ = writeln!(out, r#"<path d="M{:.3},{:.3} Q{:.3},{:.3} {:.3},{:.3}{}" fill="{}" stroke="{}" stroke-width="{:.3}" stroke-linecap="round"/>"#, p[0].0, p[0].1, p[1].0, p[1].1, p[2].0, p[2].1, if b.closed {"Z"} else {""}, fill, col(cm(&b.stroke.color)), w(b.stroke.width));
        }
        Shape::CubicBezier(b) => {
            let p: Vec<_> = b.points.iter().map(|q| pt(*q, o)).collect();
            let fill = if b.fill.a() > 0 { col(b.fill) } else { "none".into() };
            let _ = writeln!(out, r#"<path d="M{:.3},{:.3} C{:.3},{:.3} {:.3},{:.3} {:.3},{:.3}{}" fill="{}" stroke="{}" stroke-width="{:.3}" stroke-linecap="round"/>"#, p[0].0, p[0].1, p[1].0, p[1].1, p[2].0, p[2].1, p[3].0, p[3].1, if b.closed {"Z"} else {""}, fill, col(cm(&b.stroke.color)), w(b.stroke.width));
        }
        Shape::Text(t) => {
            // Only short labels ("PDF" and the like): keep the glyph run as text.
            let q = pt(t.pos, o);
            let text = t.galley.job.text.clone();
            let size = t.galley.job.sections.first().map(|s| s.format.font_id.size).unwrap_or(PX * 0.42);
            let color = t.galley.job.sections.first().map(|s| s.format.color).unwrap_or(t.fallback_color);
            let color = if color == Color32::PLACEHOLDER { t.fallback_color } else { color };
            let _ = writeln!(out, r#"<text x="{:.3}" y="{:.3}" font-family="sans-serif" font-weight="bold" font-size="{:.3}" fill="{}" dominant-baseline="hanging">{}</text>"#, q.0, q.1, w(size), col(color), text);
        }
        other => eprintln!("unhandled shape: {:?}", std::mem::discriminant(other)),
    }
}

fn render(name: &str, dir: &str, f: &dyn Fn(&egui::Painter, Rect)) {
    let ctx = egui::Context::default();
    let mut shapes = Vec::new();
    let pass = || {
        ctx.begin_pass(egui::RawInput::default());
        let painter = ctx.layer_painter(egui::LayerId::background());
        let r = Rect::from_min_size(Pos2::new(100.0, 100.0), Vec2::splat(PX));
        f(&painter, r);
        let mut o = ctx.end_pass();
        o.textures_delta.clear();
        o
    };
    let _ = pass();
    let out_full = pass();
    for cs in out_full.shapes { shapes.push(cs.shape); }
    let mut body = String::new();
    for s in &shapes { emit(&mut body, s, Pos2::new(100.0, 100.0)); }
    let svg = format!("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">\n{}</svg>\n", body);
    std::fs::write(format!("{}/{}.svg", dir, name), svg).unwrap();
}

fn main() {
    let dir = std::env::args().nth(1).expect("output dir");
    std::fs::create_dir_all(format!("{dir}/tools")).unwrap();
    std::fs::create_dir_all(format!("{dir}/actions")).unwrap();
    let c = theme::Tokens::icon();
    for t in tools::Tool::ALL {
        render(t.id(), &format!("{dir}/tools"), &|p, r| icons::draw(p, r, t, c));
    }
    use icons::Action::*;
    let acts = [("new", New), ("open", Open), ("save", Save), ("print", Print), ("cut", Cut), ("copy", Copy), ("paste", Paste), ("undo", Undo), ("redo", Redo), ("import", Import), ("export", Export), ("pdf", Pdf), ("snap-off", SnapOff), ("options", Options), ("welcome", Welcome), ("fullscreen", Fullscreen), ("show-rulers", ShowRulers), ("show-grid", ShowGrid), ("show-guidelines", ShowGuidelines), ("launch", Launch)];
    for (n, a) in acts {
        render(n, &format!("{dir}/actions"), &|p, r| icons::draw_action(p, r, a, c));
    }
}
