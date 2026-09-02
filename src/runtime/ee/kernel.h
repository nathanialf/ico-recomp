/* ee/kernel.h: internal declarations for the P2 EE kernel HLE.
 *
 * Modules:
 *   sched.cpp    threads, semaphores, coroutine scheduler, virtual clock,
 *                vblank timeline
 *   syscalls.cpp rt_syscall dispatch (numbers per ps2sdk
 *                ee/kernel/include/syscallnr.h, a clean-room structural
 *                reference)
 *   intc.cpp     INTC/DMAC handler registries + delivery, GS CSR/IMR shadow
 *   timers.cpp   EE timers T0-T3 driven by the virtual clock
 *   alarms.cpp   EE kernel alarms (SetAlarm/ReleaseAlarm) on the same clock
 *   ../sif/sif.cpp SIF register file, SifSetDma recording, entry into the
 *                RPC layer
 *   ../sif/rpc.cpp sifrpc protocol HLE (virtual IOP RAM, service registry,
 *                deferred response delivery)
 *   ../sif/cdvd.cpp cdvdman services, iopheap, loadfile, padman, boot stubs
 *   ../iso/iso9660.cpp disc image backend
 *
 * This is runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_EE_KERNEL_H
#define ICORECOMP_EE_KERNEL_H

#include "runtime.h"

#include <cstring>

/* ---- guest memory helpers ----------------------------------------------- */

static inline uint8_t* rt_gptr(uint32_t a) {
    uint8_t* p = g_pages[a >> 16];
    return p ? p + (a & 0xFFFFu) : nullptr;
}
uint32_t rt_gread32(uint32_t a);            /* fatal on unmapped address */
void rt_gwrite32(uint32_t a, uint32_t v);   /* fatal on unmapped address */

/* ---- tracing ------------------------------------------------------------ */

bool rt_trace(); /* ICORECOMP_TRACE=1: full-volume logging */

/* ---- virtual clock (sched.cpp) ------------------------------------------ */

/* Unit: EE bus cycles (BUSCLK = 147.456 MHz; the CPU core runs at 2x). */
constexpr uint64_t RT_BUSCLK_HZ = 147456000ull;
/* NTSC field rate 59.94 Hz: 147456000 / 59.94 = 2460060 bus cycles/field. */
constexpr uint64_t RT_CYCLES_PER_FIELD = 2460060ull;
/* Vertical blank ~22 lines of a 262.5-line field. */
constexpr uint64_t RT_CYCLES_VBLANK = 206184ull;
/* One NTSC H-blank in bus cycles. Both the timers' HBLNK prescale and the
 * EE kernel's alarm clock count these. Derived from the field timeline
 * (262.5 lines per field): 2460060 * 2 / 525 = 9371 cycles, so
 * 147456000 / 9371 = 15735.7 Hz against the NTSC line rate of 15734.26 Hz,
 * the difference being the rounding of the field length. Deriving it from
 * the field rather than from 147456000 / 15734.26 = 9372 keeps one clock
 * authority: the alarm clock, the H-blank timers and the vblank timeline
 * all count the same line. */
constexpr uint64_t RT_CYCLES_PER_HBLANK = RT_CYCLES_PER_FIELD * 2 / 525;

uint64_t rt_clock_now();
/* Advances virtual time; raises due timeline events (vblank INTC bits, timer
 * flags, deferred SIF responses) but does NOT dispatch guest handlers. Call
 * rt_intc_deliver() at a safe point afterwards. */
void rt_clock_tick(uint64_t cycles);

/* Per-module timeline contributions (absolute cycle, UINT64_MAX = none). */
uint64_t rt_timers_next_event();
void rt_timers_run_due();
uint64_t rt_sif_next_event();
void rt_sif_run_due();
uint64_t rt_alarms_next_event();
void rt_alarms_run_due();

/* Called by mmio.cpp on every MMIO access: small clock advance so guest
 * poll loops make time progress, then pending-interrupt delivery. */
