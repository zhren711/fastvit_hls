/*============================================================
 * add_worker.cpp — ported from add_ip/add_ip.cpp, restructured from
 * a per-byte flattened loop to a per-word (4 elements/beat) loop
 * against the packed in_a/out ports. in_b (feat_in2) stays per-element,
 * unpacked, exactly as add_ip reads it today (see add_worker.h).
 * Clip/quantize math (ap_int<9> overflow-safe add) is unchanged.
 *============================================================*/

#include "add_worker.h"

void add_worker(
    pack_t in_a[],
    wt_t   in_b[],
    pack_t out[],
    int    CH,
    int    H,
    int    W)
{
    int total   = CH * H * W;
    int n_words = (total + 3) >> 2;

    ADD_LOOP:
    for (int w = 0; w < n_words; w++) {
#pragma HLS PIPELINE II=1
        pack_t wa = in_a[w];
        pack_t wo;
        for (int b = 0; b < 4; b++) {
#pragma HLS UNROLL
            int idx = w*4 + b;
            act_t a  = (act_t)wa.range(b*8+7, b*8);
            act_t b2 = (idx < total) ? (act_t)in_b[idx] : (act_t)0;
            /* ap_int<9> 防止 int8+int8 溢出（范围 -256~255） */
            ap_int<9> sum = (ap_int<9>)a + (ap_int<9>)b2;
            act_t result;
            if      (sum >  127) result =  127;
            else if (sum < -128) result = -128;
            else                 result = (act_t)sum;
            wo.range(b*8+7, b*8) = result;
        }
        out[w] = wo;
    }
}
