#ifndef __DSCONV_FUSED_WORKER_H__
#define __DSCONV_FUSED_WORKER_H__

#include <ap_int.h>

/* Self-contained header (isolated feasibility study, see plan
 * C:\Users\zhren\.claude\plans\peppy-gathering-ripple.md), not wired
 * into fastvit_ip's shared-master top level. Types match
 * fastvit_ip_w8a4/fastvit_ip.h's W8A4 typedefs exactly (drop-in
 * DDR-layout compatible with fastvit_ip_w8a4/dwconv_worker.cpp and
 * pwconv_worker.cpp) but this header doesn't include fastvit_ip.h, to
 * avoid a build dependency on the other workers' unrelated constants. */
typedef ap_int<4>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;
typedef ap_uint<32> pack_t;

#define ACT_NONE 0
#define ACT_RELU 1

/* DW-side bounds -- same as dwconv_linebuf/dwconv_worker.h. */
#define DW_MAX_K  7
#define DW_MAX_CH 512
#define DW_MAX_H  64
#define DW_MAX_W  64

/* PW-side tiling -- same values as fastvit_ip_w8a4/fastvit_ip.h's
 * PW_TM/PW_TN (CHout/CHin tile widths for the 8x8 MAC array), but here
 * there is no PW_MAX_SPATIAL/whole-image accumulator: pwconv_worker's
 * spatial dimension is replaced by a bounded on-chip patch instead
 * (see dsconv_worker.cpp's header comment for why). */
#define PW_TM 8
#define PW_TN 8
/* Real PW1(expand) CHout goes up to 1152 (Stage4) -- deliberately NOT
 * bounded by DW_MAX_CH=512 (that's a DW-side/CHin bound only). Used
 * only for m_axi depth= sizing below, not for any on-chip array (the
 * fused design's PW-side buffers are all patch/tile-sized, not
 * CHout-sized). */
#define PW_MAX_CHOUT 1536

/* Spatial patch bound: DW is computed exactly once per (patch,channel)
 * and cached on-chip in dw_patch_buf before any PW output-channel tile
 * sweeps it -- this is what keeps the fused accumulator bounded instead
 * of spanning the whole feature map like pwconv_worker's pw_out_buf.
 * Runtime pr=pc=min(DS_PATCH_MAX,Hout); real FastVIT-T8 RepMixer shapes
 * (H=W in {64,32,16,8}) all divide evenly by this. */
#define DS_PATCH_MAX 16

/* 2026-08-11 round-3 update: measured DRAM-traffic accounting (see
 * plan) found patch-tiling forces BOTH dw_weight and pw_weight to
 * reread once per patch instead of once per call -- a NET LOSS for
 * large-Npatches ("large resolution") blocks like Stage1/Stage2
 * (small CHin, large spatial -> many patches), even though the same
 * fusion is a clean win for Stage3/Stage4 (large CHin, small spatial
 * -> Npatches=1, no reread at all). Fix: cache weights on-chip ONCE
 * per call instead of DMA-rereading per patch -- see dsconv_worker.cpp
 * for the two caches (dw_wt_cache, always; pw_wt_cache, only when it
 * fits). DW_WT_CACHE_BYTES=DW_MAX_CH*DW_MAX_K*DW_MAX_K is small (~25KB)
 * and always affordable. PW_WT_CACHE_MAX is deliberately NOT sized to
 * cover every real shape (Stage3/4's weight matrices reach 360KB,
 * too much BRAM to keep resident) -- it only needs to cover the
 * large-Npatches shapes this fix targets (Stage1 CHout*CHin<=6912,
 * Stage2<=27648), which are inherently small since large CHout*CHin
 * only occurs in this network where CHin is also large (Stage3/4),
 * and those blocks don't need this cache anyway (Npatches=1 already
 * means zero reread cost). Falls back to per-patch DMA reload
 * (harmless when Npatches=1, since "per patch" == "once" there) when
 * a shape doesn't fit -- see dsconv_worker.cpp's use_pw_cache guard. */
#define PW_WT_CACHE_MAX 32768

