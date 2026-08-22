"""
build_single_op_test_entry10_add.py -- A3 end-to-end step 1 (ZHR-92,
2026-08-22): package entry[10] (ADD -- residual add, the priority op this
round since defect-5's root cause is still unknown and can only be
exposed on real hardware) into a compact single-op board test bundle.

Reuses already-dumped, already-csim-verified real chain data from the
earlier entry-by-entry diagnostic round -- no new reference generation
needed. Entry[10]'s real inputs are entry[5]'s DW output (op0, in_off)
and entry[9]'s PW output (op1, in2_off), traced from the real 82-entry
descriptor's own offsets (g_hw_seq[5].out_off=1572864 ==
g_hw_seq[10].in_off; g_hw_seq[9].out_off=786432 == g_hw_seq[10].in2_off).
Reference output is entry_10.bin, from the same real csim run.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry10_add")
os.makedirs(OUT_DIR, exist_ok=True)

# entry[10] real fields, verbatim from mac_array_ckpt_desc.h's g_hw_seq[10].
OP_ADD = 2
cin, cout = 48, 48
h_in, w_in = 64, 64
out_shift = 0

SIZE = cin * h_in * w_in  # 196608

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_05.bin"), "rb") as f:
    op0_buf = f.read()
with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_09.bin"), "rb") as f:
    op1_buf = f.read()
with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_10.bin"), "rb") as f:
    ref_out = f.read()
assert len(op0_buf) == SIZE and len(op1_buf) == SIZE and len(ref_out) == SIZE

# Relocated compact layout: op0 at 0, op1 right after -- separate regions,
# no aliasing. out_off relocated to a THIRD region (distinct from both
# inputs) so a silently-dropped write (defect-5's exact symptom) can't
# accidentally look like a pass by reading back stale input data.
IN_OP0_OFF = 0
IN_OP1_OFF = SIZE
OUT_OFF = 0  # separate out_base buffer on the board side, so 0 is fine here

fields = [
    OP_ADD, cin, cout,
    h_in, w_in,
    1, 1, 0,           # k, stride, pad (unused by ADD, kept structurally valid)
    1,                  # fpg (unused)
    out_shift,
    IN_OP0_OFF, 0, 0, OUT_OFF,   # in_off, w_off (unused), b_off (unused), out_off
    IN_OP1_OFF,          # in2_off
    0, 0,                # h_out, w_out (unused by run_add)
    0, 0, 0,             # n_row_tiles, n_col_tiles, n_ch_tiles (unused)
    0, 0, 0,             # last_row_tile, last_col_tile, last_ch_tile (unused)
    0,                   # use_shift_table (unused)
    0,                   # shift_off (unused)
    0, 0,                # in_ch_stride, out_ch_stride (unused)
]
assert len(fields) == 27, len(fields)
desc_bytes = struct.pack("<27i", *fields)

with open(os.path.join(OUT_DIR, "desc.bin"), "wb") as f:
    f.write(desc_bytes)
with open(os.path.join(OUT_DIR, "in.bin"), "wb") as f:
    f.write(op0_buf + op1_buf)  # single in_base buffer, op0 then op1
with open(os.path.join(OUT_DIR, "ref_out.bin"), "wb") as f:
    f.write(ref_out)

print(f">>> desc.bin: {len(desc_bytes)} bytes (27 int32 fields)")
print(f">>> in.bin: {len(op0_buf) + len(op1_buf)} bytes (op0 {len(op0_buf)} + op1 {len(op1_buf)})")
print(f">>> ref_out.bin: {len(ref_out)} bytes")
print(f">>> op_type=ADD cin={cin} h={h_in} w={w_in} in_off={IN_OP0_OFF} in2_off={IN_OP1_OFF} out_off={OUT_OFF}")
print(f">>> bundle written to {OUT_DIR}")
