/* runtime.h: internal declarations shared between the src/runtime .cpp files.
 *
 * This is NOT part of the ABI contract (that is include/recomp_*.h). It only
 * needs to be consistent within this runtime's own translation units.
 */
#ifndef ICORECOMP_RUNTIME_H
#define ICORECOMP_RUNTIME_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "recomp_api.h"
#include "recomp_context.h"

/* ---- memory (mem.cpp) --------------------------------------------------- */

/* 32 MB, matching the retail EE. Shared with main.cpp for initial $sp setup. */
constexpr uint32_t RT_RAM_SIZE = 32u * 1024 * 1024;

/* Allocates EE RAM, scratchpad and VU memory windows and populates g_pages.
 * Must run before the loader or any guest code executes. */
void rt_mem_init();

/* Sentinel written into $ra before the entry call so a top-level `jr $ra`
 * (translated as rt_call_indirect with this target) is recognized as a clean
 * program exit instead of a bad-indirect fault. Chosen outside every mapped
 * range (RAM/aliases/scratchpad/VU window/MMIO) and outside the function
 * table's valid vram range, so it can never collide with a real target. */
constexpr uint32_t RT_CLEAN_EXIT_VRAM = 0xE0000000u;

/* ---- logging (log.cpp) -------------------------------------------------- */

void rt_log(const char* component, const char* fmt, ...);
void rt_vlog(const char* component, const char* fmt, va_list ap);

/* Register dump, shared by rt_break, rt_bad_indirect, the crash handler, and
 * unimplemented-strict-mode fatals. */
void rt_dump_registers(const R5900Context* ctx);

/* Prints a component-tagged fatal message (+ optional register dump when ctx
 * is non-null) and terminates the process with a nonzero exit code. Never
 * returns. */
[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...);

/* ---- sha1 (sha1.cpp) ----------------------------------------------------- */

struct Sha1Digest {
    uint8_t bytes[20];
};

Sha1Digest rt_sha1_file(const char* path, bool* ok);
Sha1Digest rt_sha1_buffer(const uint8_t* data, size_t len);
/* Lowercase hex, no separators; buf must hold at least 41 bytes. */
void rt_sha1_to_hex(const Sha1Digest& d, char* buf);
/* Case-insensitive compare against a 40-hex-char string. */
bool rt_sha1_equals_hex(const Sha1Digest& d, const char* hex40);

/* ---- loader (loader.cpp) ------------------------------------------------- */

struct LoaderConfig {
    /* [decomp] */
    char decomp_root[512] = {0};
    char decomp_elf[512] = {0};
    /* [pins] */
    char elf_sha1[64] = {0};
    /* [target] */
    uint32_t entry = 0;
    uint32_t vram_base = 0;
    uint32_t gp = 0;
};

/* Base directory for config files and relative paths (config/recomp.toml,
 * config/local.toml, saves/). ICORECOMP_SOURCE_ROOT when its
 * config/recomp.toml exists (a dev checkout); otherwise "." so a packaged
 * binary resolves everything against the directory it is run from. */
const char* rt_base_dir();

/* Reads <base>/config/recomp.toml (see rt_base_dir). When the file does not
 * exist (packaged runtime, no dev tree) the committed [pins]/[target]
 * values are filled in as compiled-in defaults and the boot ELF comes from
 * the disc instead (see rt_load_elf); that path still returns true. Returns
 * false and logs a fatal-quality message (does not exit) only when a
 * present config is missing required keys, so callers can decide how to
 * fail. */
bool rt_load_config(LoaderConfig* out);

/* Resolves decomp_root/decomp_elf into an absolute path. buf must hold at
 * least 1024 bytes. */
void rt_resolve_elf_path(const LoaderConfig& cfg, char* buf, size_t buf_size);

/* Verifies the SHA-1 pin, then loads the single PT_LOAD segment into guest
 * RAM (via g_pages) at its vaddr and zeroes bss (memsz - filesz). The ELF
 * bytes come from the decomp checkout when the config names one and the
 * file exists, otherwise from SCUS_971.13 on the mounted disc image (same
 * bytes, same pin). Fatal on any failure. */
void rt_load_elf(const LoaderConfig& cfg);

#endif /* ICORECOMP_RUNTIME_H */
