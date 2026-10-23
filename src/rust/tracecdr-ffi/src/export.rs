// SPDX-License-Identifier: GPL-2.0-or-later
// The "rosepen-cdr-export/1" export model -> drawing-format file builder.
// Copyright (C) 2026 Authors
// Released under GNU GPL v2+, read the file 'COPYING' for more information.
// The linked TraceDraw crates are MIT OR Apache-2.0 (see THIRD-PARTY.md).
//
// Rosepen writes the JSON model (SCHEMA.md, a Rosepen-owned contract); this
// module parses it and builds a document through the public builder API, so
// the internal serde layout of the model is never relied on.
//
// Coordinates: the model is in page millimetres with Y DOWN, the document
// model has Y UP from the bottom-left corner, so every shape is flipped with
// `y' = page_h - y` here (paths through one affine).

#![forbid(unsafe_code)]

use serde::Deserialize;

use tracedraw_core::document::{
    Ink, Layer, MasterScope, Page, ParagraphStyle, Shape, ShapeKind, TextAlign, TextSpan,
};
use tracedraw_core::geometry::{Affine, BezPath, PathEl, Point, Rect, Size};
use tracedraw_core::id::IdSource;
use tracedraw_core::style::{LineCap, LineJoin};
use tracedraw_core::{Color, Document, Fill, Stroke};

/// The schema string this builder accepts.
pub const SCHEMA: &str = "rosepen-cdr-export/1";

/// What went wrong building a document from the model.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportError(pub String);

impl std::fmt::Display for ExportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ExportError {}

/// Build the document, also returning the warnings about what the fallbacks
/// changed (each one line, never empty when something was dropped).
pub fn build(json: &[u8]) -> Result<(Document, Vec<String>), ExportError> {
    let spec: Spec =
        serde_json::from_slice(json).map_err(|e| ExportError(format!("invalid export model: {e}")))?;
    if spec.schema != SCHEMA {
        return Err(ExportError(format!(
            "unsupported export model schema {:?} (expected {SCHEMA})",
            spec.schema
        )));
    }
    if spec.pages.is_empty() {
        return Err(ExportError("export model has no pages".into()));
    }

    let mut warnings = Vec::new();
    let first = &spec.pages[0];
    let title = if spec.title.is_empty() {
        "Untitled".to_string()
    } else {
        spec.title.clone()
    };

    // The writer states one page size for the whole file (it writes no
    // per-page size records), so a document with pages of different sizes
    // goes out as the first page's size, with every page's content kept at
    // the same distance from its top-left corner.
    let (width_mm, height_mm) = (first.width_mm, first.height_mm);
    if spec
        .pages
        .iter()
        .any(|p| (p.width_mm - width_mm).abs() > 1e-9 || (p.height_mm - height_mm).abs() > 1e-9)
    {
        warnings.push(format!(
            "cdr export: the format stores one page size; every page was written as {width_mm}x{height_mm} mm"
        ));
    }

    let mut doc = Document::new(title, Size::new(width_mm, height_mm));
    doc.inks = spec
        .inks
        .iter()
        .enumerate()
        .map(|(i, ink)| Ink {
            id: i as u32,
            name: ink.name.clone(),
            cmyk: ink.cmyk,
        })
        .collect();

    let mut ids = doc.ids().clone();

    // Master layers are not written by the writer (it only knows the master
    // *page* of the format), so their shapes go out as a bottom layer on
    // every page instead of being lost.
    let mut master_shapes: Vec<Shape> = Vec::new();
    if !spec.master.is_empty() {
        for layer in &spec.master {
            for shape in &layer.shapes {
                master_shapes.push(build_shape(
                    shape,
                    &mut ids,
                    &spec.inks,
                    height_mm,
                    &mut warnings,
                )?);
            }
        }
        warnings.push(
            "cdr export: master layers exported as a bottom \"Master\" layer on every page".into(),
        );
    }

    doc.pages.clear();
    for page in &spec.pages {
        let mut layers: Vec<Layer> = Vec::new();
        if !master_shapes.is_empty() {
            layers.push(Layer {
                id: ids.layer(),
                name: "Master".to_string(),
                visible: true,
                printable: true,
                locked: false,
                shapes: master_shapes.clone(),
                scope: MasterScope::All,
            });
        }
        for layer in &page.layers {
            let mut shapes = Vec::with_capacity(layer.shapes.len());
            for shape in &layer.shapes {
                shapes.push(build_shape(
                    shape,
                    &mut ids,
                    &spec.inks,
                    height_mm,
                    &mut warnings,
                )?);
            }
            layers.push(Layer {
                id: ids.layer(),
                name: layer.name.clone(),
                visible: layer.visible,
                printable: true,
                locked: false,
                shapes,
                scope: MasterScope::All,
            });
        }
        doc.pages.push(Page {
            id: ids.page(),
            name: page.name.clone(),
            size: Size::new(width_mm, height_mm),
            layers,
            guides: Vec::new(),
            background: None,
        });
    }
    doc.set_ids(ids);

    Ok((doc, warnings))
}

