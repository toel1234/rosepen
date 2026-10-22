// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Specific functionality for image handling
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "bad-uri-exception.h"
#include "build-drawing.h"
#include "build-page.h"
#include "colors/cms/profile.h"
#include "colors/cms/transform-surface.h"
#include "colors/spaces/cms.h"
#include "display/cairo-utils.h"
#include "display/drawing-item.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/uri.h"
#include "page-manager.h"
#include "style.h"
#include "svg/svg.h"
#include "util/units.h"
#include "util/uri.h"
#include "xml/href-attribute-helper.h"
#include "xml/repr.h"

#include <cstddef>

namespace Inkscape::Extension::Internal::PdfBuilder {

namespace {

/** An 8-bit raster converted to DeviceCMYK, with an optional separate SMask. */
struct CmykRaster
{
    std::vector<char> pixels;
    std::vector<char> alpha;
    int width = 0;
    int height = 0;
    bool has_alpha = false;
};

/**
 * Convert an unpremultiplied 8-bit RGB buffer to DeviceCMYK in the PDF/X
 * output intent. The alpha plane is kept as-is; the caller decides whether it
 * becomes an SMask (X-4) or is dropped because the image is fully opaque.
 */
std::optional<CmykRaster> rgb_to_cmyk(char const *rgb, std::size_t rgb_size, char const *alpha,
                                      std::size_t alpha_size, int width, int height,
                                      std::shared_ptr<Colors::Space::CMS> const &output_space)
{
    if (!output_space || width <= 0 || height <= 0 ||
        rgb_size < static_cast<std::size_t>(width) * height * 3) {
        return std::nullopt;
    }

    auto const out_profile = output_space->getProfile();
    auto const srgb = Colors::CMS::Profile::create_srgb();
    if (!out_profile || !srgb) {
        return std::nullopt;
    }

    // sRGB in (image data is interpreted as sRGB), the document's output
    // profile out, at the output space's rendering intent.
    auto const transform = Colors::CMS::TransformSurface::create<uint8_t>(
        srgb, out_profile, output_space->getIntent(), nullptr, Colors::RenderingIntent::AUTO, false,
        false, false, false);
    if (!transform) {
        return std::nullopt;
    }

    CmykRaster out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    transform.do_transform<uint8_t>(width, height, reinterpret_cast<uint8_t const *>(rgb),
                                    reinterpret_cast<uint8_t *>(out.pixels.data()), width * 3,
                                    width * 4);

    auto const pixels = static_cast<std::size_t>(width) * height;
    if (alpha && alpha_size >= pixels) {
        auto const *values = reinterpret_cast<unsigned char const *>(alpha);
        out.has_alpha = false;
        for (std::size_t i = 0; i < pixels; ++i) {
            if (values[i] != 255) {
                out.has_alpha = true;
                break;
            }
        }
        out.alpha.assign(alpha, alpha + pixels);
    }
    return out;
}

/**
 * Read an image's pixels as unpremultiplied 8-bit RGBA through the document's
 * already-loaded Pixbuf.
 */
bool pixbuf_to_rgba(Inkscape::Pixbuf const &pixbuf, std::vector<char> &rgb, std::vector<char> &alpha,
                    int &width, int &height)
{
    // The exporter gets a const Pixbuf; copy it so the pixel format can be
    // normalised to GdkPixbuf's non-premultiplied RGBA.
    Inkscape::Pixbuf copy(pixbuf);
    copy.ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);

    width = copy.width();
    height = copy.height();
    auto const *data = copy.pixels();
    if (width <= 0 || height <= 0 || !data) {
        return false;
    }

    auto const stride = copy.rowstride();
    auto const count = static_cast<std::size_t>(width) * height;
    rgb.reserve(count * 3);
    alpha.reserve(count);
    for (int y = 0; y < height; ++y) {
        auto const *row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            auto const *pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
            rgb.push_back(static_cast<char>(pixel[0]));
            rgb.push_back(static_cast<char>(pixel[1]));
            rgb.push_back(static_cast<char>(pixel[2]));
            alpha.push_back(static_cast<char>(pixel[3]));
        }
    }
    return true;
}

/** Build and paint a CMYK raster, keeping the alpha only where PDF/X allows it. */
CapyPDF_ImageId add_cmyk_raster(capypdf::Generator &gen, CmykRaster const &raster,
                                capypdf::ImagePdfProperties &props, bool pdfx_1a)
{
    auto builder = capypdf::RasterImageBuilder();
    builder.set_size(raster.width, raster.height);
    builder.set_colorspace(CAPY_IMAGE_CS_CMYK);
    builder.set_pixel_depth(8);
    builder.set_pixel_data(raster.pixels.data(), raster.pixels.size());
    if (raster.has_alpha) {
        if (pdfx_1a) {
            // Preflight refuses every transparent image on X-1a, so this is
            // only reachable for an image preflight cannot see (a paint-server
            // child, for example). Drop the alpha instead of aborting.
            g_warning("PDF/X-1a: dropping the alpha channel of an image that was not preflighted.");
        } else {
            builder.set_alpha_depth(8);
            builder.set_alpha_data(raster.alpha.data(), raster.alpha.size());
        }
    }

    auto image = builder.build();
    return gen.add_image(image, props);
}

} // namespace

