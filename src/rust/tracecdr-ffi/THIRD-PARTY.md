<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Third-party licences of the crates linked into libtracecdr_ffi.a. -->
<!-- Copyright (C) 2026 Authors -->
<!-- Released under GNU GPL v2+, read the file 'COPYING' for more information. -->

# Third-party licences of the TraceDraw bridge

`libtracecdr_ffi.a` links the TraceDraw crates and their dependencies. They
keep their own licences; this table lists every package in the crate's
resolved dependency graph (dev-dependencies excluded). It is generated from
`cargo metadata --format-version 1 --locked`; regenerate it after any change
to `Cargo.lock`.

The graph includes packages of other target platforms (the wasm entries);
only the host-target packages are compiled into the static library.

Licence choice: Rosepen itself is `GPL-2.0-or-later`. TraceDraw is
`MIT OR Apache-2.0`, and Rosepen elects the **MIT** branch, which is
compatible with GPL-2.0. Every other package is permissive and offers a
GPL-2.0-compatible alternative (MIT, BSD, Zlib, ISC, 0BSD, Unlicense,
Unicode-3.0), with one exception noted below.

## Finding: `zopfli` is Apache-2.0 only

`zopfli 0.8.3` is licensed `Apache-2.0` with no MIT alternative. It enters
the graph through `zip 4.6.1`'s `deflate` feature, which the TraceDraw
crates enable. Apache-2.0 is not compatible with GPL-**2.0** (only with
GPL-3.0).

Consequences, for the project decision:

- A combined distribution of the Rosepen binary with this library must be
  under `GPL-3.0` (permitted by `GPL-2.0-or-later`); the Rosepen source stays
  `GPL-2.0-or-later` and TraceDraw stays `MIT OR Apache-2.0`.
- Or the `zip` dependency's feature set can be narrowed to
  `deflate-flate2-zlib-rs` instead of `deflate` — that is a change inside the
  vendored TraceDraw crates, so it belongs upstream, not here.

Recorded as FINDING (licence), not patched in the vendored submodule.

## Packages

