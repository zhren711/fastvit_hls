// row_read_overlap_fixed_probe_tb.cpp -- byte-exact correctness check +
// cosim target for row_read_dataflow_fixed. Real backing memory, real
// int8 byte-lane packing (matches ROW_READ_FILL's own wd.range(b*8+7,b*8)
// convention), not a synthetic all-zero/all-same pattern.
#include "mac_array.h"
#include <cstdio>
#include <cstdlib>

extern void row_read_dataflow_fixed(
    hls::burst_maxi<ap_uint<32> > in_burst,
    act_t row_buf[MAC_PR][MAX_CIN_TIMES_W]);

#define FIXED_CIN 64
#define FIXED_W 8
#define FIXED_H 4
#define FIXED_IN_CH_STRIDE (FIXED_W * FIXED_H)

int main() {
    const int total_bytes = FIXED_CIN * FIXED_H * FIXED_W;  // 256
    uint8_t bytes[total_bytes];
    for (int ci = 0; ci < FIXED_CIN; ci++) {
        for (int r = 0; r < FIXED_H; r++) {
            for (int c = 0; c < FIXED_W; c++) {
                bytes[ci * FIXED_IN_CH_STRIDE + r * FIXED_W + c] =
                    (uint8_t)(int8_t)(ci * 10 + r * 3 + c);
            }
        }
    }

    static ap_uint<32> in_mem[total_bytes / 4];
    for (int w = 0; w < total_bytes / 4; w++) {
        ap_uint<32> wd = 0;
        for (int b = 0; b < 4; b++) {
            wd.range(b * 8 + 7, b * 8) = bytes[w * 4 + b];
        }
        in_mem[w] = wd;
    }

    hls::burst_maxi<ap_uint<32> > in_burst(in_mem);
    static act_t row_buf[MAC_PR][MAX_CIN_TIMES_W];
    for (int i = 0; i < MAC_PR; i++)
        for (int j = 0; j < MAX_CIN_TIMES_W; j++)
            row_buf[i][j] = (act_t)0x55;  // poison before dispatch

    row_read_dataflow_fixed(in_burst, row_buf);

    int mismatches = 0;
    for (int rr = 0; rr < MAC_PR; rr++) {
        for (int ci = 0; ci < FIXED_CIN; ci++) {
            for (int c = 0; c < FIXED_W; c++) {
                int8_t expect = (int8_t)(ci * 10 + rr * 3 + c);
                int8_t got = (int8_t)row_buf[rr][ci * FIXED_W + c];
                if (got != expect) {
                    mismatches++;
                    if (mismatches <= 5) {
                        printf(">>> MISMATCH rr=%d ci=%d c=%d: got=%d expect=%d\n",
                               rr, ci, c, (int)got, (int)expect);
                    }
                }
            }
        }
    }
    int total_checked = MAC_PR * FIXED_CIN * FIXED_W;
    printf(">>> row_read_dataflow_fixed: %d/%d mismatches\n", mismatches, total_checked);
    printf(">>> %s\n", mismatches == 0 ? "PASS (byte-exact)" : "FAIL");
    return mismatches == 0 ? 0 : 1;
}
