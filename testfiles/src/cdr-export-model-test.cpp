// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the drawing export model: pages and units, shape kinds, verbatim
 * CMYK and spot inks, strokes and opacity, the fallbacks (each with its
 * warning), master layers, and deterministic output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <src/extension/internal/cdr-export-model.h>
#include <src/extension/internal/cdr-import-mapping.h>

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

#include "colors/cms/system.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-object.h"
#include "xml/node.h"
#include "xml/repr.h"

using Inkscape::Colors::RenderingIntent;
using Inkscape::Extension::Internal::CdrExport::build_json;

namespace {

std::string const icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
std::string const kProfile = icc_dir + "default_cmyk.icc";

/// Millimetres per CSS pixel, the same factor the model uses.
double mm_per_px_gtest()
{
    return 25.4 / 96.0;
}

// ------------------------------------------------------------ minimal JSON --

/// A parsed JSON value: object, array, string, number, bool or null. Just
/// enough to assert the model's structure instead of its exact text.
struct Json
{
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> members;

    bool is(Kind k) const { return kind == k; }
    Json const *find(char const *key) const
    {
        auto const it = members.find(key);
        return it == members.end() ? nullptr : &it->second;
    }
    Json const &at(char const *key) const
    {
        auto const *value = find(key);
        EXPECT_TRUE(value) << key;
        static Json const none;
        return value ? *value : none;
    }
    double num(char const *key) const { return at(key).number; }
    std::string str(char const *key) const { return at(key).text; }
};

class Parser
{
public:
    /// Throws std::runtime_error on malformed input.
    explicit Parser(std::string const &text)
        : _text(text)
    {
    }

    Json parse()
    {
        _pos = 0;
        return value();
    }

private:
    [[noreturn]] static void bad(char const *what)
    {
        throw std::runtime_error(what);
    }

    void skip()
    {
        while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos]))) {
            ++_pos;
        }
    }

    char peek()
    {
        skip();
        return _pos < _text.size() ? _text[_pos] : '\0';
    }

    void expect(char c)
    {
        if (peek() != c) {
            bad("unexpected character");
        }
        ++_pos;
    }

    Json value()
    {
        switch (peek()) {
            case '{': return object();
            case '[': return array();
            case '"': {
                Json out;
                out.kind = Json::Kind::String;
                out.text = string();
                return out;
            }
            case 't':
                _pos += 4;
                return boolean(true);
            case 'f':
                _pos += 5;
                return boolean(false);
            case 'n':
                _pos += 4;
                return Json{};
            default: return number();
        }
    }

    Json boolean(bool flag)
    {
        Json out;
        out.kind = Json::Kind::Bool;
        out.boolean = flag;
        return out;
    }

    Json object()
    {
        expect('{');
        Json out;
        out.kind = Json::Kind::Object;
        if (peek() == '}') {
            ++_pos;
            return out;
        }
        while (true) {
            auto const key = string();
            expect(':');
            out.members[key] = value();
            char const next = peek();
            ++_pos;
            if (next == '}') {
                break;
            }
            if (next != ',') {
                bad("expected , or }");
            }
        }
        return out;
    }

    Json array()
    {
        expect('[');
        Json out;
        out.kind = Json::Kind::Array;
        if (peek() == ']') {
            ++_pos;
            return out;
        }
        while (true) {
            out.items.push_back(value());
            char const next = peek();
            ++_pos;
            if (next == ']') {
                break;
            }
            if (next != ',') {
                bad("expected , or ]");
            }
        }
        return out;
    }

    std::string string()
    {
        expect('"');
        std::string out;
        while (_pos < _text.size() && _text[_pos] != '"') {
            if (_text[_pos] == '\\' && _pos + 1 < _text.size()) {
                ++_pos;
                switch (_text[_pos]) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    default: out.push_back(_text[_pos]);
                }
            } else {
                out.push_back(_text[_pos]);
            }
            ++_pos;
        }
        expect('"');
        return out;
    }

    Json number()
    {
        skip();
        std::size_t const start = _pos;
        while (_pos < _text.size() && (std::isdigit(static_cast<unsigned char>(_text[_pos])) ||
                                       _text[_pos] == '-' || _text[_pos] == '+' ||
                                       _text[_pos] == '.' || _text[_pos] == 'e' ||
                                       _text[_pos] == 'E')) {
            ++_pos;
        }
        if (start == _pos) {
            bad("expected a number");
        }
        // from_chars, so the test's locale cannot matter.
        Json out;
        out.kind = Json::Kind::Number;
        auto const text = _text.substr(start, _pos - start);
        auto const result = std::from_chars(text.data(), text.data() + text.size(), out.number);
        if (result.ec != std::errc()) {
            bad("bad number");
        }
        return out;
    }

    std::string const &_text;
    std::size_t _pos = 0;
};

