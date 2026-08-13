#include "dsp_pack_design.h"
#include <cstdio>

int main() {
    int errors = 0;
    long total = 0;
    for (int fi0 = -128; fi0 < 128; fi0++) {
        for (int fi1 = -128; fi1 < 128; fi1++) {
            for (int w = -128; w < 128; w++) {
                total++;
                acc_t out0, out1;
                dsp_packed_mac2_top((act_t)fi0, (act_t)fi1, (wt_t)w, out0, out1);
                int ref0 = fi0 * w;
                int ref1 = fi1 * w;
                if ((int)out0 != ref0 || (int)out1 != ref1) {
                    errors++;
                    if (errors <= 30)
                        printf("MISMATCH fi0=%d fi1=%d w=%d | out0=%d ref0=%d | out1=%d ref1=%d\n",
                               fi0, fi1, w, (int)out0, ref0, (int)out1, ref1);
                }
            }
        }
    }
    printf("=== total=%ld errors=%d ===\n", total, errors);
    return errors > 0 ? 1 : 0;
}
