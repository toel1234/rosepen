// SPDX-License-Identifier: GPL-2.0-or-later
// Page-SVG post-pass: ids, layer markup, hidden layers and source colours.
// Copyright (C) 2026 Authors
// Released under GNU GPL v2+, read the file 'COPYING' for more information.
// The linked TraceDraw crates are MIT OR Apache-2.0 (see THIRD-PARTY.md).
//
// The serializer's pages carry bare integer ids, drop hidden layers and
// paint CMYK and spot colours as their sRGB preview. This pass rewrites that
// text (no cache, one pass):
//
//  1. every `id="<digits>"` becomes `id="td<digits>"`; the serializer's
//     `grad<N>`, `clip<N>`, `mask<N>` and `maskinv<N>` ids and the
//     `url(#...)` references to them are left alone, since references never
//     use a bare shape id;
//  2. each top-level layer group gains `inkscape:groupmode="layer"` and
//     `inkscape:label` (the source layer name), and the root declares the
//     namespace;
//  3. a layer that was hidden in the file gets `style="display:none"` (the
//     caller serialises a clone with every layer visible, so its shapes are
//     there);
//  4. an element whose paint is exactly the sRGB preview of a CMYK or spot
//     paint gets `data-rp-fill` / `data-rp-stroke` with the source numbers
//     (`cmyk:c,m,y,k` or `spot:tint,c,m,y,k,<percent-encoded ink name>`),
//     guarded by comparing the element's own paint with that preview;
//  5. nothing else is touched.

#![forbid(unsafe_code)]

use std::collections::{HashMap, HashSet};

use tracedraw_core::document::{Ink, Shape, ShapeKind};
use tracedraw_core::{Color, Document, Fill, LayerId};

/// The namespace the layer attributes live in.
pub const INKSCAPE_NS: &str = "http://www.inkscape.org/namespaces/inkscape";

#[derive(Clone, Copy, PartialEq, Eq)]
enum Slot {
    Fill,
    Stroke,
}

/// One source paint, with the sRGB preview the serializer writes for it.
struct Mark {
    slot: Slot,
    hex: String,
    value: String,
}

/// Annotate one page of `doc` (the visibility-normalised clone the caller
/// holds) as serialised by `page_to_svg`.
pub fn annotate(svg: &str, doc: &Document, page_index: usize, hidden_layers: &[LayerId]) -> String {
    let Some(page) = doc.pages.get(page_index) else {
        return svg.to_string();
    };
    let layers = doc.layers_for_page(page.id).unwrap_or_default();
    let mut layer_ids: HashSet<u64> = HashSet::new();
    let mut paints: HashMap<u64, Vec<Mark>> = HashMap::new();
    for layer in &layers {
        layer_ids.insert(layer.id.raw());
        for shape in &layer.shapes {
            collect_marks(shape, &doc.inks, &mut paints);
        }
    }
    let hidden: HashSet<u64> = hidden_layers.iter().map(|l| l.raw()).collect();
    scan(svg, &layer_ids, &hidden, &paints)
}

/// Collect the source paint of every shape, parts included, under its id.
fn collect_marks(shape: &Shape, inks: &[Ink], out: &mut HashMap<u64, Vec<Mark>>) {
    if let Fill::Solid(color) = shape.fill {
        if let Some(value) = paint_value(color, inks) {
            push_mark(out, shape.id.raw(), Slot::Fill, color.to_hex(), value);
        }
    }
    if let Some(stroke) = &shape.stroke {
        // The serializer paints an outline with its own colour unless it has
        // a gradient, pattern, texture or mesh of its own.
        let by_colour = stroke
            .fill
            .as_deref()
            .is_none_or(|f| matches!(f, Fill::None | Fill::Solid(_)));
        if by_colour {
            if let Some(value) = paint_value(stroke.color, inks) {
                push_mark(
                    out,
                    shape.id.raw(),
                    Slot::Stroke,
                    stroke.color.to_hex(),
                    value,
                );
            }
        }
    }
    match &shape.kind {
        ShapeKind::Group { children } => {
            for child in children {
                collect_marks(child, inks, out);
            }
        }
        ShapeKind::ClipFrame { frame, contents } => {
            collect_marks(frame, inks, out);
            for child in contents {
                collect_marks(child, inks, out);
            }
        }
        _ => {}
    }
}

