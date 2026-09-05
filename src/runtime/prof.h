/* prof.h: opt-in per-subsystem frame-time instrumentation.
 *
 * Purpose: answer "where does a field go" with measurement rather than
 * guesswork. Off by default and costed accordingly: with the instrument
 * disabled a zone is one load of a global bool and a not-taken branch, so
 * the zones can sit on hot paths (every MMIO access, every GIF packet)
 * without being conditionally compiled out.
 *
 * Enabled by ICORECOMP_PROFILE, same idiom as ICORECOMP_VERBOSE
 * (log.cpp): unset, "0", "-" or "none" is off; "1" is on, reporting every
 * 60 fields; a number greater than 1 is on with that report interval in
 * fields.
 *
 * Accounting model: EXCLUSIVE (self) time. A zone that opens inside
 * another one stops billing the outer zone for its duration and resumes
 * it on exit, so nothing is double counted and the buckets sum to wall
 * clock time. The root bucket, "other", therefore holds everything not
 * inside any zone: scheduler idle waiting for the next timeline event,
 * boot, and process setup. Two consequences worth stating because they
 * change how a number is read:
 *   - "ee" is translated EE code only. The DMA, VIF1, GIF, VU1, syscall
 *     and MMIO work the EE triggers is subtracted out into its own
 *     bucket.
 *   - "ipu" holds a whole IPU register access, mmio.cpp's dispatch
 *     included, not only hw/ipu.cpp's handler: the movie makes tens of
 *     thousands of those a field, and a second nested zone for the handler
 *     would double the clock readings on the hottest path in the port.
 *     "mmio" is therefore every access that is not an IPU one. The IPU's
 *     DMA entry points keep their own zone; they are reached from a DMAC
 *     register write and run per transfer, not per access.
 *     The zone opens before rt_kernel_mmio_bill, so it also holds the
 *     virtual clock tick every access bills and everything that tick
 *     raises (timer compares, vblank edges, deferred SIF work) and, on the
 *     write path, the interrupt delivery after it. That was not true of
 *     logs written before this change, where the clock tick was billed to
 *     "mmio" for IPU accesses too, so the two are not comparable
 *     bucket for bucket.
 *   - "log" is not populated in this build; see RT_PROF_LOG below.
 *
 * Header only on purpose: adding a translation unit means editing
 * CMakeLists.txt, which re-runs cmake over a tree whose generated/ee
 * object library takes a very long time to rebuild. Nothing here needs
 * separate compilation.
 *
 * Single threaded by construction: every zone below is on the main OS
 * thread (guest threads are minicoro coroutines on that thread), so the
 * counters need no locking. The log writer thread (log.cpp) never calls
 * rt_log, so it never enters a zone, and the GS command ring's worker
 * (gs/gs_threaded.cpp) opens no zone either: it keeps its own atomic
 * counters and hands them over once per window through rt_gs_field_prof,
 * which is what the "gs worker" line of the summary prints.
 *
 * Coroutine caveat: a guest thread can yield (WaitSema, SleepThread) with
 * a zone still open on its coroutine stack, so zone exits are not always
 * in the order of their entries. The saved-parent restore is per object,
 * so this cannot corrupt state; it can misattribute the small interval
 * between a yield and the matching resume. Every zone below except
 * "syscall" is on a path that cannot yield.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_PROF_H
#define ICORECOMP_PROF_H

#include "host/audio.h"
#include "runtime.h"

#ifdef ICORECOMP_HAVE_SETTINGS
#include "host/settings.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum RtProfZone : int {
    RT_PROF_OTHER = 0,  /* root: scheduler idle, boot, unzoned code */
    RT_PROF_EE,         /* translated EE code (one guest thread resume) */
    RT_PROF_MMIO,       /* MMIO trap dispatch, including the clock tick */
    RT_PROF_SYSCALL,    /* kernel HLE syscalls */
    RT_PROF_INTC,       /* guest interrupt handler dispatch */
    RT_PROF_DMA,        /* DMAC channel execution: tag walk and gather */
    RT_PROF_VIF1,       /* VIF1 command stream, UNPACK, MPG */
    RT_PROF_VU1,        /* recompiled VU1 microprogram execution */
    RT_PROF_GIF,        /* GIF framing and XGKICK packet assembly */
    RT_PROF_GS,         /* GS backend: submit_gif (GIF packet ingest) */
    RT_PROF_PRESENT,    /* GS vsync hook: flush, scanout, swapchain present.
                         * Separate from GS because a blocking present and a
                         * slow packet ingest need opposite fixes, and one
                         * averaged bucket cannot tell them apart. With the
                         * command ring's worker up this holds only the cost
                         * of enqueueing the vsync record; the present itself
                         * is never on this thread. */
    RT_PROF_GSWAIT,     /* waiting on the GS command ring's worker thread at
                         * the field sync point (hw/gspriv.cpp). Zero while
                         * the ring is drained inline or bypassed. Once the
                         * worker is up, "gs" and "present" hold only the
                         * enqueue cost and this holds the field boundary's
                         * share of the handover. It excludes the present:
                         * the worker publishes a field as done once its
                         * scanout is latched and presents after that, so
                         * this waits on the renderer and never on the
                         * swapchain, and the present is on the "gs worker"
                         * line. The other two waits (a reply, a full ring)
                         * happen wherever their caller is and land in that
                         * caller's bucket; the "gs worker" line totals all
                         * three. */
    RT_PROF_DISC,       /* disc image reads (ISO sector fetch) */
    RT_PROF_LIMIT,      /* frame limiter sleeping. Large means there is
                         * headroom; near zero means the port is running
                         * flat out and cannot hold the field rate. */
    RT_PROF_IPU,        /* IPU MPEG-2 decode */
    RT_PROF_AUDIO,      /* sound engine mix and SPU uploads */
    /* Logging. Not wired up: src/runtime/log.cpp is being reworked into an
     * asynchronous writer by separate work and this instrument does not
     * contend for it. To populate the bucket, add one
     * RT_PROF_ZONE(RT_PROF_LOG) line at the top of rt_vlog and
     * rt_log_flush; nothing else is needed. Until then, the cost of
     * formatting and queueing a log line is billed to whichever subsystem
     * emitted it, and the file I/O is off the main thread and outside the
     * wall clock this instrument divides up. */
    RT_PROF_LOG,
    RT_PROF_COUNT
};