/// Build the document and write it as a file of `version`.
pub fn write(json: &[u8], version: u16) -> Result<(Vec<u8>, Vec<String>), ExportError> {
    let (doc, warnings) = build(json)?;
    let bytes = tracedraw_cdr::write::document_to_cdr_version(&doc, version);
    Ok((bytes, warnings))
}

fn build_shape(
    spec: &ShapeSpec,
    ids: &mut IdSource,
    inks: &[InkSpec],
    page_h: f64,
    warnings: &mut Vec<String>,
) -> Result<Shape, ExportError> {
    let flip = Affine::new([1.0, 0.0, 0.0, -1.0, 0.0, page_h]);
    let kind = match spec {
        ShapeSpec::Rect { x, y, w, h, rx, .. } => {
            let (x0, x1) = ordered(*x, x + w);
            let (y0, y1) = ordered(page_h - (y + h), page_h - y);
            ShapeKind::Rect {
                rect: Rect::new(x0, y0, x1, y1),
                radius: rx.abs(),
                corners: None,
            }
        }
        ShapeSpec::Ellipse { cx, cy, rx, ry, .. } => {
            let (rx, ry) = (rx.abs(), ry.abs());
            let (cx, cy) = (*cx, page_h - cy);
            ShapeKind::Ellipse {
                rect: Rect::new(cx - rx, cy - ry, cx + rx, cy + ry),
                arc: None,
            }
        }
        ShapeSpec::Path { d, .. } => {
            let path = BezPath::from_svg(d)
                .map_err(|e| ExportError(format!("invalid path data {d:?}: {e}")))?;
            let closed = matches!(path.elements().last(), Some(PathEl::ClosePath));
            let path = flip * path;
            ShapeKind::Path { path, closed }
        }
        ShapeSpec::Text {
            x, y, align, spans, ..
        } => {
            let spans: Vec<TextSpan> = spans
                .iter()
                .map(|s| {
                    let mut span = TextSpan::new(s.text.clone(), s.family.clone(), s.size_pt);
                    span.bold = s.bold;
                    span.italic = s.italic;
                    span
                })
                .collect();
            ShapeKind::Text {
                spans,
                origin: Point::new(*x, page_h - y),
                frame: None,
                align: TextAlign::from(*align),
                para: ParagraphStyle::default(),
                on_path: None,
            }
        }
        ShapeSpec::Group { children, .. } => {
            let mut built = Vec::with_capacity(children.len());
            for child in children {
                built.push(build_shape(child, ids, inks, page_h, warnings)?);
            }
            ShapeKind::Group { children: built }
        }
    };

    let mut shape = Shape::new(ids.shape(), kind);
    shape.name = spec.name().filter(|n| !n.is_empty()).map(str::to_string);
    shape.opacity = spec.opacity().clamp(0.0, 1.0);
    shape.fill = match spec.fill() {
        Some(paint) => Fill::Solid(resolve_paint(paint, inks)?),
        None => Fill::None,
    };
    shape.stroke = match spec.stroke() {
        Some(s) => {
            let mut stroke = Stroke::new(resolve_paint(&s.paint, inks)?, s.width_mm);
            stroke.cap = LineCap::from(s.cap);
            stroke.join = LineJoin::from(s.join);
            stroke.miter_limit = Stroke::miter_angle_from_ratio(s.miter);
            if !s.dash_mm.is_empty() {
                // Dashes are stored in multiples of the outline's width, as
                // the format and the SVG serializer expect.
                let unit = if s.width_mm > 0.0 { s.width_mm } else { 1.0 };
                stroke.dash = s.dash_mm.iter().map(|d| d / unit).collect();
            }
            Some(stroke)
        }
        None => None,
    };
    Ok(shape)
}

