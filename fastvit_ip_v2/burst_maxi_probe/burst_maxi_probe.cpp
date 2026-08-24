/*================================================================
 * burst_maxi_probe.cpp -- ZHR-92 angle-B step 2 (2026-08-24): does
 * hls::burst_maxi synthesize at all under -flow_target vivado (this
 * project's real IP-export flow, NOT the -flow_target vitis the official
 * Interface/Memory/manual_burst example uses)? Fully isolated -- does NOT
 * touch mac_array.cpp/run_layer. Minimal top function only: one explicit
 * read burst, one explicit write burst, no conditional branches anywhere
 * near the burst calls (the exact shape our real WRITEOUT/PW_PATCH_HOIST
 * code can't achieve without a bigger restructure -- this probe is
 * intentionally as easy as possible for the API, to isolate "does the API
 * exist/synthesize here at all" from "can our real conditional code use
 * it").
 *================================================================*/
#include <hls_burst_maxi.h>
#include <ap_int.h>

#define BURST_LEN 16

void burst_maxi_probe_top(hls::burst_maxi<ap_int<8> > in_mem,
                           hls::burst_maxi<ap_int<8> > out_mem,
                           int offset)
{
#pragma HLS INTERFACE m_axi port=in_mem  offset=slave bundle=gmem_in  max_read_burst_length=256  num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=out_mem offset=slave bundle=gmem_out max_write_burst_length=256 num_write_outstanding=4
#pragma HLS INTERFACE s_axilite port=offset bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    ap_int<8> buf[BURST_LEN];

    in_mem.read_request(offset, BURST_LEN);
    READ_LOOP: for (int i = 0; i < BURST_LEN; i++) {
        #pragma HLS PIPELINE II=1
        buf[i] = in_mem.read();
    }

    out_mem.write_request(offset, BURST_LEN);
    WRITE_LOOP: for (int i = 0; i < BURST_LEN; i++) {
        #pragma HLS PIPELINE II=1
        out_mem.write(buf[i]);
    }
    out_mem.write_response();
}
