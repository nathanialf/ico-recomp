/* hw/gspriv.cpp: GS privileged register MMIO (0x12000000-0x12001FFF)
 * routed to the GS backend's write_priv/read_priv shadow.
 *
 * CSR/IMR ownership decision: ee/intc.cpp keeps ownership of CSR and IMR
 * semantics (event flags, write-1-clear, RESET, FIELD bit, and the values
 * GsGetIMR/GsPutIMR see) because interrupt delivery depends on them and
 * moving that state would couple the delivery path to the backend. This
 * module forwards every privileged write to the backend as well, so the
 * dump's PrivRegisters snapshots stay coherent, and it snapshots the live
 * CSR value into the backend at each vsync (rt_gs_vsync_hook). Reads of
 * CSR/IMR come from intc's shadow; reads of everything else come from the
 * backend shadow (last value written, which is what the game expects from
 * write-mostly display registers).
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../gs/gs_backend.h"
#include "../host/audio.h"
#include "../host/mouse.h"
#include "../host/portable.h"
#include "../host/settings.h"
#include "../host/window.h"
#include "../prof.h"
#include "../ui/ui.h"
#include <cstring>
#include <cstdlib>
#include <optional>
#include <thread>
#include <chrono>

namespace {

bool is_priv(uint32_t addr) {
    return addr >= 0x12000000u && addr < 0x12002000u;
}

} // namespace

void rt_hw_init() {
    rt_vu1_window_page(); /* also constructs the Vu1State overlay */
    rt_vu1_init();        /* registers generated VU1 microprograms */
    rt_gs_backend();      /* opens the dump file early if configured */
    rt_log("hw", "graphics transport initialized (DMAC, VIF1, GIF, GS priv)");
}

bool rt_gspriv_mmio_read(uint32_t addr, uint64_t* out) {
    if (!is_priv(addr)) return false;
    /* CSR/IMR: intc.cpp is the authority. */
    if (rt_gs_mmio_read(addr, out)) return true;
    *out = rt_gs_backend()->read_priv(addr & 0x1FFF);
    return true;
}

bool rt_gspriv_mmio_write(uint32_t addr, uint64_t v) {
    if (!is_priv(addr)) return false;
    RT_PROF_ZONE(RT_PROF_GS);
    /* Record everything in the backend shadow (dump coherence), then let
     * intc.cpp apply CSR/IMR semantics on top. */
    rt_gs_backend()->write_priv(addr & 0x1FFF, v);
    rt_gs_mmio_write(addr, v);
    return true;
}

void rt_gs_program_crt(uint32_t interlace, uint32_t mode, uint32_t ffmd) {
    /* SMODE1 field layout (public hardware facts, ps2tek "GS privileged
     * registers"): RC bits 0-2, LC bits 3-9, T1248 10-11, SLCK 12,
     * CMOD 13-14. CMOD: 2 = NTSC, 3 = PAL. LC 32 = analog video clock.
     * Only CMOD and LC select the video mode downstream; the PLL fields do
     * not matter to an emulated CRTC, so they stay zero. */
    uint64_t cmod;
    switch (mode) {
        case 0x02: cmod = 2; break; /* GS_MODE_NTSC */
        case 0x03: cmod = 3; break; /* GS_MODE_PAL */
        default:
            rt_fatal("gs", rt_sched_current_ctx(),
                     "SetGsCrt mode 0x%02x is not NTSC/PAL; DTV/VESA modes are not modeled", mode);
    }
    const uint64_t smode1 = (cmod << 13) | (32ull << 3);
    const uint64_t smode2 = (interlace & 1u) | ((ffmd & 1u) << 1);
    GsBackend* be = rt_gs_backend();
    be->write_priv(0x0010, smode1);
    be->write_priv(0x0020, smode2);
    rt_log("gs", "SetGsCrt: SMODE1=0x%08llx SMODE2=0x%llx programmed (%s, %s, ffmd=%u)",
        (unsigned long long)smode1, (unsigned long long)smode2,
        cmod == 2 ? "NTSC" : "PAL", interlace ? "interlaced" : "progressive", ffmd);
}

