"""
build_nsteps_probe_8.py -- A3 n_steps scaling probe, n_steps=8 point
(ZHR-92, 2026-08-23). Decisive, cheap experiment: does PW's marginal
per-cbase cost scale with n_steps (favoring array widening, MAC_PD
2->8/128-wide) or stay flat (favoring a run_layer rewrite instead)?

This uses the CURRENTLY DEPLOYED bitstream (pwstage, pre-WRITEOUT-fix) --
this experiment is entirely about the reduction stages (PW_WSTAGE/
GATHER_ALL_PW/UNIFIED), not WRITEOUT, so the WRITEOUT fix's LUT/burst
status is irrelevant here.

Shape: cin=16, cout=1000, h_in=w_in=4, k=1/stride=1/pad=0/fpg=1.
  n_row_tiles=n_col_tiles=1, n_cbase=ceil(16/32)=1 (single chunk, same
  "isolated one-pass" shape as the original 870-cycle measurement),
  n_steps=ceil(min(16,32)/2)=8 -- HALF of the original cin=32 point's
  n_steps=16. n_ot=cout=1000, matching the ORIGINAL 870-cycle point's
  own n_ot exactly for direct comparability.

  cout is deliberately kept <=MAX_PW_BIAS_CACHE (1152, mac_array.h) --
  an earlier attempt at cout=4000 overflowed the static pw_bias_cache
  array (PW_BIAS_HOIST writes pw_bias_cache[oc] for oc in [0,cout)),
  which wedged the IP outright (AP_CTRL stuck at 0x1, ap_start asserted
  and never returning to done/idle, confirmed via direct /dev/mem read
  -- not just a slow computation) and required a full bitstream reload
  to recover (MD5-verified against the archived golden copy afterward).
  Board-confirmed healthy again before this corrected version ran.

Deterministic all-1s data: output = sum_{ci=0..15}(1*1) = 16 per
element, no clipping, hand-computable.

Compare this point's cycles/ot against the existing cin=32/n_steps=16
point (870 cycles/ot, board_test_glue_isolation, already on record) to
see whether the marginal reduction cost scales with n_steps (linear ->
array widening pays off) or stays flat (-> it doesn't, run_layer's
sequential glue dominates regardless of array width).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_nsteps8")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 16, 1000
h_in, w_in = 4, 4
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 256 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)           # 64,000 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 16,000 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 4 and w_out == 4
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
n_steps = (min(cin, MAX_CIN_PW) + MAC_PD - 1) // MAC_PD
assert n_row_tiles == 1 and n_col_tiles == 1 and n_cbase == 1 and n_steps == 8

ref_out = bytes([16]) * (cout * h_out * w_out)  # 64,000 bytes, every byte = 16

fields = [
    OP_PWCONV, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    out_shift,
    0, 0, 0, 0,
    0,
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    0, 0,
    in_ch_stride, out_ch_stride,
]
assert len(fields) == 27, len(fields)
desc_bytes = struct.pack("<27i", *fields)

with open(os.path.join(OUT_DIR, "desc.bin"), "wb") as f:
    f.write(desc_bytes)
with open(os.path.join(OUT_DIR, "in.bin"), "wb") as f:
    f.write(in_buf)
with open(os.path.join(OUT_DIR, "w.bin"), "wb") as f:
    f.write(w_buf)
with open(os.path.join(OUT_DIR, "b.bin"), "wb") as f:
    f.write(b_buf)
with open(os.path.join(OUT_DIR, "ref_out.bin"), "wb") as f:
    f.write(ref_out)

print(f">>> desc.bin: {len(desc_bytes)} bytes")
print(f">>> in.bin: {len(in_buf)} bytes, w.bin: {len(w_buf)} bytes, b.bin: {len(b_buf)} bytes")
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 16, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={cout} n_cbase={n_cbase} n_steps={n_steps}")
print(f">>> bundle written to {OUT_DIR}")