Json parse_model(std::string const &json)
{
    Json out;
    try {
        out = Parser(json).parse();
        EXPECT_TRUE(out.is(Json::Kind::Object));
        EXPECT_EQ(out.str("schema"), "rosepen-cdr-export/1");
    } catch (std::exception const &err) {
        ADD_FAILURE() << "the model is not valid JSON: " << err.what() << ": " << json;
    }
    return out;
}

// ---------------------------------------------------------------- fixtures --

/// The kind of the i-th shape of the first layer of the first page.
Json const *shape_at(Json const &model, std::size_t page, std::size_t layer, std::size_t shape)
{
    auto const &pages = model.at("pages");
    if (page >= pages.items.size()) {
        return nullptr;
    }
    auto const &layers = pages.items[page].at("layers");
    if (layer >= layers.items.size()) {
        return nullptr;
    }
    auto const &shapes = layers.items[layer].at("shapes");
    if (shape >= shapes.items.size()) {
        return nullptr;
    }
    return &shapes.items[shape];
}

std::size_t count_shapes(Json const &model)
{
    std::size_t total = 0;
    for (auto const &page : model.at("pages").items) {
        for (auto const &layer : page.at("layers").items) {
            total += layer.at("shapes").items.size();
        }
    }
    return total;
}

bool has_warning(std::vector<std::string> const &warnings, char const *needle)
{
    for (auto const &warning : warnings) {
        if (warning.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

SPDocument *doc_from(char const *svg)
{
    auto doc = SPDocument::createNewDocFromMem({svg, std::strlen(svg)});
    EXPECT_TRUE(doc);
    doc->ensureUpToDate();
    return doc.release();
}

/// The model of one inline document.
std::pair<Json, std::vector<std::string>> model_of(char const *svg)
{
    std::unique_ptr<SPDocument> doc(doc_from(svg));
    auto result = build_json(doc.get());
    if (char const *dump = getenv("RP_DUMP_MODEL")) {
        fprintf(stderr, "MODEL[%s]: %s\n", dump, result.json.c_str());
    }
    return {parse_model(result.json), std::move(result.warnings)};
}

} // namespace

class CdrExportModelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);

        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        cms.addDirectoryPath(icc_dir, false);
        cms.refreshProfiles();
    }
};

TEST_F(CdrExportModelTest, pages_keep_their_sizes_and_units)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="210mm" height="297mm" viewBox="0 0 210 297">
  <sodipodi:namedview xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" id="nv" inkscape:document-units="mm"/>
  <defs>
    <view id="p1" viewBox="0 0 210 297" inkscape:label="Page 1"/>
    <view id="p2" viewBox="210 0 100 50" inkscape:label="Page 2"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:label="Artwork">
    <rect id="p2-rect" x="220" y="5" width="20" height="10" fill="#3366cc"/>
  </g>
