/* snd/snd.h: native sound engine interface.
 *
 * The sndn2 RPC service (sif/sndn2.cpp) forwards every decoded 16-byte
 * command record here and calls rt_snd_flush_tick() once per fno 0x64 batch
 * (the EE library flushes once per vblank field, so one tick renders one
 * field's worth of 48 kHz audio and hands it to host/audio.cpp).
 *
 * Modules:
 *   spu.cpp    2 MB fake SPU RAM + bank transfer consumption
 *   engine.cpp 48 voices: VAG ADPCM decode, pitch resampling, SPU2 ADSR
 *              envelopes, volumes, mix; command dispatch
 *   reverb.cpp send-bus reverb (freeverb-style)
 *
 * Everything here runs on the main OS thread (guest threads are minicoro
 * coroutines; RPC handlers run synchronously on the main thread), so the
 * engine needs no locks. Cross-thread handoff to the audio device happens
 * inside host/audio.cpp (SDL_AudioStream does its own buffering).
 *
 * Protocol facts: see sif/SNDN2_NOTES.md. Runtime-internal, NOT part of the
 * ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_SND_SND_H
#define ICORECOMP_SND_SND_H

#include <cstdint>

/* ---- spu.cpp ------------------------------------------------------------- */

constexpr uint32_t RT_SPU_RAM_SIZE = 2u * 1024 * 1024;

uint8_t* rt_spu_ram(); /* allocated on first use, zero-filled */

/* cmd 0x20: copy len bytes into SPU RAM at spu_addr (byte address). src is
 * the staged data in virtual IOP RAM; the copy happens immediately because
 * the EE reuses the staging buffer for the next chunk. */
void rt_spu_upload(const uint8_t* src, uint32_t spu_addr, uint32_t len);

/* The staging witness (snd/iop_stage.h). rt_snd_pcm_note_iop_write forwards
 * every EE to IOP DMA entry here, so a cmd 0x20 can be checked against what
 * the EE actually wrote into its heap block before queueing the transfer:
 * sound/s_init.c soundBDDataSet (PAL 0x00143D68) does the raw SifSetDma and
 * the SgDmaWrite back to back for every chunk. rt_spu_staged_bytes returns
 * how much of [iop_addr, iop_addr + size) lies in a granule some transfer
 * touched, and rt_spu_consume_staging drops those marks once the bytes have
 * been taken, because the EE reuses the same heap address for the next
 * chunk. */
void rt_spu_note_iop_write(uint32_t iop_addr, uint32_t size);
uint32_t rt_spu_staged_bytes(uint32_t iop_addr, uint32_t size);
void rt_spu_consume_staging(uint32_t iop_addr, uint32_t size);

/* The number of the most recent bank transfer whose destination range
 * contains [spu_addr, spu_addr + len), or 0 when no transfer ever covered
 * it. This is the question a key-on has to ask: whether a bank was uploaded
 * there. The bytes at the address cannot answer it, since a run of zeros
 * inside an uploaded bank is ordinary content. */
uint32_t rt_spu_covered_by(uint32_t spu_addr, uint32_t len);

/* ---- engine.cpp ---------------------------------------------------------- */

/* fno 0x65 remote init. voice_budget is init word 0 (0x1E observed). */
void rt_snd_engine_init(uint32_t voice_budget);

/* One untagged command record: w1..w3 are the raw 32-bit words the EE's
 * enqueue (_SgSetPkAdd, PAL 0x002737E0) stored. Tagged records (DMA, cmd
 * 0x20/0x21) never reach this function; sndn2.cpp handles them. */
void rt_snd_command(uint32_t cmd, uint32_t w1, uint32_t w2, uint32_t w3);

/* Called once per fno 0x64 flush: renders one vblank field of audio at the
 * programmed video mode's field rate (48000 / 59.94 frames on NTSC, 960 on
 * PAL, fractional remainder carried) into host/audio. */
void rt_snd_flush_tick();

/* Fills the IOP-written fields of the 0x200-byte EE status block that the
 * library reads besides the ack word: per-voice stream read cursors at
 * +0xC0 + (v % 24) * 4 + (v / 24) * 0x60 (SgStAdpcmIopReadAddr, PAL
 * 0x002785A0). The cursor is a byte offset within the ring, not
 * an address; see the derivation on rt_snd_fill_status in engine.cpp. */
void rt_snd_fill_status(uint8_t* recv, uint32_t recv_size);

/* True when `addr` lands inside a playing stream voice's ring, with the ring
 * base, its size and the ring-relative cursor last reported to the EE. Lets
 * the disc read path describe a refill without repeating the base recovery
 * that stream_addr already does. */
bool rt_snd_stream_ring(uint32_t addr, uint32_t* base, uint32_t* ring, uint32_t* cursor);

/* Write-side witness for the SgStPcm ring (commands 0x46-0x4F). sif/rpc.cpp
 * calls this for every EE to IOP DMA entry, with the destination masked into
 * IOP RAM and the number of bytes actually staged, because the movie refills
 * that ring with raw SIF DMA rather than through this service. The engine
 * stamps the interleave blocks the transfer covered, which is what lets its
 * starvation counter tell "the EE never wrote this block" from "the EE wrote
 * the same bytes again". A transfer outside the open ring returns on the
 * first compare. */
void rt_snd_pcm_note_iop_write(uint32_t iop_addr, uint32_t size);

/* ---- reverb.cpp ---------------------------------------------------------- */

void rt_reverb_reset();
void rt_reverb_set_params(uint32_t mode, float depth_l, float depth_r);
/* Processes one mono send sample, accumulates wet stereo into out_l/out_r. */
void rt_reverb_run(float in, float* out_l, float* out_r);

#endif /* ICORECOMP_SND_SND_H */