/* Fast gate. Read on every zone entry; written once by rt_prof_init, and
 * again once per field by rt_prof_field when ICORECOMP_PROFILE is unset
 * (see g_rt_prof_env_set below). */
inline bool g_rt_prof_on = false;

/* True once rt_prof_init has seen ICORECOMP_PROFILE set. The environment
 * variable then owns g_rt_prof_on/g_every for the rest of the run, exactly
 * as before debug.profile_fields existed; unset, rt_prof_field re-reads
 * debug.profile_fields every field instead of latching it once, cheap
 * enough at once-per-field and it means a settings reload takes effect
 * without a restart. Builds with no ICORECOMP_HAVE_SETTINGS (selftest
 * targets that do not link settings.cpp; see CMakeLists.txt) never look at
 * this and keep the historical env-only, on-by-default behavior. */
inline bool g_rt_prof_env_set = false;

/* True once the one-time ICORECOMP_PROFILE-vs-debug.profile_fields mismatch
 * log has run (see rt_prof_field). Only meaningful when g_rt_prof_env_set;
 * the check lives in rt_prof_field, not rt_prof_init, because rt_prof_init
 * runs before rt_settings_init and so cannot yet see the loaded file. */
inline bool g_rt_prof_env_checked = false;

namespace rt_prof_detail {

inline const char* const kName[RT_PROF_COUNT] = {
    "other", "ee", "mmio", "syscall", "intc", "dma",
    "vif1", "vu1", "gif", "gs", "present", "gswait", "disc", "limit", "ipu", "audio", "log",
};
static_assert(sizeof(kName) / sizeof(kName[0]) == RT_PROF_COUNT,
              "one name per RtProfZone, in the same order");

inline uint64_t g_ns[RT_PROF_COUNT] = {0};
inline uint64_t g_calls[RT_PROF_COUNT] = {0};
inline int g_cur = RT_PROF_OTHER;
inline uint64_t g_last_ns = 0;      /* stamp of the last zone transition */
inline uint64_t g_window_ns = 0;    /* stamp the current report window opened */
inline uint64_t g_fields = 0;       /* fields since the profile started */
inline uint64_t g_window_fields = 0;
inline unsigned g_every = 180;

/* Per-field decomposition of the window's longest field. The bucket table
 * averages over the window and cannot say what one 200 ms field was doing;
 * this keeps the bucket deltas of the worst field seen since the last
 * report. g_field_snap is g_ns at the previous field boundary.
 *
 * The boundary is wherever rt_prof_field is called, and hw/gspriv.cpp calls
 * it before pace_field, so a field's own pacing wait lands in the "limit"
 * bucket of the field after it. A worst field whose largest bucket is
 * "limit" is reporting the sleep that preceded it, not a stall inside it. */
inline uint64_t g_field_snap[RT_PROF_COUNT] = {0};
inline bool g_field_snap_valid = false;
inline uint64_t g_worst_field[RT_PROF_COUNT] = {0};
inline uint64_t g_worst_field_total = 0;
/* Field length distribution from the same boundary, so the "fields:" line
 * and the "longest field" breakdown under it describe the same fields. A
 * field here runs from one rt_prof_field call to the next, which is the
 * previous field's pacing sleep plus this field's work. */
inline uint32_t g_fields_over_20ms = 0;
inline uint32_t g_fields_over_50ms = 0;

/* A subsystem may attach one detail line to the hitch report. Called once
 * per field with hitch=false (the subsystem resets its per-field counters)
 * and, on a field over 50 ms, first with hitch=true and a buffer to fill.
 * Set by hw/vu1rt.cpp; null in builds that do not link it. */
inline void (*g_rt_prof_field_hook)(bool hitch, char* buf, size_t cap) = nullptr;

inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Bills the open zone up to now and starts a new one. Returns the parent
 * so the scope object can restore it. */
inline int enter(int zone) {
    const uint64_t t = now_ns();
    g_ns[g_cur] += t - g_last_ns;
    g_last_ns = t;
    const int parent = g_cur;
    g_cur = zone;
    ++g_calls[zone];
    return parent;
}

inline void leave(int parent) {
    const uint64_t t = now_ns();
    g_ns[g_cur] += t - g_last_ns;
    g_last_ns = t;
    g_cur = parent;
}

} // namespace rt_prof_detail

