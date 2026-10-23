/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Rosepen's C ABI (version 1) over the TraceDraw drawing-format reader and writer.
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * The linked TraceDraw crates are MIT OR Apache-2.0; Rosepen elects the MIT
 * branch for GPL-2.0 compatibility. See THIRD-PARTY.md beside this header.
 *
 * Ownership rules, for every call:
 *  - A handle from rp_cdr_open is closed exactly once with rp_cdr_close.
 *  - A buffer the library returns (rp_cdr_page_svg, rp_cdr_warning,
 *    rp_cdr_export's two outputs) is UTF-8 with one trailing NUL byte and is
 *    freed only with rp_free(ptr, len), passing exactly the length the call
 *    wrote. The NUL is not part of that length.
 *  - Outputs are written only when a call returns RP_OK.
 *  - rp_strerror returns a static string that must not be freed.
 *  - A handle is used from one thread at a time.
 */

#ifndef ROSEPEN_TRACECDR_FFI_H
#define ROSEPEN_TRACECDR_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes. */
#define RP_OK 0
#define RP_ERR_NOT_CDR 1
#define RP_ERR_UNSUPPORTED 2
#define RP_ERR_TRUNCATED 3
#define RP_ERR_CONTAINER 4
#define RP_ERR_ARG 5
#define RP_ERR_JSON 6
#define RP_ERR_PANIC 7

/* An open drawing. */
typedef struct RpCdrDoc RpCdrDoc;

/* What the reader understood; version is 0 when the file states none. */
typedef struct {
    uint32_t version;
    uint32_t pages;
    uint32_t layers;
    uint32_t shapes;
    uint32_t skipped_objects;
    uint32_t warnings;
} RpCdrInfo;

/* The ABI version this library implements (1). */
uint32_t rp_abi_version(void);

/*
 * Open drawing-format bytes. title may be NULL (then "Untitled"); it must be
 * UTF-8. On RP_OK *out holds a handle. On any other code *out is untouched.
 */
int rp_cdr_open(const uint8_t *data, size_t len, const char *title, RpCdrDoc **out);

/* Close a handle. NULL is a no-op. */
void rp_cdr_close(RpCdrDoc *doc);

/* Fill *out with the reader's counters. */
int rp_cdr_info(const RpCdrDoc *doc, RpCdrInfo *out);

/* A page's size in millimetres (page index 0 is the first). */
int rp_cdr_page_size_mm(const RpCdrDoc *doc, size_t page, double *width, double *height);

/*
 * One page as annotated SVG (UTF-8, NUL-terminated; *len excludes the NUL).
 * Layers carry inkscape:groupmode/inkscape:label; ids are prefixed "td";
 * CMYK and spot paints carry data-rp-fill / data-rp-stroke with the source
 * numbers (see SCHEMA.md). Free with rp_free(*out, *len).
 */
int rp_cdr_page_svg(const RpCdrDoc *doc, size_t page, uint8_t **out, size_t *len);

/* One reader warning as UTF-8 (NUL-terminated; *len excludes the NUL). */
int rp_cdr_warning(const RpCdrDoc *doc, size_t index, uint8_t **out, size_t *len);

/*
 * Write the "rosepen-cdr-export/1" JSON model (see SCHEMA.md) as a
 * drawing-format file of `version`; version 0 means 14, values outside
 * 7..=27 are clamped by the writer.
 *
 * On RP_OK *out/*out_len hold the file and *warnings/*warnings_len the
 * '\n'-joined warnings (possibly empty). Free both with rp_free.
 */
int rp_cdr_export(const uint8_t *json, size_t len, uint16_t version, uint8_t **out,
                  size_t *out_len, uint8_t **warnings, size_t *warnings_len);

/* The versions the writer produces and the ones it marks as checked. */
void rp_cdr_versions(uint16_t *oldest, uint16_t *newest, uint16_t *checked_since,
                     uint16_t *checked_until);

/* A static description of a result code; never freed. */
const char *rp_strerror(int code);

/* Free a buffer this library returned. len is the length the call wrote. */
void rp_free(uint8_t *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ROSEPEN_TRACECDR_FFI_H */