</svg>)SVG");

    ASSERT_EQ(warnings.size(), 0u);
    auto const &pages = model.at("pages");
    ASSERT_EQ(pages.items.size(), 2u);
    EXPECT_NEAR(pages.items[0].num("width_mm"), 210.0, 1e-6);
    EXPECT_NEAR(pages.items[0].num("height_mm"), 297.0, 1e-6);
    EXPECT_NEAR(pages.items[1].num("width_mm"), 100.0, 1e-6);
    EXPECT_NEAR(pages.items[1].num("height_mm"), 50.0, 1e-6);
    EXPECT_EQ(pages.items[0].str("name"), "Page 1");
    EXPECT_EQ(pages.items[1].str("name"), "Page 2");

    // The rect sits at document (220, 5) mm: page 2 starts at x = 210 mm, so
    // the model carries it at (10, 5) mm of page 2, in page millimetres.
    auto const *rect = shape_at(model, 1, 0, 0);
    ASSERT_TRUE(rect);
    EXPECT_EQ(rect->str("kind"), "rect");
    EXPECT_NEAR(rect->num("x"), 10.0, 1e-6);
    EXPECT_NEAR(rect->num("y"), 5.0, 1e-6);
    EXPECT_NEAR(rect->num("w"), 20.0, 1e-6);
    EXPECT_NEAR(rect->num("h"), 10.0, 1e-6);
    EXPECT_NEAR(rect->at("fill").num("r"), 0x33 / 255.0, 1e-6);
    // And it is not on page 1: the layer is dropped there.
    EXPECT_EQ(count_shapes(model), 1u);
    EXPECT_TRUE(model.at("pages").items[0].at("layers").items.empty());
}

TEST_F(CdrExportModelTest, shape_kinds_follow_the_transforms)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <rect id="rot" x="10" y="10" width="20" height="10" fill="#3366cc" transform="rotate(30 20 15)"/>
  <circle id="circ" cx="50" cy="50" r="12" fill="#ff0000"/>
  <rect id="plain" x="60" y="10" width="10" height="10" fill="#00ff00" transform="translate(5 5)"/>
  <rect id="gone" x="10" y="80" width="10" height="10" fill="#101010" style="display:none"/>
</svg>)SVG");

    EXPECT_EQ(warnings.size(), 0u) << (warnings.empty() ? "" : warnings[0]);

    // Layer z-order (document order): the rotated rect first.
    auto const *rotated = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(rotated);
    EXPECT_EQ(rotated->str("kind"), "path");
    EXPECT_TRUE(rotated->at("d").is(Json::Kind::String)) << rotated->at("d").text;

    auto const *circle = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(circle);
    EXPECT_EQ(circle->str("kind"), "ellipse");
    EXPECT_NEAR(circle->num("cx"), 50.0 * mm_per_px_gtest(), 1e-6);
    EXPECT_NEAR(circle->num("rx"), 12.0 * mm_per_px_gtest(), 1e-6);

    // The translated rect keeps its kind; the translation rides the origin.
    auto const *plain = shape_at(model, 0, 0, 2);
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->str("kind"), "rect");
    EXPECT_NEAR(plain->num("x"), 65.0 * mm_per_px_gtest(), 1e-6);

    // The individually hidden item is absent, silently.
    EXPECT_EQ(count_shapes(model), 3u);
}

