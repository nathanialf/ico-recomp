/* ee/syscalls.cpp: rt_syscall, the EE kernel HLE dispatch.
 *
 * Numbering source: ps2sdk ee/kernel/include/syscallnr.h (clean-room
 * structural reference; numbers are public SDK facts). The kernel dispatches
 * on $v1: negative numbers are the interrupt-context "i" variants and index
 * the same 256-entry table by absolute value. Some i-variants share the
 * positive slot (SIF), some have their own (threads/semas).
 *
 * Struct layouts for CreateThread/CreateSema/Refer* come from ps2sdk
 * kernel.h (ee_thread_t / ee_sema_t / ee_thread_status_t), same clean-room
 * basis.
 *
 * Policy (CLAUDE.md): any number not in this table is a fatal log with a
 * register dump. Every distinct (number, a0) signature is logged once,
 * then on power-of-two repeats. ICORECOMP_TRACE=1 logs every call.
 */
#include "kernel.h"

#include "osd_config.h"
#include "../host/settings.h"
#include "../hw/hw.h"
#include "../prof.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

namespace {

struct Args {
    R5900Context* ctx;
    uint32_t a0, a1, a2, a3, t0;
    bool ivariant;
};

uint64_t g_syscall_count = 0;
bool g_dchain_logged = false;

/* Per-call-site log cap: the first `cap` calls land on the default
 * channel, everything after that only when `channel` is verbose. The
 * counter is tested first because rt_verbose walks a tag list, and these
 * sit on paths the game takes constantly.
 *
 * CreateThread/StartThread/CreateSema use it for the first 32 of each kind
 * (enough to see the boot-time thread and sema population, which this
 * title mostly creates up front); past that the "sched" inventory dump
 * (rt_sched_dump_inventory) already lists every live thread and semaphore
 * with its creator, so a per-call line stops earning its keep. */
bool log_capped(const char* channel, uint32_t* logged, uint32_t cap) {
    if (*logged < cap) {
        ++*logged;
        return true;
    }
    return rt_verbose(channel);
}

constexpr uint32_t kSchedLogCap = 32;
/* The cap on the generic syscall trace line for the syscalls named in
 * is_syscall_trace_capped below; SetSyscall's own "recorded" line uses it
 * too, so the two lines for one call appear and stop together. */
constexpr uint32_t kSyscallTraceCap = 8;
uint32_t g_thread_create_logged = 0;
uint32_t g_thread_start_logged = 0;
uint32_t g_sema_create_logged = 0;

void set_v0(R5900Context* ctx, int64_t v) {
    ctx->r[2].s64x[0] = v;
    ctx->r[2].u64x[1] = 0;
}

/* ---- handlers ----------------------------------------------------------- */

int64_t h_ResetEE(const Args& a) {
    rt_log_info("syscall", "ResetEE(0x%x): no-op (no devices to reset)", a.a0);
    return 0;
}

int64_t h_SetGsCrt(const Args& a) {
    g_kern.gscrt_interlace = a.a0;
    g_kern.gscrt_mode = a.a1;
    g_kern.gscrt_ffmd = a.a2;
    g_kern.gscrt_set = true;
    rt_log_info("syscall", "SetGsCrt(interlace=%u, mode=0x%x, ffmd=%u) recorded", a.a0, a.a1, a.a2);
    /* The real kernel programs SMODE1/SMODE2 here; games never write SMODE1
     * themselves, and the GS needs it to know the video mode. */
    rt_gs_program_crt(a.a0, a.a1, a.a2);
    return 0;
}

int64_t h_Exit(const Args& a) {
    rt_sched_exit_game((int)a.a0, "Exit syscall from guest");
}

int64_t h_AddIntcHandler(const Args& a) {
    return rt_intc_add_handler((int)a.a0, a.a1, (int)a.a2, (uint32_t)a.ctx->r[28].u64x[0]);
}
int64_t h_RemoveIntcHandler(const Args& a) { return rt_intc_remove_handler((int)a.a0, (int)a.a1); }
int64_t h_AddDmacHandler(const Args& a) {
    return rt_dmac_add_handler((int)a.a0, a.a1, (int)a.a2, (uint32_t)a.ctx->r[28].u64x[0]);
}
int64_t h_RemoveDmacHandler(const Args& a) { return rt_dmac_remove_handler((int)a.a0, (int)a.a1); }
int64_t h_EnableIntc(const Args& a) { return rt_intc_enable((int)a.a0); }
int64_t h_DisableIntc(const Args& a) { return rt_intc_disable((int)a.a0); }
int64_t h_EnableDmac(const Args& a) { return rt_dmac_enable((int)a.a0); }
int64_t h_DisableDmac(const Args& a) { return rt_dmac_disable((int)a.a0); }

/* SetAlarm(u16 time, handler, arg): one-shot, `time` H-blank ticks away,
 * handler(id, time, arg) in interrupt context. The handler's $gp is the
 * caller's, the same convention AddIntcHandler uses here. See alarms.cpp. */
int64_t h_SetAlarm(const Args& a) {
    return rt_alarm_set(a.a0, a.a1, a.a2, (uint32_t)a.ctx->r[28].u64x[0]);
}
int64_t h_ReleaseAlarm(const Args& a) { return rt_alarm_release((int)a.a0); }

int64_t h_CreateThread(const Args& a) {
    /* ee_thread_t: status, func, stack, stack_size, gp_reg,
     * initial_priority, current_priority, attr, option. */
    uint32_t func = rt_gread32(a.a0 + 0x04);
    uint32_t stack = rt_gread32(a.a0 + 0x08);
    uint32_t stack_size = rt_gread32(a.a0 + 0x0C);
    uint32_t gp = rt_gread32(a.a0 + 0x10);
    int prio = (int)rt_gread32(a.a0 + 0x14);
    uint32_t attr = rt_gread32(a.a0 + 0x1C);
    uint32_t option = rt_gread32(a.a0 + 0x20);
    int id = rt_thread_create(func, stack, stack_size, gp, prio, attr, option);
    if (log_capped("sched", &g_thread_create_logged, kSchedLogCap)) {
        rt_log_info("sched", "CreateThread: id=%d entry=0x%08x stack=0x%08x+0x%x gp=0x%08x prio=%d",
            id, func, stack, stack_size, gp, prio);
    }
    return id;
}
int64_t h_DeleteThread(const Args& a) { return rt_thread_delete((int)a.a0); }
int64_t h_StartThread(const Args& a) {
    if (log_capped("sched", &g_thread_start_logged, kSchedLogCap)) {
        rt_log_info("sched", "StartThread: id=%d arg=0x%08x", (int)a.a0, a.a1);
    }
    return rt_thread_start((int)a.a0, a.a1);
}
int64_t h_ExitThread(const Args&) { rt_thread_exit_current(false); return 0; }
int64_t h_ExitDeleteThread(const Args&) { rt_thread_exit_current(true); return 0; }
int64_t h_TerminateThread(const Args& a) { return rt_thread_terminate((int)a.a0); }
int64_t h_ChangeThreadPriority(const Args& a) {
    return rt_thread_change_priority((int)a.a0, (int)a.a1, a.ivariant);
}
int64_t h_RotateThreadReadyQueue(const Args& a) {
    return rt_thread_rotate_ready_queue((int)a.a0, a.ivariant);
}
int64_t h_ReleaseWaitThread(const Args& a) { return rt_thread_release_wait((int)a.a0, a.ivariant); }
int64_t h_GetThreadId(const Args&) { return rt_thread_getid(); }
int64_t h_ReferThreadStatus(const Args& a) { return rt_thread_refer((int)a.a0, a.a1); }
int64_t h_SleepThread(const Args&) { return rt_thread_sleep(); }
int64_t h_WakeupThread(const Args& a) { return rt_thread_wakeup((int)a.a0, a.ivariant); }
int64_t h_CancelWakeupThread(const Args& a) { return rt_thread_cancel_wakeup((int)a.a0); }
int64_t h_SuspendThread(const Args& a) { return rt_thread_suspend((int)a.a0); }
int64_t h_ResumeThread(const Args& a) { return rt_thread_resume((int)a.a0); }

int64_t h_SetupThread(const Args& a) {
    /* RFU060(gp, stack_base, stack_size, args, root_func). Returns the new
     * stack pointer; ps2sdk crt0.s does `move $sp, $v0` with it. */
    g_kern.main_gp = a.a0;
    g_kern.stack_base = a.a1;
    g_kern.stack_size = a.a2;
    g_kern.args_ptr = a.a3;
    g_kern.root_func = a.t0;
    if ((int32_t)a.a1 == -1) {
        /* stack = -1: take the stack from the top of RAM. */
        g_kern.stack_base = RT_RAM_SIZE - a.a2;
    }
    uint32_t top = (g_kern.stack_base + g_kern.stack_size) & ~0xFu;
    rt_thread_setup_main(a.a0, g_kern.stack_base, g_kern.stack_size);
    /* Write an empty argument block (argc=0) so a root that parses args
     * reads zeros, not stale RAM. */
    if (rt_gptr(a.a3)) rt_gwrite32(a.a3, 0);
    rt_log_info("syscall", "RFU060/SetupThread: gp=0x%08x stack=0x%08x+0x%x args=0x%08x root=0x%08x -> sp=0x%08x",
        a.a0, g_kern.stack_base, g_kern.stack_size, a.a3, a.t0, top);
    return top;
}

int64_t h_SetupHeap(const Args& a) {
    /* RFU061(heap_base, heap_size) -> end of heap. size -1 = heap runs up
     * to the main thread's stack base. */
    g_kern.heap_base = a.a0;
    if ((int32_t)a.a1 == -1) {
        g_kern.heap_end = g_kern.stack_base ? g_kern.stack_base : RT_RAM_SIZE;
    } else {
        g_kern.heap_end = a.a0 + a.a1;
    }
    rt_log_info("syscall", "RFU061/SetupHeap: base=0x%08x size=0x%x -> heap end 0x%08x",
        a.a0, a.a1, g_kern.heap_end);
    return g_kern.heap_end;
}

int64_t h_EndOfHeap(const Args&) { return g_kern.heap_end; }

int64_t h_CreateSema(const Args& a) {
    /* ee_sema_t: count, max_count, init_count, wait_threads, attr, option. */
    int max_count = (int)rt_gread32(a.a0 + 0x04);
    int init_count = (int)rt_gread32(a.a0 + 0x08);
    uint32_t attr = rt_gread32(a.a0 + 0x10);
    uint32_t option = rt_gread32(a.a0 + 0x14);
    int id = rt_sema_create(init_count, max_count, attr, option);
    if (log_capped("sched", &g_sema_create_logged, kSchedLogCap)) {
        rt_log_info("sched", "CreateSema: id=%d init=%d max=%d (thread %d, ra=0x%08x)",
            id, init_count, max_count, rt_thread_current_id(), (uint32_t)a.ctx->r[31].u64x[0]);
    }
    return id;
}
int64_t h_DeleteSema(const Args& a) { return rt_sema_delete((int)a.a0); }
int64_t h_SignalSema(const Args& a) { return rt_sema_signal((int)a.a0, a.ivariant); }
int64_t h_WaitSema(const Args& a) { return rt_sema_wait((int)a.a0); }
int64_t h_PollSema(const Args& a) { return rt_sema_poll((int)a.a0); }
int64_t h_ReferSemaStatus(const Args& a) { return rt_sema_refer((int)a.a0, a.a1); }

int64_t h_Copy(const Args& a) {
    /* Copy(dest, src, size_bytes): kernel memcpy. Sony libkernel uses it to
     * install patch code into kernel RAM (0x80070000 region) before pointing
     * SetSyscall vectors at it; all of that RAM is mapped, so copy it. */
    uint32_t dest = a.a0, src = a.a1, n = a.a2;
    for (uint32_t i = 0; i < n; ) {
        uint8_t* d = rt_gptr(dest + i);
        uint8_t* s = rt_gptr(src + i);
        if (!d || !s) {
            rt_fatal("syscall", a.ctx, "Copy(0x%08x, 0x%08x, %u): unmapped at offset %u", dest, src, n, i);
        }
        uint32_t chunk = 0x10000 - std::max((dest + i) & 0xFFFFu, (src + i) & 0xFFFFu);
        if (chunk > n - i) chunk = n - i;
        std::memcpy(d, s, chunk);
        i += chunk;
    }
    rt_log_info("syscall", "Copy(dest=0x%08x, src=0x%08x, size=%u) done", dest, src, n);
    return dest;
}

int64_t h_GetEntryAddress(const Args& a) {
    /* GetEntryAddress(index): address of a kernel internal entry point, used
     * by libkernel patch shims. There is no real kernel here; hand back an
     * address in the kernel-reserved area that is plain zeroed RAM so a
     * shim that only stores it stays harmless. Fatal if ever jumped to
     * (outside the function table).
     *
     * The one caller on this disc is InitAlarm at 0x00100C90 (the name is
     * the disc's own link map, MAIN.MAP; the address is that map's and the
     * retail ELF's alike, libkernl being linked first). It
     * walks a static table of syscall numbers at 0x0028F470 and, for the six
     * entries past the first two, does SetSyscall(n, GetEntryAddress(n)).
     * The table is byte-identical to the US build's (US 0x00274E70), so the
     * six indices are the same on both: 0xFC, 0xFE, 0xFD, 0xFF, 0x12C, 0x08.
     * The first two entries of that table are installed without asking the
     * kernel: SetSyscall(0x5A, 0x00100C48) and SetSyscall(0x5B, 0x80076000).
     *
     * Logged once per distinct index rather than once overall, so the log
     * carries the whole set the guest asked for and what each one was
     * answered with. Six lines, at boot, and then nothing. */
    static uint32_t seen[8];
    static unsigned n_seen = 0;
    bool fresh = true;
    for (unsigned i = 0; i < n_seen; ++i) {
        if (seen[i] == a.a0) { fresh = false; break; }
    }
    if (fresh) {
        if (n_seen < sizeof(seen) / sizeof(seen[0])) seen[n_seen++] = a.a0;
        rt_log_warn("syscall",
            "GetEntryAddress(0x%x): NOT MODELED, the HLE kernel has no entry point for that index; "
            "guest asked for kernel entry 0x%x and was given 0x00080000 (zeroed kernel-reserved RAM). "
            "InitAlarm then does SetSyscall(0x%x, 0x00080000); the runtime dispatches syscall 0x%x "
            "from its own table, so the vector it records is never jumped to "
            "(InitAlarm, 0x00100C90)",
            a.a0, a.a0, a.a0, a.a0 & 0xFF);
    }
    return 0x00080000;
}

/* The OSD configuration word a console's kernel holds. Every field but the
 * two below is zero here, which is what this runtime has always reported;
 * nothing in this game reads any of them.
 *
 * The two that are not zero are the ones sceScfGetLanguage looks at, and the
 * bit positions are measured on the vendor library this build links rather
 * than carried from an SDK header. Verified in the retail ELF
 * (SCES_507.60): sceScfGetLanguage is at 0x00272958, which is the target of
 * the jal at 0x001B9614 named below; it calls this
 * syscall, takes `(word >> 13) & 7` as a version (`srl $v0,$v1,13` at
 * 0x0027298C), and returns
 * `(word >> 16) & 0x1F` as the language when that version is nonzero
 * (`andi $v0,$v0,0x1f` at 0x002729A8). A
 * version of zero sends it down the other branch, `(word >> 4) & 1`, which
 * is a one-bit Japanese/English field and cannot express the four other
 * languages this disc carries; that branch is what the zeroed word used to
 * take, and it is why every run reported the same language whatever the
 * player wanted.
 *
 * So the word carries version 1 and the language from settings.json
 * (host/settings.h RtLanguage, whose values are the OSD numbers this field
 * holds). Any nonzero version does: the threshold in the library is "not
 * zero", not "two or more". Three other functions in the same vendor object
 * (sceScfGetDateNotation, sceScfGetTimeNotation and sceScfGetSummerTime)
 * branch on a nonzero version straight to GetOsdConfigParam2 (syscall 0x6F),
 * so version 1 does not avoid them, and 0x6F is implemented below rather
 * than left as an unknown-syscall stop.
 *
 * Measured on the PAL ELF, not inferred: of those three, only
 * sceScfGetSummerTime has a caller at all, sceScfGetLocalTimefromRTC at
 * 0x002731C0 (jal at 0x002731D8), and that function has no caller itself, so
 * no reachable path in this build enters GetOsdConfigParam2. The one path
 * that does run is kanbanBootMcCheck -> sceScfGetLanguage (jal at
 * 0x001B9614) -> GetOsdConfigParam, twice, and the second result is the one
 * it keeps.
 *
 * This is a PAL-only surface. The same scan over the US ELF
 * (SCUS_971.13) finds zero callers of the GetOsdConfigParam stub at
 * 0x001005D0 and zero of GetOsdConfigParam2 at 0x00100830: the US build
 * never asks the kernel for its OSD configuration, and this game's language
 * therefore only follows the console setting on the PAL disc.
 *
 * Nothing the game supplied is changed by this. The game asks the machine it
 * is running on what its configured language is; this is that answer, and on
 * a console it comes from the OSD the player set. */
int64_t h_GetOsdConfigParam(const Args& a) {
    const int language = (int)rt_settings().system.language;
    const uint32_t word = rt_osd_config_word(language);
    /* PAL-only surface: named once, with where the fact was measured, then
     * the per-call line. The US build never reaches this syscall. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        rt_log_info("syscall",
            "PAL: the game asks the kernel for its OSD configuration. "
            "kanbanBootMcCheck calls sceScfGetLanguage (jal at 0x001B9614, target 0x00272958), "
            "which reads version at bits 13..15 (0x0027298C) and language at bits 16..20 "
            "(0x002729A8) of this word. "
            "The US build (SCUS_971.13) never calls this syscall");
    }
    rt_log_info("syscall", "GetOsdConfigParam(0x%08x): version 1, language %d -> 0x%08x",
        a.a0, language, word);
    rt_gwrite32(a.a0, word);
    return 0;
}

/* GetOsdConfigParam2(out, num, offset): copies `num` words of the console's
 * OSD configuration starting at word index `offset`. Word 1 is the one the
 * vendor library reads, and it reads it as a byte: sceScfGetSummerTime
 * calls it with
 * (sp+4, 1, 1) at 0x00272B88 (verified in the retail ELF: that word is
 * `jal 0x00100830`, the GetOsdConfigParam2 stub) and then takes
 * `(byte >> 4) & 1` as daylight
 * saving; sceScfGetTimeNotation (0x00272C08) takes `(byte >> 5) & 1`;
 * sceScfGetDateNotation (0x00272B08) takes `byte >> 6`.
 *
 * This kernel has no OSD, so there is no measured value for that word and
 * none is invented: every word asked for is written as zero and the call
 * says so. Zero reads back as daylight saving off, 24-hour time and date
 * notation 0. Nothing in this build reaches any of the three readers (see
 * the note above h_GetOsdConfigParam), so this exists to keep a dead vendor
 * path from becoming an unknown-syscall stop, and to be loud if it ever
 * stops being dead. */
int64_t h_GetOsdConfigParam2(const Args& a) {
    const uint32_t out = a.a0, num = a.a1, offset = a.a2;
    for (uint32_t i = 0; i < num; ++i) {
        if (!rt_gptr(out + i * 4)) {
            rt_fatal("syscall", a.ctx,
                "GetOsdConfigParam2(0x%08x, %u, %u): unmapped at word %u", out, num, offset, i);
        }
        rt_gwrite32(out + i * 4, 0);
    }
    rt_log_warn("syscall",
        "GetOsdConfigParam2(0x%08x, num=%u, offset=%u): NOT MODELED, this kernel has no OSD "
        "configuration word %u; guest asked for %u word(s) and was given %u zero word(s) "
        "(daylight saving 0, time notation 0, date notation 0). PAL-only path, statically dead in "
        "this build (its one caller is sceScfGetSummerTime, at 0x00272B88)",
        out, num, offset, offset, num, num);
    return 0;
}

int64_t h_GetCop0(const Args& a) { return (int64_t)(int32_t)rt_cop0_read(a.ctx, (int)a.a0); }
int64_t h_FlushCache(const Args&) { return 0; }

int64_t h_SifStopDma(const Args&) {
    rt_log_info("syscall", "SifStopDma: no-op");
    return 0;
}

int64_t h_GsGetIMR(const Args&) { return (int64_t)rt_gs_get_imr(); }
int64_t h_GsPutIMR(const Args& a) {
    rt_gs_put_imr(((uint64_t)a.a1 << 32) | a.a0);
    return 0;
}

int64_t h_SetVSyncFlag(const Args& a) {
    /* One-shot: the next vblank writes the flag word and disarms it. Arming
     * a second one while the first is still waiting drops the first, and
     * whatever was waiting on that word never sees it set, so say so. Once
     * per run: if it happens at all it is a shape worth one line, and if it
     * is normal for this binary the line says that too. */
    if (g_kern.vsync_flag_ptr && g_kern.vsync_flag_ptr != a.a0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            rt_log_warn("syscall", "SetVSyncFlag(0x%08x, 0x%08x) replaces an armed flag at "
                "0x%08x that no vblank has written yet; the earlier one is dropped and nothing "
                "will ever set it",
                a.a0, a.a1, g_kern.vsync_flag_ptr);
        }
    }
    g_kern.vsync_flag_ptr = a.a0;
    g_kern.vsync_csr_ptr = a.a1;
    if (a.a0 && rt_gptr(a.a0)) rt_gwrite32(a.a0, 0);
    return 0;
}

uint32_t g_set_syscall_logged = 0;
int64_t h_SetSyscall(const Args& a) {
    /* The vector is not stored. This runtime dispatches every syscall from
     * its own table below, so a recorded vector could only ever be printed,
     * and the line right here prints it. (There used to be a 256-entry
     * shadow array that nothing read.)
     *
     * Same channel and cap as the generic trace line for this syscall
     * (kSyscallTraceCap): the first few by default, all of them when the
     * "syscall" verbose channel is on. */
    if (log_capped("syscall", &g_set_syscall_logged, kSyscallTraceCap)) {
        rt_log_info("syscall", "SetSyscall(num=0x%x, vector=0x%08x) recorded", a.a0, a.a1);
    }
    return 0;
}

int64_t h_SifDmaStat(const Args& a) { return rt_sif_dma_stat(a.a0); }
int64_t h_SifSetDma(const Args& a) {
    int64_t id = rt_sif_set_dma(a.a0, a.a1);
    /* Let the virtual SIF latency elapse so the deferred IOP response (if
     * one was scheduled) is deliverable at this syscall boundary. Without
     * this, an interrupt-less guest poll loop on plain RAM would never see
     * it.
     *
     * 8192 cycles (about 27 us of virtual time) is CHOSEN, not measured: it
     * only has to be at least kSifLatency (sif/rpc.cpp) so that one call
     * carries a queued response past its due time. No cost of a real
     * sceSifSetDma has been measured for this port. It is not billed to a
     * g_cyc_* bucket, so the profile summary's "vclk source" split covers
     * backedges, MMIO and idle and not these syscall ticks. */
    rt_clock_tick(8192);
    return id;
}
int64_t h_SifSetDChain(const Args&) {
    if (!g_dchain_logged) {
        rt_log_info("syscall", "SifSetDChain: accepted (receive chain is implicit in the SIF HLE)");
        g_dchain_logged = true;
    }
    return 0;
}
int64_t h_SifSetReg(const Args& a) {
    if (rt_trace()) rt_log_info("sif", "SifSetReg(0x%08x, 0x%08x)", a.a0, a.a1);
    rt_sif_set_reg(a.a0, a.a1);
    return 0;
}
int64_t h_SifGetReg(const Args& a) {
    uint32_t v = rt_sif_get_reg(a.a0);
    if (rt_trace()) rt_log_debug("sif", "SifGetReg(0x%08x) -> 0x%08x", a.a0, v);
    return (int64_t)(int32_t)v;
}

/* ---- DECI2 manager HLE --------------------------------------------------
 *
 * The kernel's Deci2Call syscall (0x7C) backs the SDK TTY output path
 * (scePrintf). Protocol facts (SDK deci2.h, plus the vendor handler's
 * observed contract): sceDeci2Open(proto, opt, handler) registers an event
 * handler; sceDeci2ReqSend arms a send; Deci2Poll drives the manager, which
 * calls handler(DECI2_WRITE=3, 0, opt) so the handler feeds data via
 * sceDeci2ExSend (Deci2Call -0x6, args {sock, addr, len}, returns bytes
 * taken), then handler(DECI2_WRITEDONE=4, 0, opt), which clears the
 * library's busy flag. The sent DECI2 packet is a 12-byte header + text;
 * the text is logged as guest TTY output. */

struct Deci2Sock {
    bool open = false;
    uint32_t proto = 0, opt = 0, handler = 0, gp = 0;
    bool send_pending = false;
};
constexpr uint32_t kMaxDeci2Socks = 8;
Deci2Sock g_deci2[kMaxDeci2Socks]; /* index == socket id, 0 unused */

/* Dedicated context + guest stack for kernel-invoked guest callbacks (the
 * DECI2 handler). Separate from the interrupt context/stack: a nested
 * rt_intc_deliver at the callback's own syscall boundaries may clobber the
 * interrupt context. Stack sits in the kernel-reserved low RAM below the
 * interrupt stack (0xE0000). */
constexpr uint32_t kKcallStackTop = 0x000D0000u;
R5900Context g_kcall_ctx;

uint32_t kcall_guest3(uint32_t vram, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t gp) {
    if (vram < RECOMP_TEXT_BASE || vram >= RECOMP_TEXT_LIMIT || !g_functab[RECOMP_FUNC_IDX(vram)]) {
        rt_fatal("syscall", nullptr, "guest callback vram 0x%08x has no translation", vram);
    }
    std::memset(&g_kcall_ctx, 0, sizeof(g_kcall_ctx));
    g_kcall_ctx.r[4].u64x[0] = a0;
    g_kcall_ctx.r[5].u64x[0] = a1;
    g_kcall_ctx.r[6].u64x[0] = a2;
    g_kcall_ctx.r[28].u64x[0] = gp;
    g_kcall_ctx.r[29].u64x[0] = kKcallStackTop;
    g_kcall_ctx.r[31].u64x[0] = RT_CLEAN_EXIT_VRAM;
    g_functab[RECOMP_FUNC_IDX(vram)](&g_kcall_ctx);
    return (uint32_t)g_kcall_ctx.r[2].u64x[0];
}

void deci2_log_text(const uint8_t* data, uint32_t len) {
    /* Split on newlines; drop CR; print printable text via the tty tag. */
    static char line[512];
    static size_t fill = 0;
    for (uint32_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n' || fill >= sizeof(line) - 1) {
            line[fill] = 0;
            if (fill) rt_log_info("tty", "%s", line);
            fill = 0;
            if (c != '\n' && fill < sizeof(line) - 1) line[fill++] = c;
            continue;
        }
        line[fill++] = c;
    }
}

