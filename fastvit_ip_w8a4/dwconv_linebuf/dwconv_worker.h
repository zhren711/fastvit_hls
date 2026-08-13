#ifndef __DWCONV_LINEBUF_WORKER_H__
#define __DWCONV_LINEBUF_WORKER_H__

#include <ap_int.h>

/* Self-contained header: this worker is developed/validated in isolation
 * (see plan C:\Users\zhren\.claude\plans\jaunty-questing-journal.md), not
 * wired into fastvit_ip's shared-master top level. Types match
 * fastvit_ip_w8a4/fastvit_ip.h's W8A4 typedefs exactly (drop-in
 * signature/DDR-layout compatible with fastvit_ip_w8a4/dwconv_worker.cpp)
 * but this header doesn't include fastvit_ip.h, to avoid a build
 * dependency on the other 4 workers' unrelated tiling constants. */
typedef ap_int<4>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;
typedef ap_uint<32> pack_t;

#define ACT_NONE 0
#define ACT_RELU 1

#define DW_MAX_K  7
#define DW_MAX_CH 512
#define DW_MAX_H  64
#define DW_MAX_W  64

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

#endif // __DWCONV_LINEBUF_WORKER_H__
