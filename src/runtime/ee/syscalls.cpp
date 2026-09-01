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
 * register dump. Every distinct (number, args) signature is logged once,
 * then on power-of-two repeats. ICORECOMP_TRACE=1 logs every call.
 */
#include "kernel.h"

#include "../hw/hw.h"
#include "../prof.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <unordered_map>

namespace {

struct Args {
    R5900Context* ctx;
    uint32_t a0, a1, a2, a3, t0;
    bool ivariant;
};

uint64_t g_syscall_count = 0;
uint32_t g_set_syscall_vec[256];
bool g_dchain_logged = false;

void set_v0(R5900Context* ctx, int64_t v) {
    ctx->r[2].s64x[0] = v;
    ctx->r[2].u64x[1] = 0;
}

/* ---- handlers ----------------------------------------------------------- */

int64_t h_ResetEE(const Args& a) {
    rt_log("syscall", "ResetEE(0x%x): no-op (no devices to reset)", a.a0);
    return 0;
}

int64_t h_SetGsCrt(const Args& a) {
    g_kern.gscrt_interlace = a.a0;
    g_kern.gscrt_mode = a.a1;
    g_kern.gscrt_ffmd = a.a2;
    g_kern.gscrt_set = true;
    rt_log("syscall", "SetGsCrt(interlace=%u, mode=0x%x, ffmd=%u) recorded", a.a0, a.a1, a.a2);
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

int64_t h_SetAlarm(const Args& a) {
    static bool warned = false;
    if (!warned) {
        rt_log("syscall", "WARNING: SetAlarm(time=%u, handler=0x%08x, arg=0x%08x): alarms are "
            "recorded but never fire in P2; revisit if a library parks on one", a.a0, a.a1, a.a2);
        warned = true;
    }
    return 1; /* alarm id */
}
int64_t h_ReleaseAlarm(const Args&) { return 0; }

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
    rt_log("sched", "CreateThread: id=%d entry=0x%08x stack=0x%08x+0x%x gp=0x%08x prio=%d",
        id, func, stack, stack_size, gp, prio);
    return id;
}
int64_t h_DeleteThread(const Args& a) { return rt_thread_delete((int)a.a0); }
int64_t h_StartThread(const Args& a) {
    rt_log("sched", "StartThread: id=%d arg=0x%08x", (int)a.a0, a.a1);
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
    rt_log("syscall", "RFU060/SetupThread: gp=0x%08x stack=0x%08x+0x%x args=0x%08x root=0x%08x -> sp=0x%08x",
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
    rt_log("syscall", "RFU061/SetupHeap: base=0x%08x size=0x%x -> heap end 0x%08x",
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
    rt_log("sched", "CreateSema: id=%d init=%d max=%d (thread %d, ra=0x%08x)",
        id, init_count, max_count, rt_thread_current_id(), (uint32_t)a.ctx->r[31].u64x[0]);
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
     * SetSyscall vectors at it; all of that RAM is mapped, so just copy. */
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
    rt_log("syscall", "Copy(dest=0x%08x, src=0x%08x, size=%u) done", dest, src, n);
    return dest;
}

int64_t h_GetEntryAddress(const Args& a) {
    /* GetEntryAddress(index): address of a kernel internal entry point, used
     * by libkernel patch shims. There is no real kernel here; hand back an
     * address in the kernel-reserved area that is plain zeroed RAM so a
     * shim that only stores it stays harmless. Fatal if ever jumped to
     * (outside the function table). */
    static bool warned = false;
    if (!warned) {
        rt_log("syscall", "WARNING: GetEntryAddress(%u): HLE kernel has no real entry points; "
            "returning dummy 0x00080000", a.a0);
        warned = true;
    }
    return 0x00080000;
}

int64_t h_GetOsdConfigParam(const Args& a) {
    rt_log("syscall", "GetOsdConfigParam(0x%08x): returning zeroed defaults", a.a0);
    rt_gwrite32(a.a0, 0);
    return 0;
}

int64_t h_GetCop0(const Args& a) { return (int64_t)(int32_t)rt_cop0_read(a.ctx, (int)a.a0); }
int64_t h_FlushCache(const Args&) { return 0; }

int64_t h_SifStopDma(const Args&) {
    rt_log("syscall", "SifStopDma: no-op");
    return 0;
}

int64_t h_GsGetIMR(const Args&) { return (int64_t)rt_gs_get_imr(); }
int64_t h_GsPutIMR(const Args& a) {
    rt_gs_put_imr(((uint64_t)a.a1 << 32) | a.a0);
    return 0;
}

int64_t h_SetVSyncFlag(const Args& a) {
    g_kern.vsync_flag_ptr = a.a0;
    g_kern.vsync_csr_ptr = a.a1;
    if (a.a0 && rt_gptr(a.a0)) rt_gwrite32(a.a0, 0);
    return 0;
}

int64_t h_SetSyscall(const Args& a) {
    g_set_syscall_vec[a.a0 & 0xFF] = a.a1;
    rt_log("syscall", "SetSyscall(num=0x%x, vector=0x%08x) recorded", a.a0, a.a1);
    return 0;
}

int64_t h_SifDmaStat(const Args& a) { return rt_sif_dma_stat(a.a0); }
int64_t h_SifSetDma(const Args& a) {
    int64_t id = rt_sif_set_dma(a.a0, a.a1);
    /* Let the virtual SIF latency elapse so the deferred IOP response (if
     * one was scheduled) is deliverable at this syscall boundary. Without
     * this, an interrupt-less guest poll loop on plain RAM would never see
     * it. */
    rt_clock_tick(8192);
    return id;
}
int64_t h_SifSetDChain(const Args&) {
    if (!g_dchain_logged) {
        rt_log("syscall", "SifSetDChain: accepted (receive chain is implicit in the SIF HLE)");
        g_dchain_logged = true;
    }
    return 0;
}
int64_t h_SifSetReg(const Args& a) {
    if (rt_trace()) rt_log("sif", "SifSetReg(0x%08x, 0x%08x)", a.a0, a.a1);
    rt_sif_set_reg(a.a0, a.a1);
    return 0;
}
int64_t h_SifGetReg(const Args& a) {
    uint32_t v = rt_sif_get_reg(a.a0);
    if (rt_trace()) rt_log("sif", "SifGetReg(0x%08x) -> 0x%08x", a.a0, v);
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
            if (fill) rt_log("tty", "%s", line);
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
                    rt_log("syscall", "Deci2Open: sock=%u proto=0x%x opt=0x%08x handler=0x%08x",
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
                rt_log("syscall", "WARNING: Deci2Call(cmd=0x%x) NOT MODELED, returning 0", cmd);
                warned = true;
            }
            return 0;
        }
    }
}

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

/* Once-per-distinct-signature logging with power-of-two fold. */
std::unordered_map<uint64_t, uint64_t> g_sig_counts;

bool should_log(int32_t num, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (rt_trace()) return true;
    uint64_t h = (uint64_t)(uint32_t)num;
    h = h * 0x9E3779B97F4A7C15ull ^ a0;
    h = h * 0x9E3779B97F4A7C15ull ^ a1;
    h = h * 0x9E3779B97F4A7C15ull ^ a2;
    h = h * 0x9E3779B97F4A7C15ull ^ a3;
    uint64_t& n = g_sig_counts[h];
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
    if (should_log(num, a.a0, a.a1, a.a2, a.a3)) {
        rt_log("syscall", "%s%s(a0=0x%x, a1=0x%x, a2=0x%x, a3=0x%x) thread=%d ra=0x%08x%s",
            a.ivariant && e.name[0] != 'i' && e.name[0] != '_' ? "i:" : "", e.name,
            a.a0, a.a1, a.a2, a.a3, rt_thread_current_id(), (uint32_t)ctx->r[31].u64x[0],
            rt_in_interrupt() ? " [in-interrupt]" : "");
    }
    rt_clock_tick(256);
    int64_t v0 = e.fn(a);
    set_v0(ctx, v0);
    rt_intc_deliver();
}
