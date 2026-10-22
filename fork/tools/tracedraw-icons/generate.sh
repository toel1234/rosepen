#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate share/icons/rosepen from TraceDraw's painter icons.
#
# TraceDraw (https://github.com/igtoth/tracedraw, MIT OR Apache-2.0) draws
# its toolbox and standard-toolbar icons in code (apps/tracedraw/src/ui/
# icons.rs). This script fetches that file at a pinned commit, renders every
# icon headlessly with egui and converts the painted shapes to 16x16 SVG,
# then installs the ones Rosepen uses under Inkscape's icon names.
#
# Run from the repository root: fork/tools/tracedraw-icons/generate.sh
set -euo pipefail

COMMIT=4a48ef58fac3d366406ed6799e7d23270f036959
RAW=https://raw.githubusercontent.com/igtoth/tracedraw/$COMMIT
HERE=fork/tools/tracedraw-icons
OUT=share/icons/rosepen
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cp -r "$HERE/Cargo.toml" "$HERE/src" "$WORK/"
curl -fsSL "$RAW/apps/tracedraw/src/ui/icons.rs" -o "$WORK/src/icons.rs"
curl -fsSL "$RAW/apps/tracedraw/src/tools.rs" -o "$WORK/src/tools.rs"
curl -fsSL "$RAW/LICENSE-MIT" -o "$WORK/LICENSE-MIT"
(cd "$WORK" && cargo run -q --release -- "$WORK/svg")

# Inkscape icon name -> TraceDraw icon (tools/<id> or actions/<name>).
MAP=(
  tool-pointer=tools/pick
  tool-node-editor=tools/shape
  tool-tweak=tools/smear
  draw-eraser=tools/eraser
  zoom=tools/zoom
  draw-freehand=tools/freehand
  draw-path=tools/bezier
  draw-calligraphic=tools/painterly_brush
  draw-rectangle=tools/rectangle
  draw-ellipse=tools/ellipse
  draw-polygon-star=tools/polygon
  draw-polygon=tools/polygon
  draw-star=tools/star
  draw-spiral=tools/spiral
  draw-cuboid=tools/extrude
  draw-text=tools/text
  measure-segments=tools/parallel_dimension
  tool-measure=tools/segment_dimension
  draw-connector=tools/connector
  color-picker=tools/color_eyedropper
  color-gradient=tools/interactive_fill
  mesh-gradient=tools/mesh_fill
  color-fill=tools/area_fill
  document-new=actions/new
  document-open=actions/open
  document-save=actions/save
  document-print=actions/print
  edit-cut=actions/cut
  edit-copy=actions/copy
  edit-paste=actions/paste
  edit-undo=actions/undo
  edit-redo=actions/redo
  document-import=actions/import
  document-export=actions/export
  preferences-system=actions/options
  rosepen-fullscreen=actions/fullscreen
  rosepen-show-rulers=actions/show-rulers
  rosepen-show-grid=actions/show-grid
  rosepen-show-guides=actions/show-guidelines
)

rm -rf "$OUT/scalable"
mkdir -p "$OUT/scalable/actions"
for entry in "${MAP[@]}"; do
  name=${entry%%=*}
  src="$WORK/svg/${entry#*=}.svg"
  # The plain name keeps TraceDraw's colour accents; the -symbolic copy
  # serves lookups that ask for symbolic icons.
  cp "$src" "$OUT/scalable/actions/$name.svg"
  cp "$src" "$OUT/scalable/actions/$name-symbolic.svg"
done
cp "$WORK/LICENSE-MIT" "$OUT/LICENSE-TraceDraw-MIT"
echo "wrote ${#MAP[@]} icons to $OUT/scalable/actions (TraceDraw $COMMIT)"