int64_t h_Deci2Call(const Args& a) {
    int32_t cmd = (int32_t)a.a0;
    switch (cmd) {
        case 0x01: { /* sceDeci2Open(proto, opt, handler): a1 -> args block */
            uint32_t proto = rt_gread32(a.a1 + 0);
            uint32_t opt = rt_gread32(a.a1 + 4);
            uint32_t handler = rt_gread32(a.a1 + 8);
            for (uint32_t i = 1; i < kMaxDeci2Socks; ++i) {
                if (!g_deci2[i].open) {
                    g_deci2[i] = Deci2Sock{true, proto, opt, handler,
                                           (uint32_t)a.ctx->r[28].u64x[0], false};
                    rt_log_info("syscall", "Deci2Open: sock=%u proto=0x%x opt=0x%08x handler=0x%08x",
                        i, proto, opt, handler);
                    return (int64_t)i;
                }
            }
            return -1;
        }
        case 0x02: { /* sceDeci2Close(sock) */
            uint32_t s = rt_gread32(a.a1 + 0);
            if (s < kMaxDeci2Socks) g_deci2[s].open = false;
            return 0;
        }
        case 0x03: { /* sceDeci2ReqSend(sock, dest): arm; poll drives it */
            uint32_t s = rt_gread32(a.a1 + 0);
            if (s < kMaxDeci2Socks && g_deci2[s].open) {
                g_deci2[s].send_pending = true;
                return 0;
            }
            return -1;
        }
        case 0x04: { /* sceDeci2Poll(sock): run the manager for this socket */
            uint32_t s = rt_gread32(a.a1 + 0);
            if (s < kMaxDeci2Socks && g_deci2[s].open && g_deci2[s].send_pending) {
                Deci2Sock& d = g_deci2[s];
                d.send_pending = false;
                kcall_guest3(d.handler, 3 /* DECI2_WRITE */, 0, d.opt, d.gp);
                kcall_guest3(d.handler, 4 /* DECI2_WRITEDONE */, 0, d.opt, d.gp);
            }
            return 0;
        }
        case -0x06: case 0x06: { /* sceDeci2ExSend(sock, addr, len) */
            uint32_t addr = rt_gread32(a.a1 + 4);
            uint32_t len = rt_gread32(a.a1 + 8) & 0xFFFF;
            if (len && rt_gptr(addr)) {
                const uint8_t* p = rt_gptr(addr);
                uint32_t skip = 0;
                /* First chunk of a packet carries the 12-byte DECI2 header
                 * (u16 total length first); strip it from the TTY log. */
                if (len > 12) {
                    uint16_t hdr_len;
                    std::memcpy(&hdr_len, p, 2);
                    if (hdr_len == len) skip = 12;
                }
                deci2_log_text(p + skip, len - skip);
            }
            return (int64_t)len; /* everything taken */
        }
        case -0x05: case 0x05: /* sceDeci2ExRecv: no inbound DECI2 traffic */
            return -1;
        default: {
            static bool warned = false;
            if (!warned) {
                rt_log_warn("syscall", "WARNING: Deci2Call(cmd=0x%x) NOT MODELED, returning 0", cmd);
                warned = true;
            }
            return 0;
        }
    }
}

