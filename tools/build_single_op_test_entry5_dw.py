"""
build_single_op_test_entry5_dw.py -- A3 end-to-end step 1 (ZHR-92,
2026-08-22): package entry[5] (DWCONV, K=3 S=1, real layer_0003_dwconv
shape) into a compact single-op board test bundle. Reuses already-dumped,
already-csim-verified real chain data: entry[5]'s real input is
entry[4]'s GELU output (g_hw_seq[4].out_off=786432 == g_hw_seq[5].in_off) --
NOTE: an earlier version of this script incorrectly used entry_02.bin
(entry[2] also writes out_off=786432, but entry[2] is NOT the entry
immediately preceding entry[5] in the chain -- entry[4] is. Caught via a
csim-only bundle-verification tool after a board test failed with
175,877/196,608 mismatches; the bug was in this script, not hardware.),
reference is entry_05.bin, both from the earlier entry-by-entry round.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry5_dw")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

# entry[5] real fields, verbatim from mac_array_ckpt_desc.h's g_hw_seq[5]
# + set_shift_table_fields()'s entry.
OP_DWCONV = 0
cin, cout = 48, 48
h_in, w_in = 64, 64
k, stride, pad, fpg = 3, 1, 1, 1
real_w_off, real_b_off, real_shift_off = 2736, 96, 3014640  # b_off is element offset

W_SLICE_BYTES = cout * fpg * k * k        # 48*9 = 432
SHIFT_SLICE_BYTES = cout * fpg             # 48
B_SLICE_ELEMS = cout * fpg                 # 48

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

w_base_buf = w_slice + shift_slice
shift_off_relocated = W_SLICE_BYTES

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_04.bin"), "rb") as f:
    in_buf = f.read()
assert len(in_buf) == cin * h_in * w_in, f"entry_04.bin size {len(in_buf)} != {cin*h_in*w_in}"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_05.bin"), "rb") as f:
    ref_out = f.read()

# derive_mac_array_params(), ported verbatim from mac_array.cpp.
h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
ch_dim = cin  # DW: n_ch_tiles keyed off Cin (cin==cout for depthwise)
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (ch_dim + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = ch_dim - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out

assert h_out == 64 and w_out == 64
assert len(ref_out) == cout * fpg * h_out * w_out

fields = [
    OP_DWCONV, cin, cout,
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

print(f">>> desc.bin: {len(desc_bytes)} bytes")
print(f">>> in.bin: {len(in_buf)} bytes (cin={cin} h={h_in} w={w_in})")
print(f">>> w.bin: {len(w_base_buf)} bytes ({W_SLICE_BYTES} weight + {SHIFT_SLICE_BYTES} shift)")
print(f">>> b.bin: {len(b_slice)} bytes")
print(f">>> ref_out.bin: {len(ref_out)} bytes")
print(f">>> h_out={h_out} w_out={w_out} n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} "
      f"n_ch_tiles={n_ch_tiles} last=({last_row_tile},{last_col_tile},{last_ch_tile}) "
      f"in_ch_stride={in_ch_stride} out_ch_stride={out_ch_stride}")
print(f">>> bundle written to {OUT_DIR}")