/* Scoped zone. Construct at the top of the region to be measured. */
class RtProfScope {
public:
    explicit RtProfScope(int zone) {
        m_parent = g_rt_prof_on ? rt_prof_detail::enter(zone) : -1;
    }
    ~RtProfScope() {
        if (m_parent >= 0) rt_prof_detail::leave(m_parent);
    }
    RtProfScope(const RtProfScope&) = delete;
    RtProfScope& operator=(const RtProfScope&) = delete;

private:
    int m_parent;
};

#define RT_PROF_CAT2(a, b) a##b
#define RT_PROF_CAT(a, b) RT_PROF_CAT2(a, b)
#define RT_PROF_ZONE(z) RtProfScope RT_PROF_CAT(rt_prof_scope_, __LINE__)(z)

/* Exclusive nanoseconds billed to one zone so far this window. Read it
 * either side of a scope and the difference is that scope's exclusive
 * time, nested zones already subtracted. hw/vu1rt.cpp uses this to split
 * the "vu1" bucket per microprogram without taking a second clock reading
 * on the hot path. Returns 0 with the instrument off. */
inline uint64_t rt_prof_zone_ns(int zone) { return rt_prof_detail::g_ns[zone]; }

/* Cost of one clock reading on this host, in nanoseconds, measured once.
 *
 * The instrument bills a zone by reading the clock at its edges, so on the
 * movie's register-driven path, where a field holds tens of thousands of
 * zones, the clock source is a first-order term in every bucket it prints.
 * On a host where the reading is cheap it disappears; on one where it is
 * not, an "ipu" bucket is mostly the instrument. Nothing in a log tells the
 * two apart unless the log says what a reading costs on the host it came
 * from, so it says.
 *
 * Measured over a short burst of 20000 readings. That burst is host time
 * like any other, so the caller decides where it lands: rt_prof_init runs
 * it at startup before the first window opens, and rt_prof_report runs it
 * after the window it is closing has been billed, inside the block whose
 * cost is deliberately outside both windows. */
