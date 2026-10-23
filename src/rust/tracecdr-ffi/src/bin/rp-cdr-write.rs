// SPDX-License-Identifier: GPL-2.0-or-later
// Test helper: export model JSON -> drawing-format file of a chosen version.
// Copyright (C) 2026 Authors
// Released under GNU GPL v2+, read the file 'COPYING' for more information.
// The linked TraceDraw crates are MIT OR Apache-2.0 (see THIRD-PARTY.md).
//
// Not installed: the test corpus builds it to make its own fixtures, so no
// binary file is committed.

use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 || args.len() > 3 {
        eprintln!("usage: rp-cdr-write <in.json> <out.cdr> [version]");
        return ExitCode::from(1);
    }
    let version: u16 = match args.get(2) {
        Some(v) => match v.parse() {
            Ok(v) => v,
            Err(_) => {
                eprintln!("rp-cdr-write: version {v:?} is not a number");
                return ExitCode::from(1);
            }
        },
        None => 14,
    };
    let json = match std::fs::read(&args[0]) {
        Ok(bytes) => bytes,
        Err(e) => {
            eprintln!("rp-cdr-write: cannot read {}: {e}", args[0]);
            return ExitCode::from(1);
        }
    };
    match tracecdr_ffi::export::write(&json, version) {
        Ok((bytes, warnings)) => {
            for warning in warnings {
                eprintln!("{warning}");
            }
            if let Err(e) = std::fs::write(&args[1], bytes) {
                eprintln!("rp-cdr-write: cannot write {}: {e}", args[1]);
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("rp-cdr-write: {e}");
            ExitCode::from(1)
        }
    }
}