namespace {

/* Frame limiter.
 *
 * Nothing else paces this port: the virtual clock is advanced by guest
 * execution, so the game runs at whatever rate the host can manage. Too
 * slow and the audio stream starves; too fast and it overruns and drops
 * frames, which is what "SDL queue full (running ahead of real time)"
 * reports. Either way the sound is chopped. A blocking FIFO present used
 * to hide this by accident, at the cost of collapsing to half speed.
 *
 * Paces to the NTSC field rate. ICORECOMP_FPS_LIMIT=0 disables it, or set
 * it to a field rate to override. debug.fps_limit_hz is the settings.json
 * twin (0 disables the same way); the environment variable wins for the
 * life of the run when it is set, latched once on first use like the old
 * read-once static did. Bounded diagnostic runs (ICORECOMP_MAX_VBLANKS)
 * are never paced. */
using PaceClock = std::chrono::steady_clock;

/* Field-boundary diagnostics for the profile summary (prof.h's
 * RtGsFieldProf). Counted only while the instrument is on and cleared by
 * the read, so each window's numbers describe that window alone. The
 * bucket table is an average; a stutter is one field, and only the
 * extremes show it.
 *
 * g_catch_up_fields counts fields that ran without waiting, which is not
 * the same as fields that repaid something: with the queue at the cushion
 * the deficit is zero and any field that merely overran its period is
 * counted too. It is a "the limiter did not hold this field back" count,
 * and a steady stream of them means the host is not making the field
 * rate. */
uint32_t g_catch_up_fields = 0;

double pace_period_seconds() {
    if (std::getenv("ICORECOMP_MAX_VBLANKS")) return 0.0;

    /* std::nullopt means ICORECOMP_FPS_LIMIT is unset: debug.fps_limit_hz
     * is then read fresh below on every call, so a settings reload takes
     * effect without a restart (a "hot" setting per settings.h). Set, it
     * is latched once, exactly like the pre-settings behavior. */
    static const std::optional<double> env_hz = [] () -> std::optional<double> {
        const char* e = std::getenv("ICORECOMP_FPS_LIMIT");
        if (!e) return std::nullopt;
        double hz = 0.0; /* "0" or "off": disabled, same as debug.fps_limit_hz = 0 */
        if (std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0) {
            double v = std::strtod(e, nullptr);
            hz = v > 1.0 ? v : 59.94;
        }
        if (hz != rt_settings().debug.fps_limit_hz) {
            rt_log("gs", "debug.fps_limit_hz: using ICORECOMP_FPS_LIMIT=%s, settings.json value ignored", e);
        }
        return hz;
    }();

    const double hz = env_hz.has_value() ? *env_hz : rt_settings().debug.fps_limit_hz;
    return hz > 0.0 ? 1.0 / hz : 0.0;
}

void pace_field() {
    /* The settings read stays outside the zone below. pace_period_seconds
     * re-reads rt_settings() every field so debug.fps_limit_hz is hot, and
     * the "limit" bucket is read as the headroom this port has left, so
     * settings work billed to it would be read as slack that does not
     * exist. */
    const double period = pace_period_seconds();
    if (period <= 0.0) return;
    RT_PROF_ZONE(RT_PROF_LIMIT);
    static bool started = false;
    static PaceClock::time_point deadline;

    rt_time_begin_period();
    const auto step = std::chrono::duration_cast<PaceClock::duration>(
        std::chrono::duration<double>(period));
    auto now = PaceClock::now();
    if (!started) {
        started = true;
        deadline = now;
    }
    /* Lock to the audio device's clock, not just the host wall clock.
     * They are independent crystals: pacing purely on wall time drifts a
     * fraction of a percent and the device eventually starves (a click) or
     * overruns (a drop). Nudging the field period by the queue depth makes
     * the audio device the master clock, which is what it has to be, since
     * a gap in sound is far more audible than a field arriving 0.2 ms late.
     * Gentle gain, hard clamp: this must never become a speed control.
     *
     * One read of the queue serves both users below: the steady-state
     * nudge here, and the debt bound after it. */
    const int queued = rt_audio_queued_frames();
    auto adjusted = step;
    if (queued >= 0) {
        constexpr double kTargetFrames = (double)RT_AUDIO_CUSHION_FRAMES;
        double err = (kTargetFrames - (double)queued) / kTargetFrames;
        if (err > 1.0) err = 1.0;
        if (err < -1.0) err = -1.0;
        /* Queue below target -> produce sooner (shorter period). */
        double scale = 1.0 - 0.05 * err;
        adjusted = std::chrono::duration_cast<PaceClock::duration>(step * scale);
    }
    deadline += adjusted;

    /* Bound the debt, do not discard it.
     *
     * The audio queue is the master clock, so the debt this pacer owes is
     * exactly the audio the device is missing: RT_AUDIO_CUSHION_FRAMES less
     * whatever is queued, and never more than the cushion itself. The port
     * mixes one field of audio per guest sndn2 flush, so the only way to
     * put those frames back is to run fields sooner than the field rate.
     *
     * The previous rule snapped the deadline to now whenever it was more
     * than 100 ms behind, which threw the debt away at exactly the moment
     * it was largest: the queue was empty, the pacer believed it was on
     * time, and it went back to sleeping a full period per field while the
     * 5 percent nudge refilled the cushion at 40 frames a field. Measured
     * on the Windows logs, routine single stalls are already larger than
     * the cushion that was being refilled that slowly.
     *
     * So the deadline floors at now minus the deficit instead. Fields then
     * run unpaced until the device has been repaid what it is owed, and no
     * further: the deficit shrinks as the queue refills, the floor rises
     * with it, and normal pacing resumes on its own. The sprint is bounded
     * by the cushion, six fields of guest time, because the deficit can
     * never exceed it. With no device (rt_audio_queued_frames() returns
     * -1) there is no queue to repay and no clock to lock to, so that case
     * simply resyncs to now once it is more than one period behind. The
     * tolerance there used to be 100 ms; one period is what a run with no
     * audio wants, because there is no debt for a sprint to put back and
     * carrying the overrun forward only makes the next field late too.
     *
     * All of this is host-side pacing. It decides when a field is produced
     * and changes no value the game supplied or computed. */
    /* Below one field of mix (800 frames at 48 kHz) the device is within a
     * field of running dry, so there is nothing to wait for at all. Only
     * while the sound task is actually feeding the device, though: the mix
     * arrives with the guest's sndn2 flush, and if that task stalls (a
     * blocking load) while vblanks keep coming, the queue drains to zero
     * and stays there. Skipping the wait then would free-run the port at
     * full speed for as long as the stall lasts, with no audio to repay.
     * The debt bound above still covers that case on its own: it settles
     * back into normal pacing within six fields. */
    constexpr int kFieldFrames = 800;
    static uint64_t last_total_frames = 0;
    const uint64_t total_frames = rt_audio_total_frames();
    const bool fed = total_frames != last_total_frames;
    last_total_frames = total_frames;
    const bool starving = fed && queued >= 0 && queued < kFieldFrames;
    if (queued >= 0) {
        double missing = (double)RT_AUDIO_CUSHION_FRAMES - (double)queued;
        if (missing < 0.0) missing = 0.0;
        if (missing > (double)RT_AUDIO_CUSHION_FRAMES) missing = (double)RT_AUDIO_CUSHION_FRAMES;
        const auto deficit = std::chrono::duration_cast<PaceClock::duration>(
            std::chrono::duration<double>(missing / (double)RT_AUDIO_RATE));
        if (deadline < now - deficit) deadline = now - deficit;
    } else if (now > deadline + step) {
        deadline = now;
    }
    /* A catch-up field is one that runs without waiting: the deadline is
     * already past once the debt is applied, or the queue is starving. */
    if (g_rt_prof_on && (starving || now >= deadline)) ++g_catch_up_fields;
    if (starving) return;
    /* Sleep the bulk, spin the tail. Even at 1 ms timer resolution a
     * Windows sleep routinely overshoots by about a millisecond, and there
     * is under a millisecond of slack in a field, so an overshoot every
     * field pushes the whole port below the field rate. Audio production is
     * locked to the field rate, so that shortfall lands directly on the
     * device as a starved queue. Only sleep when there is enough left to
     * absorb the granularity error, and hand the last stretch to a spin. */
    constexpr auto kSpinTail = std::chrono::microseconds(2500);
    for (;;) {
        now = PaceClock::now();
        if (now >= deadline) break;
        auto remain = deadline - now;
        if (remain > kSpinTail + std::chrono::milliseconds(1)) {
            std::this_thread::sleep_for(remain - kSpinTail);
        } else {
            std::this_thread::yield();
        }
    }
}

/* Field-rate measurement for the menu's FPS readout (rt_gs_field_stats).
 * A handful of statics, counted in the vsync hook: no allocation, no
 * per-field work beyond an increment and one clock read. The window is
 * closed and republished once a second, so the readout is a real average
 * over that second rather than the reciprocal of one field interval. */
using StatClock = std::chrono::steady_clock;
StatClock::time_point g_stat_window_start;
bool g_stat_started = false;
unsigned g_stat_fields = 0;
double g_stat_fps = 0.0;
double g_stat_field_ms = 0.0;

void note_field() {
    const auto now = StatClock::now();
    if (!g_stat_started) {
        g_stat_started = true;
        g_stat_window_start = now;
        g_stat_fields = 0;
        return;
    }
    ++g_stat_fields;
    const std::chrono::duration<double> elapsed = now - g_stat_window_start;
    if (elapsed.count() < 1.0) return;
    g_stat_fps = double(g_stat_fields) / elapsed.count();
    g_stat_field_ms = elapsed.count() * 1000.0 / double(g_stat_fields);
    g_stat_window_start = now;
    g_stat_fields = 0;
}

} // namespace