inline void rt_prof_measure_clock() {
    static bool done = false;
    if (done) return;
    done = true;
    constexpr int kN = 20000;
    const uint64_t t0 = rt_prof_detail::now_ns();
    uint64_t acc = 0;
    for (int i = 0; i < kN; ++i) acc += rt_prof_detail::now_ns();
    const uint64_t t1 = rt_prof_detail::now_ns();
    const double per = (double)(t1 - t0) / (double)kN;
    rt_log_info("prof", "clock source: %.1f ns per reading, so one profiled MMIO access carries about "
                   "%.1f ns of instrument on this host (sum 0x%016llx)",
        per, 2.0 * per, (unsigned long long)acc);
}

/* Parses ICORECOMP_PROFILE. Call once from main, after rt_log_init.
 *
 * Set: identical to before debug.profile_fields existed, including being
 * on by default for any spelling that is not one of the "off" ones below,
 * and it wins over the settings file for the rest of the run (a mismatch
 * between the two is logged once, here). Unset: debug.profile_fields
 * decides instead (0 = off; its compiled-in default is the same 180-field
 * interval as g_every, so a run with neither set behaves as before),
 * re-read every field by rt_prof_field rather than latched here -- see
 * g_rt_prof_env_set. Builds with no ICORECOMP_HAVE_SETTINGS keep today's
 * on-by-default behavior when the env var is unset, since there is no
 * settings.json in those targets to ask.
 *
 * The report is one block of at most 14 lines every g_every fields, which
 * is negligible next to the rest of the log, and a run that arrives
 * without it cannot be diagnosed at all. */
inline void rt_prof_init() {
    const char* e = std::getenv("ICORECOMP_PROFILE");
    g_rt_prof_env_set = e != nullptr;
    if (e && (!*e || std::strcmp(e, "0") == 0 || std::strcmp(e, "-") == 0
              || std::strcmp(e, "none") == 0)) {
        return;
    }
    if (e) {
        unsigned long n = std::strtoul(e, nullptr, 10);
        if (n > 1) rt_prof_detail::g_every = (unsigned)n;
        rt_prof_detail::g_last_ns = rt_prof_detail::now_ns();
        rt_prof_detail::g_window_ns = rt_prof_detail::g_last_ns;
        g_rt_prof_on = true;
        rt_log_info("prof", "profiling on: one summary every %u fields (ICORECOMP_PROFILE=%s)."
                       " Buckets are exclusive self time and sum to wall clock.",
            rt_prof_detail::g_every, e);
        rt_prof_measure_clock();
        return;
    }
#ifndef ICORECOMP_HAVE_SETTINGS
    /* No settings link in this target (e.g. icorecomp-ipu-selftest): keep
     * the historical on-by-default behavior. With ICORECOMP_HAVE_SETTINGS,
     * rt_prof_field decides instead, once debug.profile_fields is actually
     * loaded: rt_prof_init runs before rt_settings_init (see main.cpp), so
     * rt_settings() here would still read the compiled-in defaults, not
     * whatever settings.json says. */
    rt_prof_detail::g_last_ns = rt_prof_detail::now_ns();
    rt_prof_detail::g_window_ns = rt_prof_detail::g_last_ns;
    g_rt_prof_on = true;
    rt_log_info("prof", "profiling on: one summary every %u fields (ICORECOMP_PROFILE unset, default)."
                   " Buckets are exclusive self time and sum to wall clock.",
        rt_prof_detail::g_every);
    rt_prof_measure_clock();
#endif
}

/* Emits one window summary, largest bucket first, then reopens the
 * window. The lines the report itself costs land outside both windows,
 * because every counter is reset after the last line is written. */
/* Implemented in ee/sched.cpp; reading it clears the counters. */
extern "C" void rt_clock_sources(uint64_t* backedge, uint64_t* mmio, uint64_t* idle);

/* Detail lines contributed by one subsystem to the summary. Each writes
 * its own rt_log lines and clears its window counters, the same contract
 * as rt_clock_sources; each is silent when it has nothing to report.
 * Implemented in hw/vu1rt.cpp and hw/geomcheck.cpp. */
extern "C" void rt_vu1_prof_report(double fields);
extern "C" void rt_geom_prof_report(double fields);