static CapyPDF_Image_Interpolation get_interpolation(SPImageRendering rendering)
{
    switch (rendering) {
        case SP_CSS_IMAGE_RENDERING_OPTIMIZEQUALITY:
            return CAPY_INTERPOLATION_SMOOTH;
            break;
        case SP_CSS_IMAGE_RENDERING_OPTIMIZESPEED:
        case SP_CSS_IMAGE_RENDERING_PIXELATED:
        case SP_CSS_IMAGE_RENDERING_CRISPEDGES:
            return CAPY_INTERPOLATION_PIXELATED;
            break;
        case SP_CSS_IMAGE_RENDERING_AUTO:
        default:
            break;
    }
    return CAPY_INTERPOLATION_AUTO;
}

/**
 * Draw the raster data stored in URI into the PDF context.
 */
void DrawContext::paint_raster(SPImage const *image)
{
    auto image_box = image->bbox(Geom::identity(), SPItem::GEOMETRIC_BBOX);
    if (!image_box) {
        g_warning("Couldn't get the size of image, ignoring in PDF output.");
        return;
    }

    auto props = capypdf::ImagePdfProperties();
    props.set_interpolate(get_interpolation(image->style->image_rendering.computed));
    // TODO: props.set_conversion_intent(...)

    if (_doc.is_pdfx()) {
        // PDF/X only permits a CMYK image with the output intent. Decode
        // through the document's Pixbuf instead of the capypdf loaders, which
        // reject RGB under PDF/X. An embedded SVG image is rasterised here.
        std::vector<char> rgb;
        std::vector<char> alpha;
        int width = 0;
        int height = 0;
        if (!image->pixbuf || !pixbuf_to_rgba(*image->pixbuf, rgb, alpha, width, height)) {
            g_warning("No image loaded for image tag.");
            return;
        }
        auto const cmyk =
            rgb_to_cmyk(rgb.data(), rgb.size(), alpha.data(), alpha.size(), width, height,
                        _doc.output_space());
        if (!cmyk) {
            g_warning("Couldn't convert image to the PDF/X output profile.");
            return;
        }

        auto const image_id =
            add_cmyk_raster(_doc.generator(), *cmyk, props, _doc.is_pdfx_1a());

        auto paint_box = image->get_paintbox(cmyk->width, cmyk->height, image_box);
        auto outside =
            Geom::Affine(paint_box->width(), 0, 0, -paint_box->height(), paint_box->left(), paint_box->bottom());

        _ctx.cmd_q();
        transform(outside);
        _ctx.cmd_Do(image_id);
        _ctx.cmd_Q();
        return;
    }

    // If pixbuf is requested AFTER getURI it will sometimes return zero. This is a bug.
    auto img_width = image->pixbuf->width();
    auto img_height = image->pixbuf->height();

    auto href = Inkscape::getHrefAttribute(*image->getRepr()).second;
    auto [base64, base64_type] = extract_uri_data(href);
    std::optional<CapyPDF_ImageId> raster_id;
    std::unique_ptr<SPDocument> svg;

    gsize decoded_len = 0;
    const char *decoded = nullptr;

    if (base64_type == Base64Data::NONE) {
        // We don't use Inkscape::URI for base64 because if it's memory limits
        try {
            auto uri = image->getURI();
            if (uri.getMimeType() == "image/svg+xml") {
                svg = SPDocument::createNewDoc(uri.toNativeFilename().c_str());
            } else {
                // This is cached against the doc cache as the same filename might
                // be loaded multiple times in the same document.
                raster_id = _doc.get_image(uri.toNativeFilename(), props);
            }
        } catch (Inkscape::BadURIException const &e) {
            g_warning("Couldn't read image: %s", e.what());
        } catch (std::exception const &e) {
            g_warning("Could not add image file to PDF: %s", e.what());
        }
    } else {
        decoded = (char *)g_base64_decode(base64, &decoded_len);
    }

    if (decoded && base64_type == Base64Data::RASTER) {
        try {
            // Note this memory image is not cached.
            auto image = _doc._gen.load_image_from_memory(decoded, decoded_len);
            raster_id = _doc._gen.add_image(image, props);
        } catch (std::exception &e) {
            g_warning("Could not add image file to PDF: %s", e.what());
        }
    }

    if (decoded && base64_type == Base64Data::SVG) {
        // Sizing and painting the loaded document depends on the SVG being
        // up to date and having a viewBox and width/height that makes sense.
        svg = SPDocument::createNewDocFromMem({decoded, decoded_len});
    }

    if (svg) {
        svg->ensureUpToDate();
        auto doc_width = svg->getWidth().value("px");
        auto doc_height = svg->getHeight().value("px");

        // The inside is how the SVG is painted wrt its own viewbox in the svg tag.
        auto inside_box = svg->getRoot()->get_paintbox(doc_width, doc_height, svg->getViewBox());
        auto inside =
            Geom::Affine(inside_box->width(), 0, 0, inside_box->height(), inside_box->left(), inside_box->bottom());

        // The outside is where on the document this svg will be placed.
        auto outside_box = image->get_paintbox(doc_width, doc_height, image_box);
        auto outside =
            Geom::Affine(outside_box->width(), 0, 0, outside_box->height(), outside_box->left(), outside_box->bottom());

        if (auto drawing_id = _doc.item_to_transparency_group(svg->getRoot())) {
            _ctx.cmd_q();
            // Clip to the outside box, because svg's can run over their defined edges.
            set_clip_rectangle(outside_box);

            // The inverse of Inside reduces the svg rendering to a unit square.
            transform(inside.inverse() * outside);

            _ctx.cmd_Do(*drawing_id);
            _ctx.cmd_Q();
        } else {
            g_warning("Unable to paint embedded SVG image into PDF.");
        }
    } else if (raster_id) {
        // Format the width and height into a transformation matrix, the image is a unit square painted
        // from the bottom upwards so must be scaled out and flipped. No cropping is needed.
        auto paint_box = image->get_paintbox(img_width, img_height, image_box);
        auto outside =
            Geom::Affine(paint_box->width(), 0, 0, -paint_box->height(), paint_box->left(), paint_box->bottom());

        _ctx.cmd_q();
        transform(outside);
        _ctx.cmd_Do(*raster_id);
        _ctx.cmd_Q();
    } else {
        g_warning("No image loaded for image tag.");
    }

    g_free(const_cast<char *>(decoded));
}

