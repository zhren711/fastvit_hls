#ifndef __DWCONV_WORKER_H__
#define __DWCONV_WORKER_H__

#include "fastvit_ip.h"

/* Ported from dwconv_ip/dwconv_ip_v12_backup.cpp (the plain v12 compute
 * core -- NOT the v13 FILM-QNN DSP-packed version, which didn't fit on
 * this LUT-bound chip and is being abandoned by this merge). Already
 * ap_uint<32>-native (feat_in/feat_out), so no pack/unpack changes
 * needed here -- only port renames (in_a/in_b/out). */
void dwconv_worker(
    pack_t feat_in[],
    wt_t   weight[],
    acc_t  bias[],
    pack_t feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    fpg,
    int    act_mode,
    int    out_shift
);

#endif // __DWCONV_WORKER_H__