/* Field-boundary stall diagnostics for the summary's "fields:" line. The
 * bucket table is an average over a whole window, which is exactly the
 * wrong shape for a stutter: one field lost to a stall is invisible in it.
 * These are the extremes instead, filled and cleared by one read, the same
 * contract as rt_clock_sources.
 *
 *   catch_up            fields that ran without waiting: the deadline was
 *                       already past once the audio debt was applied, or
 *                       the queue held less than one field of mix
 *                       (hw/gspriv.cpp pace_field). Not the same as fields
 *                       that repaid something; with the queue at the
 *                       cushion there is no debt and a field that merely
 *                       overran its period is counted too.
 *   flush/scanout/present_ns, present_fields, presents, present_repeats
 *                       the GS backend's present path split three ways,
 *                       summed over the fields it covers; zero when the
 *                       backend has no present path to report. present_ns is
 *                       per present and not per field: with
 *                       display.present_rate set, the newest field is
 *                       presented again between fields, `presents` counts
 *                       every present and `present_repeats` the ones that
 *                       showed a picture already on screen
 *
 * Implemented in hw/gspriv.cpp; rt_disc_prof_max_ms in iso/iso9660.cpp. */
struct RtGsFieldProf {
    uint32_t catch_up;
    uint64_t flush_ns;
    uint64_t scanout_ns;
    uint64_t present_ns;
    uint64_t present_fields;
    uint64_t presents;
    uint64_t present_repeats;
    /* The GS command ring's worker thread, when there is one (see
     * RtGsConsumerTimings in gs/gs_backend.h). All zero with the ring
     * drained inline or bypassed, and the summary drops the line. The three
     * costs are that thread's, not this one's, so they do not belong in the
     * bucket table above: they are a second budget against the same field
     * period, and both have to fit in it. */
    uint64_t worker_gs_ns;
    uint64_t worker_present_ns;
    uint64_t worker_idle_ns;
    uint64_t worker_fields;
    uint64_t worker_wait_ns;
    uint64_t worker_waits;
};
extern "C" void rt_gs_field_prof(RtGsFieldProf* out);
extern "C" double rt_disc_prof_max_ms(void);

