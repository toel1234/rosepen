// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for live dimensions, application level and headless.
 *
 * app.dimension-add / app.dimension-list / app.dimension-set are the single
 * source of truth for creating and editing Rosepen dimension objects; the
 * interactive Dimension tool commits through the same DimensionOps functions.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_DIMENSION_H
#define INK_ACTIONS_DIMENSION_H

#include <string>
#include <utility>
#include <vector>

#include <2geom/point.h>

class SPDocument;
class SPGroup;
class SPObject;
class InkscapeApplication;

namespace Inkscape {

/**
 * Programmatic creation and editing of live dimension groups
 * (g.rosepen-dimension = LPE path + label text).
 */
namespace DimensionOps {

/// One measured point: an anchored point of another object, or a free point.
struct Ref {
    std::string id;      ///< measured object id; empty for a free point.
    std::string anchor;  ///< anchor spec on that object ("bbox:r", "node:0", "rel:0.5/1"); empty for free points.
    Geom::Point point;   ///< free point in document user units (only when id is empty).
};

/// Everything DimensionOps::create needs; options are key/value pairs with
/// keys offset, precision, unit, scale, text, horizontal, units, prefix,
/// suffix, font-size and id.
struct Spec {
    std::string kind = "auto"; ///< aligned|horizontal|vertical|angular|segment|auto.
    std::vector<Ref> refs;     ///< 2 points; 3 for angular, in arm A, vertex, arm B order.
    std::vector<std::pair<std::string, std::string>> options;
};

/// One dimension for listing.
struct Row {
    std::string id;
    std::string kind;
    double value = 0.0; ///< user units, or degrees for angular dimensions.
    std::string label;  ///< current label text.
};

/**
 * Create a dimension group from @a spec.
 *
 * parent_or_null hosts the group; when null, the parent of the first
 * referenced item is used, else the document root (the GUI tool passes the
 * current layer instead).
 *
 * On invalid input logs "dimension-add: <reason>" and returns nullptr with
 * the document unchanged.
 */
SPGroup *create(SPDocument *doc, Spec const &spec, SPObject *parent_or_null);

/// All dimension groups in document order.
std::vector<Row> list(SPDocument *doc);

/// Format the dimension-list TSV: one "id\tkind\tvalue\tlabel" line per group.
std::string list_tsv(SPDocument *doc);

/// Set one option on a dimension group; false (logged "dimension-set: ...")
/// on an unknown key or unparsable value.
bool set(SPGroup *group, std::string const &key, std::string const &value);

} // namespace DimensionOps

/**
 * Format the dimension list output: one line per dimension group in document
 * order, value printed with 6 decimals in the classic locale.
 */
std::string dimension_list(SPDocument *doc);

} // namespace Inkscape

void add_actions_dimension(InkscapeApplication *app);

#endif // INK_ACTIONS_DIMENSION_H

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