fn push_mark(out: &mut HashMap<u64, Vec<Mark>>, id: u64, slot: Slot, hex: String, value: String) {
    let marks = out.entry(id).or_default();
    if marks
        .iter()
        .any(|m| m.slot == slot && m.hex == hex && m.value == value)
    {
        return;
    }
    marks.push(Mark { slot, hex, value });
}

/// The `data-rp-*` value of a colour, when the model carries source numbers.
fn paint_value(color: Color, inks: &[Ink]) -> Option<String> {
    match color {
        Color::Cmyk { c, m, y, k } => Some(format!(
            "cmyk:{},{},{},{}",
            number(c),
            number(m),
            number(y),
            number(k)
        )),
        Color::Spot {
            ink, tint, c, m, y, k, ..
        } => {
            let name = inks.iter().find(|i| i.id == ink)?.name.as_str();
            Some(format!(
                "spot:{},{},{},{},{},{}",
                number(tint),
                number(c),
                number(m),
                number(y),
                number(k),
                percent_encode(name)
            ))
        }
        _ => None,
    }
}

/// Six decimals, trailing zeros dropped: `0.1`, `1`, `0.25`.
fn number(v: f32) -> String {
    let mut s = format!("{:.6}", f64::from(v));
    if s.contains('.') {
        while s.ends_with('0') {
            s.pop();
        }
        if s.ends_with('.') {
            s.pop();
        }
    }
    if s == "-0" {
        s = "0".to_string();
    }
    s
}

