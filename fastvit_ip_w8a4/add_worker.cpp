/*============================================================
 * add_worker.cpp — ported from add_ip/add_ip.cpp, restructured from
 * a per-byte flattened loop to a per-word loop against the packed
 * in_a/out ports. in_b (feat_in2) stays per-element, unpacked, exactly
 * as add_ip reads it today (see add_worker.h) -- NOTE (W8A4): in_b's
 * port type is still wt_t (ap_int<8>, weights stay 8-bit) even though
 * this op reuses it for an activation operand, so in_b's DRAM storage
 * itself is untouched by the nibble-repacking below; the `(act_t)in_b[idx]`
 * cast now truncates ap_int<8> -> ap_int<4> instead of the previous
 * identity-width cast. This is safe as long as whatever produced
 * feat_in2 already clamped it to the -7..7 range before writing it
 * (true for every producer op in this pipeline, all of which now clamp
 * via conv/dwconv/pwconv_apply_act).
 * Clip/quantize math (ap_int<9> overflow-safe add) updated to the
 * symmetric 4-bit range (-7..7, see fastvit_ip.h).
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
    /* W8A4: 8 nibble-packed act_t lanes per 32-bit pack_t word (was 4
     * byte lanes) -- see fastvit_ip.h. */
    int total   = CH * H * W;
    int n_words = (total + 7) >> 3;

    ADD_LOOP:
    for (int w = 0; w < n_words; w++) {
#pragma HLS PIPELINE II=1
        pack_t wa = in_a[w];
        pack_t wo;
        for (int b = 0; b < 8; b++) {
#pragma HLS UNROLL
            int idx = w*8 + b;
            act_t a  = (act_t)wa.range(b*4+3, b*4);
            act_t b2 = (idx < total) ? (act_t)in_b[idx] : (act_t)0;
            /* ap_int<9> 防止 int4+int4 溢出（范围 -14~14，9-bit 足够富余） */
            ap_int<9> sum = (ap_int<9>)a + (ap_int<9>)b2;
            act_t result;
            if      (sum >  7) result =  7;
            else if (sum < -7) result = -7;
            else               result = (act_t)sum;
            wo.range(b*4+3, b*4) = result;
        }
        out[w] = wo;
    }
}
