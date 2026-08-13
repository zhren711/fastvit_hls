#ifndef __GELU_WORKER_H__
#define __GELU_WORKER_H__

#include "fastvit_ip.h"

/* GELU activation, in-place-capable elementwise LUT (out may alias in_a).
 * Same 256-entry int8 approximation table as the ARM software path
 * (petalinux/software/fastvit_app/src/gelu_lut.c), moved to hardware so
 * the op streams over the shared m_axi ports (burst AXI) instead of
 * running as a scalar loop on ARM over the non-cacheable /dev/mem
 * O_SYNC DMA mapping used for feature-map buffers (see fastvit_infer.c
 * main.c dma_init) -- that mapping is what made the ARM loop slow,
 * not the LUT itself. */
void gelu_worker(
    pack_t in_a[],
    pack_t out[],
    int    CH,
    int    H,
    int    W
);

#endif // __GELU_WORKER_H__
