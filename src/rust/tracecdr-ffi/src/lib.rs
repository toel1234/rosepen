// SPDX-License-Identifier: GPL-2.0-or-later
// Rosepen C ABI version 1 over the TraceDraw drawing-format reader and writer.
// Copyright (C) 2026 Authors
// Released under GNU GPL v2+, read the file 'COPYING' for more information.
//
// The crates linked into this static library are MIT OR Apache-2.0; Rosepen
// elects the MIT branch for GPL-2.0 compatibility. THIRD-PARTY.md lists every
// crate in the dependency graph with its licence.
//
// Every entry point checks its pointers and lengths first and runs its body
// inside `catch_unwind`, so a panic in the reader, the writer or the
// annotation pass becomes RP_ERR_PANIC instead of unwinding into C. Buffers
// handed to C are allocated with one trailing NUL byte and freed only by
// `rp_free(ptr, len)`; `len` is always the length the call wrote, never the
// NUL byte.

pub mod annotate;
pub mod export;

use std::ffi::{c_char, CStr};
use std::os::raw::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::sync::Once;

use tracedraw_cdr::ParseReport;
use tracedraw_core::{Document, LayerId};

/// Success.
pub const RP_OK: c_int = 0;
/// The bytes are not a drawing-format file (or exchange-format file).
pub const RP_ERR_NOT_CDR: c_int = 1;
/// The container is a form the reader does not support.
pub const RP_ERR_UNSUPPORTED: c_int = 2;
/// The file ends before its records do.
pub const RP_ERR_TRUNCATED: c_int = 3;
/// Archive or stream I/O failed.
pub const RP_ERR_CONTAINER: c_int = 4;
/// A null pointer, an empty buffer, bad UTF-8 or an out-of-range index.
pub const RP_ERR_ARG: c_int = 5;
/// The export model JSON is invalid or its schema is not the expected one.
pub const RP_ERR_JSON: c_int = 6;
/// The library caught a panic; nothing is guaranteed about the outputs.
pub const RP_ERR_PANIC: c_int = 7;

/// Counters a caller reports to the user (see `rp_cdr_info`).
#[repr(C)]
pub struct RpCdrInfo {
    pub version: u32,
    pub pages: u32,
    pub layers: u32,
    pub shapes: u32,
    pub skipped_objects: u32,
    pub warnings: u32,
}

/// An open drawing. Opaque to C; owned by the caller until `rp_cdr_close`.
///
/// The document is held with every layer made visible (the serializer drops
/// hidden ones); `hidden_layers` remembers which were hidden so the
/// annotation pass can mark their groups `display:none`.
pub struct RpCdrDoc {
    doc: Document,
    hidden_layers: Vec<LayerId>,
    report: ParseReport,
}

/// Run `f`, turning a panic into RP_ERR_PANIC.
fn guarded(f: impl FnOnce() -> c_int) -> c_int {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(RP_ERR_PANIC)
}

/// Load the text engine once per process, then the drawing's own fonts.
fn install_text(doc: &Document) {
    static TEXT: Once = Once::new();
    TEXT.call_once(tracedraw_text::install);
    if !doc.fonts.is_empty() {
        tracedraw_text::fonts().add_document_fonts(&doc.fonts);
    }
}

/// Map a reader error to its ABI code.
fn map_error(e: &tracedraw_cdr::Error) -> c_int {
    use tracedraw_cdr::Error;
    match e {
        Error::NotCdr | Error::NotCmx => RP_ERR_NOT_CDR,
        Error::UnsupportedContainer(_) => RP_ERR_UNSUPPORTED,
        Error::Truncated(_) => RP_ERR_TRUNCATED,
        Error::Io(_) | Error::Zip(_) => RP_ERR_CONTAINER,
    }
}

/// Hand `bytes` to C: NUL-terminated, `len` bytes before the NUL. Free with
/// `rp_free(ptr, len)`; the extra byte is accounted for there.
fn into_c_buffer(mut bytes: Vec<u8>) -> (*mut u8, usize) {
    let len = bytes.len();
    bytes.push(0);
    let boxed = bytes.into_boxed_slice();
    (Box::into_raw(boxed) as *mut u8, len)
}