fn ordered(a: f64, b: f64) -> (f64, f64) {
    if a <= b {
        (a, b)
    } else {
        (b, a)
    }
}

fn resolve_paint(paint: &Paint, inks: &[InkSpec]) -> Result<Color, ExportError> {
    Ok(match paint {
        Paint::Rgb { r, g, b } => Color::Rgb {
            r: clamp01(*r),
            g: clamp01(*g),
            b: clamp01(*b),
        },
        Paint::Cmyk { c, m, y, k } => Color::Cmyk {
            c: clamp01(*c),
            m: clamp01(*m),
            y: clamp01(*y),
            k: clamp01(*k),
        },
        Paint::Spot { ink, tint } => {
            let entry = inks.get(*ink as usize).ok_or_else(|| {
                ExportError(format!("spot paint names ink {ink}, which the ink table lacks"))
            })?;
            Color::Spot {
                ink: *ink,
                tint: clamp01(*tint),
                c: entry.cmyk[0],
                m: entry.cmyk[1],
                y: entry.cmyk[2],
                k: entry.cmyk[3],
            }
        }
    })
}

fn clamp01(v: f64) -> f32 {
    v.clamp(0.0, 1.0) as f32
}

fn one() -> f64 {
    1.0
}

fn yes() -> bool {
    true
}

#[derive(Deserialize)]
struct Spec {
    schema: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    inks: Vec<InkSpec>,
    #[serde(default)]
    pages: Vec<PageSpec>,
    #[serde(default)]
    master: Vec<LayerSpec>,
}

#[derive(Deserialize)]
struct InkSpec {
    name: String,
    cmyk: [f32; 4],
}

#[derive(Deserialize)]
struct PageSpec {
    #[serde(default)]
    name: String,
    width_mm: f64,
    height_mm: f64,
    #[serde(default)]
    layers: Vec<LayerSpec>,
}

#[derive(Deserialize)]
struct LayerSpec {
    #[serde(default)]
    name: String,
    #[serde(default = "yes")]
    visible: bool,
    #[serde(default)]
    shapes: Vec<ShapeSpec>,
}

#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
enum ShapeSpec {
    Rect {
        #[serde(default)]
        name: Option<String>,
        #[serde(default = "one")]
        opacity: f64,
        #[serde(default)]
        fill: Option<Paint>,
        #[serde(default)]
        stroke: Option<StrokeSpec>,
        x: f64,
        y: f64,
        w: f64,
        h: f64,
        #[serde(default)]
        rx: f64,
    },
    Ellipse {
        #[serde(default)]
        name: Option<String>,
        #[serde(default = "one")]
        opacity: f64,
        #[serde(default)]
        fill: Option<Paint>,
        #[serde(default)]
        stroke: Option<StrokeSpec>,
        cx: f64,
        cy: f64,
        rx: f64,
        ry: f64,
    },
    Path {
        #[serde(default)]
        name: Option<String>,
        #[serde(default = "one")]
        opacity: f64,
        #[serde(default)]
        fill: Option<Paint>,
        #[serde(default)]
        stroke: Option<StrokeSpec>,
        d: String,
    },
    Text {
        #[serde(default)]
        name: Option<String>,
        #[serde(default = "one")]
        opacity: f64,
        #[serde(default)]
        fill: Option<Paint>,
        #[serde(default)]
        stroke: Option<StrokeSpec>,
        x: f64,
        y: f64,
        #[serde(default)]
        align: Align,
        #[serde(default)]
        spans: Vec<SpanSpec>,
    },
    Group {
        #[serde(default)]
        name: Option<String>,
        #[serde(default = "one")]
        opacity: f64,
        #[serde(default)]
        fill: Option<Paint>,
        #[serde(default)]
        stroke: Option<StrokeSpec>,
        #[serde(default)]
        children: Vec<ShapeSpec>,
    },
}

