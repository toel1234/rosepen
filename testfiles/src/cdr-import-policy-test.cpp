// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the importer choice (preference and environment override, clamped to
 * what the build has) and the fallback decision that replaces a crate reading
 * with the library reader's.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <src/extension/internal/cdr-import-policy.h>

#include <gtest/gtest.h>

#include <string>

using Inkscape::Extension::Internal::CdrImport::choose_engine;
using Inkscape::Extension::Internal::CdrImport::CrateSummary;
using Inkscape::Extension::Internal::CdrImport::Engine;
using Inkscape::Extension::Internal::CdrImport::fallback_reason;

// --------------------------------------------------------------- the choice --

TEST(CdrImportPolicy, auto_is_the_default)
{
    EXPECT_EQ(Engine::Auto, choose_engine("", nullptr, true, true));
    EXPECT_EQ(Engine::Auto, choose_engine("auto", nullptr, true, true));
}

TEST(CdrImportPolicy, names_pick_the_engine)
{
    EXPECT_EQ(Engine::TraceCdr, choose_engine("tracecdr", nullptr, true, true));
    EXPECT_EQ(Engine::LibCdr, choose_engine("libcdr", nullptr, true, true));
}

TEST(CdrImportPolicy, an_unknown_name_is_auto)
{
    EXPECT_EQ(Engine::Auto, choose_engine("nonsense", nullptr, true, true));
    EXPECT_EQ(Engine::Auto, choose_engine("TraceCdr", nullptr, true, true)) << "names are lower case";
}

TEST(CdrImportPolicy, the_environment_beats_the_preference)
{
    EXPECT_EQ(Engine::TraceCdr, choose_engine("libcdr", "tracecdr", true, true));
    EXPECT_EQ(Engine::LibCdr, choose_engine("tracecdr", "libcdr", true, true));
}

TEST(CdrImportPolicy, an_empty_environment_value_is_ignored)
{
    EXPECT_EQ(Engine::LibCdr, choose_engine("libcdr", "", true, true));
    EXPECT_EQ(Engine::TraceCdr, choose_engine("tracecdr", "", true, true));
    EXPECT_EQ(Engine::Auto, choose_engine("auto", "", true, true));
}

TEST(CdrImportPolicy, auto_uses_the_only_reader_a_build_has)
{
    // Both compiled in: nothing to decide.
    EXPECT_EQ(Engine::Auto, choose_engine("auto", nullptr, true, true));
    // Only one of them: auto resolves to it.
    EXPECT_EQ(Engine::TraceCdr, choose_engine("auto", nullptr, true, false));
    EXPECT_EQ(Engine::LibCdr, choose_engine("auto", nullptr, false, true));
    // Neither (a build with no drawing reader at all): auto stays auto and
    // the caller finds nothing to open with.
    EXPECT_EQ(Engine::Auto, choose_engine("auto", nullptr, false, false));
}

TEST(CdrImportPolicy, a_requested_engine_is_clamped_to_the_build)
{
    // The crate is asked for but not compiled in.
    EXPECT_EQ(Engine::LibCdr, choose_engine("tracecdr", nullptr, false, true));
    // The library reader is asked for but not compiled in.
    EXPECT_EQ(Engine::TraceCdr, choose_engine("libcdr", nullptr, true, false));
    // The environment is clamped the same way.
    EXPECT_EQ(Engine::LibCdr, choose_engine("auto", "tracecdr", false, true));
}

// ------------------------------------------------------------- the fallback --

TEST(CdrImportPolicy, an_abi_error_always_falls_back)
{
    for (int code = 1; code <= 7; ++code) {
        CrateSummary summary;
        summary.code = code;
        summary.pages = 3;
        summary.shapes = 40;
        auto const reason = fallback_reason(summary);
        ASSERT_TRUE(reason.has_value()) << "code " << code;
        EXPECT_FALSE(reason->empty()) << "code " << code;
    }

    CrateSummary summary;
    summary.code = 1;
    EXPECT_EQ("not a drawing-format file", fallback_reason(summary));

    summary.code = 7;
    EXPECT_EQ("internal panic caught", fallback_reason(summary)) << "a panic is not trusted either";

    summary.code = 3;
    EXPECT_EQ("truncated file", fallback_reason(summary));
}

TEST(CdrImportPolicy, no_page_falls_back)
{
    CrateSummary summary;
    summary.pages = 0;
    EXPECT_EQ("no pages", fallback_reason(summary));
}

TEST(CdrImportPolicy, nothing_readable_falls_back)
{
    CrateSummary summary;
    summary.pages = 1;
    summary.shapes = 0;
    summary.skipped = 4;
    EXPECT_EQ("nothing readable", fallback_reason(summary));
}

TEST(CdrImportPolicy, an_empty_drawing_is_kept)
{
    // No shape and nothing skipped: the file really is empty.
    CrateSummary summary;
    summary.pages = 1;
    summary.shapes = 0;
    summary.skipped = 0;
    EXPECT_FALSE(fallback_reason(summary).has_value());
}

TEST(CdrImportPolicy, partial_reads_are_kept)
{
    // Some shapes arrived and some were skipped: the crate's reading is the
    // better one, the skips are reported.
    CrateSummary summary;
    summary.pages = 2;
    summary.shapes = 17;
    summary.skipped = 5;
    EXPECT_FALSE(fallback_reason(summary).has_value());
}

TEST(CdrImportPolicy, a_page_one_that_cannot_be_built_falls_back)
{
    CrateSummary summary;
    summary.pages = 3;
    summary.shapes = 12;
    summary.page1_unreadable = true;
    EXPECT_EQ("page 1 unreadable", fallback_reason(summary));
}

TEST(CdrImportPolicy, the_first_reason_wins)
{
    CrateSummary summary;
    summary.code = 2;
    summary.pages = 0;
    summary.page1_unreadable = true;
    EXPECT_EQ("unsupported container", fallback_reason(summary))
        << "a reader error is the reason the user should see";
}

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