/// The ABI version this library implements.
#[no_mangle]
pub extern "C" fn rp_abi_version() -> u32 {
    1
}

/// Open drawing-format bytes into a handle. `title` may be NULL.
///
/// On RP_OK `*out` receives a handle the caller closes with `rp_cdr_close`.
/// On any other code `*out` is left untouched.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_open(
    data: *const u8,
    len: usize,
    title: *const c_char,
    out: *mut *mut RpCdrDoc,
) -> c_int {
    guarded(|| {
        if out.is_null() || data.is_null() || len == 0 {
            return RP_ERR_ARG;
        }
        let title = if title.is_null() {
            "Untitled".to_string()
        } else {
            match unsafe { CStr::from_ptr(title) }.to_str() {
                Ok(s) => s.to_string(),
                Err(_) => return RP_ERR_ARG,
            }
        };
        let bytes = unsafe { slice::from_raw_parts(data, len) };
        match tracedraw_cdr::open_bytes(bytes, &title) {
            Ok((mut doc, report)) => {
                install_text(&doc);
                let mut hidden_layers = Vec::new();
                for layer in doc.all_layers_mut() {
                    if !layer.visible {
                        hidden_layers.push(layer.id);
                        layer.visible = true;
                    }
                }
                let handle = Box::new(RpCdrDoc {
                    doc,
                    hidden_layers,
                    report,
                });
                unsafe { *out = Box::into_raw(handle) };
                RP_OK
            }
            Err(e) => map_error(&e),
        }
    })
}

/// Close a handle from `rp_cdr_open`. Null-safe.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_close(doc: *mut RpCdrDoc) {
    if doc.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(doc)) }));
}

/// Fill `*out` with what the reader understood. `version` is 0 when unknown.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_info(doc: *const RpCdrDoc, out: *mut RpCdrInfo) -> c_int {
    guarded(|| {
        if doc.is_null() || out.is_null() {
            return RP_ERR_ARG;
        }
        let handle = unsafe { &*doc };
        unsafe {
            *out = RpCdrInfo {
                version: handle.report.version.map_or(0, |v| u32::from(v.0)),
                // The pages rp_cdr_page_svg serves, whatever the report counted.
                pages: handle.doc.pages.len() as u32,
                layers: handle.report.layers as u32,
                shapes: handle.report.shapes as u32,
                skipped_objects: handle.report.skipped_objects as u32,
                warnings: handle.report.warnings.len() as u32,
            }
        };
        RP_OK
    })
}

/// A page's size in millimetres.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_page_size_mm(
    doc: *const RpCdrDoc,
    page: usize,
    width: *mut f64,
    height: *mut f64,
) -> c_int {
    guarded(|| {
        if doc.is_null() || width.is_null() || height.is_null() {
            return RP_ERR_ARG;
        }
        let handle = unsafe { &*doc };
        let Some(p) = handle.doc.pages.get(page) else {
            return RP_ERR_ARG;
        };
        unsafe {
            *width = p.size.width;
            *height = p.size.height;
        }
        RP_OK
    })
}

/// One page as annotated SVG: UTF-8, NUL-terminated, `*len` excludes the NUL.
/// Free with `rp_free(*out, *len)`.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_page_svg(
    doc: *const RpCdrDoc,
    page: usize,
    out: *mut *mut u8,
    len: *mut usize,
) -> c_int {
    guarded(|| {
        if doc.is_null() || out.is_null() || len.is_null() {
            return RP_ERR_ARG;
        }
        let handle = unsafe { &*doc };
        if page >= handle.doc.pages.len() {
            return RP_ERR_ARG;
        }
        let svg = tracedraw_io::svg::page_to_svg(&handle.doc, page);
        let annotated = annotate::annotate(&svg, &handle.doc, page, &handle.hidden_layers);
        let (ptr, n) = into_c_buffer(annotated.into_bytes());
        unsafe {
            *out = ptr;
            *len = n;
        }
        RP_OK
    })
}

