"""
build_dw_linebuf_realdata.py -- ZHR-92 Phase 1 Step 1 (2026-08-28): extract
real input/weight/bias/shift bundles for the 4 real K x S combinations
DW covers in the network (K3S1=layer_0003, K7S1=layer_0004, K3S2=layer_0001,
K7S2=layer_0011), for the dw_linebuf_probe2 real-data-flow correctness
check. All 4 real layers happen to share cin=48, letting the C++ harness
use a single MAC_PD=48 compile.

Real input activations come from accuracy_test_imgs_256/entry_XX.bin
(already-verified real captured chain data, same source
build_single_op_test_entry5_dw.py uses). Weight/bias/shift come from
ckpt_weights_flat.bin/ckpt_bias_flat.bin via the same offsets recorded in
mac_array_ckpt_desc.h -- guarantees byte-identical to what's actually on
the real deployed hardware, not a re-derivation from a different weights_*
directory that might have silently diverged (see CLAUDE.md's own repeated
stale-artifact lesson).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "fastvit_ip_v2", "dw_linebuf_realdata")
os.makedirs(OUT_DIR, exist_ok=True)

with open(os.path.join(ROOT, "fastvit_ip_v2", "ckpt_weights_flat.bin"), "rb") as f:
    w_flat = f.read()
with open(os.path.join(ROOT, "fastvit_ip_v2", "ckpt_bias_flat.bin"), "rb") as f:
    b_flat = f.read()

# (tag, entry_idx, in_source, cin, cout, h_in, w_in, k, stride, pad, fpg,
#  w_off, b_off, shift_off) -- entry idx / offsets verbatim from
# mac_array_ckpt_desc.h, cross-checked against layer_descriptor_256.json.
# in_source: "entry_XX.bin" name (from the 82-entry chain dump) OR an
# absolute-ish filename under accuracy_test_imgs_256 directly (used for
# layer_0049/FinalDW, whose immediate predecessor entry wasn't dumped in
# the 0-16/74-80 range -- ckpt_hw_stage4_0000.bin is real captured board
# data for the exact same tensor: stage4's output IS FinalDW's real
# input, sizes cross-checked: 384*8*8=24576 matches on disk).
LAYERS = [
    ("K3S1_layer0003",  5,  "entry_04.bin",         48, 48,  64, 64, 3, 1, 1, 1, 2736,    96,   3014640),
    ("K7S1_layer0004",  6,  "entry_05.bin",         48, 48,  64, 64, 7, 1, 3, 1, 3168,    144,  3014688),
    ("K3S2_layer0001",  1,  "entry_00.bin",         48, 48, 128,128, 3, 2, 1, 1, 0,       0,    3014544),
    ("K7S2_layer0011",  17, "entry_16.bin",         48, 96,  64, 64, 7, 2, 3, 2, 35952,   672,  3015216),
    ("K3S1fpg2_layer0049", 74, "ckpt_hw_stage4_0000.bin", 384, 768, 8, 8, 3, 1, 1, 2, 2933904, 12048, 3026592),
]

for tag, entry_idx, in_name, cin, cout, h_in, w_in, k, stride, pad, fpg, w_off, b_off, shift_off in LAYERS:
    w_bytes = cout * k * k
    b_elems = cout
    shift_bytes = cout

    w_slice = w_flat[w_off: w_off + w_bytes]
    shift_slice = w_flat[shift_off: shift_off + shift_bytes]
    assert len(w_slice) == w_bytes, (tag, "weight", len(w_slice), w_bytes)
    assert len(shift_slice) == shift_bytes, (tag, "shift", len(shift_slice), shift_bytes)

    b_byte_off = b_off * 4
    b_slice = b_flat[b_byte_off: b_byte_off + b_elems * 4]
    assert len(b_slice) == b_elems * 4, (tag, "bias", len(b_slice), b_elems * 4)

    in_path = os.path.join(ROOT, "accuracy_test_imgs_256", in_name)
    with open(in_path, "rb") as f:
        in_buf = f.read()
    assert len(in_buf) == cin * h_in * w_in, (tag, "input", len(in_buf), cin * h_in * w_in, in_path)

    layer_dir = os.path.join(OUT_DIR, tag)
    os.makedirs(layer_dir, exist_ok=True)
    with open(os.path.join(layer_dir, "in.bin"), "wb") as f:
        f.write(in_buf)
    with open(os.path.join(layer_dir, "w.bin"), "wb") as f:
        f.write(w_slice)
    with open(os.path.join(layer_dir, "b.bin"), "wb") as f:
        f.write(b_slice)
    with open(os.path.join(layer_dir, "shift.bin"), "wb") as f:
        f.write(shift_slice)
    with open(os.path.join(layer_dir, "meta.txt"), "w") as f:
        f.write(f"{cin} {cout} {h_in} {w_in} {k} {stride} {pad} {fpg}\n")

    print(f">>> {tag}: cin={cin} cout={cout} h_in={h_in} w_in={w_in} k={k} stride={stride} "
          f"fpg={fpg} | in={len(in_buf)}B w={len(w_slice)}B b={len(b_slice)}B shift={len(shift_slice)}B "
          f"-> {layer_dir}")

print(">>> done.")
