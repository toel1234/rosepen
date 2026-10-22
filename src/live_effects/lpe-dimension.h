// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live "dimension" path effect: a persistent, live-linked dimension line.
 *
 * A Rosepen dimension is a group <g class="rosepen-dimension"> containing a
 * path carrying this effect (the extension lines, dimension line/arc and
 * arrowheads) and a sibling <text class="rosepen-dimension-label">. The
 * effect measures between anchored points of up to three other objects
 * (see dimension-geometry.h for the pure model) and keeps the label text,
 * position and rotation in sync with the built geometry. When a measured
 * object disappears or an anchor stops resolving, the dimension freezes at
 * its last resolved points instead of collapsing.
 *
 * The supported entry points are the Dimension tool and the app.dimension-*
 * actions; the effect is hidden from the default LPE gallery.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_LPE_DIMENSION_H
#define INKSCAPE_LPE_DIMENSION_H

#include <sigc++/connection.h>
#include <sigc++/sigc++.h>

#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <glibmm/ustring.h>

#include "live_effects/dimension-geometry.h"
#include "live_effects/effect.h"
#include "live_effects/parameter/bool.h"
#include "live_effects/parameter/enum.h"
#include "live_effects/parameter/hidden.h"
#include "live_effects/parameter/parameter.h"
#include "live_effects/parameter/satellite.h"
#include "live_effects/parameter/text.h"
#include "live_effects/parameter/unit.h"

class SPItem; // global namespace, full type in object/sp-item.h

namespace Inkscape {

namespace LivePathEffect {

/** Stored dimension kind (EnumParam needs a plain enum with a data table). */
enum DimKindParam {
    DK_ALIGNED,
    DK_HORIZONTAL,
    DK_VERTICAL,
    DK_ANGULAR,
    DK_SEGMENT,
    DK_END
};

/** Stored label position relative to the dimension line / arc. */
enum DimTextPos {
    DTP_ABOVE,
    DTP_INLINE,
    DTP_BELOW,
    DTP_END
};

class LPEDimension final : public Effect {
public:
    LPEDimension(LivePathEffectObject *lpeobject);
    ~LPEDimension() override;

    void doBeforeEffect(SPLPEItem const *lpeitem) override;
    Geom::PathVector doEffect_path(Geom::PathVector const &path_in) override;
    bool doOnOpen(SPLPEItem const *lpeitem) override;
    void doOnRemove(SPLPEItem const *lpeitem) override;

    /// The kind as currently stored in the "kind" parameter.
    Dimension::Kind dimension_kind() const;
    /// The last built value: length in user units, or degrees for angular.
    double value() const
    {
        if (!_result.valid) {
            return 0.0;
        }
        return dimension_kind() == Dimension::Kind::Angular ? _result.value_uu_or_deg
                                                            : _result.value_uu_or_deg * _length_factor;
    }
    /// True when the last build produced usable geometry.
    bool is_valid() const { return _result.valid; }

    /**
     * (Re-)arm the live-update listeners.
     *
     * Writing to the effect repr can make Inkscape discard and recreate the
     * Effect instance; the new instance needs its satellite listeners and
     * item watchers armed again. Callers that touch LPE parameters
     * programmatically (actions, tool) invoke this after their writes.
     */
    void arm_live_updates();

private:
    LPEDimension(const LPEDimension &) = delete;
    LPEDimension &operator=(const LPEDimension &) = delete;

    void resolve_points(SPLPEItem const *lpeitem);
    std::optional<Geom::Point> resolve_ref(SatelliteParam &ref, char const *ref_key,
                                           TextParam const &anchor_param);
    SPObject *resolve_ref_object(SatelliteParam &ref, char const *ref_key);
    Glib::ustring ref_href(char const *ref_key);
    std::optional<Geom::Point> fallback_point(SPLPEItem const *lpeitem, std::size_t index,
                                              bool prefer_original_d) const;
    std::vector<Geom::Point> original_d_nodes(SPLPEItem const *lpeitem) const;
    void store_last_points();
    void update_label(SPLPEItem const *lpeitem);
    void connect_parent_transform(SPLPEItem const *lpeitem);
    void on_parent_transformed(Geom::Affine const *rel_transf, SPItem *moved);
    void refresh_item_watchers();
    void disconnect_item_watchers();
    void on_ref_item_changed();

    EnumParam<DimKindParam> kind;
    SatelliteParam start_ref;
    SatelliteParam end_ref;
    SatelliteParam vertex_ref;
    TextParam start_anchor;
    TextParam end_anchor;
    TextParam vertex_anchor;
    ScalarParam offset;        // mm; angular: arc radius
    ScalarParam precision;
    UnitParam unit;
    ScalarParam scale;
    EnumParam<DimTextPos> text_position;
    BoolParam text_horizontal;
    BoolParam show_units;
    TextParam prefix;
    TextParam suffix;
    ScalarParam font_size;     // pt, used at creation time only
    ScalarParam arrow_length;  // mm
    ScalarParam ext_gap;       // mm
    ScalarParam ext_overshoot; // mm
    HiddenParam last_points;   // freeze state: the last resolved document points
    HiddenParam ref_ids;       // "key=#id;..." copy of the refs, used to restore them
                               // after a fork/reload that lost the satellite hrefs

    std::vector<Geom::Point> _doc_points;  ///< resolved measured points, document coords
    Dimension::Result _result;             ///< last built geometry in path coordinates
    double _length_factor = 1.0;           ///< path user units -> document user units (scaled parents)
    std::vector<Geom::Point> _last_points_cache; ///< parsed last_points
    bool _updating_last_points = false;
    std::size_t _label_debug_emitted = 0;

    SPItem *_parent_item = nullptr;               ///< group holding path + label
    sigc::connection _parent_transform_connection;
    sigc::connection _lpeobj_modified_connection;  ///< arms watchers on load/fork

    /// Direct watchers on the measured items: SatelliteParam::linked_modified
    /// never fires for repr-level edits (those raise uflags, not mflags), so
    /// the effect watches transform/modified itself and re-evaluates only on
    /// a real geometry change.
    std::vector<SPItem *> _watched_items;
    std::vector<std::string> _item_signatures;
    std::vector<sigc::connection> _item_watchers;
    bool _evaluating = false;
};

} // namespace LivePathEffect
} // namespace Inkscape

#endif // INKSCAPE_LPE_DIMENSION_H

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
