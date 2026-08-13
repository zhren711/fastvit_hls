/*============================================================
 * fastvit_ip.cpp — unified top-level function
 *
 * Merges conv_ip/dwconv_ip/pwconv_ip/add_ip/gelu into one HLS IP,
 * dispatched by op_code. See fastvit_ip.h for the v1.5 (15 master,
 * fully independent) and v1.8/v1.9 (7 master, grouped) experiments
 * that were tried and reverted -- both failed to fix 200MHz, and
 * together they close off the entire "adjust shared-master count"
 * axis. v1.2 (current): 4 shared m_axi masters.
 *============================================================*/

#include "fastvit_ip.h"
#include "conv_worker.h"
#include "dwconv_worker.h"
#include "pwconv_worker.h"
#include "add_worker.h"
#include "gelu_worker.h"

void fastvit_ip(
    pack_t  in_a[],
    wt_t    in_b[],
    acc_t   bias[],
    pack_t  out[],

    int     op_code,
    int     CHin,
    int     Hin,
    int     Win,
    int     CHout,
    int     act_mode,
    int     out_shift,

    int     stride_h,
    int     stride_w,
    int     pad_h,
    int     pad_w,
    int     Kh,
    int     Kw,
    int     fpg)
{
#pragma HLS INTERFACE m_axi port=in_a offset=slave bundle=gmem0 depth=131072 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=in_b offset=slave bundle=gmem1 depth=262144 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=bias offset=slave bundle=gmem2 depth=1152
#pragma HLS INTERFACE m_axi port=out  offset=slave bundle=gmem3 depth=131072 \
    latency=64 num_write_outstanding=4 max_write_burst_length=256

#pragma HLS INTERFACE s_axilite port=op_code   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=CHin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=CHout     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=act_mode  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_h  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_w  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kh        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kw        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=fpg       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

    switch (op_code) {
        case OP_CONV:
            conv_worker(in_a, in_b, bias, out,
                        CHin, Hin, Win, CHout,
                        stride_h, stride_w, pad_h, pad_w,
                        act_mode, out_shift);
            break;

        case OP_DWCONV:
            dwconv_worker(in_a, in_b, bias, out,
                          CHin, Hin, Win, Kh, Kw,
                          stride_h, stride_w, pad_h, pad_w,
                          fpg, act_mode, out_shift);
            break;

        case OP_PWCONV:
            pwconv_worker(in_a, in_b, bias, out,
                          CHin, Hin, Win, CHout,
                          act_mode, out_shift);
            break;

        case OP_ADD:
            add_worker(in_a, in_b, out, CHin, Hin, Win);
            break;

        case OP_GELU:
            gelu_worker(in_a, out, CHin, Hin, Win);
            break;

        default:
            break;
    }
}
