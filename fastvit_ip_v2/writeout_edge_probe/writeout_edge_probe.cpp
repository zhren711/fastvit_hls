/*================================================================
 * writeout_edge_probe.cpp -- ZHR-92 (2026-08-24): csim-only probe for
 * entry76/entry78's exact real shape (SE block fc1/fc2, w_out=1) BEFORE
 * attempting an isolated board test. These are the only two real-network
 * layers where WRITEOUT's slow path (col_sz<MAC_PC) is exercised, and
 * they've never run on real silicon before the full-network hang.
 *
 * Real descriptor field values pulled directly from desc_all.bin (not
 * guessed): entry76 cin=768/cout=48, entry78 cin=48/cout=768, both
 * h_in=w_in=h_out=w_out=1, n_row_tiles=n_col_tiles=1, last_row_tile=
 * last_col_tile=1 -- so r_sz=col_sz=1 for the single tile, matching the
 * suspicion that only 1 of 16 WRITEOUT slots is valid per ot. Weight/
 * bias/input data here is synthetic (all-ones), NOT the real network's
 * actual values -- this probe checks CONTROL FLOW (does it terminate,
 * does it produce the arithmetically-expected result for a known input),
 * not numerical accuracy against the real network.
 *================================================================*/
#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

static bool run_shape(const char *name, int cin, int cout) {
    LayerDescV2 d{};
    d.op_type = LDESC_OP_PWCONV;
    d.cin = cin; d.cout = cout;
    d.h_in = 1; d.w_in = 1;
    d.k = 1; d.stride = 1; d.pad = 0;
    d.fpg = 1;
    d.out_shift = 0;
    d.in_off = 0; d.w_off = 0; d.b_off = 0; d.out_off = 0; d.in2_off = 0;

    MacArrayParams p = derive_mac_array_params(d);
    d.h_out = p.h_out; d.w_out = p.w_out;
    d.n_row_tiles = p.n_row_tiles; d.n_col_tiles = p.n_col_tiles; d.n_ch_tiles = p.n_ch_tiles;
    d.last_row_tile = p.last_row_tile; d.last_col_tile = p.last_col_tile; d.last_ch_tile = p.last_ch_tile;
    d.in_ch_stride = p.in_ch_stride; d.out_ch_stride = p.out_ch_stride;
    d.use_shift_table = 0; d.shift_off = 0;

    printf(">>> %s: cin=%d cout=%d h_out=%d w_out=%d n_row_tiles=%d n_col_tiles=%d "
           "last_row_tile=%d last_col_tile=%d n_ch_tiles=%d\n",
           name, cin, cout, d.h_out, d.w_out, d.n_row_tiles, d.n_col_tiles,
           d.last_row_tile, d.last_col_tile, d.n_ch_tiles);

    int r_sz = (d.n_row_tiles == 1) ? d.last_row_tile : MAC_PR;
    int col_sz = (d.n_col_tiles == 1) ? d.last_col_tile : MAC_PC;
    printf(">>> %s: this tile's r_sz=%d col_sz=%d (expect 1,1 -- 1/%d WRITEOUT slots valid)\n",
           name, r_sz, col_sz, MAC_PR * MAC_PC);

    std::vector<act_t> in_buf(cin, act_t(1));
    std::vector<wt_t>  w_buf(cout * cin, wt_t(1));
    std::vector<acc_t> b_buf(cout, acc_t(0));
    std::vector<act_t> out_buf(cout, act_t(-1));  /* poison */
    std::vector<int>   out_written(1, 0);

    LayerDescV2 desc[1] = { d };

    mac_array_top(desc, 1,
                  in_buf.data(), w_buf.data(), b_buf.data(),
                  out_buf.data(), out_written.data(),
                  reinterpret_cast<const ap_uint<32>*>(in_buf.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(out_buf.data())));

    printf(">>> %s: mac_array_top RETURNED (no hang in csim)\n", name);
    printf(">>> %s: out_written[0] = %d\n", name, out_written[0]);

    /* expected: every output channel = sum_{ci=0..cin-1}(1*1) = cin, clipped to int8 [-128,127] */
    int expect = cin;
    if (expect > 127) expect = 127;
    int mismatches = 0;
    for (int c = 0; c < cout; c++) {
        if ((int)out_buf[c] != expect) mismatches++;
    }
    printf(">>> %s: mismatches = %d / %d (expect every channel = %d)\n", name, mismatches, cout, expect);
    printf(">>> %s: %s\n\n", name, mismatches == 0 && out_written[0] != 0 ? "PASS" : "FAIL");
    return mismatches == 0 && out_written[0] != 0;
}

int main() {
    bool ok76 = run_shape("entry76-shape", 768, 48);
    bool ok78 = run_shape("entry78-shape", 48, 768);
    printf(">>> overall: %s\n", (ok76 && ok78) ? "PASS" : "FAIL");
    return (ok76 && ok78) ? 0 : 1;
}
