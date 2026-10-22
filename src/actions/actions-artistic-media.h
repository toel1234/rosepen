// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for live artistic media strokes, application level and headless.
 *
 * app.artistic-add / app.artistic-apply / app.artistic-set / app.artistic-break
 * are the single source of truth for creating and editing Rosepen artistic
 * media objects; the interactive Artistic Media tool and its property bar
 * commit through the same ArtisticOps functions.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_ARTISTIC_MEDIA_H
#define INK_ACTIONS_ARTISTIC_MEDIA_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/point.h>

#include "live_effects/artistic-media-library.h"

class SPDocument;
class SPObject;
class SPPath;
class InkscapeApplication;

namespace Inkscape {

class ObjectSet;

/**
 * Programmatic creation and editing of live artistic media strokes
 * (a single svg:path whose original-d is the skeleton and whose d is the
 * brush outline drawn by the artistic_media live path effect).
 */
namespace ArtisticOps {

/// Which action is parsing; each verb accepts a different option set.
enum class Verb {
    Add,   ///< create a stroke from a point list
    Apply, ///< turn selected paths into strokes
    Set,   ///< change an existing stroke
};

/// Everything a verb can be told; unset members keep the object's current value.
struct Options {
    std::optional<LivePathEffect::ArtisticMedia::Mode> mode;
    std::optional<std::string> shape;      ///< library name of the preset/brush
    std::optional<double> width;           ///< VISUAL millimetres
    std::optional<double> angle;           ///< nib angle in degrees
    std::optional<bool> scale_with_object;
    std::optional<std::string> color;      ///< #rrggbb (add and apply only)
    std::optional<double> smoothing;       ///< 0..100, creation only
    std::string id;                        ///< central path id (add only)
    std::string parent;                    ///< host group id (add only)
    std::optional<std::string> skeleton;   ///< new skeleton path data (set only, must be last)
};

/// One artistic media object for listing.
struct Row {
    std::string id;
    std::string mode;          ///< preset|brush|calligraphic
    std::string shape;         ///< library name, "-" for calligraphic
    double width = 0.0;        ///< visual millimetres
    double angle = 0.0;        ///< stored nib angle, [0, 180)
    std::size_t nodes = 0;     ///< skeleton node count
    double length = 0.0;       ///< skeleton length in document millimetres
    std::string source;        ///< embedded|fallback, "-" for calligraphic
};

/**
 * Parse "key=value[,key=value]*" into @a options.
 *
 * Option keys: mode, shape, width, angle, scale-with-object, color (add and
 * apply), smoothing (add), id (add), parent (add), skeleton (set, must be
 * last: it takes the rest of the string including commas).
 *
 * A shape name is checked against the library when the mode is given in the
 * same string; otherwise the caller checks it against the effective mode.
 * On error returns nullopt and sets @a error to the bare reason.
 */
std::optional<Options> parse_options(std::string_view spec, Verb verb, std::string &error);

/**
 * Create a stroke through @a points_doc (document user units) as one undoable
 * step. @a parent_or_null hosts it; when null the last top-level layer is
 * used, else the document root.
 *
 * On invalid input logs nothing, returns nullptr and sets @a error to
 * "artistic-add: <reason>"; the document is unchanged.
 */
SPPath *create(SPDocument *doc, std::vector<Geom::Point> const &points_doc, Options const &options,
               SPObject *parent_or_null, std::string &error);

/// Count applied. Non-path items are reported into @a errors and skipped.
int apply(ObjectSet &selection, Options const &options, std::string &errors);

/// Change one existing artistic media path; false (with @a error) on bad input,
/// which leaves the document untouched.
bool set(SPPath *path, Options const &options, std::string &error);

/// All artistic media paths in document order.
std::vector<Row> list(SPDocument *doc);

/// The list TSV: one row per stroke plus a final "artistic-count <n>" line.
std::string list_tsv(SPDocument *doc);

/// Freeze the selected strokes: keep the outline, add a sibling skeleton path.
std::vector<SPPath *> break_apart(ObjectSet &selection);

/// True when @a object is an SPPath carrying this effect (structure, not class).
bool is_artistic(SPObject const *object);

/// "preset\t<name>\t<label>" for every preset, then every brush, in library order.
std::string library_tsv();

} // namespace ArtisticOps
} // namespace Inkscape

void add_actions_artistic_media(InkscapeApplication *app);

#endif // INK_ACTIONS_ARTISTIC_MEDIA_H

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