void rt_kernel_mmio_tick();

/* ---- scheduler (sched.cpp) ---------------------------------------------- */

void rt_sched_init();
[[noreturn]] void rt_sched_boot(uint32_t entry_vram, uint32_t gp, uint32_t sp);
R5900Context* rt_sched_current_ctx(); /* may be null (scheduler context) */
int rt_thread_current_id();
void rt_sched_maybe_preempt();
void rt_sched_dump_inventory(const char* why);
[[noreturn]] void rt_sched_exit_game(int code, const char* why);

/* Thread ops. id 0 = current thread where the kernel accepts that. Return
 * value conventions follow the EE kernel (id on success, negative error). */
int rt_thread_create(uint32_t entry, uint32_t stack, uint32_t stack_size,
                     uint32_t gp, int prio, uint32_t attr, uint32_t option);
int rt_thread_delete(int id);
int rt_thread_start(int id, uint32_t arg);
void rt_thread_exit_current(bool and_delete); /* does not return to caller */
int rt_thread_terminate(int id);
int rt_thread_change_priority(int id, int prio, bool from_int);
int rt_thread_rotate_ready_queue(int prio, bool from_int);
int rt_thread_getid();
int rt_thread_refer(int id, uint32_t out_guest_ptr);
int rt_thread_sleep();                      /* may block (yield) */
int rt_thread_wakeup(int id, bool from_int);
int rt_thread_cancel_wakeup(int id);
int rt_thread_suspend(int id);
int rt_thread_resume(int id);
int rt_thread_release_wait(int id, bool from_int);
/* RFU060: crt0 re-declares the main thread's gp/stack; update thread 1. */
void rt_thread_setup_main(uint32_t gp, uint32_t stack_base, uint32_t stack_size);

int rt_sema_create(int init_count, int max_count, uint32_t attr, uint32_t option);
int rt_sema_delete(int id);
int rt_sema_signal(int id, bool from_int);
int rt_sema_wait(int id);                   /* may block (yield) */
int rt_sema_poll(int id);
int rt_sema_refer(int id, uint32_t out_guest_ptr);

/* Recorded one-shot kernel state (RFU060/RFU061/SetGsCrt...). */
struct EEKernelState {
    uint32_t main_gp = 0;
    uint32_t stack_base = 0;
    uint32_t stack_size = 0;
    uint32_t args_ptr = 0;
    uint32_t root_func = 0;
    uint32_t heap_base = 0;
    uint32_t heap_end = 0;
    uint32_t gscrt_interlace = 0, gscrt_mode = 0, gscrt_ffmd = 0;
    bool gscrt_set = false;
    /* SetVSyncFlag one-shot pointers (0 = unarmed). */
    uint32_t vsync_flag_ptr = 0, vsync_csr_ptr = 0;
};
extern EEKernelState g_kern;

/* ---- INTC / DMAC / GS shadow (intc.cpp) --------------------------------- */

/* EE INTC cause bits (public hardware facts, ps2tek "EE interrupts"):
 * 0 GS, 1 SBUS, 2 VB_ON (vblank start), 3 VB_OFF (vblank end), 4 VIF0,
 * 5 VIF1, 6 VU0, 7 VU1, 8 IPU, 9-12 TIMER0-3, 13 SFIFO, 14 VU0WD. */
constexpr int RT_INTC_GS = 0;
constexpr int RT_INTC_VB_ON = 2;
constexpr int RT_INTC_VB_OFF = 3;
constexpr int RT_INTC_TIMER0 = 9;

constexpr int RT_DMAC_SIF0 = 5; /* IOP -> EE */
constexpr int RT_DMAC_SIF1 = 6; /* EE -> IOP */

void rt_intc_init();
void rt_intc_set_eie(bool on);
bool rt_intc_get_eie();
uint32_t rt_cop0_status_word();
bool rt_in_interrupt();

