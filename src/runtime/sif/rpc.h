/* sif/rpc.h: internal declarations for the SIF RPC HLE layer.
 *
 * rpc.cpp implements the sifrpc wire protocol (packets parsed out of
 * SifSetDma transfers, responses delivered as sifcmd packets into the EE's
 * registered receive buffer + a DMAC SIF0 interrupt). cdvd.cpp registers the
 * cdvdman services on top of it. Structure layouts follow ps2sdk
 * common/include/sifrpc-common.h (clean-room structural reference; the wire
 * format is a public SDK fact).
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_SIF_RPC_H
#define ICORECOMP_SIF_RPC_H

#include "../ee/kernel.h"

/* ---- virtual IOP RAM and its address map --------------------------------- */

/* Virtual IOP RAM. Raw SifSetDma payloads land here so a later RPC CALL can
 * read the send data the EE staged into a server buffer, and the
 * sceSifAllocIopHeap heap is carved out of it.
 *
 * The size is the hardware size. IOP addresses are not opaque tokens to the
 * EE: the game's MPEG helper validates every IOP address it is handed
 * against the retail IOP's 2 MB of RAM (SgStPcmOpen, PAL 0x00278640, a
 * function entry in the retail ELF; it returns -1 when an address is above
 * 0x1FFFFF), so an
 * address outside 2 MB fails the attract movie's init. Everything the
 * runtime mints therefore has to fit inside 2 MB alongside the heap.
 *
 * Address map. All addresses are virtual-IOP physical addresses; the whole
 * map lives here so there is one place to check for an overlap.
 *
 *   0x000000 - 0x00FFFF  low memory. Nothing is minted here: on hardware
 *                        this is the IOP kernel and the bottom of the
 *                        loaded module region.
 *   0x010000 - 0x06FFFF  per-service RPC staging buffers, kBufStride
 *                        (0x4000) bytes for each of kMaxServices (24)
 *                        services (rpc.cpp kBufBase). A CALL whose
 *                        send_size exceeds the stride is fatal, so the
 *                        stride is also the largest RPC send this runtime
 *                        accepts.
 *   0x070000 - 0x070BFF  minted SifRpcServerData structs, kServerStride
 *                        (0x80) per service (rpc.cpp kServerBase).
 *   0x078000 - 0x07807F  pad actuator receive blocks, 0x40 per port
 *                        (pad.cpp kActBufBase).
 *   0x080000 - 0x08FFFF  sifcmd packet buffer (RT_SIF_IOP_CMDBUF below).
 *   0x090000 - 0x1FFFFF  sceSifAllocIopHeap heap, 1.4375 MB (cdvd.cpp
 *                        kIopHeapBase/kIopHeapEnd).
 *
 * The heap base is a runtime choice, not a measured hardware fact. On
 * hardware sceSifAllocIopHeap is served by AllocSysMemory(SMEM_LOW), which
 * hands out the lowest free block above the loaded modules, so the real
 * base depends on which IOP modules this title loads and that layout is not
 * known here. What is known is the 2 MB ceiling and the game's measured
 * peak live heap of 1,411,088 bytes, which fits the span above with about
 * 94 KB spare after 256-byte rounding. */
constexpr uint32_t RT_IOP_RAM_SIZE = 2u * 1024 * 1024;
uint8_t* rt_iop_ptr(uint32_t addr); /* masked into IOP RAM, never null */

/* Fictional IOP-side sifcmd packet buffer address handed to the EE via
 * SUBADDR/SMCOM at SIF init. The EE addresses every sifcmd packet (INIT,
 * RPC BIND/CALL/RDATA) to this IOP destination; SifSetDma entries with this
 * dest are command packets, everything else is raw data. The EE only ever
 * learns this address from SifGetReg(SUBADDR), so any value outside the
 * heap works. */
constexpr uint32_t RT_SIF_IOP_CMDBUF = 0x00080000u;

/* ---- guest memory block helpers (fatal on unmapped addresses) ------------ */

void rt_gread_bytes(uint32_t addr, void* dst, uint32_t n);
void rt_gwrite_bytes(uint32_t addr, const void* src, uint32_t n);