impl ShapeSpec {
    fn name(&self) -> Option<&str> {
        match self {
            ShapeSpec::Rect { name, .. }
            | ShapeSpec::Ellipse { name, .. }
            | ShapeSpec::Path { name, .. }
            | ShapeSpec::Text { name, .. }
            | ShapeSpec::Group { name, .. } => name.as_deref(),
        }
    }

    fn opacity(&self) -> f64 {
        match self {
            ShapeSpec::Rect { opacity, .. }
            | ShapeSpec::Ellipse { opacity, .. }
            | ShapeSpec::Path { opacity, .. }
            | ShapeSpec::Text { opacity, .. }
            | ShapeSpec::Group { opacity, .. } => *opacity,
        }
    }

    fn fill(&self) -> Option<&Paint> {
        match self {
            ShapeSpec::Rect { fill, .. }
            | ShapeSpec::Ellipse { fill, .. }
            | ShapeSpec::Path { fill, .. }
            | ShapeSpec::Text { fill, .. }
            | ShapeSpec::Group { fill, .. } => fill.as_ref(),
        }
    }

    fn stroke(&self) -> Option<&StrokeSpec> {
        match self {
            ShapeSpec::Rect { stroke, .. }
            | ShapeSpec::Ellipse { stroke, .. }
            | ShapeSpec::Path { stroke, .. }
            | ShapeSpec::Text { stroke, .. }
            | ShapeSpec::Group { stroke, .. } => stroke.as_ref(),
        }
    }
}

#[derive(Deserialize)]
struct SpanSpec {
    text: String,
    #[serde(default)]
    family: String,
    size_pt: f64,
    #[serde(default)]
    bold: bool,
    #[serde(default)]
    italic: bool,
}

