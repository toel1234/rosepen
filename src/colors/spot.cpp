// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SpotColors - Pure helpers for named (spot) inks.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/spot.h"

#include <cstddef>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cmyk.h"

namespace Inkscape::Colors::Spot {
namespace {

constexpr std::size_t MAX_NAME_BYTES = 127;

bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * Strict UTF-8 validation, rejecting overlong encodings, surrogates and
 * code points above U+10FFFF.
 */
bool valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        auto const c = static_cast<unsigned char>(text[i]);
        std::size_t extra;
        unsigned int codepoint;

        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            extra = 1;
            codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            codepoint = c & 0x07;
        } else {
            return false;
        }

        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            auto const cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (cc & 0x3f);
        }

        // Overlong form, UTF-16 surrogate or out of range.
        static constexpr unsigned int minimum[4] = {0x0, 0x80, 0x800, 0x10000};
        if (codepoint < minimum[extra] || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }

        i += extra + 1;
    }
    return true;
}

} // namespace

std::optional<std::string> normalize_name(std::string_view name)
{
    if (!valid_utf8(name)) {
        return std::nullopt;
    }

    std::string out;
    out.reserve(name.size());
    bool pending_space = false;

    for (auto const c : name) {
        auto const uc = static_cast<unsigned char>(c);
        if (is_space(uc)) {
            if (!out.empty()) {
                pending_space = true;
            }
            continue;
        }
        if (uc < 0x20 || uc == 0x7f) {
            return std::nullopt;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }

    if (out.empty() || out.size() > MAX_NAME_BYTES) {
        return std::nullopt;
    }
    return out;
}

std::string swatch_id(std::string_view name)
{
    std::string id = "spot-";
    id.reserve(id.size() + name.size());
    for (auto const c : name) {
        auto const uc = static_cast<unsigned char>(c);
        if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') ||
            uc == '_' || uc == '-') {
            id.push_back(c);
        } else {
            id.push_back('_');
        }
    }
    return id;
}

Alternate alternate_for(Color const &color)
{
    auto const &space = color.getSpace();

    if (std::dynamic_pointer_cast<Space::DeviceCMYK>(space) ||
        (space->getType() == Space::Type::CMS && space->getComponentType() == Space::Type::CMYK)) {
        return {Model::CMYK, {color.get(0), color.get(1), color.get(2), color.get(3)}};
    }

    if (auto rgb = color.converted(Space::Type::RGB)) {
        return {Model::RGB, {rgb->get(0), rgb->get(1), rgb->get(2)}};
    }

    return {Model::RGB, {0.0, 0.0, 0.0}};
}

} // namespace Inkscape::Colors::Spot