inline void rt_prof_report() {
    using namespace rt_prof_detail;
    const uint64_t t = now_ns();
    g_ns[g_cur] += t - g_last_ns;
    const uint64_t wall = t - g_window_ns;
    /* One shot, and only for a run that got here without rt_prof_init
     * having done it: with ICORECOMP_PROFILE set, or in a build with no
     * settings (the selftest targets), rt_prof_init measured the clock at
     * startup and this returns immediately. What is left is the
     * debug.profile_fields path, where rt_prof_field turns the instrument
     * on mid-run and this is the first place that can measure.
     *
     * Called after the window above has been billed and before the report
     * lines, so the 20000 readings land in the same gap the report itself
     * does: everything from here to the counter reset at the end of this
     * function is outside both the window just closed and the one reopened
     * there. */
    rt_prof_measure_clock();
    const double wall_ms = (double)wall / 1e6;
    const double fields = (double)g_window_fields;

    int order[RT_PROF_COUNT];
    for (int i = 0; i < RT_PROF_COUNT; ++i) order[i] = i;
    std::sort(order, order + RT_PROF_COUNT,
              [](int a, int b) { return g_ns[a] > g_ns[b]; });

    /* Absolute field numbers, not just a count. A report of "it goes wrong
     * ten seconds in" can only be matched to a summary if the summary says
     * which fields it covers. A field is rt_field_rate_hz() long (59.94 Hz
     * on NTSC, 50 Hz on PAL), so the seconds are game time, not host time;
     * the two diverge exactly when this instrument is worth reading. The
     * rate is read now rather than accumulated per field, so a window that
     * straddles a video mode change reports its whole span at the mode it
     * ended in. */
    /* Inclusive range. The window's last field is g_fields, and it covers
     * g_window_fields of them, so the first is that many back plus one;
     * without the +1 the line claims one field too many and overlaps the
     * previous window's last field. */
    const uint64_t first_field = g_fields - g_window_fields + 1;
    const double field_hz = rt_field_rate_hz();
    rt_log_info("prof", "fields %llu..%llu (game time %.2f..%.2f s): %.0f fields in %.1f ms host"
                   " = %.2f fields/s (PS2 %s target %.2f);"
                   " exclusive self time, buckets sum to wall",
        (unsigned long long)first_field, (unsigned long long)g_fields,
        (double)first_field / field_hz, (double)g_fields / field_hz,
        fields, wall_ms, wall > 0 ? fields * 1e9 / (double)wall : 0.0,
        rt_video_mode_name(), field_hz);
    for (int i = 0; i < RT_PROF_COUNT; ++i) {
        const int z = order[i];
        if (g_ns[z] == 0) continue;
        const double ms = (double)g_ns[z] / 1e6;
        rt_log_info("prof", "  %-7s %8.1f ms %5.1f%% %7.3f ms/field  n=%-9llu mean %8.2f us",
            kName[z], ms, wall > 0 ? 100.0 * (double)g_ns[z] / (double)wall : 0.0,
            fields > 0 ? ms / fields : 0.0,
            (unsigned long long)g_calls[z],
            g_calls[z] ? (double)g_ns[z] / 1e3 / (double)g_calls[z] : 0.0);
    }

    /* Decomposition of two of the buckets above: which microprogram the
     * "vu1" time went to, and how many vertices the packets carried. Both
     * clear their own counters. */
    rt_vu1_prof_report(fields);
    rt_geom_prof_report(fields);

    /* The worst field in the window, and the present path it spent that
     * time in. Read unconditionally so the counters clear even on a window
     * with no fields in it. */
    {
        RtGsFieldProf fp = {};
        rt_gs_field_prof(&fp);
        const double disc_ms = rt_disc_prof_max_ms();
        rt_log_info("prof", "  fields: longest %.1f ms, %u over 20 ms, %u over 50 ms;"
                       " %u catch-up fields (ran without waiting);"
                       " longest disc read %.1f ms",
            (double)g_worst_field_total / 1e6, (unsigned)g_fields_over_20ms,
            (unsigned)g_fields_over_50ms, (unsigned)fp.catch_up, disc_ms);
        if (g_worst_field_total > 0) {
            /* Where the longest field went, largest buckets first. This is
             * the line that separates a GPU wait (present, gs) from a
             * host read (disc) or a decode (ipu) when a field stalls. */
            int worder[RT_PROF_COUNT];
            for (int i = 0; i < RT_PROF_COUNT; ++i) worder[i] = i;
            std::sort(worder, worder + RT_PROF_COUNT,
                      [](int a, int b) { return g_worst_field[a] > g_worst_field[b]; });
            char line[256];
            int len = std::snprintf(line, sizeof line, "    longest field %.1f ms:",
                                    (double)g_worst_field_total / 1e6);
            for (int i = 0; i < RT_PROF_COUNT && i < 5 && len < (int)sizeof line; ++i) {
                const int z = worder[i];
                if (g_worst_field[z] == 0) break;
                len += std::snprintf(line + len, sizeof line - (size_t)len, " %s %.1f",
                                     kName[z], (double)g_worst_field[z] / 1e6);
            }
            rt_log_info("prof", "%s", line);
        }
        if (fp.present_fields) {
            const double pf = (double)fp.present_fields;
            /* present_frame is per present, the other two per field: with
             * display.present_rate set there is more than one present per
             * field, and dividing the present cost by fields would report a
             * present that got cheaper the more often it ran. `presents` and
             * `repeats` are the counts that make the two divisors readable;
             * repeats are recomposites of the field already on screen. */
            const double pp = fp.presents ? (double)fp.presents : 1.0;
            rt_log_info("prof", "    present flush %.3f ms/field  scanout %.3f ms/field"
                           "  present_frame %.3f ms/present"
                           "  (%llu presents of %llu fields, %llu repeats)",
                (double)fp.flush_ns / 1e6 / pf,
                (double)fp.scanout_ns / 1e6 / pf,
                (double)fp.present_ns / 1e6 / pp,
                (unsigned long long)fp.presents,
                (unsigned long long)fp.present_fields,
                (unsigned long long)fp.present_repeats);
        }
        /* The other thread's budget. Read against the same field period as
         * the buckets above: the EE thread and the GS worker run at the same
         * time, so the port holds the field rate only if each of them fits
         * in 16.68 ms on its own. "idle" is the worker's headroom. The wait
         * is every nanosecond the EE spent blocked on the consumer: the
         * field sync point, which is also the "gswait" bucket above, plus
         * the two waits that bucket does not cover because they happen
         * wherever the caller is -- a reply to an overlay texture upload,
         * and a full ring. A wait total much larger than gswait means one of
         * those two, not the field boundary. */
        if (fp.worker_fields) {
            const double wf = (double)fp.worker_fields;
            rt_log_info("prof", "  gs worker: replay %.3f ms/field  present %.3f ms/field"
                           "  idle %.3f ms/field over %llu fields;"
                           " EE waited %.3f ms/field in %llu waits",
                (double)fp.worker_gs_ns / 1e6 / wf,
                (double)fp.worker_present_ns / 1e6 / wf,
                (double)fp.worker_idle_ns / 1e6 / wf,
                (unsigned long long)fp.worker_fields,
                (double)fp.worker_wait_ns / 1e6 / wf,
                (unsigned long long)fp.worker_waits);
        }
    }

    /* Audio device queue. An empty queue is a click; this is the number
     * that says whether choppy sound is starvation or something else. */
    {
        uint32_t qmin = 0, qmean = 0, qmax = 0;
        uint64_t under = 0;
        rt_audio_queue_stats(&qmin, &qmean, &qmax, &under);
        uint64_t subf = rt_audio_window_frames();
        if (wall > 0 && subf) {
            const double rate = (double)subf * 1e9 / (double)wall;
            rt_log_info("prof", "  audio rate: %llu frames submitted = %.0f Hz "
                           "(device is %u Hz; over means it plays fast and drops, "
                           "under means it starves)",
                (unsigned long long)subf, rate, (unsigned)RT_AUDIO_RATE);
        }
        if (qmean || qmax || under) {
            rt_log_info("prof", "  audio queue: min %u mean %u max %u frames "
                           "(%.1f/%.1f/%.1f ms), %llu submits found it empty",
                qmin, qmean, qmax,
                (double)qmin / 48.0, (double)qmean / 48.0, (double)qmax / 48.0,
                (unsigned long long)under);
        }
    }

    /* Virtual time by source. "spun" is time the guest paid host cycles to
     * pass; "idle" is time skipped for free. A wait-bound title screen shows
     * a large spun share, and the lever there is the cycle billing rate, not
     * raw throughput. */
    uint64_t cb = 0, cm = 0, ci = 0;
    rt_clock_sources(&cb, &cm, &ci);
    const double tot = (double)(cb + cm + ci);
    if (tot > 0.0) {
        rt_log_info("prof", "  vclk source: backedge %.1f%%  mmio %.1f%%  idle %.1f%% "
                       "(%.2f fields of virtual time advanced)",
            100.0 * (double)cb / tot, 100.0 * (double)cm / tot,
            100.0 * (double)ci / tot, tot / (double)rt_cycles_per_field());
    }

    for (int i = 0; i < RT_PROF_COUNT; ++i) {
        g_ns[i] = 0;
        g_calls[i] = 0;
        g_worst_field[i] = 0;
    }
    g_worst_field_total = 0;
    g_fields_over_20ms = 0;
    g_fields_over_50ms = 0;
    g_field_snap_valid = false; /* g_ns just restarted; the next delta is void */
    g_window_fields = 0;
    g_last_ns = now_ns();
    g_window_ns = g_last_ns;
}