#[derive(Deserialize)]
#[serde(tag = "model", rename_all = "lowercase")]
enum Paint {
    Rgb { r: f64, g: f64, b: f64 },
    Cmyk { c: f64, m: f64, y: f64, k: f64 },
    Spot { ink: u32, #[serde(default = "one")] tint: f64 },
}

#[derive(Deserialize)]
struct StrokeSpec {
    paint: Paint,
    width_mm: f64,
    #[serde(default)]
    cap: Cap,
    #[serde(default)]
    join: Join,
    #[serde(default = "four")]
    miter: f64,
    #[serde(default)]
    dash_mm: Vec<f64>,
}

fn four() -> f64 {
    4.0
}

#[derive(Deserialize, Default, Clone, Copy)]
#[serde(rename_all = "lowercase")]
enum Cap {
    #[default]
    Butt,
    Round,
    Square,
}

impl From<Cap> for LineCap {
    fn from(cap: Cap) -> Self {
        match cap {
            Cap::Butt => LineCap::Butt,
            Cap::Round => LineCap::Round,
            Cap::Square => LineCap::Square,
        }
    }
}

#[derive(Deserialize, Default, Clone, Copy)]
#[serde(rename_all = "lowercase")]
enum Join {
    #[default]
    Miter,
    Round,
    Bevel,
}

impl From<Join> for LineJoin {
    fn from(join: Join) -> Self {
        match join {
            Join::Miter => LineJoin::Miter,
            Join::Round => LineJoin::Round,
            Join::Bevel => LineJoin::Bevel,
        }
    }
}

#[derive(Deserialize, Default, Clone, Copy)]
#[serde(rename_all = "lowercase")]
enum Align {
    #[default]
    Left,
    Center,
    Right,
}

impl From<Align> for TextAlign {
    fn from(align: Align) -> Self {
        match align {
            Align::Left => TextAlign::Left,
            Align::Center => TextAlign::Center,
            Align::Right => TextAlign::Right,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Two pages, a CMYK rect, a spot ellipse, an RGB path, text, a group and
    /// a dashed translucent outline. Both pages share one size: the writer
    /// states the size once for the whole file.
    fn model() -> String {
        r#"{
          "schema": "rosepen-cdr-export/1",
          "title": "Bridge",
          "inks": [{"name": "Test Ink", "cmyk": [0.0, 0.5, 1.0, 0.0]}],
          "pages": [
            {
              "name": "Page 1", "width_mm": 210.0, "height_mm": 297.0,
              "layers": [
                {
                  "name": "Artwork", "visible": true,
                  "shapes": [
                    {"kind": "rect", "name": "box", "x": 10.0, "y": 20.0, "w": 50.0, "h": 30.0,
                     "fill": {"model": "cmyk", "c": 0.1, "m": 0.2, "y": 0.3, "k": 0.4},
                     "stroke": {"paint": {"model": "cmyk", "c": 0.0, "m": 0.0, "y": 0.0, "k": 1.0},
                                "width_mm": 1.0, "cap": "round", "join": "bevel",
                                "dash_mm": [2.0, 1.0]}},
                    {"kind": "ellipse", "cx": 120.0, "cy": 60.0, "rx": 20.0, "ry": 10.0,
                     "fill": {"model": "spot", "ink": 0, "tint": 1.0}},
                    {"kind": "path", "opacity": 0.5, "d": "M10 10 L40 10 L40 40 Z",
                     "fill": {"model": "rgb", "r": 0.2, "g": 0.4, "b": 0.8}},
                    {"kind": "text", "x": 20.0, "y": 280.0, "align": "left",
                     "spans": [{"text": "Hello", "family": "DejaVu Sans", "size_pt": 24.0, "bold": true}]},
                    {"kind": "group",
                     "children": [
                       {"kind": "rect", "x": 10.0, "y": 200.0, "w": 10.0, "h": 10.0,
                        "fill": {"model": "rgb", "r": 1.0, "g": 0.0, "b": 0.0}},
                       {"kind": "rect", "x": 30.0, "y": 200.0, "w": 10.0, "h": 10.0,
                        "fill": {"model": "rgb", "r": 0.0, "g": 1.0, "b": 0.0}}
                     ]}
                  ]
                },
                {"name": "Hidden notes", "visible": false, "shapes": []}
              ]
            },
            {
              "name": "Page 2", "width_mm": 210.0, "height_mm": 297.0,
              "layers": [
                {"name": "Back", "visible": true,
                 "shapes": [
                   {"kind": "rect", "x": 5.0, "y": 10.0, "w": 10.0, "h": 20.0,
                    "fill": {"model": "rgb", "r": 0.5, "g": 0.5, "b": 0.5}}
                 ]}
              ]
            }
          ]
        }"#
        .to_string()
    }

    fn round_trip(version: u16) -> (Document, tracedraw_cdr::ParseReport) {
        let (bytes, warnings) = write(model().as_bytes(), version).expect("writes");
        assert!(warnings.is_empty(), "{warnings:?}");
        let (doc, report) = tracedraw_cdr::open_bytes(&bytes, "back").expect("reads back");
        assert_eq!(report.version.map(|v| v.0), Some(version));
        assert_eq!(report.skipped_objects, 0, "{version}: {:?}", report.warnings);
        (doc, report)
    }

    #[test]
    fn shapes_round_trip() {
        for version in [13u16, 14, 16, 27] {
            let (doc, _) = round_trip(version);
            assert_eq!(doc.pages.len(), 2, "{version}");
            let p1 = &doc.pages[0];
            assert!(
                (p1.size.width - 210.0).abs() < 0.01 && (p1.size.height - 297.0).abs() < 0.01,
                "{version}: {:?}",
                p1.size
            );
            let p2 = &doc.pages[1];
            assert!(
                (p2.size.width - 210.0).abs() < 0.01 && (p2.size.height - 297.0).abs() < 0.01,
                "{version}: {:?}",
                p2.size
            );
            assert_eq!(p1.layers[0].name, "Artwork", "{version}");
            assert_eq!(p2.layers[0].name, "Back", "{version}");

            let shapes = &p1.layers[0].shapes;
            assert_eq!(shapes.len(), 5, "{version}: {:?}", p1.layers[0].shapes.len());

            // The CMYK fill keeps its numbers (percent steps).
            let fill = match shapes[0].fill {
                Fill::Solid(c) => c,
                ref other => panic!("{version}: {other:?}"),
            };
            match fill {
                Color::Cmyk { c, m, y, k } => {
                    assert!((c - 0.1).abs() < 1e-3, "{version}: {c}");
                    assert!((m - 0.2).abs() < 1e-3, "{version}: {m}");
                    assert!((y - 0.3).abs() < 1e-3, "{version}: {y}");
                    assert!((k - 0.4).abs() < 1e-3, "{version}: {k}");
                }
                other => panic!("{version}: {other:?}"),
            }
            let stroke = shapes[0].stroke.as_ref().expect("outline");
            assert!((stroke.width - 1.0).abs() < 0.01, "{version}");
            assert_eq!(stroke.cap, LineCap::Round, "{version}");
            assert_eq!(stroke.join, LineJoin::Bevel, "{version}");
            assert_eq!(stroke.dash.len(), 2, "{version}");
            assert!((stroke.dash[0] - 2.0).abs() < 0.01, "{version}: {:?}", stroke.dash);
            assert!((stroke.dash[1] - 1.0).abs() < 0.01, "{version}: {:?}", stroke.dash);

            // A spot colour goes out as its process stand-in: the format's
            // writer has no ink table (a FINDING, see SCHEMA.md).
            match shapes[1].fill {
                Fill::Solid(Color::Cmyk { c, m, y, k }) => {
                    assert!((c - 0.0).abs() < 1e-3, "{version}: {c}");
                    assert!((m - 0.5).abs() < 1e-3, "{version}: {m}");
                    assert!((y - 1.0).abs() < 1e-3, "{version}: {y}");
                    assert!((k - 0.0).abs() < 1e-3, "{version}: {k}");
                }
                ref other => panic!("{version}: {other:?}"),
            }

            let group = match &shapes[4].kind {
                ShapeKind::Group { children } => children,
                other => panic!("{version}: not a group: {other:?}"),
            };
            assert_eq!(group.len(), 2, "{version}");
            // An object's own opacity travels in the style string, which the
            // writer emits from version 16; group transparency is not
            // written at all (both are FINDINGS, see SCHEMA.md).
            let want = if version >= 16 { 0.5 } else { 1.0 };
            assert!(
                (shapes[2].opacity - want).abs() < 1e-3,
                "{version}: {}",
                shapes[2].opacity
            );

            // The text keeps its characters, family and size.
            let (spans, origin) = match &shapes[3].kind {
                ShapeKind::Text { spans, origin, .. } => (spans, *origin),
                other => panic!("{version}: not text: {other:?}"),
            };
            assert_eq!(spans[0].text, "Hello", "{version}");
            assert_eq!(spans[0].font_family, "DejaVu Sans", "{version}");
            assert!((spans[0].size_pt - 24.0).abs() < 0.01, "{version}");
            assert!(spans[0].bold, "{version}");
            // The first baseline sits 17 mm above the page's bottom edge.
            // Before version 16 the file keeps the place in the object's
            // transform instead of the text argument, so check the placed
            // point rather than the field.
            let baseline = shapes[3].transform * origin;
            assert!(
                (baseline.y - 17.0).abs() < 0.01,
                "{version}: {origin:?} in {:?}",
                shapes[3].transform
            );
        }
    }

    #[test]
    fn y_axis_is_flipped() {
        // A 50 mm high page; the rect sits at JSON y=10 (its top edge),
        // 20 mm tall, so its bottom edge is 40 mm up in page space.
        let json = r#"{
          "schema": "rosepen-cdr-export/1",
          "pages": [{"name": "Only", "width_mm": 100.0, "height_mm": 50.0,
                     "layers": [{"name": "Art", "shapes": [
                       {"kind": "rect", "x": 5.0, "y": 10.0, "w": 10.0, "h": 20.0,
                        "fill": {"model": "rgb", "r": 0.5, "g": 0.5, "b": 0.5}}]}]}]
        }"#;
        let (bytes, _) = write(json.as_bytes(), 14).expect("writes");
        let (doc, _) = tracedraw_cdr::open_bytes(&bytes, "back").expect("reads back");
        let bounds = doc.pages[0].layers[0].shapes[0].bounds();
        assert!((bounds.y1 - 40.0).abs() < 0.01, "{bounds:?}");
        assert!((bounds.y0 - 20.0).abs() < 0.01, "{bounds:?}");
    }

