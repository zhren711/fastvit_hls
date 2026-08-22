"""
build_single_op_test_entry3.py -- A3 board bring-up step 1 (ZHR-92,
2026-08-21): package entry[3] (a PW conv -- the simplest op: no window
addressing, no fpg) from the real 82-entry hardware sequence into a
compact, self-contained single-op test bundle for the real board.

entry[3]'s real input is entry[2]'s real output (already dumped as
entry_02.bin by fastvit_ip_v2/mac_array_ckpt_dump.cpp's csim run through
the real chain -- NOT a synthetic float input, closing the exact
methodology gap this session flagged earlier: what a layer actually
receives, not an isolated true-float substitute). entry_03.bin is that
same csim run's real output for entry 3 -- the byte-exact reference this
board test must match, not just "did it run."

Relocates in_off/w_off/b_off/out_off to 0 within compact, single-purpose
buffers (this is ONE isolated op, not the full chain -- no need to ship
the whole ~3MB weight blob or replicate the real ~1.8MB activation
arena's offsets). The per-channel shift table is appended immediately
after the weight slice in the SAME w_base buffer (mac_array.h's own
convention: shift values live inside w_base, read via
w_base[shift_off+channel] -- shift_off is relocated to point right after
the relocated weights, not copied verbatim from the real sequence).

Descriptor field order/derivation is a direct, unmodified port of
mac_array.cpp's derive_mac_array_params() (MAC_PR=4, MAC_PC=4, MAC_PD=2)
-- ported here instead of hand-derived, so there is zero chance of a
by-hand tile-count mismatch against what csim actually used.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry3")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

# entry[3] real fields, verbatim from fastvit_ip_v2/mac_array_ckpt_desc.h's
# g_hw_seq[3] + set_shift_table_fields()'s g_hw_seq[3] entry.
OP_PWCONV = 1
cin, cout = 48, 48
h_in, w_in = 64, 64
k, stride, pad, fpg = 1, 1, 0, 1
real_w_off, real_b_off, real_shift_off = 432, 48, 3014592  # b_off is an
                                                             # INT32-element
                                                             # offset, not bytes

W_SLICE_BYTES = cout * cin * k * k          # 2304
SHIFT_SLICE_BYTES = cout                     # 48, one int8 per out channel
B_SLICE_ELEMS = cout                         # 48 int32 words

with open(os.path.join(ROOT, "fastvit_ip_v2", "ckpt_weights_flat.bin"), "rb") as f:
    w_flat = f.read()
with open(os.path.join(ROOT, "fastvit_ip_v2", "ckpt_bias_flat.bin"), "rb") as f:
    b_flat = f.read()

w_slice = w_flat[real_w_off: real_w_off + W_SLICE_BYTES]
shift_slice = w_flat[real_shift_off: real_shift_off + SHIFT_SLICE_BYTES]
assert len(w_slice) == W_SLICE_BYTES and len(shift_slice) == SHIFT_SLICE_BYTES

b_byte_off = real_b_off * 4
b_slice = b_flat[b_byte_off: b_byte_off + B_SLICE_ELEMS * 4]
assert len(b_slice) == B_SLICE_ELEMS * 4

# Compact w_base buffer: [weights][shift table] -- shift_off relocated to
# point right after the relocated weight slice.
w_base_buf = w_slice + shift_slice
shift_off_relocated = W_SLICE_BYTES

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_02.bin"), "rb") as f:
    in_buf = f.read()
assert len(in_buf) == cin * h_in * w_in, f"entry_02.bin size {len(in_buf)} != {cin*h_in*w_in}"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_03.bin"), "rb") as f:
    ref_out = f.read()

# ---- derive_mac_array_params(), ported verbatim from mac_array.cpp ----
h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
ch_dim = cin
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (ch_dim + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = ch_dim - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out

assert h_out == 64 and w_out == 64
assert len(ref_out) == cout * h_out * w_out

# ---- pack MacLayerDesc (mac_array_driver.h field order, 27x int32 LE) ----
fields = [
    OP_PWCONV, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    0,          # out_shift (unused, use_shift_table=1)
    0, 0, 0, 0,  # in_off, w_off, b_off, out_off -- all relocated to 0
    0,          # in2_off (unused)
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    1,          # use_shift_table
    shift_off_relocated,
    in_ch_stride, out_ch_stride,
]
assert len(fields) == 27, len(fields)
desc_bytes = struct.pack("<27i", *fields)

with open(os.path.join(OUT_DIR, "desc.bin"), "wb") as f:
    f.write(desc_bytes)
with open(os.path.join(OUT_DIR, "in.bin"), "wb") as f:
    f.write(in_buf)
with open(os.path.join(OUT_DIR, "w.bin"), "wb") as f:
    f.write(w_base_buf)
with open(os.path.join(OUT_DIR, "b.bin"), "wb") as f:
    f.write(b_slice)
with open(os.path.join(OUT_DIR, "ref_out.bin"), "wb") as f:
    f.write(ref_out)

print(f">>> desc.bin: {len(desc_bytes)} bytes (27 int32 fields)")
print(f">>> in.bin: {len(in_buf)} bytes (cin={cin} h={h_in} w={w_in})")
print(f">>> w.bin: {len(w_base_buf)} bytes ({W_SLICE_BYTES} weight + {SHIFT_SLICE_BYTES} shift, shift_off={shift_off_relocated})")
print(f">>> b.bin: {len(b_slice)} bytes ({B_SLICE_ELEMS} int32)")
print(f">>> ref_out.bin: {len(ref_out)} bytes (cout={cout} h_out={h_out} w_out={w_out})")
print(f">>> h_out={h_out} w_out={w_out} n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} "
      f"n_ch_tiles={n_ch_tiles} last=({last_row_tile},{last_col_tile},{last_ch_tile}) "
      f"in_ch_stride={in_ch_stride} out_ch_stride={out_ch_stride}")
print(f">>> bundle written to {OUT_DIR}")
