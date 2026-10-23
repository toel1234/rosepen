// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Choose which reader opens a drawing file, and decide when the crate's result
 * is too poor to keep.
 *
 * Pure logic: no document, no GTK, no Rust. The caller passes what the build
 * has and what the ABI reported, so the decisions are locked by a gtest that
 * runs in every build, with or without the crate.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_POLICY_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_POLICY_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Inkscape::Extension::Internal::CdrImport {

/// Which reader to use for a drawing file.
enum class Engine {
    Auto,     ///< The crate first, the library reader when it fails
    TraceCdr, ///< The crate only
    LibCdr,   ///< The library reader only
};

/**
 * Pick the engine.
 *
 * @a env (the ROSEPEN_CDR_IMPORTER value, may be null) beats the preference
 * @a pref; an empty environment value counts as unset. Values are "auto"
 * (the default, also for an empty or unknown value), "tracecdr" and "libcdr".
 * The choice is then clamped to what the build has: a reader that is not
 * compiled in is replaced by the other one, with a warning.
 */
Engine choose_engine(std::string_view pref, char const *env, bool have_crate, bool have_libcdr);

/// What the crate reported for a file, plus what Rosepen made of its pages.
struct CrateSummary
{
    int code = 0;               ///< RP_OK, or the ABI error code
    std::uint32_t pages = 0;
    std::uint32_t shapes = 0;
    std::uint32_t skipped = 0;
    bool page1_unreadable = false; ///< Rosepen could not build a document from page 1
};

/**
 * Why the crate's result should be replaced by the library reader's, or
 * nothing when it should be kept.
 *
 * Falls back when the ABI returned any error, when the file gave no page, or
 * when the crate understood nothing it saw (no shapes but skipped objects).
 * Warnings and partial skips never trigger a fallback: they are reported, and
 * the crate's reading is the better one.
 */
std::optional<std::string> fallback_reason(CrateSummary const &summary);

} // namespace Inkscape::Extension::Internal::CdrImport

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_POLICY_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
