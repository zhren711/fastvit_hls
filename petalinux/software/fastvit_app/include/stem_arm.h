/*================================================================
 * stem_arm.h -- A3 (ZHR-92, 2026-08-21): ARM-side C port of
 * tools/compute_stem_arm.py (Route C: Stem's plain 3x3 s=2 conv computed
 * off the MAC array, its output DMA'd into in_off=0 before the hardware
 * sequence's entry[0] GELU runs). Bit-identical algorithm to the Python
 * reference: per-channel shift + rescaled bias are precomputed host-side
 * (log2/weight_scale stay off-board, same division of labor as every
 * other layer's shift table) and shipped as flat int32 binaries; this
 * port does pure int32 accumulate + clip_shift, no floating point.
 *================================================================*/
#ifndef __STEM_ARM_H__
#define __STEM_ARM_H__

#include <stdint.h>

#define STEM_CIN   3
#define STEM_COUT  48
#define STEM_K     3
#define STEM_S     2
#define STEM_P     1
#define STEM_HIN   256
#define STEM_WIN   256
#define STEM_HOUT  128
#define STEM_WOUT  128

/* image: [CIN][HIN][WIN] int8, weight: [COUT][CIN][K][K] int8,
 * bias_rescaled/shift: [COUT] int32 (both from compute_stem_arm.py's
 * --shift-table-out), out: [COUT][HOUT][WOUT] int8 (786432 bytes) --
 * caller-allocated, e.g. the ARM-visible virtual address of in_off=0
 * inside the activation arena, written directly so no extra copy is
 * needed before the hardware sequence's entry[0] reads it. */
void stem_arm_conv(
    const int8_t  *image,
    const int8_t  *weight,
    const int32_t *bias_rescaled,
    const int32_t *shift_per_channel,
    int8_t        *out
);

#endif /* __STEM_ARM_H__ */
