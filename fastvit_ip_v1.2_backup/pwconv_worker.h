#ifndef __PWCONV_WORKER_H__
#define __PWCONV_WORKER_H__

#include "fastvit_ip.h"

/* Ported from pwconv_ip/pwconv_ip.cpp v14, with correctness fixes and a
 * performance fix on top of the port (see pwconv_worker.cpp for the full
 * writeup):
 *   1. [correctness] Original TM>TN>TS loop nesting reused a
 *      one-spatial-tile accumulator across TN passes, losing partial
 *      sums whenever CHin>PW_TN and spatial>PW_TS (virtually always for
 *      real layers).
 *   2. [correctness] WRITE_PW_OUT wrote a full PW_TS-wide tile
 *      regardless of sp_valid, spilling unaccumulated padding into the
 *      next channel whenever spatial % PW_TS != 0 (inert on real
 *      FastVIT-T8 dims, which are always power-of-2, but a live bug
 *      generally).
 *   3. [performance] Fixing #1 by moving TN innermost (TM>TS>TN) made
 *      weight loads happen once per (tm,ts,tn) instead of once per
 *      (tm,tn) -- ~25x slower in practice (measured: PW1+PW2 went from
 *      pwconv_ip.h's documented ~420ms historical total to ~9.8s).
 *      Fixed via weight-stationary GEMM blocking: pw_out_buf enlarged
 *      to span the FULL spatial range (BRAM-backed, PW_MAX_SPATIAL wide)
 *      so the loop order can go back to TM>TN>TS (weight loaded once per
 *      (tm,tn), reused across all of TS) while staying correct (the
 *      accumulator is now big enough to hold partial sums for the whole
 *      layer, not just one spatial tile, so nothing gets lost when TN
 *      advances).
 * Already ap_uint<32>-native, so no pack/unpack changes -- only port
 * renames (in_a/in_b/out). */
void pwconv_worker(
    pack_t feat_in_w32[],
    wt_t   weight[],
    acc_t  bias[],
    pack_t feat_out_w32[],
    int    CHin,
    int    H,
    int    W,
    int    CHout,
    int    act_mode,
    int    out_shift
);

#endif // __PWCONV_WORKER_H__
