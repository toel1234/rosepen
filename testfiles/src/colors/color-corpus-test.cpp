// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Round-trip tests for the color corpus fixtures.
 *
 * Every fixture is loaded, saved with href rebasing into a directory that is
 * not the fixture's own, and loaded again. The color-bearing properties and
 * the spot/output-profile attributes must not change, the second save must be
 * byte-identical to the first, and a relative profile href must still resolve.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <string>

#include "colors/cms/system.h"
#include "colors/color.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "colors/spot-swatch.h"
#include "colors/spot.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-gradient.h"
#include "object/sp-stop.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::Colors;

namespace {

char const *const CORPUS_DIR = INKSCAPE_TESTS_DIR "/cli_tests/testcases/colors/";

char const *const FIXTURES[] = {
    "cmyk-device", "cmyk-icc-fallback", "opacity-policy", "spot-basic",
    "spot-collision", "spot-rename", "softproof-gamut", "rgb-control",
};

char const *const PRESENTATION_ATTRIBUTES[] = {
    "fill",     "stroke",    "stop-color",   "flood-color",   "lighting-color",
    "color",    "opacity",   "fill-opacity", "stroke-opacity", "stop-opacity",
    "flood-opacity",
};

using Props = std::map<std::string, std::string>;
using Collected = std::map<std::string, Props>;

bool one_of(std::string const &name, std::initializer_list<char const *> names)
{
    for (auto const candidate : names) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

std::string trim(std::string value)
{
    auto const not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool is_tracked_property(std::string const &prop)
{
    for (auto const name : PRESENTATION_ATTRIBUTES) {
        if (prop == name) {
            return true;
        }
    }
    return false;
}

bool is_opacity_property(std::string const &prop)
{
    return prop == "opacity" || (prop.size() > 8 && prop.ends_with("-opacity"));
}

void parse_style(char const *style, Props &out)
{
    std::string const text(style);
    std::string::size_type pos = 0;
    while (pos < text.size()) {
        auto end = text.find(';', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        auto const declaration = text.substr(pos, end - pos);
        auto const colon = declaration.find(':');
        if (colon != std::string::npos) {
            auto key = trim(declaration.substr(0, colon));
            if (is_tracked_property(key) || key == "inkscape:spot-name" || key == "inkscape:output-profile") {
                out[key] = trim(declaration.substr(colon + 1));
            }
        }
        pos = end + 1;
    }
}

void collect_node(Inkscape::XML::Node *node, Collected &out)
{
    for (; node; node = node->next()) {
        if (node->type() == Inkscape::XML::NodeType::ELEMENT_NODE) {
            if (auto const id = node->attribute("id")) {
                Props props;
                if (auto const style = node->attribute("style")) {
                    parse_style(style, props);
                }
                for (auto const name : PRESENTATION_ATTRIBUTES) {
                    if (auto const value = node->attribute(name)) {
                        props.emplace(name, value);
                    }
                }
                for (auto const name : {"inkscape:spot-name", "inkscape:output-profile"}) {
                    if (auto const value = node->attribute(name)) {
                        props.emplace(name, value);
                    }
                }
                if (!props.empty()) {
                    out[id] = std::move(props);
                }
            }
            collect_node(node->firstChild(), out);
        }
    }
}

Collected collect(SPDocument &doc)
{
    Collected out;
    collect_node(doc.getReprDoc()->firstChild(), out);
    return out;
}

std::string read_file(std::string const &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/**
 * The icc opacity policy (02-02): an `icc-color()` segment carries a profile
 * name and exactly its channel count, never an opacity value, and the fallback
 * in front of it is a plain `#rrggbb` (the alpha of third-party `#rrggbbaa`
 * input is dropped).
 */
void expect_icc_opacity_policy(std::string const &id, std::string const &prop, std::string const &value,
                               bool authored)
{
    auto const pos = value.find("icc-color(");
    if (pos == std::string::npos) {
        return;
    }
    SCOPED_TRACE(id + "." + prop + " = " + value);

    // Opacity policy (02-02 option 1): OUR serialization always writes a 6-digit fallback.
    // Limitation L1 (02-04 triage): an AUTHORED third-party `#rrggbbaa icc-color(...)` passes
    // through the repr byte-for-byte (alpha is dropped for painting at parse time, but the raw
    // style string is not rewritten on save). 8 digits therefore only ever come from authored
    // input passthrough (a value identical to the loaded source), never from our own output.
    static std::regex const fallback_re("^#[0-9a-f]{6}$");
    static std::regex const authored_re("^#[0-9a-f]{6}([0-9a-f]{2})?$");
    EXPECT_TRUE(std::regex_match(trim(value.substr(0, pos)), authored ? authored_re : fallback_re))
        << "icc-color fallback must be #rrggbb (8 digits only for authored third-party passthrough, L1)";

    static std::regex const segment_re(
        "^icc-color\\(\\s*[A-Za-z0-9_.-]+\\s*(?:,\\s*-?[0-9]+(?:\\.[0-9]+)?\\s*){4}\\)$");
    EXPECT_TRUE(std::regex_match(trim(value.substr(pos)), segment_re))
        << "icc-color segment must be a profile plus exactly 4 CMYK channels";
}

class ColorCorpusTest : public ::testing::TestWithParam<char const *>
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);

        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        cms.addDirectoryPath(INKSCAPE_TESTS_DIR "/data/colors/", false);
        cms.refreshProfiles();

        fixture = GetParam();
        source = std::string(CORPUS_DIR) + fixture + ".svg";
        ASSERT_TRUE(g_file_test(source.c_str(), G_FILE_TEST_EXISTS)) << source;

        doc = SPDocument::createNewDoc(source.c_str());
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();

        GError *error = nullptr;
        gchar *temp = g_dir_make_tmp("rosepen-color-corpus-XXXXXX", &error);
        if (error) {
            g_error_free(error);
        }
        ASSERT_NE(temp, nullptr);
        temp_dir = temp;
        g_free(temp);

        first_save = temp_dir + "/first.svg";
        second_save = temp_dir + "/second.svg";
    }

    void TearDown() override
    {
        doc.reset();
        if (!temp_dir.empty()) {
            g_remove(first_save.c_str());
            g_remove(second_save.c_str());
            g_rmdir(temp_dir.c_str());
        }
    }

    /// Save like production `Svg::save` does: relative hrefs are rebased from
    /// the document base to the file's directory.
    bool save_rebased(SPDocument &document, std::string const &path)
    {
        return sp_repr_save_rebased_file(document.getReprDoc(), path.c_str(), nullptr,
                                         document.getDocumentBase(), path.c_str());
    }

    std::unique_ptr<SPDocument> reload(std::string const &path)
    {
        auto document = SPDocument::createNewDoc(path.c_str());
        if (document) {
            document->ensureUpToDate();
        }
        return document;
    }

    std::string fixture;
    std::string source;
    std::string temp_dir;
    std::string first_save;
    std::string second_save;
    std::unique_ptr<SPDocument> doc;
};

/**
 * Loading, saving elsewhere and reloading keeps every collected color property
 * and the raw spot/output-profile attributes unchanged.
 */
TEST_P(ColorCorpusTest, roundTripPreservesCollectedProperties)
{
    auto const before = collect(*doc);
    ASSERT_FALSE(before.empty());

    ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
    auto const reloaded = reload(first_save);
    ASSERT_TRUE(reloaded);

    auto const after = collect(*reloaded);
    EXPECT_EQ(before, after);
}

/**
 * A second save (of the reloaded document) into the same directory is
 * byte-identical to the first save.
 */
TEST_P(ColorCorpusTest, secondSaveIsByteIdentical)
{
    ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
    auto const reloaded = reload(first_save);
    ASSERT_TRUE(reloaded);
    ASSERT_TRUE(save_rebased(*reloaded, second_save)) << second_save;

    EXPECT_EQ(read_file(first_save), read_file(second_save));

    auto const after = collect(*reloaded);
    EXPECT_EQ(collect(*doc), after);
}

/**
 * The opacity policy holds after the round trip: no icc segment gains a number
 * and no fallback grows an alpha channel; opacity stays in *-opacity.
 */
TEST_P(ColorCorpusTest, opacityPolicy)
{
    auto const before = collect(*doc);
    ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
    auto const reloaded = reload(first_save);
    ASSERT_TRUE(reloaded);
    auto const after = collect(*reloaded);

    for (auto const &[id, props] : after) {
        for (auto const &[prop, value] : props) {
            auto const source = before.find(id);
            bool const authored = source != before.end() && source->second.contains(prop) &&
                                  source->second.at(prop) == value;
            expect_icc_opacity_policy(id, prop, value, authored);
        }
    }

    for (auto const &[id, props] : before) {
        for (auto const &[prop, value] : props) {
            if (!is_opacity_property(prop)) {
                continue;
            }
            SCOPED_TRACE(id + "." + prop);
            auto const element = after.find(id);
            ASSERT_NE(element, after.end()) << "element lost in the round trip";
            auto const kept = element->second.find(prop);
            ASSERT_NE(kept, element->second.end()) << "opacity lost in the round trip";
            EXPECT_EQ(kept->second, value);
        }
    }
}

/**
 * Spot inventory per fixture; a marked solid swatch is the only thing listed
 * and an ordinary element that happens to share the swatch id is not.
 */
TEST_P(ColorCorpusTest, spotInventory)
{
    auto const expected = fixture == "spot-basic"     ? 1u
                          : fixture == "spot-collision" ? 4u
                          : fixture == "spot-rename"    ? 3u
                                                        : 0u;

    auto const spots = Spot::list_spots(doc.get());
    ASSERT_EQ(spots.size(), expected);
    for (auto const spot : spots) {
        SCOPED_TRACE(spot->getId());
        EXPECT_TRUE(spot->isSwatch());
        EXPECT_TRUE(Spot::get_spot_name(spot).has_value());
        if (fixture == "spot-collision") {
            EXPECT_NE(std::string(spot->getId()), "spot-PANTONE_185_C")
                << "the ordinary rect must not be listed as a spot";
        }
    }

    ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
    auto const reloaded = reload(first_save);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(Spot::list_spots(reloaded.get()).size(), expected);
}

/**
 * Per-fixture invariants that need more than the collected attributes.
 */
TEST_P(ColorCorpusTest, fixtureSpecificInvariants)
{
    if (one_of(fixture, {"cmyk-icc-fallback", "opacity-policy", "softproof-gamut"})) {
        // The relative href must be rebased on save-elsewhere, otherwise the
        // reloaded document has no valid cmyk-out space at all.
        ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
        auto const reloaded = reload(first_save);
        ASSERT_TRUE(reloaded);

        auto &cms = reloaded->getDocumentCMS();
        auto const space = cms.getOutputSpace();
        ASSERT_TRUE(space) << "no output space after the reload";
        EXPECT_EQ(space->getName(), "cmyk-out");
        EXPECT_EQ(space->getComponentType(), Space::Type::CMYK);
        EXPECT_TRUE(space->hasValidCmsProfile()) << "cmyk-out did not resolve after the href rebase";
    }

    if (fixture == "opacity-policy") {
        ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
        auto const reloaded = reload(first_save);
        ASSERT_TRUE(reloaded);

        auto const &third_party = collect(*reloaded);
        auto const element = third_party.find("third-party-alpha");
        ASSERT_NE(element, third_party.end());
        auto const fill = element->second.find("fill");
        ASSERT_NE(fill, element->second.end());
        // Limitation L1 (02-04 triage): authored third-party #rrggbbaa passes through the repr
        // unchanged; alpha is dropped for painting at parse time, and the fallback normalizes
        // to #rrggbb only when the style is rewritten through SPStyle.
        static std::regex const passthrough("^#ff000080 icc-color\\(cmyk-out, 0, 1, 1, 0\\)$");
        EXPECT_TRUE(std::regex_match(fill->second, passthrough))
            << "third-party #rrggbbaa must pass through unchanged (L1), got: " << fill->second;
    }

    if (fixture == "spot-collision") {
        auto const first = Spot::find_spot(doc.get(), "PANTONE 185 C");
        ASSERT_TRUE(first);
        EXPECT_EQ(std::string(first->getId()), "spot-PANTONE_185_C-2");

        // The ordinary rect owns the base id but is never returned.
        auto const ordinary = doc->getObjectById("spot-PANTONE_185_C");
        ASSERT_TRUE(ordinary);
        EXPECT_NE(static_cast<SPObject *>(first), ordinary);
        EXPECT_FALSE(is<SPGradient>(ordinary));

        // The double-spaced duplicate normalizes to the same ink and loses to
        // the first swatch in resource order.
        EXPECT_EQ(Spot::find_spot(doc.get(), "  PANTONE   185   C "), first);

        // ensure_spot reuses the existing swatch without touching its alternate.
        auto const alternate = Color::parse("device-cmyk(0.5 0.5 0 0)");
        ASSERT_TRUE(alternate);
        auto const gradients_before = doc->getResourceList("gradient").size();
        auto const reused = Spot::ensure_spot(doc.get(), "PANTONE 185 C", *alternate);
        EXPECT_EQ(reused, first);
        EXPECT_EQ(doc->getResourceList("gradient").size(), gradients_before);

        auto const expected = Color::parse("device-cmyk(0 0.92 0.76 0)");
        ASSERT_TRUE(expected);
        auto const stop = reused->getFirstStop();
        ASSERT_TRUE(stop);
        auto const color = stop->getColor();
        EXPECT_EQ(color.getSpace(), expected->getSpace());
        for (unsigned i = 0; i < 4; ++i) {
            EXPECT_DOUBLE_EQ(color.get(i), expected->get(i));
        }
    }

    if (fixture == "spot-rename") {
        // An id that is not derived from the ink name still reports its own name.
        auto const renamed = Spot::find_spot(doc.get(), "Special Red");
        ASSERT_TRUE(renamed);
        EXPECT_EQ(std::string(renamed->getId()), "myink");
        EXPECT_NE(std::string(renamed->getId()), Spot::swatch_id("Special Red"));

        for (auto const ink : {"Special Red", "Grün 1", "Ink #5/A"}) {
            SCOPED_TRACE(ink);
            auto const spot = Spot::find_spot(doc.get(), ink);
            ASSERT_TRUE(spot);
            auto const name = Spot::get_spot_name(spot);
            ASSERT_TRUE(name);
            EXPECT_EQ(*name, ink);
        }

        // The unmarked control swatch is a swatch but not a spot.
        auto const control = doc->getObjectById("swatch-control");
        ASSERT_TRUE(control);
        EXPECT_EQ(control->getAttribute(Spot::SPOT_ATTR), nullptr);
        auto const gradient = cast<SPGradient>(control);
        ASSERT_TRUE(gradient);
        EXPECT_FALSE(Spot::get_spot_name(gradient));

        // The names survive the reload, including the non-ASCII one.
        ASSERT_TRUE(save_rebased(*doc, first_save)) << first_save;
        auto const reloaded = reload(first_save);
        ASSERT_TRUE(reloaded);
        for (auto const ink : {"Special Red", "Grün 1", "Ink #5/A"}) {
            SCOPED_TRACE(ink);
            auto const spot = Spot::find_spot(reloaded.get(), ink);
            ASSERT_TRUE(spot);
            auto const name = Spot::get_spot_name(spot);
            ASSERT_TRUE(name);
            EXPECT_EQ(*name, ink);
        }
        EXPECT_FALSE(Spot::find_spot(reloaded.get(), "swatch-control"));
    }

    if (one_of(fixture, {"cmyk-device", "rgb-control"})) {
        auto const collected = collect(*doc);

        // The second gradient stop proves presentation attributes are collected
        // and not only the style attribute.
        auto const stop2 = collected.find("stop2");
        ASSERT_NE(stop2, collected.end()) << "presentation attribute stop-color was not collected";
        EXPECT_NE(stop2->second.find("stop-color"), stop2->second.end());

        // No icc-color or spot machinery may appear from nowhere.
        for (auto const &[id, props] : collected) {
            for (auto const &[prop, value] : props) {
                SCOPED_TRACE(id + "." + prop);
                EXPECT_EQ(value.find("icc-color("), std::string::npos);
                EXPECT_EQ(props.count("inkscape:spot-name"), 0u);
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(ColorCorpus, ColorCorpusTest, ::testing::ValuesIn(FIXTURES),
                         [](::testing::TestParamInfo<char const *> const &info) {
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

} // namespace

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
