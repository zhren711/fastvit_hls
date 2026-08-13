#ifndef __CONV_WORKER_H__
#define __CONV_WORKER_H__

#include "fastvit_ip.h"

/* Ported from conv_ip/conv_ip.cpp — fixed 3x3 standard conv, stem layer only.
 * feat_in/feat_out upgraded from byte-wide act_t* to packed pack_t* (in_a/out);
 * weight/bias stay native (wt_t*, acc_t*) as in_b/bias, unchanged from original.
 * NOTE: assumes Wout is a multiple of 4 (matches CONV_TC=4) so that packed
 * output writes land on word boundaries -- true for FastVIT-T8's actual
 * stem dimensions (the only caller). */
void conv_worker(
    pack_t in_a[],
    wt_t   in_b[],
    acc_t  bias[],
    pack_t out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    CHout,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    act_mode,
    int    out_shift
);

#endif // __CONV_WORKER_H__
