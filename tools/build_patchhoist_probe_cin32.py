"""
build_patchhoist_probe_cin32.py -- A3 PW_PATCH_HOIST cost-vs-Cin probe,
cin=32 point (ZHR-92, 2026-08-24). Checks whether PW_PATCH_HOIST's own
per-(rt,colt) DRAM staging cost explains the ~6x per-tile gap found by
the tile-count scaling probe.

Hand math first (per-tile arithmetic, no measurement needed for this
part): PW_PATCH_HOIST's innermost loop is (ci<Cin, rr<MAC_PR), each
iteration issuing 1 (aligned) or 2 (misaligned) 32-bit word reads via
in_base_wide -- Cin*MAC_PR potential transactions per tile. For entry3's
cin=48: 48*4=192 transactions. At ~30 cycles/transaction (this
project's own real-measured short-AXI-transaction cost, from the
earlier n_cbase=36 glue-decomp round: 296ns/burst ~= 30 cycles), that's
5,760 cycles = 57.6us -- 7x short of the measured ~400us/tile. Either
the real per-transaction cost is much higher than 30 cycles, or
something else dominates. Don't guess -- measure whether PW_PATCH_HOIST
cost scales with Cin (tile count and cout held fixed) the same way the
tile-count probe isolated (rt,colt)-level cost from the flat-pipeline's
own modeled cost.

Shape: cin=32, cout=48, h_in=w_in=32 (SAME spatial size and tile count
as the tile-count probe's 32x32/64-tile point, for direct comparability
-- that point's own cin=48/n_cbase=2 result, 25.95ms/64=405.5us/tile, is
a third reference bracketing this experiment's two points).
  n_row_tiles=n_col_tiles=8 -> 64 tiles (same as build_tilecount_probe_32.py).
  n_cbase=ceil(32/32)=1 (single cbase -- kept deliberately at n_cbase=1
  vs. the cin=64 companion's n_cbase=2, NOT 48-vs-96's n_cbase 2-vs-3,
  so the flat-pipeline's own known per-cbase cost is trivial to subtract:
  going cin=32->64 adds exactly one extra 16-step cbase pass, a known,
  small, precisely computable delta (16 steps * 48 ot * 3 cycles(II=3)
  * 10ns = 23.04us total, NOT per-tile-count-scaling) -- distinct from
  whatever PW_PATCH_HOIST itself costs, which scales with Cin*MAC_PR
  transactions, not with n_cbase directly.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_patchhoist_cin32")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 32, 48
h_in, w_in = 32, 32
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 32,768 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)           # 1,536 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 192 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 32 and w_out == 32
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
assert n_row_tiles == 8 and n_col_tiles == 8 and n_cbase == 1 and cout == 48
print(f">>> total tiles = {n_row_tiles * n_col_tiles} (expect 64), n_cbase={n_cbase}")

ref_out = bytes([32]) * (cout * h_out * w_out)  # sum_{ci=0..31}(1*1)=32, hand-computed

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
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 32, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={cout} n_cbase={n_cbase}")
print(f">>> bundle written to {OUT_DIR}")
