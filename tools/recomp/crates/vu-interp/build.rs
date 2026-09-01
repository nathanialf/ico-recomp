//! Compiles the C shim (csrc/shim.c) that exposes the recomp_ops.h VU1
//! helpers to the Rust reference interpreter. Mirrors ee-interp/build.rs:
//! the same compiler and the same flags as the generated code, because the
//! whole point of the shim is that both sides run one implementation.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // crates/vu-interp -> crates -> recomp -> tools -> repo root
    let repo_root = manifest
        .ancestors()
        .nth(4)
        .expect("crate dir has expected depth")
        .to_path_buf();
    let include = repo_root.join("include");
    let shim = manifest.join("csrc/shim.c");

    let mut build = cc::Build::new();
    build
        .file(&shim)
        .include(&include)
        .std("c11")
        .opt_level(1)
        .warnings(true);
    if build.get_compiler().is_like_msvc() {
        build.flag("/fp:precise");
        build.flag_if_supported("/fp:contract-");
    } else {
        build
            .flag("-fno-strict-aliasing")
            .flag("-ffp-contract=off")
            .flag("-Werror");
    }
    build.compile("vu1shim");

    if std::env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
        println!("cargo:rustc-link-lib=dylib=m");
    }
    println!("cargo:rerun-if-changed=csrc/shim.c");
    for h in ["recomp_ops.h", "recomp_api.h", "recomp_context.h"] {
        println!("cargo:rerun-if-changed={}", include.join(h).display());
    }
}
