# minicoro (vendored)

Single-header stackful coroutine library used by the EE thread scheduler
(src/runtime/ee/sched.cpp). One guest thread = one coroutine.

- Upstream: https://github.com/edubart/minicoro
- Version: v0.2.0 (header banner: "minicoro - v0.2.0 - 15/Nov/2023")
- Vendored from upstream commit ff5321d93fe2a3fb067a8dd97a37dd623337b9c0
  (last commit touching minicoro.h as of 2026-08-31)
- File: minicoro.h, unmodified. sha1 d4a88ed414f00f7343d423c2fa221a79ac8f6d62
- License: MIT OR Public Domain (dual, chosen at user's option; license text
  is at the bottom of minicoro.h). MIT is compatible with this repository's
  MIT license.

Local patches: none. If a patch ever becomes necessary, carry it as a .patch
file next to this README per the repository's third-party policy.
