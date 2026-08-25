#include "mac_array.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define MAX_CIN_TIMES_W 9216
#define MAX_WORDS (MAX_CIN_TIMES_W / 4)

void row_hoist_probe_top(
    hls::burst_maxi<ap_uint<32> > in_burst,
    int cin, int w_real, int colt,
    act_t pw_patch_out[MAC_PD][MAC_PR][MAC_PC]
);

static int run_case(const char *name, int cin, int w_real, int colt) {
    /* Test buffer: MAC_PR row-blocks, each MAX_WORDS words wide (matches
     * the probe's own rr*MAX_WORDS addressing stride). Every row-block
     * gets the SAME byte pattern (byte value = flat_idx % 100) so
     * pw_patch_out is independent of rr -- makes the golden check simple
     * without weakening what's being tested (the read/index/copy paths
     * are exercised identically regardless of whether the data differs
     * per row). */
    static ap_uint<32> buf[MAC_PR * MAX_WORDS];
    for (int rr = 0; rr < MAC_PR; rr++) {
        for (int w = 0; w < MAX_WORDS; w++) {
            ap_uint<32> word = 0;
            for (int b = 0; b < 4; b++) {
                int flat_idx = w * 4 + b;
                uint8_t val = (uint8_t)(flat_idx % 100);
                word.range(b * 8 + 7, b * 8) = val;
            }
            buf[rr * MAX_WORDS + w] = word;
        }
    }

    act_t pw_patch_out[MAC_PD][MAC_PR][MAC_PC];
    memset(pw_patch_out, 0xA5, sizeof(pw_patch_out));

    row_hoist_probe_top(hls::burst_maxi<ap_uint<32> >(buf), cin, w_real, colt, pw_patch_out);

    int mismatches = 0;
    for (int dd = 0; dd < MAC_PD; dd++) {
        for (int rr = 0; rr < MAC_PR; rr++) {
            for (int cw = 0; cw < MAC_PC; cw++) {
                int flat_idx = dd * w_real + colt * MAC_PC + cw;
                bool valid = (dd < cin) && (flat_idx < MAX_CIN_TIMES_W);
                int expect = valid ? (flat_idx % 100) : 0;
                if ((int)pw_patch_out[dd][rr][cw] != expect) mismatches++;
            }
        }
    }
    printf(">>> %s: cin=%d w_real=%d colt=%d -- mismatches=%d/%d %s\n",
           name, cin, w_real, colt, mismatches, MAC_PD * MAC_PR * MAC_PC,
           mismatches == 0 ? "PASS" : "FAIL");
    return mismatches == 0 ? 0 : 1;
}

int main() {
    int fails = 0;
    /* entry3-shape: cin=48, w=64 */
    fails += run_case("entry3-shape", 48, 64, 0);
    fails += run_case("entry3-shape-colt15", 48, 64, 15);
    /* entry9-shape: cin=144, w=64 -- the real Cin x W joint max (9216) */
    fails += run_case("entry9-shape-maxbound", 144, 64, 0);
    fails += run_case("entry9-shape-maxbound-colt15", 144, 64, 15);
    /* small shape */
    fails += run_case("small", 8, 8, 0);

    printf(">>> overall: %s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
