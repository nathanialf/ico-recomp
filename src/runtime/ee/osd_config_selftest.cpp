/* ee/osd_config_selftest.cpp: standalone exercise of the console OSD
 * configuration word this kernel reports (ee/osd_config.h).
 *
 * What this target is for: the OSD configuration is a PAL-only surface. The
 * PAL ELF (SCES_507.60) reaches GetOsdConfigParam at boot and the US ELF
 * (SCUS_971.13) never calls it, so this word decides which language the PAL
 * disc runs in and nothing on the US disc reads it at all. The word is built
 * by this runtime, so the check that matters is that the vendor library's
 * own arithmetic, transcribed in osd_config.h from the shifts and masks at
 * the retail addresses that header names, reads back out of it what was put
 * in.
 *
 * The subject is header only, so this links nothing: no settings model, no
 * guest memory, no runtime services.
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 */
#include "osd_config.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_fail = 0;

void check(bool ok, const char* what, const char* file, int line) {
    if (ok) return;
    std::printf("FAIL %s:%d  %s\n", file, line, what);
    g_fail = 1;
}
} // namespace

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

int main() {
    /* The five languages the disc's own table has an entry for: 1 English,
     * 2 French, 3 Spanish, 4 German, 5 Italian (the SCE OSD numbering; the
     * table is jtbl_0061D6F0, indexed by language - 1). */
    for (uint32_t lang = 1; lang <= 5; ++lang) {
        const uint32_t w = rt_osd_config_word((int)lang);

        /* sceScfGetLanguage takes the version first, at 0x0027298C. It has
         * to be nonzero or the language field is never read. */
        CHECK(rt_osd_cfg_version(w) != 0);

        /* ...and then the language, at 0x002729A8. This is the check that
         * the word the runtime hands back says what the setting said. */
        CHECK(rt_osd_cfg_language(w) == lang);

        /* kanbanBootMcCheck's own range test, at 0x001B9624. All five have
         * to pass it or the disc keeps its compiled-in default instead. */
        CHECK(rt_osd_language_selects_subtitles(rt_osd_cfg_language(w)));
    }

    /* The version-zero branch (0x0027299C) is a one-bit Japanese/English
     * field, which is why version 0 cannot carry these five. A zero word
     * takes that branch and reports 0, which is what every run reported
     * before the word carried a version. This is the bug the version field
     * fixes, asserted rather than described. */
    CHECK(rt_osd_cfg_version(0) == 0);
    CHECK(rt_osd_cfg_language_v0(0) == 0);
    CHECK(!rt_osd_language_selects_subtitles(rt_osd_cfg_language(0)));

    /* Nothing but the version and the language is set. sceScfGetAspect
     * (0x00272A04) reads bits 1..2 of the same word and this runtime has no
     * measured value for them, so they read back zero for every language. */
    for (uint32_t lang = 1; lang <= 5; ++lang) {
        CHECK(rt_osd_cfg_aspect(rt_osd_config_word((int)lang)) == 0);
    }

    /* The language field is five bits wide, so a value outside it would
     * alias onto another language rather than being reported as itself.
     * The settings model bounds the value; this is the floor under it. */
    CHECK(rt_osd_cfg_language(rt_osd_config_word(0x21)) == 1);

    if (g_fail) {
        std::printf("osd-config selftest FAILED\n");
        return 2;
    }
    std::printf("osd-config selftest OK\n");
    return 0;
}
