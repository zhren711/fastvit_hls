// dw_raster_layer_csynth_tb.cpp -- minimal tb, only needed so the HLS
// project has something to compile/link against set_top; csynth doesn't
// require it to actually run meaningfully. Calls run_dw_layer_raster()
// once with a small real-shaped case (K3S1) purely so csim (if anyone
// runs it) doesn't crash on garbage pointers.
#include "dw_raster_layer.h"
#include <vector>

int main() {
    const int cin = 8, cout = 8, h = 16, w = 16, K = 3, S = 1, pad = 1, fpg = 1;
    std::vector<act_t> in_buf(cin * h * w, act_t(1));
    std::vector<wt_t>  w_buf(cout * K * K + cout, wt_t(1));
    std::vector<acc_t> b_buf(cout, acc_t(0));
    std::vector<act_t> out_buf(cout * h * w, act_t(0));
    run_dw_layer_raster(in_buf.data(), w_buf.data(), b_buf.data(), out_buf.data(),
                         cin, cout, h, w, K, S, pad, fpg,
                         0, 0, 0, 0, cout * K * K,
                         h * w, h * w, h, w);
    return 0;
}