/// Percent-encode what the comma grammar cannot carry: everything but
/// unreserved ASCII and UTF-8 bytes.
fn percent_encode(name: &str) -> String {
    let mut out: Vec<u8> = Vec::with_capacity(name.len());
    for &b in name.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') || b >= 0x80 {
            out.push(b);
        } else {
            out.extend_from_slice(format!("%{b:02X}").as_bytes());
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The inverse of [`percent_encode`], for readers and tests.
pub fn percent_decode(name: &str) -> String {
    let bytes = name.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).ok();
            if let Some(v) = hex.and_then(|h| u8::from_str_radix(h, 16).ok()) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The token pass over the serializer's text.
fn scan(
    svg: &str,
    layer_ids: &HashSet<u64>,
    hidden: &HashSet<u64>,
    paints: &HashMap<u64, Vec<Mark>>,
) -> String {
    let mut out = String::with_capacity(svg.len() + svg.len() / 8);
    let mut i = 0;
    while let Some(pos) = svg[i..].find('<') {
        let lt = i + pos;
        out.push_str(&svg[i..lt]);
        let rest = &svg[lt..];
        if rest.starts_with("</") || rest.starts_with("<!") || rest.starts_with("<?") {
            let Some(end_rel) = rest.find('>') else {
                out.push_str(rest);
                return out;
            };
            let end = lt + end_rel + 1;
            out.push_str(&svg[lt..end]);
            i = end;
            continue;
        }
        let Some(end_rel) = rest.find('>') else {
            out.push_str(rest);
            return out;
        };
        let end = lt + end_rel + 1;
        out.push_str(&rewrite_start_tag(
            &svg[lt..end],
            layer_ids,
            hidden,
            paints,
        ));
        i = end;
    }
    out.push_str(&svg[i..]);
    out
}

/// Rewrite one start tag: namespace on the root, id prefix, extra attributes.
fn rewrite_start_tag(
    tag: &str,
    layer_ids: &HashSet<u64>,
    hidden: &HashSet<u64>,
    paints: &HashMap<u64, Vec<Mark>>,
) -> String {
    let mut with_ns = String::new();
    let tag = if is_svg_root(tag) {
        with_ns.push_str("<svg");
        with_ns.push_str(" xmlns:inkscape=\"");
        with_ns.push_str(INKSCAPE_NS);
        with_ns.push('"');
        with_ns.push_str(&tag[4..]);
        with_ns.as_str()
    } else {
        tag
    };

    let Some((id_start, id_end, id)) = find_id(tag) else {
        return tag.to_string();
    };

    let mut extra = String::new();
    let is_layer_group = tag.starts_with("<g ") && tag.contains(" data-name=\"") && layer_ids.contains(&id);
    if is_layer_group {
        if let Some(name) = attr_value(tag, "data-name") {
            extra.push_str(" inkscape:groupmode=\"layer\" inkscape:label=\"");
            extra.push_str(name);
            extra.push('"');
        }
        if hidden.contains(&id) {
            extra.push_str(" style=\"display:none\"");
        }
    } else if let Some(marks) = paints.get(&id) {
        for slot in [Slot::Fill, Slot::Stroke] {
            let own = match slot {
                Slot::Fill => attr_value(tag, "fill"),
                Slot::Stroke => attr_value(tag, "stroke"),
            };
            let Some(own) = own else { continue };
            let Some(mark) = marks.iter().find(|m| m.slot == slot && m.hex == own) else {
                continue;
            };
            extra.push_str(match slot {
                Slot::Fill => " data-rp-fill=\"",
                Slot::Stroke => " data-rp-stroke=\"",
            });
            extra.push_str(&mark.value);
            extra.push('"');
        }
    }

    let mut out = String::with_capacity(tag.len() + extra.len() + 2);
    out.push_str(&tag[..id_start]);
    out.push_str("td");
    out.push_str(&tag[id_start..id_end]);
    out.push_str(&tag[id_end..id_end + 1]);
    out.push_str(&extra);
    out.push_str(&tag[id_end + 1..]);
    out
}

fn is_svg_root(tag: &str) -> bool {
    tag.starts_with("<svg") && matches!(tag.as_bytes().get(4), Some(b' ') | Some(b'>'))
}

/// The byte range of a bare integer `id="<digits>"` value, and its number.
fn find_id(tag: &str) -> Option<(usize, usize, u64)> {
    let bytes = tag.as_bytes();
    let mut from = 0;
    while let Some(pos) = tag[from..].find(" id=\"") {
        let start = from + pos + 5;
        let mut end = start;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            end += 1;
        }
        if end > start && bytes.get(end) == Some(&b'"') {
            return Some((start, end, tag[start..end].parse().ok()?));
        }
        from = from + pos + 1;
    }
    None
}

/// The raw value of `name="..."` (the serializer escapes `"` inside).
fn attr_value<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
    let pattern = format!(" {name}=\"");
    let start = tag.find(&pattern)? + pattern.len();
    let end = start + tag[start..].find('"')?;
    Some(&tag[start..end])
}

#[cfg(test)]
mod tests {
    use super::*;
    use tracedraw_core::document::{Layer, Shape};
    use tracedraw_core::geometry::{Affine, Rect, Size};
    use tracedraw_core::{ShapeId, ShapeKind, Stroke};
    use tracedraw_io::svg::page_to_svg;

    fn layer(doc: &mut Document, name: &str, visible: bool) -> usize {
        let mut ids = doc.ids().clone();
        let l = Layer {
            id: ids.layer(),
            name: name.to_string(),
            visible,
            printable: true,
            locked: false,
            shapes: Vec::new(),
            scope: Default::default(),
        };
        doc.set_ids(ids);
        doc.pages[0].layers.push(l);
        doc.pages[0].layers.len() - 1
    }