| Package | Version | Licence | Source |
| --- | --- | --- | --- |
| adler2 | 2.0.1 | 0BSD OR MIT OR Apache-2.0 | registry |
| aes | 0.8.4 | MIT OR Apache-2.0 | registry |
| arbitrary | 1.5.0 | MIT OR Apache-2.0 | registry |
| arrayref | 0.3.9 | BSD-2-Clause | registry |
| arrayvec | 0.7.8 | MIT OR Apache-2.0 | registry |
| autocfg | 1.5.1 | Apache-2.0 OR MIT | registry |
| base64 | 0.22.1 | MIT OR Apache-2.0 | registry |
| bitflags | 2.13.2 | MIT OR Apache-2.0 | registry |
| block-buffer | 0.10.4 | MIT OR Apache-2.0 | registry |
| block-padding | 0.3.3 | MIT OR Apache-2.0 | registry |
| bumpalo | 3.20.3 | MIT OR Apache-2.0 | registry |
| bytecount | 0.6.9 | Apache-2.0/MIT | registry |
| bytemuck | 1.25.2 | Zlib OR Apache-2.0 OR MIT | registry |
| bytemuck_derive | 1.12.1 | Zlib OR Apache-2.0 OR MIT | registry |
| byteorder-lite | 0.1.0 | Unlicense OR MIT | registry |
| cbc | 0.1.2 | MIT OR Apache-2.0 | registry |
| cfg-if | 1.0.5 | MIT OR Apache-2.0 | registry |
| cipher | 0.4.4 | MIT OR Apache-2.0 | registry |
| core_detect | 1.0.0 | MIT/Apache-2.0 | registry |
| core_maths | 0.1.1 | MIT | registry |
| cpufeatures | 0.2.17 | MIT OR Apache-2.0 | registry |
| crc32fast | 1.5.2 | MIT OR Apache-2.0 | registry |
| crypto-common | 0.1.7 | MIT OR Apache-2.0 | registry |
| data-url | 0.3.2 | MIT OR Apache-2.0 | registry |
| derive_arbitrary | 1.5.0 | MIT OR Apache-2.0 | registry |
| digest | 0.10.7 | MIT OR Apache-2.0 | registry |
| ecb | 0.1.2 | MIT | registry |
| encoding_rs | 0.8.42 | (Apache-2.0 OR MIT) AND BSD-3-Clause | registry |
| equivalent | 1.0.2 | Apache-2.0 OR MIT | registry |
| euclid | 0.22.14 | MIT OR Apache-2.0 | registry |
| fdeflate | 0.3.7 | MIT OR Apache-2.0 | registry |
| flate2 | 1.1.10 | MIT OR Apache-2.0 | registry |
| float-cmp | 0.9.0 | MIT | registry |
| font-types | 0.11.3 | MIT OR Apache-2.0 | registry |
| fontconfig-parser | 0.5.8 | MIT | registry |
| fontdb | 0.24.0 | MIT | registry |
| futures-core | 0.3.34 | MIT OR Apache-2.0 | registry |
| futures-task | 0.3.34 | MIT OR Apache-2.0 | registry |
| futures-util | 0.3.34 | MIT OR Apache-2.0 | registry |
| generic-array | 0.14.7 | MIT | registry |
| getrandom | 0.3.4 | MIT OR Apache-2.0 | registry |
| hashbrown | 0.17.1 | MIT OR Apache-2.0 | registry |
| i_float | 5.0.0 | MIT | registry |
| i_key_sort | 0.11.0 | MIT | registry |
| i_overlay | 9.0.1 | MIT OR Apache-2.0 | registry |
| i_shape | 5.0.0 | MIT | registry |
| i_tree | 0.19.0 | MIT | registry |
| image | 0.25.10 | MIT OR Apache-2.0 | registry |
| imagesize | 0.15.0 | MIT | registry |
| indexmap | 2.14.2 | Apache-2.0 OR MIT | registry |
| inout | 0.1.4 | MIT OR Apache-2.0 | registry |
| itoa | 1.0.18 | MIT OR Apache-2.0 | registry |
| js-sys | 0.3.106 | MIT OR Apache-2.0 | registry |
| kurbo | 0.13.1 | Apache-2.0 OR MIT | registry |
| libc | 0.2.190 | MIT OR Apache-2.0 | registry |
| libm | 0.2.16 | MIT | registry |
| log | 0.4.34 | MIT OR Apache-2.0 | registry |
| lopdf | 0.36.0 | MIT | registry |
| md-5 | 0.10.6 | MIT OR Apache-2.0 | registry |
| memchr | 2.8.3 | Unlicense OR MIT | registry |
| memmap2 | 0.9.11 | MIT OR Apache-2.0 | registry |
| miniz_oxide | 0.8.9 | MIT OR Zlib OR Apache-2.0 | registry |
| miniz_oxide | 0.9.1 | MIT OR Zlib OR Apache-2.0 | registry |
| moxcms | 0.8.1 | BSD-3-Clause OR Apache-2.0 | registry |
| multiversion_no_op | 1.0.0 | Apache-2.0 OR MIT | registry |
| nom | 8.0.0 | MIT | registry |
| nom_locate | 5.0.0 | MIT | registry |
| num-traits | 0.2.19 | MIT OR Apache-2.0 | registry |
| once_cell | 1.21.4 | MIT OR Apache-2.0 | registry |
| pico-args | 0.5.0 | MIT | registry |
| pin-project-lite | 0.2.17 | Apache-2.0 OR MIT | registry |
| png | 0.18.1 | MIT OR Apache-2.0 | registry |
| polycool | 0.4.0 | MIT OR Apache-2.0 | registry |
| ppv-lite86 | 0.2.21 | MIT OR Apache-2.0 | registry |
| proc-macro2 | 1.0.107 | MIT OR Apache-2.0 | registry |
| pxfm | 0.1.30 | BSD-3-Clause OR Apache-2.0 | registry |
| quote | 1.0.47 | MIT OR Apache-2.0 | registry |
| r-efi | 5.3.0 | MIT OR Apache-2.0 OR LGPL-2.1-or-later | registry |
| rand | 0.9.5 | MIT OR Apache-2.0 | registry |
| rand_chacha | 0.9.0 | MIT OR Apache-2.0 | registry |
| rand_core | 0.9.5 | MIT OR Apache-2.0 | registry |
| rangemap | 1.8.0 | MIT/Apache-2.0 | registry |
| read-fonts | 0.39.2 | MIT OR Apache-2.0 | registry |
| roxmltree | 0.20.0 | MIT OR Apache-2.0 | registry |
| roxmltree | 0.21.1 | MIT OR Apache-2.0 | registry |
| rustc-hash | 2.1.3 | Apache-2.0 OR MIT | registry |
| rustversion | 1.0.23 | MIT OR Apache-2.0 | registry |
| rustybuzz | 0.20.1 | MIT | registry |
| scopeguard | 1.2.0 | MIT OR Apache-2.0 | registry |
| serde | 1.0.229 | MIT OR Apache-2.0 | registry |
| serde_core | 1.0.229 | MIT OR Apache-2.0 | registry |
| serde_derive | 1.0.229 | MIT OR Apache-2.0 | registry |
| serde_json | 1.0.151 | MIT OR Apache-2.0 | registry |
| sha2 | 0.10.9 | MIT OR Apache-2.0 | registry |
| simd-adler32 | 0.3.10 | MIT | registry |
| simdutf8 | 0.1.5 | MIT OR Apache-2.0 | registry |
| simplecss | 0.2.2 | Apache-2.0 OR MIT | registry |
| siphasher | 1.0.4 | MIT OR Apache-2.0 | registry |
| skrifa | 0.42.1 | MIT OR Apache-2.0 | registry |
| slab | 0.4.12 | MIT | registry |
| slotmap | 1.1.1 | Zlib | registry |
| smallvec | 1.16.2 | MIT OR Apache-2.0 | registry |
| strict-num | 0.1.1 | MIT | registry |
| stringprep | 0.1.5 | MIT/Apache-2.0 | registry |
| subsetter | 0.2.6 | MIT OR Apache-2.0 | registry |
| svgtypes | 0.16.1 | Apache-2.0 OR MIT | registry |
| syn | 2.0.119 | MIT OR Apache-2.0 | registry |
| syn | 3.0.6 | MIT OR Apache-2.0 | registry |
| thiserror | 2.0.21 | MIT OR Apache-2.0 | registry |
| thiserror-impl | 2.0.21 | MIT OR Apache-2.0 | registry |
| tiny-skia | 0.12.0 | BSD-3-Clause | registry |
| tiny-skia-path | 0.12.0 | BSD-3-Clause | registry |
| tinyvec | 1.13.3 | Zlib OR Apache-2.0 OR MIT | registry |
| tracedraw-cdr | 0.13.0 | MIT OR Apache-2.0 | path |
| tracedraw-core | 0.13.0 | MIT OR Apache-2.0 | path |
| tracedraw-io | 0.13.0 | MIT OR Apache-2.0 | path |
| tracedraw-render | 0.13.0 | MIT OR Apache-2.0 | path |
| tracedraw-text | 0.13.0 | MIT OR Apache-2.0 | path |
| ttf-parser | 0.25.1 | MIT OR Apache-2.0 | registry |
| typenum | 1.20.1 | MIT OR Apache-2.0 | registry |
| unicode-bidi | 0.3.18 | MIT OR Apache-2.0 | registry |
| unicode-bidi-mirroring | 0.4.0 | MIT/Apache-2.0 | registry |
| unicode-ccc | 0.4.0 | MIT/Apache-2.0 | registry |
| unicode-ident | 1.0.26 | (MIT OR Apache-2.0) AND Unicode-3.0 | registry |
| unicode-normalization | 0.1.25 | MIT OR Apache-2.0 | registry |
| unicode-properties | 0.1.4 | MIT/Apache-2.0 | registry |
| unicode-script | 0.5.8 | MIT OR Apache-2.0 | registry |
| usvg | 0.48.1 | Apache-2.0 OR MIT | registry |
| version_check | 0.9.5 | MIT/Apache-2.0 | registry |
| wasip2 | 1.0.4+wasi-0.2.12 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT | registry |
| wasm-bindgen | 0.2.129 | MIT OR Apache-2.0 | registry |
| wasm-bindgen-macro | 0.2.129 | MIT OR Apache-2.0 | registry |
| wasm-bindgen-macro-support | 0.2.129 | MIT OR Apache-2.0 | registry |
| wasm-bindgen-shared | 0.2.129 | MIT OR Apache-2.0 | registry |
| web-time | 1.1.0 | MIT OR Apache-2.0 | registry |
| weezl | 0.1.12 | MIT OR Apache-2.0 | registry |
| wit-bindgen | 0.57.1 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT | registry |
| write-fonts | 0.48.1 | MIT OR Apache-2.0 | registry |
| zerocopy | 0.8.62 | BSD-2-Clause OR Apache-2.0 OR MIT | registry |
| zerocopy-derive | 0.8.62 | BSD-2-Clause OR Apache-2.0 OR MIT | registry |
| zip | 4.6.1 | MIT | registry |
| zlib-rs | 0.6.8 | Zlib | registry |
| zmij | 1.0.23 | MIT | registry |
| zopfli | 0.8.3 | Apache-2.0 | registry |
| zune-core | 0.5.3 | MIT OR Apache-2.0 OR Zlib | registry |
| zune-jpeg | 0.5.15 | MIT OR Apache-2.0 OR Zlib | registry |