/* MachineType: 0 is what this kernel has always answered. It is a
 * PLACEHOLDER, not measured: no retail console's answer has been read for
 * this port, and nothing in SCES_507.60 is known to call the syscall (it is
 * in the table so that a call is not an unknown-syscall stop). */
int64_t h_MachineType(const Args&) { return 0; }
int64_t h_GetMemorySize(const Args&) { return (int64_t)RT_RAM_SIZE; }

/* ---- table -------------------------------------------------------------- */

using Handler = int64_t (*)(const Args&);

struct Entry {
    const char* name = nullptr;
    Handler fn = nullptr;
};

struct Table {
    Entry e[256];
    Table() {
        auto set = [this](uint8_t key, const char* name, Handler fn) {
            e[key] = Entry{name, fn};
        };
        set(0x01, "ResetEE", h_ResetEE);
        set(0x02, "SetGsCrt", h_SetGsCrt);
        set(0x04, "Exit", h_Exit);
        set(0x10, "AddIntcHandler", h_AddIntcHandler);
        set(0x11, "RemoveIntcHandler", h_RemoveIntcHandler);
        set(0x12, "AddDmacHandler", h_AddDmacHandler);
        set(0x13, "RemoveDmacHandler", h_RemoveDmacHandler);
        set(0x14, "_EnableIntc", h_EnableIntc);
        set(0x15, "_DisableIntc", h_DisableIntc);
        set(0x16, "_EnableDmac", h_EnableDmac);
        set(0x17, "_DisableDmac", h_DisableDmac);
        set(0x18, "_SetAlarm", h_SetAlarm);
        set(0x19, "_ReleaseAlarm", h_ReleaseAlarm);
        set(0x1A, "_iEnableIntc", h_EnableIntc);
        set(0x1B, "_iDisableIntc", h_DisableIntc);
        set(0x1C, "_iEnableDmac", h_EnableDmac);
        set(0x1D, "_iDisableDmac", h_DisableDmac);
        set(0x1E, "_iSetAlarm", h_SetAlarm);
        set(0x1F, "_iReleaseAlarm", h_ReleaseAlarm);
        set(0x20, "CreateThread", h_CreateThread);
        set(0x21, "DeleteThread", h_DeleteThread);
        set(0x22, "StartThread", h_StartThread);
        set(0x23, "ExitThread", h_ExitThread);
        set(0x24, "ExitDeleteThread", h_ExitDeleteThread);
        set(0x25, "TerminateThread", h_TerminateThread);
        set(0x26, "iTerminateThread", h_TerminateThread);
        set(0x29, "ChangeThreadPriority", h_ChangeThreadPriority);
        set(0x2A, "iChangeThreadPriority", h_ChangeThreadPriority);
        set(0x2B, "RotateThreadReadyQueue", h_RotateThreadReadyQueue);
        set(0x2C, "iRotateThreadReadyQueue", h_RotateThreadReadyQueue);
        set(0x2D, "ReleaseWaitThread", h_ReleaseWaitThread);
        set(0x2E, "iReleaseWaitThread", h_ReleaseWaitThread);
        set(0x2F, "GetThreadId", h_GetThreadId);
        set(0x30, "ReferThreadStatus", h_ReferThreadStatus);
        set(0x31, "iReferThreadStatus", h_ReferThreadStatus);
        set(0x32, "SleepThread", h_SleepThread);
        set(0x33, "WakeupThread", h_WakeupThread);
        set(0x34, "iWakeupThread", h_WakeupThread);
        set(0x35, "CancelWakeupThread", h_CancelWakeupThread);
        set(0x36, "iCancelWakeupThread", h_CancelWakeupThread);
        set(0x37, "SuspendThread", h_SuspendThread);
        set(0x38, "iSuspendThread", h_SuspendThread);
        set(0x39, "ResumeThread", h_ResumeThread);
        set(0x3A, "iResumeThread", h_ResumeThread);
        set(0x3C, "RFU060:SetupThread", h_SetupThread);
        set(0x3D, "RFU061:SetupHeap", h_SetupHeap);
        set(0x3E, "EndOfHeap", h_EndOfHeap);
        set(0x40, "CreateSema", h_CreateSema);
        set(0x41, "DeleteSema", h_DeleteSema);
        set(0x42, "SignalSema", h_SignalSema);
        set(0x43, "iSignalSema", h_SignalSema);
        set(0x44, "WaitSema", h_WaitSema);
        set(0x45, "PollSema", h_PollSema);
        set(0x46, "iPollSema", h_PollSema);
        set(0x47, "ReferSemaStatus", h_ReferSemaStatus);
        set(0x48, "iReferSemaStatus", h_ReferSemaStatus);
        set(0x49, "iDeleteSema", h_DeleteSema);
        set(0x4B, "GetOsdConfigParam", h_GetOsdConfigParam);
        set(0x5A, "Copy", h_Copy);
        set(0x5B, "GetEntryAddress", h_GetEntryAddress);
        set(0x63, "GetCop0", h_GetCop0);
        set(0x64, "FlushCache", h_FlushCache);
        set(0x67, "iGetCop0", h_GetCop0);
        set(0x68, "iFlushCache", h_FlushCache);
        set(0x6B, "SifStopDma", h_SifStopDma);
        /* PAL-only: the US build never calls the GetOsdConfigParam2 stub at
         * 0x00100830, the PAL build links three vendor readers that do. */
        set(0x6F, "GetOsdConfigParam2", h_GetOsdConfigParam2);
        set(0x70, "GsGetIMR", h_GsGetIMR);
        set(0x71, "GsPutIMR", h_GsPutIMR);
        set(0x73, "SetVSyncFlag", h_SetVSyncFlag);
        set(0x74, "SetSyscall", h_SetSyscall);
        set(0x76, "SifDmaStat", h_SifDmaStat);
        set(0x77, "SifSetDma", h_SifSetDma);
        set(0x78, "SifSetDChain", h_SifSetDChain);
        set(0x79, "SifSetReg", h_SifSetReg);
        set(0x7A, "SifGetReg", h_SifGetReg);
        set(0x7C, "Deci2Call", h_Deci2Call);
        set(0x7E, "MachineType", h_MachineType);
        set(0x7F, "GetMemorySize", h_GetMemorySize);
        set(0xFC, "SetAlarm", h_SetAlarm);
        set(0xFD, "iSetAlarm", h_SetAlarm);
        set(0xFE, "ReleaseAlarm", h_ReleaseAlarm);
        set(0xFF, "iReleaseAlarm", h_ReleaseAlarm);
    }
};

