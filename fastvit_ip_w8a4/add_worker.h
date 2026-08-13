#ifndef __ADD_WORKER_H__
#define __ADD_WORKER_H__

#include "fastvit_ip.h"

/* Ported from add_ip/add_ip.cpp. feat_in1/feat_out upgraded to packed
 * pack_t (in_a/out) -- 4 elements/beat instead of 1, a strict bandwidth
 * improvement for this 89-times-used op. feat_in2 stays on in_b
 * (native wt_t/act_t, unpacked, exactly as add_ip reads it today) since
 * in_b is shared with the weight ports and repacking weight loading
 * wasn't worth the risk for this merge. */
void add_worker(
    pack_t in_a[],
    wt_t   in_b[],
    pack_t out[],
    int    CH,
    int    H,
    int    W
);

#endif // __ADD_WORKER_H__
