# third_party/patches

Local patches for submodules, applied at configure time. Submodule sources
are never edited in place (LGPL compliance for parallel-gs requires the
pinned sources to stay pristine and replaceable).

Naming: `<submodule>-NNNN-short-description.patch`, produced with
`git -C third_party/<submodule> format-patch`.

No patches are currently carried; the top-level CMakeLists.txt grows an
apply step the first time one is added.