const Table g_table;

/* Once-per-distinct-signature logging with power-of-two fold. The
 * signature key is (syscall num, a0) only: a0 is the thread id or
 * semaphore id for every thread and sema syscall, the one argument that
 * actually distinguishes one caller from another. a1..a3 are left out of
 * the key on purpose, they carry whatever the caller left in those
 * registers (loop counters, stack pointers) and differ on almost every
 * call, which used to make nearly every call its own signature. The
 * register values are still printed in the log line itself.
 *
 * The table is a fixed-size open-addressed set so logging never
 * allocates and never grows unbounded on this hot path. The probe is
 * bounded as well as the table: an unbounded linear probe would walk all
 * kSigTableSize slots on every syscall once the table filled, which is a
 * worse hot path than the map this replaced. A signature that finds
 * neither an empty nor a matching slot within kSigProbeLimit falls back to
 * a plain per-num counter, and that fallback is noted once per number. */
constexpr size_t kSigTableSize = 1024;
constexpr size_t kSigProbeLimit = 16;
struct SigSlot {
    uint64_t key = 0;   // 0 means empty; a live key is never allowed to hash to 0
    uint64_t count = 0;
};
SigSlot g_sig_table[kSigTableSize];
uint64_t g_sig_fallback_counts[256];
bool g_sig_fallback_warned[256];

