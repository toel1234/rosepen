# SPDX-License-Identifier: GPL-2.0-or-later
# Rosepen 06-02: the TraceDraw drawing-format reader/writer bridge (Rust, optional).
# Copyright (C) 2026 Authors
# Released under GNU GPL v2+, read the file 'COPYING' for more information.
#
# Builds src/rust/tracecdr-ffi (a staticlib over the pinned submodule in
# src/3rdparty/tracedraw) and exposes it as Rosepen::TraceCdrFfi. Without
# cargo, or without the submodule, WITH_TRACECDR switches itself off: a
# cargo-less build never breaks.

find_program(CARGO_EXECUTABLE cargo
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")

if(WITH_TRACECDR)
    if(NOT CARGO_EXECUTABLE OR NOT EXISTS "${CARGO_EXECUTABLE}")
        message(STATUS "TraceDraw reader disabled: cargo was not found (install Rust, or set CARGO_EXECUTABLE)")
        set(WITH_TRACECDR OFF)
    elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/3rdparty/tracedraw/crates/cdr/Cargo.toml")
        message(STATUS "TraceDraw reader disabled: the src/3rdparty/tracedraw submodule is missing")
        set(WITH_TRACECDR OFF)
    elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/rust/tracecdr-ffi/Cargo.toml")
        message(STATUS "TraceDraw reader disabled: src/rust/tracecdr-ffi is missing")
        set(WITH_TRACECDR OFF)
    endif()
endif()

if(WITH_TRACECDR)
    add_definitions(-DWITH_TRACECDR)

    set(TRACECDR_TARGET_DIR "${CMAKE_BINARY_DIR}/rust")
    set(TRACECDR_LIB "${TRACECDR_TARGET_DIR}/release/libtracecdr_ffi.a")

    # Always runs: cargo's own incremental check makes it a second when clean.
    add_custom_target(tracecdr_ffi_build
        COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${TRACECDR_TARGET_DIR}"
                "${CARGO_EXECUTABLE}" build --release --locked
                --manifest-path "${CMAKE_SOURCE_DIR}/src/rust/tracecdr-ffi/Cargo.toml"
        BYPRODUCTS "${TRACECDR_LIB}"
        USES_TERMINAL
        COMMENT "cargo: tracecdr-ffi"
        VERBATIM)

    add_library(Rosepen::TraceCdrFfi STATIC IMPORTED GLOBAL)
    set_target_properties(Rosepen::TraceCdrFfi PROPERTIES
        IMPORTED_LOCATION "${TRACECDR_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/rust/tracecdr-ffi/include"
        # Printed by `cargo rustc --release --locked --lib -- --print native-static-libs`.
        INTERFACE_LINK_LIBRARIES "gcc_s;util;rt;pthread;m;dl;c")

    message(STATUS "TraceDraw reader: enabled (cargo: ${CARGO_EXECUTABLE})")
endif()