    fn rect(id: u64, fill: Fill) -> Shape {
        let mut s = Shape::new(
            ShapeId(id),
            ShapeKind::Rect {
                rect: Rect::new(10.0, 10.0, 40.0, 40.0),
                radius: 0.0,
                corners: None,
            },
        );
        s.fill = fill;
        s.stroke = None;
        s
    }

    /// Serialise with every layer visible, as the library does.
    fn annotate_doc(doc: &Document) -> String {
        let mut clone = doc.clone();
        let mut hidden = Vec::new();
        for l in clone.all_layers_mut() {
            if !l.visible {
                hidden.push(l.id);
                l.visible = true;
            }
        }
        let svg = page_to_svg(&clone, 0);
        annotate(&svg, &clone, 0, &hidden)
    }

    #[test]
    fn cmyk_fill_becomes_a_data_attribute() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.pages[0].layers[0].name = "Ink".into();
        doc.pages[0].layers[0].shapes = vec![rect(
            7,
            Fill::Solid(Color::Cmyk {
                c: 0.1,
                m: 0.2,
                y: 0.3,
                k: 0.4,
            }),
        )];
        let out = annotate_doc(&doc);
        assert!(out.contains("id=\"td7\""), "{out}");
        assert!(
            out.contains("data-rp-fill=\"cmyk:0.1,0.2,0.3,0.4\""),
            "{out}"
        );
        assert!(out.contains("inkscape:groupmode=\"layer\""), "{out}");
        assert!(out.contains("inkscape:label=\"Ink\""), "{out}");
        assert!(out.contains("xmlns:inkscape=\""), "{out}");
    }

    #[test]
    fn rgb_paints_stay_plain() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.pages[0].layers[0].shapes = vec![rect(3, Fill::Solid(Color::rgb8(0x33, 0x66, 0xcc)))];
        let out = annotate_doc(&doc);
        assert!(!out.contains("data-rp-"), "{out}");
    }

    #[test]
    fn cmyk_stroke_becomes_a_data_attribute() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        let mut s = Shape::new(
            ShapeId(9),
            ShapeKind::Rect {
                rect: Rect::new(10.0, 10.0, 40.0, 40.0),
                radius: 0.0,
                corners: None,
            },
        );
        s.fill = Fill::None;
        s.stroke = Some(Stroke::new(
            Color::Cmyk {
                c: 0.0,
                m: 0.0,
                y: 0.0,
                k: 1.0,
            },
            1.0,
        ));
        doc.pages[0].layers[0].shapes = vec![s];
        let out = annotate_doc(&doc);
        assert!(
            out.contains("data-rp-stroke=\"cmyk:0,0,0,1\""),
            "{out}"
        );
        assert!(!out.contains("data-rp-fill="), "{out}");
    }

    #[test]
    fn spot_ink_names_are_percent_encoded() {
        let name = "Test Ink, 50%\"&<x>";
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.inks = vec![Ink {
            id: 3,
            name: name.to_string(),
            cmyk: [0.0, 0.5, 1.0, 0.0],
        }];
        let spot = |tint| {
            Color::Spot {
                ink: 3,
                tint,
                c: 0.0,
                m: 0.5,
                y: 1.0,
                k: 0.0,
            }
        };
        let mut s = Shape::new(
            ShapeId(11),
            ShapeKind::Ellipse {
                rect: Rect::new(10.0, 10.0, 40.0, 40.0),
                arc: None,
            },
        );
        s.fill = Fill::Solid(spot(1.0));
        s.stroke = None;
        doc.pages[0].layers[0].shapes = vec![s];
        let out = annotate_doc(&doc);
        let want = format!(
            "data-rp-fill=\"spot:1,0,0.5,1,0,{}\"",
            percent_encode(name)
        );
        assert!(out.contains(&want), "{out}");
        assert!(out.contains("Test%20Ink"), "{out}");
        assert_eq!(
            percent_decode(&percent_encode(name)),
            name,
            "round trip"
        );

        // A tint is carried too.
        doc.pages[0].layers[0].shapes[0].fill = Fill::Solid(spot(0.5));
        let out = annotate_doc(&doc);
        assert!(
            out.contains("data-rp-fill=\"spot:0.5,0,0.5,1,0,Test%20Ink"),
            "{out}"
        );
    }

    #[test]
    fn hidden_layers_are_kept_and_marked() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.pages[0].layers[0].name = "Artwork".into();
        doc.pages[0].layers[0].shapes = vec![rect(5, Fill::Solid(Color::Cmyk {
            c: 0.1,
            m: 0.2,
            y: 0.3,
            k: 0.4,
        }))];
        let idx = layer(&mut doc, "Hidden notes", false);
        doc.pages[0].layers[idx].shapes = vec![rect(6, Fill::None)];
        let out = annotate_doc(&doc);
        assert!(out.contains("inkscape:label=\"Hidden notes\""), "{out}");
        assert!(out.contains("style=\"display:none\""), "{out}");
        assert!(out.contains("id=\"td6\""), "{out}");
        assert!(out.contains("Artwork"), "{out}");
    }

    #[test]
    fn layer_names_are_xml_escaped() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.pages[0].layers[0].name = "A & B <c> \"d\"".into();
        let out = annotate_doc(&doc);
        assert!(
            out.contains("inkscape:label=\"A &amp; B &lt;c&gt; &quot;d&quot;\""),
            "{out}"
        );
    }

    #[test]
    fn every_id_is_prefixed_and_the_result_is_well_formed_xml() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        doc.pages[0].layers[0].shapes = vec![
            rect(1, Fill::Solid(Color::rgb8(1, 2, 3))),
            rect(2, Fill::None),
        ];
        let out = annotate_doc(&doc);

        // No bare integer id survives.
        let mut rest = out.as_str();
        while let Some(pos) = rest.find("id=\"") {
            let value = &rest[pos + 4..];
            let end = value.find('"').unwrap();
            let value = &value[..end];
            assert!(
                !value.chars().all(|c| c.is_ascii_digit()),
                "bare id {value} in {out}"
            );
            assert!(
                value.starts_with("td")
                    || value.starts_with("grad")
                    || value.starts_with("clip")
                    || value.starts_with("mask")
                    || value.starts_with("maskinv"),
                "unexpected id {value}"
            );
            rest = &value[end..];
        }

        let parsed = roxmltree::Document::parse(&out).expect("parses as XML");
        for element in parsed.descendants().filter(|n| n.is_element()) {
            let mut seen: Vec<String> = Vec::new();
            for attr in element.attributes() {
                let name = attr.name().to_string();
                assert!(!seen.contains(&name), "duplicate attribute {name}");
                seen.push(name);
            }
        }
    }

    #[test]
    fn ids_inside_references_are_untouched() {
        let mut doc = Document::new("t", Size::new(100.0, 100.0));
        let mut s = Shape::new(
            ShapeId(4),
            ShapeKind::Path {
                path: {
                    let mut p = tracedraw_core::BezPath::new();
                    p.move_to((0.0, 0.0));
                    p.line_to((10.0, 0.0));
                    p.line_to((10.0, 10.0));
                    p.close_path();
                    p
                },
                closed: true,
            },
        );
        s.fill = Fill::Fountain(tracedraw_core::Fountain::two(
            tracedraw_core::FountainKind::Linear,
            Color::BLACK,
            Color::WHITE,
            0.0,
        ));
        s.transform = Affine::IDENTITY;
        doc.pages[0].layers[0].shapes = vec![s];
        let out = annotate_doc(&doc);
        assert!(out.contains("url(#grad1)"), "{out}");
        assert!(out.contains("id=\"grad1\""), "{out}");
        assert!(out.contains("id=\"td4\""), "{out}");
    }
}
