/*============================================================
 * fastvit_ip.h
 * Unified FastVIT accelerator IP — merges conv_ip, dwconv_ip,
 * pwconv_ip, add_ip, gelu into ONE HLS top function dispatched by
 * op_code, sharing 4 m_axi masters instead of 17.
 *
 * (pool_ip / global_avgpool_ip are NOT merged: confirmed dead
 *  hardware today, fv_run_pool has zero call sites in
 *  fastvit_infer.c — SE-block GlobalAvgPool runs on ARM.)
 *
 * v1.5 dedicated-bundle experiment TRIED AND REVERTED (2026-07-31):
 * gave conv/dwconv/pwconv each their own m_axi bundles (15 masters
 * total) hoping to eliminate the op_code-gated FIFO fanout that
 * blocked 200MHz (WNS -2.87ns on v1.2, traced via report_timing to
 * op_code fanning into each shared master's auto-generated FIFO
 * adapter). Result: Vivado placement FAILED even at 100MHz —
 * 2007 control sets, 63643 LUT (119.6% of 53200 available), needing
 * 8152 more slices than available. This is essentially the same
 * failure signature as the pre-merge 5-IP/17-master design (v13/v14
 * era: 1961-1995 control sets, 85.86% LUT) — giving each op its own
 * masters undoes almost all of the LUT savings the 5-IP-to-1-IP merge
 * earned, regardless of how the masters are grouped onto PS7 HP ports.
 * Full independence (5-way -> 15 masters) is proven dead. Register-
 * replication of the op_code gating logic (MAX_FANOUT on op_code_read_reg
 * AND on ap_predicate_pred* candidates, 2026-07-31/08-01) is ALSO proven
 * dead: report_timing showed the actual bottleneck cell each run is an
 * auto-generated SRL-adapter internal (e.g. "mem_reg[68][0]_srl32_i_8__0")
 * whose name is not stable across synthesis runs, so no wildcard filter
 * can target it. See project notes for the full 7-attempt history.
 *
 * v1.8/v1.9 grouped-master experiment TRIED AND REVERTED (2026-08-01):
 * conv/dwconv/pwconv kept sharing gmem0-3 (3-way decode); add/gelu moved
 * to their own separate gmem4-6 group (2-way decode) on a second PS7 HP
 * port, hoping physical separation would help. Placed fine at 100MHz
 * (WNS +0.250ns, LUT 67.10%) but at 200MHz only reached WNS -2.792ns
 * (vs -2.873ns baseline, noise-level gain) at WORSE LUT (77.54%) -- and
 * the bottleneck simply moved into the 2-way-shared add/gelu group,
 * proving the op_code-fanout problem exists at ANY sharing level, not
 * just high sharing. Combined with v1.5's full-independence failure,
 * this closes the entire "adjust shared-master count" axis: 4 masters
 * shares-but-places, 7 shares-less-but-still-broken-and-costs-more,
 * 15 doesn't place at all. See project notes for the full 8-attempt
 * history. Do not re-try any master-count/grouping variant.
 *
 * Port sharing plan (v1.2, current):
 *   in_a  (pack_t, ap_uint<32>) : primary input feature map
 *                                 (conv/dwconv/pwconv feat_in, add feat_in1,
 *                                 gelu in)
 *   in_b  (wt_t,   ap_int<8>)   : weight (conv/dwconv/pwconv)
 *                                 or feat_in2 (add) -- wt_t and act_t
 *                                 are both ap_int<8>, so this port is
 *                                 reused unchanged, no repacking needed.
 *                                 Unused by gelu.
 *   bias  (acc_t,  ap_int<32>)  : bias (conv/dwconv/pwconv), unused by
 *                                 add/gelu
 *   out   (pack_t, ap_uint<32>) : output feature map (all ops)
 *
 * Only in_a/out need pack/unpack shims (conv/add are currently
 * byte-wide; dwconv/pwconv are already ap_uint<32>-native).
 *============================================================*/

#ifndef __FASTVIT_IP_H__
#define __FASTVIT_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;
typedef ap_uint<32> pack_t;

#define OP_CONV    0
#define OP_DWCONV  1
#define OP_PWCONV  2
#define OP_ADD     3
#define OP_GELU    4

#define ACT_NONE  0
#define ACT_RELU  1

/* ---- conv_worker tiling (from conv_ip.h, unchanged) ----
 * CONV_TN 2->4 was tried (2026-07-30) to eliminate the CHin=3-vs-
 * Tn_loops=2 zero-pad waste, but reverted: csim passed (never exercised
 * the CHin=3-specific partial-tn_valid path for TN=4 -- all tested
 * CHin values divided evenly into 4) yet the real board HUNG on the
 * exact production Stem-Conv call (CHin=3), reproducibly, via both the
 * full inference binary and a minimal standalone driver call. Likely
 * cause: WNS dropped from +0.163ns (TN=2 baseline) to a razor-thin
 * +0.074ns under the wider TN=4 datapath. Do not retry TN=4 without
 * first adding a csim test for CHin=3 specifically and recovering
 * timing margin. */
#define CONV_TN  2
#define CONV_TM  2
#define CONV_TR  4
#define CONV_TC  4
#define CONV_K   3
#define CONV_IN_TILE_H ((CONV_TR-1)*2 + CONV_K)   /* = 9 */
#define CONV_IN_TILE_W ((CONV_TC-1)*2 + CONV_K)   /* = 9 */

