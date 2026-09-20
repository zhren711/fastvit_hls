/* pw_scalar_probe.cpp -- ZHR-92 (2026-09-19), single-instance round, option (b) resource probe.
 *
 * A deliberately minimal PW path for the two h=w=1 SE fc layers (entry 76: cin 768 -> cout 48,
 * entry 78: cin 48 -> cout 768; 36,864 MACs each, 1.123 + 0.773 ms today through the NARROW
 * pw_flat_pipeline_impl<false> instance, 12,648 isolated LUT). No tiling, no weight cache, no
 * row buffer: out[co] = clip_shift(bias[co] + sum_ci in[ci] * w[co*cin + ci], shift[co]).
 *
 *   pw_scalar_1  : one weight byte per cycle through the plain byte pointer (burst-inferred,
 *                  contiguous) -- 1 MAC/cycle, 73,728 cycles = 0.74 ms for both layers
 *   pw_scalar_4  : one 32-bit word per cycle through the existing w_burst port -- 4 MACs/cycle,
 *                  18,432 cycles = 0.18 ms; cin % 4 == 0 on both real layers (768, 48)
 *
 * Standalone csynth only (set_top pw_scalar_1 / pw_scalar_4): the number wanted is the LUT of
 * the loop itself, to set against the 12,648 the NARROW instance costs. Not wired into run_layer.
 */
#include <ap_int.h>
#include <hls_burst_maxi.h>

typedef ap_int<8>  act_t;
typedef ap_int<8>  wt_t;
typedef ap_int<32> acc_t;
#define SCALAR_MAX_CIN  1152   /* real PW cin max */
#define SCALAR_MAX_COUT 1152

static acc_t clip_shift(acc_t acc, int shift)
{
    acc_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return v;
}

/* ---- form 1: byte pointer, 1 MAC per cycle ------------------------------------------- */
void pw_scalar_1(const act_t in_base[], const wt_t w_base[], const acc_t b_base[], act_t out_base[],
                 int cin, int cout, int in_off, int w_off, int b_off, int out_off, int shift_off,
                 bool use_shift_table, int out_shift)
{
#pragma HLS INTERFACE m_axi port=in_base  offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=w_base   offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=b_base   offset=slave bundle=gmem_b
#pragma HLS INTERFACE m_axi port=out_base offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=return
    static act_t xin[SCALAR_MAX_CIN];
    const ap_uint<11> cin_n = cin;
    SC1_IN: for (ap_uint<11> ci = 0; ci < cin_n; ci++) {
#pragma HLS PIPELINE II=1
        xin[ci] = in_base[in_off + ci];
    }
    int w_ptr = w_off;
    SC1_CO: for (ap_uint<11> co = 0; co < (ap_uint<11>)cout; co++) {
        acc_t acc = b_base[b_off + co];
        SC1_CI: for (ap_uint<11> ci = 0; ci < cin_n; ci++) {
#pragma HLS PIPELINE II=1
            acc += (acc_t)xin[ci] * (acc_t)w_base[w_ptr + ci];
        }
        w_ptr += cin;
        const int sh = use_shift_table ? (int)w_base[shift_off + co] : out_shift;
        out_base[out_off + co] = (act_t)clip_shift(acc, sh);
    }
}

/* ---- form 2: 32-bit w_burst words, 4 MACs per cycle ---------------------------------- */
void pw_scalar_4(const act_t in_base[], hls::burst_maxi<ap_uint<32> > w_burst, const wt_t w_base[],
                 const acc_t b_base[], act_t out_base[],
                 int cin, int cout, int in_off, int w_off, int b_off, int out_off, int shift_off,
                 bool use_shift_table, int out_shift)
{
#pragma HLS INTERFACE m_axi port=in_base  offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=w_burst  offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=w_base   offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=b_base   offset=slave bundle=gmem_b
#pragma HLS INTERFACE m_axi port=out_base offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=return
    static act_t xin[SCALAR_MAX_CIN];
#pragma HLS ARRAY_PARTITION variable=xin cyclic factor=4 dim=1
    const ap_uint<11> cin_n = cin;
    SC4_IN: for (ap_uint<11> ci = 0; ci < cin_n; ci++) {
#pragma HLS PIPELINE II=1
        xin[ci] = in_base[in_off + ci];
    }
    const ap_uint<9> cin_w = cin >> 2;          /* words per output channel, <= 288 */
    int w_word = w_off >> 2;                    /* w_off is word-aligned on the real layers (checked in run_layer) */
    SC4_CO: for (ap_uint<11> co = 0; co < (ap_uint<11>)cout; co++) {
        acc_t acc = b_base[b_off + co];
        w_burst.read_request(w_word, cin_w);
        SC4_CI: for (ap_uint<9> wi = 0; wi < cin_w; wi++) {
#pragma HLS PIPELINE II=1
            const ap_uint<32> w4 = w_burst.read();
            acc_t p = 0;
            for (int l = 0; l < 4; l++) {
#pragma HLS UNROLL
                const wt_t w = (wt_t)w4.range(8 * l + 7, 8 * l);
                p += (acc_t)xin[(ap_uint<11>)(wi * 4 + l)] * (acc_t)w;
            }
            acc += p;
        }
        w_word += cin_w;
        const int sh = use_shift_table ? (int)w_base[shift_off + co] : out_shift;
        out_base[out_off + co] = (act_t)clip_shift(acc, sh);
    }
}