/// Warning `index` as UTF-8, NUL-terminated, `*len` excludes the NUL.
/// Free with `rp_free(*out, *len)`.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_warning(
    doc: *const RpCdrDoc,
    index: usize,
    out: *mut *mut u8,
    len: *mut usize,
) -> c_int {
    guarded(|| {
        if doc.is_null() || out.is_null() || len.is_null() {
            return RP_ERR_ARG;
        }
        let handle = unsafe { &*doc };
        let Some(w) = handle.report.warnings.get(index) else {
            return RP_ERR_ARG;
        };
        let (ptr, n) = into_c_buffer(w.as_bytes().to_vec());
        unsafe {
            *out = ptr;
            *len = n;
        }
        RP_OK
    })
}

/// Write the export model JSON as a drawing-format file of `version`
/// (0 means 14; values outside 7..=27 are clamped by the writer).
///
/// On RP_OK `*out`/`*out_len` hold the file and `*warnings`/`*warnings_len`
/// the '\n'-joined warnings (possibly empty). Free both with `rp_free`.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_export(
    json: *const u8,
    len: usize,
    version: u16,
    out: *mut *mut u8,
    out_len: *mut usize,
    warnings: *mut *mut u8,
    warnings_len: *mut usize,
) -> c_int {
    guarded(|| {
        if json.is_null()
            || len == 0
            || out.is_null()
            || out_len.is_null()
            || warnings.is_null()
            || warnings_len.is_null()
        {
            return RP_ERR_ARG;
        }
        let bytes = unsafe { slice::from_raw_parts(json, len) };
        let version = if version == 0 { 14 } else { version };
        match export::write(bytes, version) {
            Ok((cdr, warns)) => {
                let (file_ptr, file_len) = into_c_buffer(cdr);
                let (warn_ptr, warn_len) = into_c_buffer(warns.join("\n").into_bytes());
                unsafe {
                    *out = file_ptr;
                    *out_len = file_len;
                    *warnings = warn_ptr;
                    *warnings_len = warn_len;
                }
                RP_OK
            }
            Err(_) => RP_ERR_JSON,
        }
    })
}

/// The versions the writer produces and the ones it marks as checked.
#[no_mangle]
pub unsafe extern "C" fn rp_cdr_versions(
    oldest: *mut u16,
    newest: *mut u16,
    checked_since: *mut u16,
    checked_until: *mut u16,
) {
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        if !oldest.is_null() {
            *oldest = tracedraw_cdr::write::OLDEST_VERSION;
        }
        if !newest.is_null() {
            *newest = tracedraw_cdr::write::NEWEST_VERSION;
        }
        if !checked_since.is_null() {
            *checked_since = tracedraw_cdr::write::CHECKED_SINCE;
        }
        if !checked_until.is_null() {
            *checked_until = tracedraw_cdr::write::CHECKED_UNTIL;
        }
    }));
}

/// A static description of `code`; never freed.
#[no_mangle]
pub extern "C" fn rp_strerror(code: c_int) -> *const c_char {
    let text: &'static [u8] = match code {
        RP_OK => b"ok\0",
        RP_ERR_NOT_CDR => b"not a drawing-format file\0",
        RP_ERR_UNSUPPORTED => b"unsupported container\0",
        RP_ERR_TRUNCATED => b"truncated file\0",
        RP_ERR_CONTAINER => b"container read failed\0",
        RP_ERR_ARG => b"invalid argument\0",
        RP_ERR_JSON => b"invalid export model\0",
        RP_ERR_PANIC => b"internal panic caught\0",
        _ => b"unknown error code\0",
    };
    text.as_ptr() as *const c_char
}

/// Free a buffer this library returned. Pass exactly the length the call
/// wrote; null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn rp_free(ptr: *mut u8, len: usize) {
    if ptr.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let total = len.saturating_add(1);
        unsafe {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                ptr, total,
            )))
        };
    }));
}
