// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the window → document coordinate helper used by canvas
 * tools (07-01 Artistic Media click-offset regression).
 *
 * Every expected value below is hand-derived from the affine composition
 * documented in desktop-coordinates.h, not copied from program output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/tools/desktop-coordinates.h"

#include <2geom/point.h>
#include <2geom/transforms.h>

#include <gtest/gtest.h>

using Inkscape::UI::Tools::doc_point_from_window;

namespace {

/// Minimal desktop stand-in mirroring SPDesktop's coordinate contract:
/// w2d() applies the view affine (zoom + scroll), dt2doc() applies the
/// document scale.
struct FakeDesktop {
    Geom::Affine view;      // window → desktop
    Geom::Affine doc_scale; // desktop → document
    Geom::Point w2d(Geom::Point const &p) const { return p * view; }
    Geom::Point dt2doc(Geom::Point const &p) const { return p * doc_scale; }
};

} // namespace

TEST(DesktopCoordinatesTest, AppliesViewAffineThenDocScale)
{
    // 300 % zoom scrolled (120, 80) px away from the origin; document scale 2.
    FakeDesktop const dt{Geom::Affine(3.0, 0.0, 0.0, 3.0, 120.0, 80.0),
                         Geom::Affine(0.5, 0.0, 0.0, 0.5, 0.0, 0.0)};

    auto const got = doc_point_from_window(dt, Geom::Point(100, 50));
    EXPECT_DOUBLE_EQ(got.x(), 210.0); // (100·3 + 120) / 2
    EXPECT_DOUBLE_EQ(got.y(), 115.0); // ( 50·3 +  80) / 2
}

TEST(DesktopCoordinatesTest, IdentityDesktopKeepsPoint)
{
    FakeDesktop const dt{Geom::Affine(), Geom::Affine()};

    auto const got = doc_point_from_window(dt, Geom::Point(-7.25, 3.5));
    EXPECT_DOUBLE_EQ(got.x(), -7.25);
    EXPECT_DOUBLE_EQ(got.y(), 3.5);
}