/**
 * Draw an item as a bitmap and return.
 *
 * @arg item - The SPItem to convert to a bitmap
 * @arg tr - Item transformation to apply
 * @arg resolution - The DPI resolution to use.
 * @arg antialias - Optionally turn off antialiasing.
 */
void DrawContext::paint_item_to_raster(SPItem const *item, Geom::Affine const &tr, double resolution, bool antialias)
{
    auto doc = item->document;

    std::vector<SPItem const *> items = {item};
    auto const bbox = item->visualBounds(item->i2doc_affine(), true, false, true);
    auto const gbox = item->visualBounds(Geom::identity(), true, false, true);
    auto aa = antialias ? Antialiasing::Good : Antialiasing::None;

    auto pb = std::unique_ptr<Pixbuf>{sp_generate_internal_bitmap(doc, *bbox, resolution, items, false, nullptr, 1, aa)};
    if (!pb || pb->height() <= 0) {
        return;
    }

    auto surface = pb->getSurfaceRaw();

    cairo_surface_flush(surface);
    // cairo_surface_write_to_png(surface, "/tmp/out.png");
    auto data = cairo_image_surface_get_data(surface);
    auto width = cairo_image_surface_get_width(surface);
    auto height = cairo_image_surface_get_height(surface);
    auto stride = cairo_image_surface_get_stride(surface);

    auto builder = capypdf::RasterImageBuilder();
    builder.set_size(width, pb->height());
    builder.set_pixel_depth(8);

    std::vector<char> pixels;
    std::vector<char> alpha;
    pixels.reserve(width * height * 3);
    alpha.reserve(width * height * 1);

    // Split the color and alpha from each other
    for (int y = 0; y < height; y++) {
        int p = y * stride;
        for (int x = 0; x < width; x++) {
            // Cairo surfaces are alpha pre-multiplied, PDF is not.
            pixels.push_back(unpremul_alpha(data[p + 2], data[p + 3]));
            pixels.push_back(unpremul_alpha(data[p + 1], data[p + 3]));
            pixels.push_back(unpremul_alpha(data[p],     data[p + 3]));
            alpha.push_back(data[p + 3]);
            p += 4;
        }
    }

    auto props = capypdf::ImagePdfProperties();
    CapyPDF_ImageId image_id;
    if (_doc.is_pdfx()) {
        // A rasterized filter is an sRGB raster; PDF/X needs it in the output
        // intent as DeviceCMYK. Its raster alpha is refused on X-1a, so the
        // alpha is only kept as an SMask on X-4 and dropped when fully opaque.
        auto const cmyk = rgb_to_cmyk(pixels.data(), pixels.size(), alpha.data(), alpha.size(),
                                      width, height, _doc.output_space());
        if (!cmyk) {
            g_warning("Couldn't convert a rasterized filter to the PDF/X output profile.");
            return;
        }
        image_id = add_cmyk_raster(_doc.generator(), *cmyk, props, _doc.is_pdfx_1a());
    } else {
        builder.set_colorspace(CAPY_IMAGE_CS_RGB);
        builder.set_alpha_depth(8);
        builder.set_pixel_data(pixels.data(), pixels.size());
        builder.set_alpha_data(alpha.data(), alpha.size());

        auto image = builder.build();
        image_id = _doc.generator().add_image(image, props);
    }

    auto boxtr = Geom::Affine(gbox->width(), 0, 0, -gbox->height(), gbox->left(), gbox->bottom());

    _ctx.cmd_q();
    transform(boxtr * tr);
    _ctx.cmd_Do(image_id);
    _ctx.cmd_Q();
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