/* Some syscalls are called constantly with an a0 that differs almost every
 * time (WakeupThread/iWakeupThread take the woken thread's id, SifSetDma an
 * entry count, SleepThread none but still gets called every field), so the
 * per-signature table above never folds them down: each call looks like a
 * fresh signature and the power-of-two sampling barely bites. These six
 * names were measured to dominate a run's default log this way; capped at
 * kSyscallTraceCap occurrences of each name (not signature) in the default
 * channel. rt_verbose("syscall") keeps every one, exactly like the rest of
 * this function's sampling. Identified by syscall table key since
 * should_log sees only (num, a0), not the resolved Entry::name. */
bool is_syscall_trace_capped(uint32_t key) {
    switch (key) {
    case 0x32: /* SleepThread */
    case 0x33: /* WakeupThread */
    case 0x34: /* iWakeupThread */
    case 0x74: /* SetSyscall */
    case 0x76: /* SifDmaStat */
    case 0x77: /* SifSetDma */
        return true;
    default:
        return false;
    }
}
uint32_t g_syscall_trace_capped_counts[256];

bool should_log(int32_t num, uint32_t a0) {
    if (rt_trace()) return true;
    /* Masked the same way rt_syscall picks the table entry, so a number
     * that is dispatched and named as one of the six is capped as one of
     * the six. */
    const uint32_t key = (uint32_t)(num < 0 ? -num : num) & 0xFF;
    if (is_syscall_trace_capped(key)) {
        return log_capped("syscall", &g_syscall_trace_capped_counts[key], kSyscallTraceCap);
    }
    uint64_t h = (uint64_t)(uint32_t)num;
    h = h * 0x9E3779B97F4A7C15ull ^ a0;
    h ^= h >> 32;
    if (h == 0) h = 1;
    size_t start = (size_t)(h % kSigTableSize);
    for (size_t probe = 0; probe < kSigProbeLimit; ++probe) {
        SigSlot& slot = g_sig_table[(start + probe) % kSigTableSize];
        if (slot.key == 0 || slot.key == h) {
            slot.key = h;
            ++slot.count;
            return (slot.count & (slot.count - 1)) == 0;
        }
    }
    uint8_t fkey = (uint8_t)(uint32_t)(num < 0 ? -num : num);
    if (!g_sig_fallback_warned[fkey]) {
        g_sig_fallback_warned[fkey] = true;
        rt_log_warn("syscall", "signature table crowded (%zu entries, %zu probes), syscall %d falls back "
                          "to a per-num counter",
            kSigTableSize, kSigProbeLimit, num);
    }
    uint64_t& n = g_sig_fallback_counts[fkey];
    ++n;
    return (n & (n - 1)) == 0;
}

} // namespace