/* ---- framework ----------------------------------------------------------- */

void rt_rpc_init();

/* Called by sif.cpp for every SifSetDma entry, in order: copies the payload
 * into virtual IOP RAM and, when dest is RT_SIF_IOP_CMDBUF, parses it as a
 * sifcmd packet (INIT_CMD handshake, RPC BIND/CALL/RDATA). */
void rt_rpc_on_dma_entry(uint32_t src_ee, uint32_t dest_iop, uint32_t size);

/* Deferred-delivery timeline (wired into rt_sif_next_event/rt_sif_run_due).
 * Deliveries go out in due order, not in the order they were queued: an
 * IOP server that is still working on one request does not hold up the
 * replies of every other server. */
uint64_t rt_rpc_next_event();
void rt_rpc_run_due();

/* For a service handler, while it runs inside an RPC CALL: hold that call's
 * completion (its END packet, or the packet release for rmode=0) back by
 * this many bus cycles beyond the SIF latency. It models an IOP server that
 * returns from its handler only when the work is done, so the EE client's
 * sceSifCheckStatRpc / END callback cannot see the call finished before the
 * device would have finished it. Only the completion is held; receive data
 * and out-of-band writes the handler made land immediately, as before. The
 * hold applies to the current call only and resets after it: two holds
 * inside one handler add up, and a hold set outside a CALL handler is
 * refused with a warn rather than carried into the next call. */
void rt_rpc_hold_completion(uint64_t cycles);

void rt_rpc_dump_inventory();

/* ---- service registry ---------------------------------------------------- */

/* An RPC service handler. send points at the service's virtual-IOP receive
 * buffer (send_size bytes staged by the EE, 0 if none). recv is a zeroed
 * host buffer of recv_size bytes (the EE caller's declared receive size);
 * whatever the handler leaves there is written to the EE receive buffer.
 * Handlers may also write EE RAM directly (rt_gwrite_bytes) for out-of-band
 * results (sceCdRead data, sceCdSearchFile file info): that models the IOP
 * server's own SifSetDma transfers, which are synchronous in this runtime. */
typedef void (*RtRpcServiceFn)(uint32_t fno, const uint8_t* send, uint32_t send_size,
                               uint8_t* recv, uint32_t recv_size);

/* Registers a service. fn == nullptr makes it a loud stub: BIND succeeds,
 * every CALL is logged and answered with a zeroed receive buffer. */
void rt_rpc_register_service(uint32_t sid, const char* name, RtRpcServiceFn fn);

/* cdvd.cpp: registers the cdvdman servers + the documented boot stubs and
 * mounts the disc image (fatal if no usable image is found). */
void rt_cdvd_register_services();

/* cdvd.cpp: logs the stream state machine, the drive busy timer, and the
 * last handful of read/stream requests. Called from rt_sif_dump_inventory
 * so every inventory dump (deadlock fatal, SIGINT, exit, unmapped MMIO)
 * carries it. */
void rt_cdvd_dump_state();

/* sndn2.cpp: the game's own IOP sound server (SNDN2DRV.IRX). */
void rt_sndn2_register_service();

/* pad.cpp: the padman servers (old wire protocol) plus the per-field pad
 * data delivery timeline (aggregated into rt_sif_next_event/run_due). */
void rt_pad_register_services();
uint64_t rt_pad_next_event();
void rt_pad_run_due();

/* mc.cpp: the mcserv server (old MCSERV wire protocol), backed by
 * config/local.toml [saves] dir on the host disk. */
void rt_mc_register_service();

/* The directory the virtual memory card sits in, which is where anything
 * kept beside the card belongs (guest/achievements.cpp's store). Resolves
 * and creates the card's backing directory on the first call, exactly as the
 * first mcserv RPC would, so calling this early only moves that work (and
 * its fatal, if the directory cannot be created) earlier in the run.
 * Returns null when the card directory has no parent to name, which no
 * resolution this runtime performs produces; callers pass that straight to
 * rt_achievements_init, whose contract accepts null. */
const char* rt_mc_saves_dir();

#endif /* ICORECOMP_SIF_RPC_H */