TEST_F(CdrExportModelTest, colours_travel_verbatim_or_as_inks)
{
    std::unique_ptr<SPDocument> doc(doc_from(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="100" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="spot" inkscape:swatch="solid" inkscape:spot-name="Test Ink" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" style="stop-color:device-cmyk(0 0.5 1 0)"/>
    </linearGradient>
  </defs>
  <rect id="dev" x="5" y="5" width="20" height="10" style="fill:device-cmyk(0.1 0.2 0.3 0.4)"/>
  <rect id="rgb" x="30" y="5" width="20" height="10" fill="#ff0000"/>
  <rect id="spot-rect" x="5" y="25" width="20" height="10" style="fill:url(#spot)"/>
  <rect id="icc" x="30" y="25" width="20" height="10" fill="#907f6f"/>
</svg>)SVG"));

    auto const profile = Inkscape::Extension::Internal::CdrImport::attach_output_profile(
        doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(profile);

    // One paint becomes an icc-color of the attached profile; writing it
    // through the repr re-parses it against the attached profile, exactly
    // like an imported drawing carries it.
    {
        auto *object = doc->getObjectById("icc");
        ASSERT_TRUE(object);
        SPCSSAttr *css = sp_repr_css_attr(object->getRepr(), "style");
        sp_repr_css_set_property(
            css, "fill",
            ("#907f6f icc-color(" + *profile + ", 0.1, 0.2, 0.3, 0.4)").c_str());
        sp_repr_css_change(object->getRepr(), css, "style");
        sp_repr_css_attr_unref(css);
    }
    doc->ensureUpToDate();

    auto result = build_json(doc.get());
    auto const model = parse_model(result.json);

    // The device-cmyk paint travels verbatim.
    auto const *dev = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(dev);
    ASSERT_EQ(dev->at("fill").str("model"), "cmyk");
    EXPECT_NEAR(dev->at("fill").num("c"), 0.1, 1e-9);
    EXPECT_NEAR(dev->at("fill").num("m"), 0.2, 1e-9);
    EXPECT_NEAR(dev->at("fill").num("y"), 0.3, 1e-9);
    EXPECT_NEAR(dev->at("fill").num("k"), 0.4, 1e-9);

    auto const *rgb = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(rgb);
    EXPECT_EQ(rgb->at("fill").str("model"), "rgb");
    EXPECT_NEAR(rgb->at("fill").num("r"), 1.0, 1e-9);
    EXPECT_NEAR(rgb->at("fill").num("g"), 0.0, 1e-9);

    auto const *spot = shape_at(model, 0, 0, 2);
    ASSERT_TRUE(spot);
    EXPECT_EQ(spot->at("fill").str("model"), "spot");
    EXPECT_NEAR(spot->at("fill").num("ink"), 0.0, 1e-9);
    EXPECT_NEAR(spot->at("fill").num("tint"), 1.0, 1e-9);

    // The icc-color paint of the attached CMYK profile travels verbatim too.
    auto const *icc = shape_at(model, 0, 0, 3);
    ASSERT_TRUE(icc);
    ASSERT_EQ(icc->at("fill").str("model"), "cmyk");
    EXPECT_NEAR(icc->at("fill").num("c"), 0.1, 1e-9);
    EXPECT_NEAR(icc->at("fill").num("m"), 0.2, 1e-9);
    EXPECT_NEAR(icc->at("fill").num("y"), 0.3, 1e-9);
    EXPECT_NEAR(icc->at("fill").num("k"), 0.4, 1e-9);

    auto const &inks = model.at("inks");
    ASSERT_EQ(inks.items.size(), 1u);
    EXPECT_EQ(inks.items[0].str("name"), "Test Ink");
    auto const &cmyk = inks.items[0].at("cmyk");
    ASSERT_EQ(cmyk.items.size(), 4u);
    EXPECT_NEAR(cmyk.items[0].number, 0.0, 1e-6);
    EXPECT_NEAR(cmyk.items[1].number, 0.5, 1e-6);
    EXPECT_NEAR(cmyk.items[2].number, 1.0, 1e-6);
    EXPECT_NEAR(cmyk.items[3].number, 0.0, 1e-6);
}

TEST_F(CdrExportModelTest, strokes_dashes_and_opacity_scale_to_millimetres)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <rect id="dash" x="5" y="5" width="20" height="10" fill="none" stroke="#000000"
        stroke-width="2" stroke-dasharray="4 2" stroke-linecap="round" stroke-linejoin="round"/>
  <rect id="half" x="30" y="5" width="20" height="10" fill="#123456" opacity="0.5"/>
</svg>)SVG");

    EXPECT_EQ(warnings.size(), 0u);
    auto const *dash = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(dash);
    auto const &stroke = dash->at("stroke");
    EXPECT_EQ(stroke.str("cap"), "round");
    EXPECT_EQ(stroke.str("join"), "round");
    EXPECT_NEAR(stroke.num("width_mm"), 2.0 * mm_per_px_gtest(), 1e-9);
    auto const &dashes = stroke.at("dash_mm");
    ASSERT_EQ(dashes.items.size(), 2u);
    EXPECT_NEAR(dashes.items[0].number, 4.0 * mm_per_px_gtest(), 1e-9);
    EXPECT_NEAR(dashes.items[1].number, 2.0 * mm_per_px_gtest(), 1e-9);
    EXPECT_TRUE(dash->at("fill").is(Json::Kind::Null));

    auto const *half = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(half);
    EXPECT_NEAR(half->num("opacity"), 0.5, 1e-9);
}

