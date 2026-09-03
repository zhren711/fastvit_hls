A3 mac_array_top bitstream -- PW chunked weight loading (option b, all 26 PW layers cached)
=============================================================================================

Archived 2026-09-03. See ZHR-92 (Linear) for the full round-by-round writeup (including the
3-round shared-multiplier regression chase), ZHR-63 for the mainline latency-line summary.
Replaces the deployed baseline (mac_array_a3_pw_weight_hoist, 2026-09-02) as this line's new
reference build -- real board and real P&R verified.

What changed
------------
The prior baseline's 144KB pw_weight_cache covered only 22 of 26 real PW layers; the 4 layers
whose weight exceeds 144KB (layer_0043/44/47/48_pwconv) fell back to a direct-DRAM-read path
that re-read weight redundantly on every spatial tile (up to 341.3x redundancy). A real board
probe (2026-09-02, ZHR-92) measured these 4 layers' own weight-read cost at 351.44ms combined
(78.7% of their own 446.42ms) -- a real, substantial, uncaptured opportunity.

This round replaces the single-cutoff cache with PW_WCHUNK: an outer loop (wrapping the entire
(rt,colt) spatial sweep in run_layer) that loads the weight cache in <=144KB CHUNKS instead of
one all-or-nothing load. Every real PW layer is now served via the SAME 144KB on-chip cache --
the direct-DRAM-read fallback is dead code on any constructible shape (kept, not deleted, per
this project's convention for prior dead-but-kept fallbacks) but no longer a real dispatch path.

Chunk boundaries are always per-ot (never mid-row), confirmed against real descriptors:
layer_0043/44 split into 3 EVEN chunks (384/128 ot's each); layer_0047/48 split into 3 UNEVEN
chunks (384+384+192 / 153+153+78 ot's) -- the uneven case is the trickiest correctness path and
is explicitly covered by pw_weight_hoist_tb.cpp's own case6.

Real P&R timing history (this is the headline finding of this round, not just an implementation
note): the first two chunking attempts REGRESSED real timing hard, both traced to this project's
own previously-documented "shared multiplier bound to FSM state" mechanism, but via two
DIFFERENT specific causes exposed in sequence:
  - Attempt 1: WNS=-2.264415ns. `ot_out_ch_base = ot_start * d.out_ch_stride`, computed inside
    pw_flat_pipeline_impl's own init (once per spatial tile, up to 4x per chunk), bound into the
    shared mul_32s_32s_32_2_1 resource from inside the function's own separately-synthesized FSM.
  - Attempt 2 (after fixing attempt 1 via a loop-carried accumulator in run_layer's own
    PW_WCHUNK loop): WNS=-1.127835ns -- improved but still negative. The critical path had MOVED
    to run_layer's OWN top-level FSM, still sinking into the same shared multiplier, but via a
    DIFFERENT operation (pw_flat_pipeline_impl's pre-existing `total_iters` computation) --
    PW_WCHUNK's own new loop-nesting level had itself restructured run_layer's FSM enough to
    change its competition for the shared resource, independent of attempt 1's specific fix.
  - Attempt 3 (hoisted total_iters to PW_WCHUNK too, same accumulator technique): **WNS=
    +0.133715ns -- closed.** Bind-database check confirmed zero Multiplier-core operations
    remain in either pw_flat_pipeline_impl instance.
Both accumulator fixes step by each chunk's REAL (possibly clamped) count, not the nominal
chunk size -- deliberately robust to the uneven last chunk by construction, not by relying on
"only the last chunk is ever partial" holding at every future call site.

Real P&R (no pblock, route_design alone, no phys_opt needed)
--------------------------------------------------------------
  WNS:  +0.133715ns             (was +0.153200ns -- still positive, closed)
  LUT:  32,664/53,200 (61.40%)  (was 31,953/53,200, 60.06%, +711/+1.34pp)
  BRAM: 74/140 tiles (52.86%)   (was 74/140, 52.86% -- EXACTLY unchanged)
  DSP:  50/220 (22.73%)         (was 52/220, 23.64% -- actually LOWER)

Board verification (2026-09-03, this round)
---------------------------------------------
Single-op, byte-exact, all PASS vs csim reference:
  - The 4 previously-fallback layers (entry64/66/70/72, layer_0043/44/47/48_pwconv): all
    byte-exact, 0 mismatches each. Combined time: 446.42ms -> 143.36ms (-67.9%), somewhat above
    the ~114ms model estimate but a massive real improvement either way.
      entry64 (L43): 120.10 -> 33.46ms
      entry66 (L44): 123.18 -> 44.15ms
      entry70 (L47): 100.49 -> 29.13ms
      entry72 (L48): 102.65 -> 36.62ms
  - Degenerate-path regression check (hard condition -- 22/26 real PW layers are degenerate,
    any regression here would be amplified): entry3 (cin=cout=48) unchanged at 22.67ms (vs the
    prior baseline's 22.66ms, noise-level). entry60 (layer_0040_pwconv, the EXACT 144KB cutoff
    boundary layer, first time board-tested with a real-chain bundle) byte-exact, 11.89ms.

Full network (82 entries, board_test_full_network bundle):
  - 82/82 entries written, no timeouts, no hangs.
  - PL-side total: **1,806.45ms** vs. baseline's 2,111.27ms -- -304.82ms (-14.4%), inside the
    pre-registered 1,750-1,820ms "model holds" band.
  - Cumulative from this whole latency-optimization line's original starting point (6,050ms):
    **-70.1%**.
  - Six-checkpoint correctness (primary judge, per this project's own standing process rule):
    cosine similarity vs `ckpt_ref_*_0000.npy` (the ONNX float32 reference) for stage1/stage2/
    stage3/stage4/finaldw/se: **0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811** -- EXACT
    match to the project's own long-established figures (4 decimal places). Chunking does not
    change numeric semantics; this confirms it.
  - `ckpt_hw_*` byte-exact-vs-csim (secondary, same-round-regression-only signal): non-zero
    mismatch counts, consistent with the already-documented, not-yet-root-caused staleness in
    `ckpt_hw_*`'s own generation (see CLAUDE.md's "Known open issues") -- not a correctness
    concern for this build specifically.

Golden rollback image /lib/firmware/fastvit_bd_wrapper.bin was NOT touched (md5
7ee26f67a1fca38a2752e99cf0bac25b, verified before and after this round's deployment).

Source
------
fastvit_ip_v2/mac_array_raster_integrated.cpp (PW_WCHUNK, pw_ot_per_chunk/pw_n_chunks,
  pw_w_chunk_off/pw_ot_out_ch_base accumulators, pw_flat_pipeline_impl's ot_start/ot_count/
  ot_out_ch_base_init/total_iters_in parameters -- see the source's own header comments at
  PW_WEIGHT_CACHE_ELEMS and pw_flat_pipeline_impl's declaration for the full design rationale
  and the two-round shared-multiplier diagnosis)
fastvit_ip_v2/mac_array_ckpt_dump.cpp, fastvit_ip_v2/run_ckpt_dump.tcl (dump range extended to
  i>=59, covering the new entry60/layer40-boundary degenerate test)
fastvit_ip_v2/pw_weight_hoist_tb.cpp (6 cases redesigned for the chunked mechanism: degenerate
  x2 incl. exact boundary, chunked-even x2, degenerate-narrow x1, chunked-narrow-uneven x1)
tools/build_single_op_test_entry60.py (new single-op bundle builder for the 144KB boundary
  degenerate layer, using freshly-dumped real-chain entry_59/60.bin)

HLS solution: fastvit_ip_v2/pwchunk3/s1 (csynth+export, 10ns/100MHz).
Vivado: vivado_impl/run_impl_bitstream_pwchunk3.tcl (P&R + bitstream in one script, route_design
  only, no phys_opt needed).

Files in this directory
------------------------
mac_array_bd_wrapper_pw_wchunk.bit          -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_pw_wchunk_swapped.bin  -- byte-swapped .bin, board-loadable (pulled
                                                 directly from the board's own /lib/firmware
                                                 after load via the board's fpga_overlay.py
                                                 Overlay class, not locally re-derived)

Deployed to board as /home/root/fpga/pwchunk3.bit.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_pwchunk3.tcl (fresh project name each time, per
   this project's export_design stale-cache discipline)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_pwchunk3.tcl -nolog -nojournal
   (P&R + bitstream in one script; writes .bit directly, no separate bitstream-only step needed
   since no phys_opt was required)
3. Board load: the board's own /home/root/fpga/fpga_overlay.py Overlay class does its own
   byte-swap + /lib/firmware deploy from the raw .bit -- no separate local byte-swap step needed.
4. ARM test driver: register map is UNCHANGED from mac_array_a3_pw_weight_hoist (PW_WCHUNK is
   entirely internal to the IP body, not a new register) -- the existing compiled
   /home/root/mac_array_single_op_test and /home/root/mac_array_full_network_test binaries can
   be reused as-is, no rebuild needed.