void rt_gs_field_stats(double* fields_per_second, double* field_ms) {
    if (fields_per_second) *fields_per_second = g_stat_fps;
    if (field_ms) *field_ms = g_stat_field_ms;
}

/* See prof.h. One read fills the struct and clears every window counter,
 * the same contract as rt_clock_sources, so a report that prints nothing
 * still resets. The present decomposition comes from the GS backend
 * (gs_backend.h present_timings); backends with no present path leave it
 * at zero and the summary drops that sub-line. */
extern "C" void rt_gs_field_prof(RtGsFieldProf* out) {
    if (!out) return;
    *out = RtGsFieldProf{};
    out->catch_up = g_catch_up_fields;
    g_catch_up_fields = 0;
    /* rt_gs_backend_if_created(), not rt_gs_backend(): a profile report must
     * never be the call that builds the backend, which would open the Vulkan
     * device and the window from inside the instrument (see gs_backend.h).
     * With no backend the three present counters stay zero and the summary
     * drops that sub-line. */
    if (GsBackend* be = rt_gs_backend_if_created()) {
        be->present_timings(&out->flush_ns, &out->scanout_ns,
                            &out->present_ns, &out->present_fields);
        /* The GS command ring's worker, when there is one: what it spent on
         * replaying packets and on presenting, how much of the window it sat
         * idle, and what its being a thread cost this one in waiting. A
         * backend with no worker leaves the struct alone (it is already
         * zeroed) and the summary drops the line. */
        RtGsConsumerTimings ct = {};
        be->consumer_timings(&ct);
        out->worker_gs_ns = ct.gs_ns;
        out->worker_present_ns = ct.present_ns;
        out->worker_idle_ns = ct.idle_ns;
        out->worker_fields = ct.fields;
        out->worker_wait_ns = ct.ee_wait_ns;
        out->worker_waits = ct.ee_waits;
    }
}