/* Called once per field from the GS vsync hook (hw/gspriv.cpp).
 *
 * When ICORECOMP_PROFILE is unset, debug.profile_fields is re-read here
 * every field rather than latched once by rt_prof_init: rt_prof_init runs
 * before rt_settings_init (see main.cpp), so on the very first call this is
 * what actually picks up the loaded settings.json value, and afterwards it
 * is what makes an in-run settings edit take effect without a restart.
 * Transitions log once; a steady value never logs again here. */
inline void rt_prof_field() {
#ifdef ICORECOMP_HAVE_SETTINGS
    if (g_rt_prof_env_set) {
        /* rt_prof_init already fully decided g_rt_prof_on/g_every from
         * ICORECOMP_PROFILE; this only logs, once, whether settings.json
         * would have said something different, now that it is actually
         * loaded. */
        if (!g_rt_prof_env_checked) {
            g_rt_prof_env_checked = true;
            const int settings_fields = rt_settings().debug.profile_fields;
            const bool settings_on = settings_fields > 0;
            const bool differs = settings_on != g_rt_prof_on
                || (settings_on && (unsigned)settings_fields != rt_prof_detail::g_every);
            if (differs) {
                rt_log_warn("prof", "debug.profile_fields: using ICORECOMP_PROFILE (applied at startup),"
                               " settings.json value ignored");
            }
        }
    } else {
        const int fields = rt_settings().debug.profile_fields;
        const bool was_on = g_rt_prof_on;
        if (fields > 0) {
            if (!was_on) {
                rt_prof_detail::g_last_ns = rt_prof_detail::now_ns();
                rt_prof_detail::g_window_ns = rt_prof_detail::g_last_ns;
                rt_prof_detail::g_window_fields = 0;
                rt_log_info("prof", "profiling on: one summary every %d fields (debug.profile_fields=%d)."
                               " Buckets are exclusive self time and sum to wall clock.",
                    fields, fields);
            } else if ((unsigned)fields != rt_prof_detail::g_every) {
                rt_log_info("prof", "debug.profile_fields changed to %d; one summary every %d fields from here",
                    fields, fields);
            }
            rt_prof_detail::g_every = (unsigned)fields;
            g_rt_prof_on = true;
        } else if (was_on) {
            rt_log_info("prof", "profiling off (debug.profile_fields=0)");
            g_rt_prof_on = false;
        }
    }
#endif
    if (!g_rt_prof_on) return;
    {
        /* Close the field: bill the open zone up to now, then take the
         * per-bucket deltas since the previous boundary and keep them if
         * this was the longest field of the window so far. */
        using namespace rt_prof_detail;
        const uint64_t t = now_ns();
        g_ns[g_cur] += t - g_last_ns;
        g_last_ns = t;
        if (g_field_snap_valid) {
            uint64_t total = 0;
            for (int i = 0; i < RT_PROF_COUNT; ++i) total += g_ns[i] - g_field_snap[i];
            if (total > g_worst_field_total) {
                g_worst_field_total = total;
                for (int i = 0; i < RT_PROF_COUNT; ++i) g_worst_field[i] = g_ns[i] - g_field_snap[i];
            }
            if (total > 20000000ull) ++g_fields_over_20ms;
            if (total > 50000000ull) {
                ++g_fields_over_50ms;
                /* A hitch names itself at warn, so a run at the default log
                 * level says where its long fields went without the info
                 * level summaries. 50 ms is two and a half PAL fields.
                 * The limiter's sleep is not a hitch and is left out of
                 * the total named here; the three largest buckets are
                 * printed. Folded by powers of two over the run. */
                if ((g_fields_over_50ms & (g_fields_over_50ms - 1)) == 0) {
                    uint64_t d[RT_PROF_COUNT];
                    uint64_t busy = 0;
                    for (int i = 0; i < RT_PROF_COUNT; ++i) {
                        d[i] = g_ns[i] - g_field_snap[i];
                        if (i != RT_PROF_LIMIT) busy += d[i];
                    }
                    int top[3] = {-1, -1, -1};
                    for (int k = 0; k < 3; ++k) {
                        for (int i = 0; i < RT_PROF_COUNT; ++i) {
                            if (i == RT_PROF_LIMIT || i == top[0] || i == top[1]) continue;
                            if (top[k] < 0 || d[i] > d[top[k]]) top[k] = i;
                        }
                    }
                    char extra[160] = "";
                    if (g_rt_prof_field_hook) g_rt_prof_field_hook(true, extra, sizeof(extra));
                    rt_log_warn("prof", "hitch #%llu: field %llu took %.1f ms (%.1f ms outside the "
                        "frame limiter): %s %.1f ms, %s %.1f ms, %s %.1f ms%s%s",
                        (unsigned long long)g_fields_over_50ms, (unsigned long long)g_fields,
                        (double)total / 1e6, (double)busy / 1e6,
                        top[0] >= 0 ? kName[top[0]] : "-", top[0] >= 0 ? (double)d[top[0]] / 1e6 : 0.0,
                        top[1] >= 0 ? kName[top[1]] : "-", top[1] >= 0 ? (double)d[top[1]] / 1e6 : 0.0,
                        top[2] >= 0 ? kName[top[2]] : "-", top[2] >= 0 ? (double)d[top[2]] / 1e6 : 0.0,
                        extra[0] ? "; " : "", extra);
                }
            }
        }
        if (g_rt_prof_field_hook) g_rt_prof_field_hook(false, nullptr, 0);
        for (int i = 0; i < RT_PROF_COUNT; ++i) g_field_snap[i] = g_ns[i];
        g_field_snap_valid = true;
    }
    ++rt_prof_detail::g_fields;
    if (++rt_prof_detail::g_window_fields >= rt_prof_detail::g_every) rt_prof_report();
}

#endif /* ICORECOMP_PROF_H */
