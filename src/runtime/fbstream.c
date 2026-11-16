/* Lammy's guitar: the game's CSpuFeedbackStream (fbstream.cpp in the executable's assert strings).
   The guitar track plays on SPU voices 1 and 3 at volume 0. The SPU writes those voices' output (after
   ADSR, before volume) into the capture buffers at 0x800 and 0xC00. An SPU IRQ on the capture position
   (0x400/0x600) fires every 256 samples; the game then DMAs the finished half of the voice 1/3 capture
   into RAM, processes it (the guitar effects and the "mute when the player misses" logic), and DMAs
   256 bytes back into a ring at 0x7FE00 that another voice plays.

   libspu's SpuSetTransferStartAddr (8004e554 in SCPS_180.11) refuses addresses below 0x1010, which is
   where the capture buffers live. At init the stream code checks that the instruction at 0x8004e56c is
   the range check's `bnez v0` (0x1440000b) and overwrites it with a nop. Recompiled code doesn't see
   that patch, so the read went to the previous address (the 0x7FE00 ring) and the guitar was silent.
   This replacement runs the original logic but skips the check once the game has nopped it. */
#include "psx.h"
#include "recomp.h"
#include <stdlib.h>

#define SPU_ADDR_UNITS_FN 0x800476b4u /* libspu: byte address -> SPU address units (also stores the register) */

#define CHECK_INSN_ADDR 0x8004e56cu
#define CHECK_INSN      0x1440000bu /* bnez v0, +0xb */
#define SPU_XFER_ADDR   0x800705c0u /* libspu's cached transfer start address (u16) */
#define SPU_ADDR_SHIFT  0x800705d0u /* address unit shift */

void hle_spu_set_transfer_addr(CPU *c) {
    uint32_t addr = c->r[4];
    static int off = -1; if (off < 0) off = getenv("UJL_NO_GUITAR_FIX") != NULL; /* A/B: original libspu check */
    int check = off || mem_r32(CHECK_INSN_ADDR) == CHECK_INSN;
    if (check && addr - 0x1010u > 0x7efe8u) { c->r[2] = 0; return; }
    uint32_t ra = c->r[31];
    c->r[29] -= 24;
    c->r[5] = addr;
    c->r[4] = (uint32_t)-1;
    c->r[31] = 0x8004e57cu;
    call_indirect(c, SPU_ADDR_UNITS_FN); /* by address, so the runtime links against any generated code */
    c->r[29] += 24;
    c->r[31] = ra;
    mem_w16(SPU_XFER_ADDR, (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)mem_r16(SPU_XFER_ADDR) << (mem_r32(SPU_ADDR_SHIFT) & 31);
}
