//! Compiles the C shim (csrc/shim.c) that exposes the recomp_ops.h helpers
//! to the Rust interpreter. gcc is invoked directly so the build carries no
//! extra crate dependencies; the flags mirror tools/build_generated.sh plus
//! -ffp-contract=off, which the shared float helpers require.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // crates/ee-interp -> crates -> recomp -> tools -> repo root
    let repo_root = manifest
        .ancestors()
        .nth(4)
        .expect("crate dir has expected depth")
        .to_path_buf();
    let include = repo_root.join("include");
    let shim = manifest.join("csrc/shim.c");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let obj = out.join("shim.o");
    let lib = out.join("libeeshim.a");

    let status = Command::new("gcc")
        .args([
            "-std=c11",
            "-O1",
            "-fPIC",
            "-fno-strict-aliasing",
            "-ffp-contract=off",
            "-Wall",
            "-Werror",
            "-c",
        ])
        .arg("-I")
        .arg(&include)
        .arg(&shim)
        .arg("-o")
        .arg(&obj)
        .status()
        .expect("running gcc");
    assert!(status.success(), "gcc failed on {}", shim.display());

    let status = Command::new("ar")
        .arg("crs")
        .arg(&lib)
        .arg(&obj)
        .status()
        .expect("running ar");
    assert!(status.success(), "ar failed");

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=eeshim");
    println!("cargo:rustc-link-lib=dylib=m");
    println!("cargo:rerun-if-changed=csrc/shim.c");
    for h in ["recomp_ops.h", "recomp_api.h", "recomp_context.h"] {
        println!("cargo:rerun-if-changed={}", include.join(h).display());
    }
}