void rt_gs_vsync_hook(unsigned field) {
    /* The event pump also runs from inside WSI::begin_frame via the
     * pump_events callback (gs_parallel_present.cpp's RtPgs::present_frame), but
     * that only happens when a frame is actually presented. Running it here
     * too guarantees events (and window-close) still drain on a skipped
     * field -- a minimized window, or a dump/headless backend that never
     * calls begin_frame at all. rt_settings_apply_pending() applies warm
     * settings (present mode, render scale) here because their subsystem
     * calls fatal if made mid-frame; this is the one place guaranteed to run
     * between frames every field. rt_ui_tick() follows for the same reason:
     * RmlUi's update and render call the overlay texture and set_frame entry
     * points, which are between-frames-only too (see ui/ui.h).
     *
     * rt_mouse_tick() comes last of the four because it reads what the other
     * three settled: the pump's focus events, the settings the applier just
     * committed, and whether the overlay now wants input. It is here rather
     * than in the pump for a mechanical reason, not a technical one: SDL
     * calls are legal from the pump, but keeping every mode change (here,
     * relative mouse mode) at the field boundary means the rule "the pump
     * only records, the field boundary acts" needs no exceptions to check. */
    note_field();
    rt_window_pump();
    rt_settings_apply_pending();
    rt_ui_tick();
    rt_mouse_tick();
    GsBackend* be = rt_gs_backend();
    {
        RT_PROF_ZONE(RT_PROF_PRESENT);
        uint64_t csr = 0;
        if (rt_gs_mmio_read(0x12001000u, &csr)) be->write_priv(0x1000, csr);
        be->write_priv(0x1010, rt_gs_get_imr());
        be->vsync(field);
    }
    {
        /* The one sync point with the GS command ring's worker thread
         * (gs/gs_threaded.h): it returns once the worker has finished every
         * field but the one just enqueued, so at most one field is ever in
         * flight. It pumps the window while it waits, because a worker
         * parked on a minimized window is waiting for an event only this
         * thread can deliver.
         *
         * Its own zone rather than "present": with the ring drained by the
         * worker, the "gs" and "present" buckets hold only the enqueue cost,
         * and what the move to a thread actually costs this thread is
         * exactly this wait. The worker's own budget is the "gs worker" line
         * of the profile summary (prof.h). A no-op while the ring is drained
         * inline, and when it is bypassed. */
        RT_PROF_ZONE(RT_PROF_GSWAIT);
        be->field_sync();
    }
    /* Window closure, polled here rather than acted on inside the backend:
     * the backend's vsync can run on the worker thread now, and process
     * teardown (the atexit handler that joins that worker, the Vulkan
     * wait-idle, the pipeline cache write) has to happen on this one. The
     * flag is sticky and set by the live backend for a closed window or a
     * quit request; host/window.cpp's rt_request_exit and the pump's
     * SDL_EVENT_QUIT both reach it through rt_pgs_notify_quit. */
    if (be->window_closed()) {
        rt_log("gs", "paraLLEl-GS: window closed or exit requested, exiting");
        std::exit(0);
    }
    static const bool geom = rt_verbose("geom");
    if (geom) rt_geom_field(field);
    rt_log_flush();
    /* One field boundary; the profile summary comes out of here every
     * ICORECOMP_PROFILE-th field. */
    rt_prof_field();
    pace_field();
}