/* ---- dwconv_worker tiling (from dwconv_ip_v12_backup.h, unchanged) ---- */
#define DW_TN    1
#define DW_TR    4
#define DW_TC    4
#define DW_MAX_K 7
#define DW_MAX_CH 512
#define DW_MAX_H  64
#define DW_MAX_W  64
#define DW_MAX_IN_TILE_H ((DW_TR - 1) * 2 + DW_MAX_K)  /* = 13 */
#define DW_MAX_IN_TILE_W ((DW_TC - 1) * 2 + DW_MAX_K)  /* = 13 */

/* ---- pwconv_worker tiling (from pwconv_ip.h, unchanged) ---- */
#define PW_TM 8
/* PW_TN=4 (original): each (tm,tn) weight-tile switch carries a ~481-cycle
 * fixed handshake cost (measured via pw_sweep, 2026-07-29 -- solved
 * C_wt+C_ts=801.2 at Ts_loops=1 vs C_wt+64*C_ts=20996 at Ts_loops=64,
 * giving C_ts~=320.6, C_wt~=480.7 cycles). That cost does NOT amortize
 * when Ts_loops==1 (spatial<=PW_TS, e.g. Stage4 8x8=64), which is exactly
 * why Stage4's PW1/PW2 were the single most expensive ops in the network
 * (~100ms each) even after the PW_TS fix. Bumped to 8 (matching PW_TM) to
 * halve Tn_loops and hence halve the total (tm,tn)-switch overhead.
 *
 * (2026-07-31/08-01: tried reverting to 4 as a 200MHz resource-pressure
 * experiment -- pwconv_worker's LUT/DSP did drop as predicted (LUT
 * 24,802->18,425 -26%, DSP 61->45 -26%, confirmed via clean HLS rebuild),
 * but the real Vivado 200MHz WNS barely moved: -2.873ns (PW_TN=8) vs
 * -2.858ns (PW_TN=4) -- a 0.015ns difference, i.e. noise. report_timing
 * showed the IDENTICAL critical-path signature both times (op_code_read_reg
 * -> gmem3 store_unit FIFO predicate logic -> gmem0 load_unit FIFO SRL,
 * 81% route delay). This is a clean, decisive negative result: the 200MHz
 * bottleneck is genuine routing-distance/architecture, not a function of
 * overall device utilization pressure -- freeing area elsewhere in the
 * fabric does not shorten this specific net. Reverted back to 8 (the
 * validated 100MHz production value, matches the deployed board) since
 * there is no longer any open question this experiment was testing. Do
 * not retry "shrink some other worker to relieve congestion" as a 200MHz
 * strategy -- it has now been tested and falsified with real hardware
 * numbers, not just theory. See fastvit_ip.h/memory for the only untested
 * direction left: per-op_code local register replication for the shared
 * m_axi FIFO gating logic itself (LUT/FF for wire distance), which is an
 * RTL-level change, not a tiling-parameter change. */
#define PW_TN 8
/* PW_TS=8 (original) meant each (tm,tn,ts) tile call did only 8 pixels of
 * real work per LOAD_PW_IN/COMPUTE_PW_S pipeline launch, so the fixed
 * per-launch fill/drain + FSM handshake overhead (measured ~132 cycles/tile
 * vs ~16 cycles of nominal work -- an 8x gap, via pw_sweep on real hardware,
 * 2026-07-29) dominated. Bumped to 64 to amortize that overhead over 8x
 * more useful cycles per launch, without touching TM>TN>TS loop order or
 * accumulation logic (already the site of two real correctness bugs). */
#define PW_TS 64
/* Max spatial extent (H*W) across all FastVIT-T8 layers -- 64x64 (stem/Stage1).
 * pw_out_buf is sized to this so weights can be loaded once per (tm,tn) and
 * reused across the ENTIRE spatial range (weight-stationary GEMM blocking),
 * instead of once per (tm,ts,tn) which was ~25x slower in practice. */
#define PW_MAX_SPATIAL 4096

/* A whole-tensor input cache (PW_CACHE_BUDGET_BYTES) was tried here on
 * 2026-07-30 to cut redundant per-tm DRAM re-reads on high-Tm_loops
 * layers (Stage3/4 PW1/PW2). It passed csim but was REVERTED after board
 * measurement showed a net regression: the cache stored the input
 * unpacked (1 byte/BRAM word), so its LOAD_PW_IN read path cost 4x more
 * cycles per (tm,tn) tile than the packed 4-bytes/cycle DRAM burst path
 * it replaced, and Stage3/4's large Tm_loops*Tn_loops made that overhead
 * outweigh the DRAM-read savings (Stage4 PW1/PW2 got 33% slower). See
 * pwconv_worker.cpp's header comment for the full writeup -- worth
 * retrying with a packed (4-bytes/word) cache layout if this is revisited. */

/*============================================================
 * Unified top-level function
 *
 * Register layout: common header (op_code, CHin/Hin/Win/CHout,
 * act_mode, out_shift) always written by the driver; tail fields
 * (stride/pad/Kh/Kw/fpg) only meaningful for the ops that use them.
 *============================================================*/
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
    int     fpg
);

#endif // __FASTVIT_IP_H__
