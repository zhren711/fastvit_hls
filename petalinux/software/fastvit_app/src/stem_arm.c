/*================================================================
 * stem_arm.c -- see stem_arm.h. Direct C port of compute_stem_arm.py's
 * loop nest (kh,kw outer, oh/ow inner via the same padded-index formula:
 * ih = oh*S+kh-P, iw = ow*S+kw-P), scalar accumulate in int64 to match
 * numpy's int64 accumulator exactly, then clip_shift identical to
 * mac_array.cpp / fastvit_infer.c's convention (arithmetic right-shift,
 * clamp to int8).
 *================================================================*/
#include "stem_arm.h"

static inline int8_t clip_shift64(int64_t acc, int32_t shift) {
    int64_t v = acc >> shift;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

void stem_arm_conv(
    const int8_t  *image,
    const int8_t  *weight,
    const int32_t *bias_rescaled,
    const int32_t *shift_per_channel,
    int8_t        *out)
{
    for (int oc = 0; oc < STEM_COUT; oc++) {
        int64_t bias = bias_rescaled[oc];
        int32_t shift = shift_per_channel[oc];
        for (int oh = 0; oh < STEM_HOUT; oh++) {
            for (int ow = 0; ow < STEM_WOUT; ow++) {
                int64_t acc = bias;
                for (int ic = 0; ic < STEM_CIN; ic++) {
                    for (int kh = 0; kh < STEM_K; kh++) {
                        int ih = oh * STEM_S + kh - STEM_P;
                        if (ih < 0 || ih >= STEM_HIN) continue;
                        for (int kw = 0; kw < STEM_K; kw++) {
                            int iw = ow * STEM_S + kw - STEM_P;
                            if (iw < 0 || iw >= STEM_WIN) continue;
                            int8_t px = image[(ic * STEM_HIN + ih) * STEM_WIN + iw];
                            int8_t wt = weight[((oc * STEM_CIN + ic) * STEM_K + kh) * STEM_K + kw];
                            acc += (int64_t)px * (int64_t)wt;
                        }
                    }
                }
                out[(oc * STEM_HOUT + oh) * STEM_WOUT + ow] = clip_shift64(acc, shift);
            }
        }
    }
}
