// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SpotSwatch - Document-level storage of named (spot) inks.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/spot-swatch.h"

#include <string>

#include "colors/color.h"
#include "colors/spot.h"
#include "document.h"
#include "gradient-chemistry.h"
#include "object/sp-gradient.h"
#include "util/cast.h"

namespace Inkscape::Colors::Spot {
namespace {

std::string unique_spot_id(SPDocument *document, std::string const &name)
{
    auto const base = swatch_id(name);
    if (!document->getObjectById(base)) {
        return base;
    }
    for (int suffix = 2;; ++suffix) {
        auto const id = base + "-" + std::to_string(suffix);
        if (!document->getObjectById(id)) {
            return id;
        }
    }
}

} // namespace

std::optional<std::string> get_spot_name(SPGradient const *gradient)
{
    if (!gradient || !gradient->isSwatch() || !gradient->isSolid()) {
        return std::nullopt;
    }
    auto const name = gradient->getAttribute(SPOT_ATTR);
    if (!name) {
        return std::nullopt;
    }
    return normalize_name(name);
}

SPGradient *find_spot(SPDocument *document, std::string_view name)
{
    if (!document) {
        return nullptr;
    }
    auto const normalized = normalize_name(name);
    if (!normalized) {
        return nullptr;
    }
    for (auto const object : document->getResourceList("gradient")) {
        auto const gradient = cast_unsafe<SPGradient>(object);
        if (auto const spot = get_spot_name(gradient); spot && *spot == *normalized) {
            return gradient;
        }
    }
    return nullptr;
}

SPGradient *ensure_spot(SPDocument *document, std::string_view name, Color const &alternate)
{
    if (!document) {
        return nullptr;
    }
    auto const normalized = normalize_name(name);
    if (!normalized) {
        return nullptr;
    }
    if (auto const existing = find_spot(document, *normalized)) {
        return existing;
    }

    auto gradient = sp_document_default_gradient_vector(document, alternate, 1.0, true);
    if (!gradient) {
        return nullptr;
    }
    gradient->setSwatch();
    gradient->setAttribute("id", unique_spot_id(document, *normalized).c_str());
    set_spot_name(gradient, *normalized);
    return gradient;
}

void set_spot_name(SPGradient *gradient, std::optional<std::string_view> name)
{
    if (!gradient) {
        return;
    }
    if (!name) {
        gradient->removeAttribute(SPOT_ATTR);
        return;
    }
    auto const normalized = normalize_name(*name);
    if (!normalized) {
        gradient->removeAttribute(SPOT_ATTR);
        return;
    }
    gradient->setAttribute(SPOT_ATTR, normalized->c_str());
}

std::vector<SPGradient *> list_spots(SPDocument *document)
{
    std::vector<SPGradient *> spots;
    if (!document) {
        return spots;
    }
    for (auto const object : document->getResourceList("gradient")) {
        auto const gradient = cast_unsafe<SPGradient>(object);
        if (get_spot_name(gradient)) {
            spots.push_back(gradient);
        }
    }
    return spots;
}

} // namespace Inkscape::Colors::Spot
