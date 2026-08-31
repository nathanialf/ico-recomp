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

/* Fictional IOP-side sifcmd packet buffer address handed to the EE via
 * SUBADDR/SMCOM at SIF init. The EE addresses every sifcmd packet (INIT,
 * RPC BIND/CALL/RDATA) to this IOP destination; SifSetDma entries with this
 * dest are command packets, everything else is raw data. */
constexpr uint32_t RT_SIF_IOP_CMDBUF = 0x000BD000u;

/* Virtual IOP RAM. Raw SifSetDma payloads land here so a later RPC CALL can
 * read the send data the EE staged into a server buffer. Retail IOP RAM is
 * 2 MB, but this virtual one is 8 MB: sceSifAllocIopHeap addresses are
 * opaque tokens to the EE, and the extra space lets the heap serve the
 * game's sound-buffer allocations without modeling IOP module layout. */
constexpr uint32_t RT_IOP_RAM_SIZE = 8u * 1024 * 1024;
uint8_t* rt_iop_ptr(uint32_t addr); /* masked into IOP RAM, never null */

/* ---- guest memory block helpers (fatal on unmapped addresses) ------------ */

void rt_gread_bytes(uint32_t addr, void* dst, uint32_t n);
void rt_gwrite_bytes(uint32_t addr, const void* src, uint32_t n);

/* ---- framework ----------------------------------------------------------- */

void rt_rpc_init();

/* Called by sif.cpp for every SifSetDma entry, in order: copies the payload
 * into virtual IOP RAM and, when dest is RT_SIF_IOP_CMDBUF, parses it as a
 * sifcmd packet (INIT_CMD handshake, RPC BIND/CALL/RDATA). */
void rt_rpc_on_dma_entry(uint32_t src_ee, uint32_t dest_iop, uint32_t size);

/* Deferred-delivery timeline (wired into rt_sif_next_event/rt_sif_run_due). */
uint64_t rt_rpc_next_event();
void rt_rpc_run_due();

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

/* sndn2.cpp: the game's own IOP sound server (SNDN2DRV.IRX). */
void rt_sndn2_register_service();

#endif /* ICORECOMP_SIF_RPC_H */
