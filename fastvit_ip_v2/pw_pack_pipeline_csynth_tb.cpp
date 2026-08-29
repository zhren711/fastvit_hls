// pw_pack_pipeline_csynth_tb.cpp -- minimal tb so csynth has something to
// compile/link against; csynth doesn't need it to run meaningfully.
#include "pw_pack_pipeline.h"

static wt_t w_base[2000];
static act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC];
static acc_t pw_bias_cache[MAX_PW_BIAS_CACHE];
static wt_t pw_shift_cache[MAX_PW_BIAS_CACHE];
static act_t out_base[2000];

int main() {
    LayerDescV2 d{};
    d.cin = 48; d.cout = 8;
    d.w_off = 0; d.out_off = 0; d.out_ch_stride = 1; d.w_out = 1;
    d.use_shift_table = 1; d.out_shift = 4;
    pw_flat_pipeline_packed(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, 0, 0, 1, 1);
    pw_flat_pipeline_packed_fixed(w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base);
    return 0;
}