TEST_F(CdrExportModelTest, hidden_layers_are_announced_not_lost)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="100" viewBox="0 0 100 100">
  <g inkscape:groupmode="layer" inkscape:label="Artwork">
    <rect id="a" x="5" y="5" width="10" height="10" fill="#3366cc"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Hidden notes" style="display:none">
    <rect id="h" x="5" y="20" width="10" height="10" fill="#454545"/>
  </g>
</svg>)SVG");

    // The hidden layer goes out visible (the format stores no hidden layers),
    // announced by a warning; its shape is not lost.
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("Hidden notes"), std::string::npos);
    EXPECT_NE(warnings[0].find("hidden layer"), std::string::npos);

    auto const &pages = model.at("pages");
    ASSERT_EQ(pages.items.size(), 1u);
    auto const &layers = pages.items[0].at("layers");
    ASSERT_EQ(layers.items.size(), 2u);
    EXPECT_EQ(layers.items[0].str("name"), "Artwork");
    EXPECT_EQ(layers.items[1].str("name"), "Hidden notes");
    EXPECT_EQ(count_shapes(model), 2u) << "the hidden layer's shapes are exported";
}

TEST_F(CdrExportModelTest, fallbacks_are_named)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="grad" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" style="stop-color:#ff0000"/>
      <stop offset="1" style="stop-color:#0000ff"/>
    </linearGradient>
    <clipPath id="clip"><rect x="0" y="0" width="5" height="5"/></clipPath>
    <rect id="orig" x="0" y="0" width="8" height="4" fill="#abcdef"/>
    <image id="img" width="2" height="2" xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGNwaDgARAwQCgAoDgYBQzpj2gAAAABJRU5ErkJggg=="/>
  </defs>
  <rect id="grad" x="5" y="5" width="20" height="10" style="fill:url(#grad)"/>
  <use id="clone" x="30" y="5" xlink:href="#orig"/>
  <rect id="clipped" x="5" y="20" width="20" height="10" fill="#101010" clip-path="url(#clip)"/>
  <rect id="offpage" x="500" y="500" width="5" height="5" fill="#222222"/>
</svg>)SVG");

    EXPECT_TRUE(has_warning(warnings, "grad"));
    EXPECT_TRUE(has_warning(warnings, "clone"));
    EXPECT_TRUE(has_warning(warnings, "clipped"));
    EXPECT_TRUE(has_warning(warnings, "offpage"));

    auto const *gradient = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(gradient);
    ASSERT_EQ(gradient->at("fill").str("model"), "rgb");
    EXPECT_NEAR(gradient->at("fill").num("r"), 0.5, 1e-9); // mean of red and blue
    EXPECT_NEAR(gradient->at("fill").num("g"), 0.0, 1e-9);
    EXPECT_NEAR(gradient->at("fill").num("b"), 0.5, 1e-9);

    // The clone renders the referenced rect at the use's position.
    auto const *clone = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(clone);
    EXPECT_EQ(clone->str("kind"), "rect");
    EXPECT_NEAR(clone->num("x"), (30 + 0.0) * mm_per_px_gtest(), 1e-6);
    EXPECT_NEAR(clone->num("y"), 5.0 * mm_per_px_gtest(), 1e-6);

    // The clipped rect is exported unclipped; the off-page item lands on
    // page 1 with a warning.
    EXPECT_EQ(count_shapes(model), 4u);
}

