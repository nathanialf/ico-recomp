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
     * Gentle gain, hard clamp: this must never become a speed control. */
    auto adjusted = step;
    {
        const int queued = rt_audio_queued_frames();
        if (queued >= 0) {
            /* Target a few fields of cushion. 800 frames is one field. */
            constexpr double kTargetFrames = 2400.0;
            double err = (kTargetFrames - (double)queued) / kTargetFrames;
            if (err > 1.0) err = 1.0;
            if (err < -1.0) err = -1.0;
            /* Queue below target -> produce sooner (shorter period). */
            double scale = 1.0 - 0.05 * err;
            adjusted = std::chrono::duration_cast<PaceClock::duration>(step * scale);
        }
    }
    deadline += adjusted;
    /* Fell far behind (a load spike, or the host simply cannot keep up).
     * Resync rather than accumulate debt and then sprint to catch up. */
    if (now > deadline + std::chrono::milliseconds(100)) {
        deadline = now;
        return;
    }
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

} // namespace

void rt_gs_vsync_hook(unsigned field) {
    /* The event pump also runs from inside WSI::begin_frame via the
     * pump_events callback (gs_parallel_lib.cpp's RtPgs::present_frame), but
     * that only happens when a frame is actually presented. Running it here
     * too guarantees events (and window-close) still drain on a skipped
     * field -- a minimized window, or a dump/headless backend that never
     * calls begin_frame at all. rt_settings_apply_pending() applies warm
     * settings (present mode, render scale) here because their subsystem
     * calls fatal if made mid-frame; this is the one place guaranteed to run
     * between frames every field. rt_ui_tick() follows for the same reason:
     * RmlUi's update and render call the overlay texture and set_frame entry
     * points, which are between-frames-only too (see ui/ui.h). */
    rt_window_pump();
    rt_settings_apply_pending();
    rt_ui_tick();
    {
        RT_PROF_ZONE(RT_PROF_PRESENT);
        GsBackend* be = rt_gs_backend();
        uint64_t csr = 0;
        if (rt_gs_mmio_read(0x12001000u, &csr)) be->write_priv(0x1000, csr);
        be->write_priv(0x1010, rt_gs_get_imr());
        be->vsync(field);
    }
    static const bool geom = rt_verbose("geom");
    if (geom) rt_geom_field(field);
    rt_log_flush();
    /* One field boundary; the profile summary comes out of here every
     * ICORECOMP_PROFILE-th field. */
    rt_prof_field();
    pace_field();
}