void rt_syscall(R5900Context* ctx) {
    /* The one zone that can be open across a coroutine yield: WaitSema
     * and SleepThread block here. See the caveat in prof.h. */
    RT_PROF_ZONE(RT_PROF_SYSCALL);
    ++g_syscall_count;
    int32_t num = ctx->r[3].s32x[0];
    uint32_t key = (uint32_t)(num < 0 ? -num : num) & 0xFF;
    Args a{
        ctx,
        (uint32_t)ctx->r[4].u64x[0], (uint32_t)ctx->r[5].u64x[0],
        (uint32_t)ctx->r[6].u64x[0], (uint32_t)ctx->r[7].u64x[0],
        (uint32_t)ctx->r[8].u64x[0],
        num < 0,
    };
    const Entry& e = g_table.e[key];
    if (!e.fn) {
        rt_fatal("syscall", ctx,
            "unknown syscall number %d (0x%x, table key 0x%02x), a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%08x",
            num, (uint32_t)num, key, a.a0, a.a1, a.a2, a.a3, (uint32_t)ctx->r[31].u64x[0]);
    }
    if (should_log(num, a.a0)) {
        rt_log_debug("syscall", "%s%s(a0=0x%x, a1=0x%x, a2=0x%x, a3=0x%x) thread=%d ra=0x%08x%s",
            a.ivariant && e.name[0] != 'i' && e.name[0] != '_' ? "i:" : "", e.name,
            a.a0, a.a1, a.a2, a.a3, rt_thread_current_id(), (uint32_t)ctx->r[31].u64x[0],
            rt_in_interrupt() ? " [in-interrupt]" : "");
    }
    /* The last syscall the end-of-run summary and the field watchdog
     * report (host/run_state.cpp). Recorded before the handler runs, not
     * after, so a syscall that never returns (a fatal inside it, a fault) is
     * the one named. e.name is a table entry with static lifetime, which is
     * what makes storing the pointer rather than a copy safe. */
    rt_run_note_syscall(num, e.name);
    /* The same fact per guest thread, for the inventory: the run-state
     * version is the last syscall of the whole run, which on a run with a
     * dozen threads says nothing about which of them is stuck. */
    rt_sched_note_syscall(num, e.name);
    /* Every syscall advances the virtual clock a little, so a guest loop
     * that only ever calls the kernel still reaches the next timeline
     * event. 256 cycles (about 0.9 us) is CHOSEN, not measured: no retail
     * syscall entry cost has been timed for this port, and the number only
     * has to be small against a field and nonzero. Like the SifSetDma tick
     * above it is not billed to a g_cyc_* bucket, so the profile summary's
     * "vclk source" percentages describe backedges, MMIO and idle only. */
    rt_clock_tick(256);
    int64_t v0 = e.fn(a);
    set_v0(ctx, v0);
    rt_intc_deliver();
}