TEST_F(CdrExportModelTest, images_are_skipped_with_a_warning)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100" viewBox="0 0 100 100">
  <image id="img" x="5" y="5" width="10" height="10" xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGNwaDgARAwQCgAoDgYBQzpj2gAAAABJRU5ErkJggg=="/>
  <rect id="r" x="30" y="5" width="10" height="10" fill="#3366cc"/>
</svg>)SVG");

    EXPECT_TRUE(has_warning(warnings, "image"));
    EXPECT_EQ(count_shapes(model), 1u);
}

TEST_F(CdrExportModelTest, text_keeps_its_kind_when_it_can)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <text id="hello" x="5" y="30" style="font-family:DejaVu Sans;font-size:12px">Hello</text>
  <text id="rot" x="5" y="60" transform="rotate(15 5 60)" style="font-family:DejaVu Sans;font-size:12px">Rotated</text>
</svg>)SVG");

    auto const *hello = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(hello);
    EXPECT_EQ(hello->str("kind"), "text");
    EXPECT_NEAR(hello->num("x"), 5.0 * mm_per_px_gtest(), 1e-6);
    auto const &spans = hello->at("spans");
    ASSERT_EQ(spans.items.size(), 1u);
    EXPECT_EQ(spans.items[0].str("text"), "Hello");
    EXPECT_EQ(spans.items[0].str("family"), "DejaVu Sans");
    EXPECT_NEAR(spans.items[0].num("size_pt"), 9.0, 1e-6) << "12 px are 9 pt";

    // The rotated text becomes outlines, with a warning.
    auto const *rotated = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(rotated);
    EXPECT_EQ(rotated->str("kind"), "path");
    EXPECT_TRUE(has_warning(warnings, "outlines"));
}

// 06-02 gate: paints a group hands down are computed paints of its items.
TEST_F(CdrExportModelTest, inherited_paints_are_exported)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="grad" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" style="stop-color:#ff0000"/>
      <stop offset="1" style="stop-color:#0000ff"/>
    </linearGradient>
  </defs>
  <g id="outer" style="fill:#3366cc;stroke:#ff0000;stroke-width:2">
    <rect id="a" x="5" y="5" width="10" height="10"/>
  </g>
  <g id="graded" style="fill:url(#grad)">
    <rect id="b" x="30" y="5" width="10" height="10"/>
  </g>
</svg>)SVG");

    auto const *group = shape_at(model, 0, 0, 0);
    ASSERT_TRUE(group);
    ASSERT_EQ(group->str("kind"), "group");
    auto const &rect = group->at("children").items.at(0);
    ASSERT_EQ(rect.at("fill").str("model"), "rgb");
    EXPECT_NEAR(rect.at("fill").num("r"), 0x33 / 255.0, 1e-6);
    ASSERT_TRUE(rect.at("stroke").is(Json::Kind::Object)) << "the inherited stroke is kept";
    EXPECT_NEAR(rect.at("stroke").num("width_mm"), 2.0 * mm_per_px_gtest(), 1e-9);

    auto const *graded = shape_at(model, 0, 0, 1);
    ASSERT_TRUE(graded);
    auto const &child = graded->at("children").items.at(0);
    ASSERT_EQ(child.at("fill").str("model"), "rgb") << "the inherited gradient is not lost";
    EXPECT_NEAR(child.at("fill").num("r"), 0.5, 1e-9);
    EXPECT_TRUE(has_warning(warnings, "gradient"));
}