/* Grouped-patch weight reuse ("scheme 3": fused-layer/pyramid tiling,
 * Alwani et al. MICRO'16 "Fused-Layer CNN Accelerators"). PW_WT_CACHE_MAX
 * above already eliminates PW's per-patch DRAM re-read whenever
 * CHout*CHin fits in 32768 elements (Stage1/2) or Npatches==1
 * (Stage3/4) -- by this network's own channel/spatial trade-off (large
 * CHout*CHin only occurs where CHin is also large, which only happens
 * at small spatial size / Npatches==1), NONE of FastVIT-T8's real
 * RepMixer blocks actually hit the remaining gap (CHout*CHin>32768 AND
 * Npatches>1 simultaneously) -- see dsconv_worker.cpp's use_pw_cache
 * comment. PATCH_GROUP covers that gap anyway, for robustness against
 * future shapes/networks that might: dsconv_worker groups PATCH_GROUP
 * patches together and loads each PW weight tile ONCE per group instead
 * of once per patch (LOAD_PW_WT hoisted above the per-patch compute),
 * amortizing the fallback DMA re-read by up to PATCH_GROUP x instead of
 * eliminating it entirely (full elimination would need one weight tile
 * load per CALL, which needs ALL patches' DW output resident on-chip
 * simultaneously -- back to the whole-image accumulator this fused
 * design was built specifically to avoid, see dsconv_worker.cpp's
 * header comment on pw_acc's 2.25MB/~3.5x-budget rejection).
 * Deliberately kept SMALL (2, not e.g. 8) because dw_patch_buf and
 * pw_acc both become PATCH_GROUP x their single-patch size -- HLS
 * statically allocates arrays at their declared max size regardless of
 * runtime CHin/Npatches, so this is a permanent BRAM tax paid by EVERY
 * shape (including S1-S4, none of which need it) in exchange for a
 * benefit that only a hypothetical future shape would ever see.
 * Verified via csim only so far (a synthetic shape added specifically
 * to exercise the CHout*CHin>32768-AND-Npatches>1 gap, since no real
 * FastVIT-T8 shape does) -- NOT yet run through place & route, pending
 * the round-5/6 WRITEBACK_PW timing-recovery work concluding first, so
 * as not to add BRAM pressure to an already-tight (58% BRAM at round 5),
 * still-unresolved design mid-campaign.
 * (Briefly set to 1 for a round-6 P&R attempt that was reverted before
 * reaching P&R -- see dsconv_worker.cpp's pw_acc comment -- restored to
 * 2 here since that attempt's isolation is no longer needed.) */
#define PATCH_GROUP 2

/* Targets the ONE zero-intervening-op DW->PW adjacency in the real
 * network: each RepMixer block's trailing DW7 -> PW1(expand), always
 * stride=1, fpg=1, act_mode=ACT_NONE both sides. No stride_h/w or fpg
 * parameters -- this link never uses them (see plan for the driver
 * trace that established this). Kh/Kw stay runtime parameters (real
 * usage always passes 7) purely so the testbench can assert the
 * invariant rather than bake it in silently. */
void dsconv_worker(
    pack_t dw_feat_in[],    /* CHin x Hin x Win, nibble-packed */
    wt_t   dw_weight[],     /* CHin x Kh x Kw (depthwise, one filter/ch) */
    acc_t  dw_bias[],       /* CHin */
    pack_t dw_feat_out[],   /* CHin x Hout x Wout -- RAW DW copy, for the
                              * block's later residual Add (see design
                              * note in dsconv_worker.cpp: this must be
                              * written exactly once per pixel) */
    wt_t   pw_weight[],     /* CHout x CHin */
    acc_t  pw_bias[],       /* CHout */
    pack_t pw_feat_out[],   /* CHout x Hout x Wout */
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    pad_h,
    int    pad_w,
    int    CHout,
    int    dw_act_mode,
    int    dw_out_shift,
    int    pw_act_mode,
    int    pw_out_shift
);

#endif // __DSCONV_FUSED_WORKER_H__
