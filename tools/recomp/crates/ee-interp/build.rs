//! Compiles the C shim (csrc/shim.c) that exposes the recomp_ops.h helpers
//! to the Rust interpreter. Uses the cc crate so the shim builds with the
//! right compiler for the target (gcc/clang on unix, cl.exe for msvc
//! targets, the mingw cross gcc for *-pc-windows-gnu). The flags mirror
//! tools/build_generated.sh plus -ffp-contract=off, which the shared float
//! helpers require: multiply and add must round separately, no FMA.

use std::path::PathBuf;

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

    let mut build = cc::Build::new();
    build
        .file(&shim)
        .include(&include)
        .std("c11")
        .opt_level(1)
        .warnings(true);
    if build.get_compiler().is_like_msvc() {
        // /fp:precise alone still permits contraction on recent MSVC;
        // /fp:contract- (VS2022 17.0+) turns it off explicitly.
        build.flag("/fp:precise");
        build.flag_if_supported("/fp:contract-");
    } else {
        build
            .flag("-fno-strict-aliasing")
            .flag("-ffp-contract=off")
            .flag("-Werror");
    }
    build.compile("eeshim");

    // The shim calls libm functions; on unix libm is separate. Windows
    // CRTs (msvc and mingw) provide the math symbols without it.
    if std::env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
        println!("cargo:rustc-link-lib=dylib=m");
    }
    println!("cargo:rerun-if-changed=csrc/shim.c");
    for h in ["recomp_ops.h", "recomp_api.h", "recomp_context.h"] {
        println!("cargo:rerun-if-changed={}", include.join(h).display());
    }
}