// 06-02 gate: text that one plain span cannot carry becomes outlines, named.
TEST_F(CdrExportModelTest, complex_text_becomes_outlines)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="100" height="100" viewBox="0 0 100 100">
  <text id="lines" x="5" y="20" style="font-family:DejaVu Sans;font-size:12px"><tspan sodipodi:role="line" x="5" y="20">One</tspan><tspan sodipodi:role="line" x="5" y="35">Two</tspan></text>
  <text id="runs" x="5" y="60" style="font-family:DejaVu Sans;font-size:12px">Plain <tspan style="font-weight:bold">bold</tspan></text>
  <text id="mirror" x="5" y="80" transform="scale(-1 1) translate(-60 0)" style="font-family:DejaVu Sans;font-size:12px">Mirror</text>
  <text id="line" x="5" y="95" style="font-family:DejaVu Sans;font-size:12px"><tspan sodipodi:role="line" x="5" y="95" style="stroke-width:0.5">Single</tspan></text>
</svg>)SVG");

    for (std::size_t i = 0; i < 3; ++i) {
        auto const *shape = shape_at(model, 0, 0, i);
        ASSERT_TRUE(shape) << i;
        EXPECT_EQ(shape->str("kind"), "path") << i;
    }
    EXPECT_TRUE(has_warning(warnings, "lines: multi-line text"));
    EXPECT_TRUE(has_warning(warnings, "runs: text with differently styled runs"));
    EXPECT_TRUE(has_warning(warnings, "mirror: transformed text"));

    // One line in a styled tspan that draws alike is still plain text.
    auto const *line = shape_at(model, 0, 0, 3);
    ASSERT_TRUE(line);
    EXPECT_EQ(line->str("kind"), "text");
    EXPECT_EQ(line->at("spans").items.at(0).str("text"), "Single");
}

TEST_F(CdrExportModelTest, master_layers_travel_in_the_master_list)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="100" viewBox="0 0 100 100">
  <g inkscape:groupmode="layer" inkscape:label="Page layer">
    <rect id="a" x="5" y="5" width="10" height="10" fill="#3366cc"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Background" inkscape:master-layer="true">
    <rect id="m" x="0" y="0" width="100" height="100" fill="#eeeeee"/>
  </g>
</svg>)SVG");

    EXPECT_EQ(warnings.size(), 0u);
    auto const &master = model.at("master");
    ASSERT_EQ(master.items.size(), 1u);
    EXPECT_EQ(master.items[0].str("name"), "Background");
    ASSERT_EQ(master.items[0].at("shapes").items.size(), 1u);
    EXPECT_EQ(master.items[0].at("shapes").items[0].str("kind"), "rect");

    // The master's shapes are not repeated in the page layers.
    EXPECT_EQ(count_shapes(model), 1u);
}

TEST_F(CdrExportModelTest, an_empty_document_is_one_empty_page)
{
    auto const [model, warnings] = model_of(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"/>)SVG");

    EXPECT_EQ(warnings.size(), 0u);
    auto const &pages = model.at("pages");
    ASSERT_EQ(pages.items.size(), 1u);
    EXPECT_NEAR(pages.items[0].num("width_mm"), 100.0 * mm_per_px_gtest(), 1e-6);
    EXPECT_EQ(count_shapes(model), 0u);
    EXPECT_TRUE(model.at("master").items.empty());
    EXPECT_TRUE(model.at("inks").items.empty());
}

TEST_F(CdrExportModelTest, the_model_is_deterministic)
{
    char const *svg =
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="100" height="100" viewBox="0 0 100 100">
  <g inkscape:groupmode="layer" inkscape:label="Artwork">
    <rect id="r" x="5" y="5" width="10" height="10" fill="#3366cc"/>
    <ellipse id="e" cx="50" cy="50" rx="8" ry="4" fill="#ff0000"/>
  </g>
</svg>)SVG";

    std::unique_ptr<SPDocument> doc(doc_from(svg));
    auto const first = build_json(doc.get());
    auto const second = build_json(doc.get());
    EXPECT_EQ(first.json, second.json);
    EXPECT_EQ(first.warnings, second.warnings);
}