    #[test]
    fn one_page_size_per_file() {
        // The writer states one page size for the whole file, so pages of
        // another size are written as the first page's and a warning says so.
        let json = r#"{
          "schema": "rosepen-cdr-export/1",
          "pages": [
            {"name": "A", "width_mm": 210.0, "height_mm": 297.0, "layers": []},
            {"name": "B", "width_mm": 100.0, "height_mm": 50.0, "layers": []}
          ]
        }"#;
        let (doc, warnings) = build(json.as_bytes()).expect("builds");
        assert_eq!(warnings.len(), 1);
        assert!(warnings[0].contains("page size"), "{}", warnings[0]);
        let (bytes, _) = write(json.as_bytes(), 14).expect("writes");
        let (back, _) = tracedraw_cdr::open_bytes(&bytes, "back").expect("reads back");
        assert_eq!(back.pages.len(), 2);
        for page in &back.pages {
            assert!((page.size.width - 210.0).abs() < 0.01, "{:?}", page.size);
            assert!((page.size.height - 297.0).abs() < 0.01, "{:?}", page.size);
        }
        assert_eq!(doc.pages.len(), 2);
    }

    #[test]
    fn master_layers_become_a_bottom_layer_with_a_warning() {
        let json = r#"{
          "schema": "rosepen-cdr-export/1",
          "pages": [{"name": "Only", "width_mm": 100.0, "height_mm": 50.0,
                     "layers": [{"name": "Art", "shapes": []}]}],
          "master": [{"name": "Brand", "shapes": [
            {"kind": "rect", "x": 1.0, "y": 1.0, "w": 5.0, "h": 5.0,
             "fill": {"model": "rgb", "r": 0.0, "g": 0.0, "b": 0.0}}]}]
        }"#;
        let (doc, warnings) = build(json.as_bytes()).expect("builds");
        assert_eq!(doc.pages[0].layers[0].name, "Master");
        assert_eq!(doc.pages[0].layers[0].shapes.len(), 1);
        assert_eq!(doc.pages[0].layers[1].name, "Art");
        assert_eq!(warnings.len(), 1);
        assert!(warnings[0].contains("master"), "{}", warnings[0]);
    }

    #[test]
    fn invalid_models_are_rejected() {
        assert!(build(b"not json").is_err());
        assert!(build(br#"{"schema": "rosepen-cdr-export/2", "pages": []}"#).is_err());
        assert!(build(br#"{"schema": "rosepen-cdr-export/1", "pages": []}"#).is_err());
        assert!(build(
            br#"{"schema": "rosepen-cdr-export/1", "pages": [
                 {"width_mm": 10.0, "height_mm": 10.0, "layers": [
                   {"name": "x", "shapes": [{"kind": "path", "d": "Q"}]}]}]}"#
        )
        .is_err());
        // A spot paint naming an ink the table lacks.
        assert!(build(
            br#"{"schema": "rosepen-cdr-export/1", "pages": [
                 {"width_mm": 10.0, "height_mm": 10.0, "layers": [
                   {"name": "x", "shapes": [{"kind": "rect", "x": 0, "y": 0, "w": 1, "h": 1,
                     "fill": {"model": "spot", "ink": 4, "tint": 1}}]}]}]}"#
        )
        .is_err());
    }
}

