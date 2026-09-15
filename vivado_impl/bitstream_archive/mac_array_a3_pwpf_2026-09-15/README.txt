mac_array_a3_pwpf -- deployed baseline as of 2026-09-15 (ZHR-92)
=================================================================

Supersedes mac_array_a3_pwdefer (same day: ~496ms, PW 307ms, WNS +0.292ns).

What changed: PW_ROWREAD_PREFETCH (commit fecf979 + default flip this round).
After PW_DEFER_WRESP, the real-board fit put PW at 1.12 cycles/PW_FLAT
iteration + 71 cycles per ROW_READ request (179,904 requests, 128ms, ~42%
of PW) + 1.05 cycles/weight byte -- each request's own latency was paid
serially (request, wait, read w/4 words, next). Now the requests are issued
PW_ROWREAD_PF = 8 channels AHEAD: a ROW_READ_PRIME loop issues the first 8
before ROW_READ_CH, and after each channel's fill the request for ci+8 is
issued; read() is unchanged inside ROW_READ_FILL at II=1. PF sized from the
exported gmem_act adapter (NUM_READ_OUTSTANDING 16, request FIFO 16,
load-unit read buffer 16*16 = 256 words; the adapter issues an AR only with
buffer credit, so a full buffer stalls AR issue and cannot deadlock while
our own outstanding count stays <= 16): 8 in flight, <= 128 words buffered,
prefetch distance >= 56 cycles even for w=8 rows. Request-side addresses are
the same accumulator arithmetic as the fill side -- no queue, no new
multiply (binding DB: run_layer still has only the 4 narrow per-chunk
multiplies, zero 32x32). NOT the 2026-08-29 ROW_READ DATAFLOW split (that
overlapped a 4% component; this one is 42%). PW_ROWREAD_PREFETCH_OFF reverts.

Files
-----
  mac_array_bd_wrapper_pwpf.bit           raw, md5 94d44ccc0e07a76daed363643b34f9d9
  mac_array_bd_wrapper_pwpf_swapped.bin   board-pulled /lib/firmware/pwpf.bin, md5 4ec76c227343369091061c792d49ae90
  timing_wide_pwpf.rpt, utilization_pwpf.rpt

Deployed on the board as /lib/firmware/pwpf.bin (raw .bit at
/home/root/fpga/pwpf.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.131472ns   (pwdefer +0.292 -- placement roll on the route-dominated
                       population; see the timing report)
  LUT:  44,230/53,200 (83.14%)   (pwdefer 43,909: +321; isolated said +722)
  BRAM: 107/140, DSP 32/220     (flat)

csim: six suites, with the flag and flag-less: raster 5/5, wiring 4/4,
gelu_add 8/8, scalar_ops 4/4, pw_weight_hoist 6/6, pw_scaling_probe 20/20.

Board (2026-09-15), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry3 (PW-only) first, 3 runs: byte-exact, 7.5 -> 5.63 / 5.97 / 6.23ms (-21%).
   The INDEPENDENT prediction (the chunked big-cin entries, which barely moved
   under PW_DEFER_WRESP because they are ROW_READ-request dominated, should be
   the biggest movers now) held: entry66 (cin 1152) 29.1 -> 17.5ms (-40%),
   entry72 (cin 960) 24.9 -> 15.3 (-39%), entry64 17.3 -> 12.9 (-25%),
   entry70 15.2 -> 11.7 (-23%), entry60 5.4 -> 4.9. All byte-exact.
2. Controls entry5_dw 4.46ms / SE ops / entry0 GELU / entry10 ADD byte-exact, flat.
3. Full network 82/82, two runs: PL total 419.10 / 415.99ms (pwdefer ~496: -16%);
   per-entry sums 395.7 / 395.7; all 7 checkpoint files MD5-identical.
   PWCONV 231.25 / 231.17ms (-24.8%; pre-registered 190-220 "works" / 250-280
   "partial" -> landed between); DWCONV 115.6, GELU 22.9, ADD 13.1, SE 12.9
   (flat). ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

Refit on this run (R^2 0.989): PW 231ms = 1.08 cycles/PW_FLAT iteration
(141ms; floor 131) + 34.0 cycles per ROW_READ request (61ms; was 71) + 0.91
cycles/weight byte (26ms). The request term HALVED, not vanished: the
residual ~34 cycles/request (32-47 across layers, independent of cin) matches
csynth's ROW_READ_CH body latency of 24-31 -- ROW_READ_FILL runs its
compile-time MAX_WORDS_PER_CH = 17 iterations regardless of n_words (2-16),
plus fill/drain and loop control. That residual is loop overhead, not AXI
latency -- a separate, smaller lever (~61ms).

Cumulative on this latency line: 6,050ms -> ~418ms = -93.1%.
Operator split now: PW 55%, DW 28%, GELU 5.5%, ADD 3.1%, SE 3.1%.

Register map: UNCHANGED (hw.h identical). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_pwpf.tcl (built with -DPW_ROWREAD_PREFETCH;
   since the default flip the flag-less build is the same design -- verified)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_pwpf.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