int rt_intc_add_handler(int cause, uint32_t vram, int next, uint32_t gp);
int rt_intc_remove_handler(int cause, int hid);
int rt_intc_enable(int cause);
int rt_intc_disable(int cause);
int rt_dmac_add_handler(int ch, uint32_t vram, int next, uint32_t gp);
int rt_dmac_remove_handler(int ch, int hid);
int rt_dmac_enable(int ch);
int rt_dmac_disable(int ch);

/* Run a guest function the way the kernel runs an interrupt handler: a
 * zeroed context, a0/a1/a2 as given, the $gp recorded when the handler was
 * registered, the dedicated interrupt stack, and the clean-exit ra
 * sentinel so a plain `jr $31` return lands back here. Returns $v0. Used by
 * INTC/DMAC dispatch and by alarms.cpp. */
uint32_t rt_intc_run_handler(uint32_t vram, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t gp,
                             int32_t sp_delta = 0);

void rt_intc_raise(int cause);
void rt_dmac_raise(int ch);
/* Dispatch pending INTC/DMAC handlers on the interrupt context, then check
 * for preemption. No-op when nested inside a handler or interrupts are
 * disabled. Only call at instruction boundaries (MMIO trap, syscall end,
 * scheduler idle loop, rt_ei). */
void rt_intc_deliver();

bool rt_intc_mmio_read(uint32_t addr, uint32_t* out);
bool rt_intc_mmio_write(uint32_t addr, uint32_t v);

void rt_gs_vblank_start(unsigned field);
void rt_gs_vblank_end();
bool rt_gs_mmio_read(uint32_t addr, uint64_t* out);
bool rt_gs_mmio_write(uint32_t addr, uint64_t v);
uint64_t rt_gs_get_imr();
void rt_gs_put_imr(uint64_t v);

/* ---- timers (timers.cpp) ------------------------------------------------ */

void rt_timers_init();
bool rt_timers_mmio_read(uint32_t addr, uint32_t* out);
bool rt_timers_mmio_write(uint32_t addr, uint32_t v);

/* ---- alarms (alarms.cpp) ------------------------------------------------ */

/* SetAlarm/ReleaseAlarm. `time` is in H-blank ticks; `gp` is the caller's
 * $gp, restored for the handler call. rt_alarm_set returns the alarm id or
 * -1 when the table is full; rt_alarm_release returns the id or -1 when it
 * is not armed. Both i-variants behave identically. */
int rt_alarm_set(uint32_t time, uint32_t handler, uint32_t arg, uint32_t gp);
int rt_alarm_release(int id);
/* True when an alarm came due and its handler has not run yet. Alarm
 * handlers are dispatched from rt_intc_deliver(), not from rt_clock_tick. */
bool rt_alarms_pending();
void rt_alarms_dispatch_pending();

/* ---- SIF (../sif/sif.cpp) ----------------------------------------------- */

void rt_sif_init();
uint32_t rt_sif_get_reg(uint32_t idx);
void rt_sif_set_reg(uint32_t idx, uint32_t v);
uint32_t rt_sif_set_dma(uint32_t tx_addr, uint32_t count);
int rt_sif_dma_stat(uint32_t id);
bool rt_sif_mmio_read(uint32_t addr, uint32_t* out);
bool rt_sif_mmio_write(uint32_t addr, uint32_t v);
void rt_sif_dump_inventory();

/* Last-chance hook for rt_call_indirect: the EE sifcmd library's system
 * command handlers are data-referenced sub-entries inside a handwritten
 * libkernel blob the translator cannot cover. When the library's (fully
 * translated) dispatcher jalr's such an entry, this recognizes the call by
 * the packet being dispatched (a0) and supplies the HLE. Returns true when
 * handled. */
bool rt_sif_try_resolve_indirect(R5900Context* ctx, uint32_t target, uint32_t caller_vram);

#endif /* ICORECOMP_EE_KERNEL_H */
