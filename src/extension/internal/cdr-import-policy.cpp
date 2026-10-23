// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Choose which reader opens a drawing file, and decide when the crate's result
 * is too poor to keep.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-import-policy.h"

#include <glib.h>

namespace Inkscape::Extension::Internal::CdrImport {

namespace {

/// The reader a name asks for; Auto for an empty or unknown name.
Engine engine_from_name(std::string_view name, char const *source)
{
    if (name == "tracecdr") {
        return Engine::TraceCdr;
    }
    if (name == "libcdr") {
        return Engine::LibCdr;
    }
    if (!name.empty() && name != "auto") {
        g_warning("%s: unknown drawing reader %.*s; using auto", source,
                  static_cast<int>(name.size()), name.data());
    }
    return Engine::Auto;
}

/**
 * The text of an ABI result code, kept in step with rp_strerror() of ABI v1.
 *
 * The policy stays free of the ABI header so its gtest runs in every build.
 */
char const *code_text(int code)
{
    switch (code) {
    case 0: return "ok";
    case 1: return "not a drawing-format file";
    case 2: return "unsupported container";
    case 3: return "truncated file";
    case 4: return "container read failed";
    case 5: return "invalid argument";
    case 6: return "invalid export model";
    case 7: return "internal panic caught";
    default: return "unknown error code";
    }
}

} // namespace

Engine choose_engine(std::string_view pref, char const *env, bool have_crate, bool have_libcdr)
{
    // The environment override is for tests and diagnostics; an empty value
    // counts as unset so a stray variable cannot change behaviour.
    auto const from_env = env != nullptr && env[0] != '\0';
    auto engine = engine_from_name(from_env ? std::string_view(env) : pref,
                                   from_env ? "ROSEPEN_CDR_IMPORTER" : "cdr reader preference");

    if (engine == Engine::TraceCdr && !have_crate) {
        g_warning("cdr import: the drawing reader is not compiled in; using libcdr");
        engine = Engine::LibCdr;
    } else if (engine == Engine::LibCdr && !have_libcdr) {
        g_warning("cdr import: libcdr is not compiled in; using the drawing reader");
        engine = Engine::TraceCdr;
    }

    if (engine == Engine::Auto) {
        // Only one of the two is there: nothing to choose between.
        if (have_crate && !have_libcdr) {
            return Engine::TraceCdr;
        }
        if (!have_crate && have_libcdr) {
            return Engine::LibCdr;
        }
    }
    return engine;
}

std::optional<std::string> fallback_reason(CrateSummary const &summary)
{
    if (summary.code != 0) {
        return code_text(summary.code);
    }
    if (summary.pages == 0) {
        return "no pages";
    }
    if (summary.shapes == 0 && summary.skipped > 0) {
        return "nothing readable";
    }
    if (summary.page1_unreadable) {
        return "page 1 unreadable";
    }
    return std::nullopt;
}

} // namespace Inkscape::Extension::Internal::CdrImport

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
