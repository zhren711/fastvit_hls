"""
build_pw_scaling_probes.py -- ZHR-92 (2026-09-04): PW cost-model scaling
experiment. Five real-P&R address-fixing probes (weight-cache, activation,
output, bias/shift, ROW_READ) all came back ~0% -- the addressing-site
candidate list is exhausted, so the remainder (54.1%/entry3, 43.2%/entry64)
is NOT DRAM-access-bound. Switching to the scaling-experiment technique
this project has used 3 times before (n_cbase decomposition, n_steps
scaling, tile-count scaling) instead of a per-region timeline (no
fine-grained on-board timer exists to measure named HLS regions directly).

Method: synthetic PW descriptors, all-ones input/weight/zero-bias/
out_shift=7 (trivial golden: out = clip((cin*1*1+0)>>7) = cin>>7, always
in-range for every cin used here, no simulator needed -- same pattern as
tools/build_dw_ot_probe_large.py). One dimension varied per group, three
points each, board-loadable bundles with a real correct reference (this
is a CORRECT-VALUES construction, not a wrong-values timing probe -- byte-
exact is a real pass/fail signal here, not "N/A by design").

Dimensions (12 points total, one point shared between group1/group3):
  1. n_ot (=cout): cin=48, h=w=64 fixed. cout in {48,96,192}.
  2. n_cbase (via cin): cout=48, h=w=64 fixed. cin in {32,64,128}.
  3. spatial tile count: cin=cout=48 fixed. h=w in {16,32,64}
     (16=point3a, 32=point3b, 64=shared with point1a/2b... actually shares
     with point1a specifically: cin=48,cout=48,h=w=64).
  4. n_chunks: cout=384, h=w=8 fixed. cin in {192,576,960} -> n_chunks
     {1,2,3} (144KB cache, PW_WEIGHT_CACHE_ELEMS=147,456).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
BASE_OUT = os.path.join(ROOT, "accuracy_test_imgs_256")

MAC_PR, MAC_PC, MAC_PD = 4, 4, 1
MAX_CIN_PW = 32
OP_PWCONV = 1
OUT_SHIFT = 7


def build(tag, cin, cout, h, w):
    out_dir = os.path.join(BASE_OUT, f"board_test_{tag}")
    os.makedirs(out_dir, exist_ok=True)

    k, stride, pad, fpg = 1, 1, 0, 1

    in_buf = bytes([1]) * (cin * h * w)
    w_buf = bytes([1]) * (cout * cin)
    b_buf = struct.pack("<%di" % cout, *([0] * cout))

    h_out = (h + 2 * pad - k) // stride + 1
    w_out = (w + 2 * pad - k) // stride + 1
    assert h_out == h and w_out == w

    val = cin >> OUT_SHIFT
    assert -128 <= val <= 127, f"{tag}: golden value {val} out of int8 range"
    ref_out = bytes([val & 0xFF]) * (cout * h_out * w_out)

    n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
    n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
    n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
    last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
    last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
    last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
    in_ch_stride = h * w
    out_ch_stride = h_out * w_out

    n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
    pw_ot_per_chunk = 147456 // cin
    if pw_ot_per_chunk < 1:
        pw_ot_per_chunk = 1
    n_chunks = (cout + pw_ot_per_chunk - 1) // pw_ot_per_chunk
    n_tiles = n_row_tiles * n_col_tiles

    fields = [
        OP_PWCONV, cin, cout,
        h, w,
        k, stride, pad,
        fpg,
        OUT_SHIFT,
        0, 0, 0, 0,  # in_off, w_off, b_off, out_off -- all 0, matching the board driver's own
                     # convention (mac_array_single_op_test.c: in_v/w_v/b_v/out_v are SEPARATE
                     # fixed DRAM offsets, not one shared buffer -- out_off=0 does NOT alias
                     # in_off=0 there; the csim testbench must mirror this with separate vectors).
        0,
        h_out, w_out,
        n_row_tiles, n_col_tiles, n_ch_tiles,
        last_row_tile, last_col_tile, last_ch_tile,
        0,          # use_shift_table
        0,          # shift_off (unused)
        in_ch_stride, out_ch_stride,
        0,          # use_wide_path
    ]
    assert len(fields) == 28, len(fields)
    desc_bytes = struct.pack("<28i", *fields)

    with open(os.path.join(out_dir, "desc.bin"), "wb") as f:
        f.write(desc_bytes)
    with open(os.path.join(out_dir, "in.bin"), "wb") as f:
        f.write(in_buf)
    with open(os.path.join(out_dir, "w.bin"), "wb") as f:
        f.write(w_buf)
    with open(os.path.join(out_dir, "b.bin"), "wb") as f:
        f.write(b_buf)
    with open(os.path.join(out_dir, "ref_out.bin"), "wb") as f:
        f.write(ref_out)

    print(f">>> {tag}: cin={cin} cout={cout} h=w={h} n_ot={cout} n_cbase={n_cbase} "
          f"n_tiles={n_tiles} n_chunks={n_chunks} w_bytes={cin*cout} golden_val={val} "
          f"-> {out_dir}")


# Group 1: vary n_ot (cout), fixed cin=48, h=w=64
build("pwscale_1a_ot48",  cin=48, cout=48,  h=64, w=64)   # n_ot=48  (also group3's h=w=64 point)
build("pwscale_1b_ot96",  cin=48, cout=96,  h=64, w=64)   # n_ot=96
build("pwscale_1c_ot192", cin=48, cout=192, h=64, w=64)   # n_ot=192

# Group 2: vary n_cbase (cin), fixed cout=48, h=w=64
build("pwscale_2a_cb1",  cin=32,  cout=48, h=64, w=64)    # n_cbase=1
build("pwscale_2b_cb2",  cin=64,  cout=48, h=64, w=64)    # n_cbase=2
build("pwscale_2c_cb4",  cin=128, cout=48, h=64, w=64)    # n_cbase=4

# Group 3: vary spatial tile count, fixed cin=cout=48
build("pwscale_3a_t16",  cin=48, cout=48, h=16, w=16)     # n_tiles=16
build("pwscale_3b_t64",  cin=48, cout=48, h=32, w=32)     # n_tiles=64
# 3c (h=w=64, n_tiles=256) == pwscale_1a_ot48, reused, not rebuilt

# Group 4: vary n_chunks, fixed cout=384, h=w=8
build("pwscale_4a_nc1", cin=192, cout=384, h=8, w=8)      # n_chunks=1
build("pwscale_4b_nc2", cin=576, cout=384, h=8, w=8)      # n_chunks=2
build("pwscale_4c_nc3", cin=960, cout=384, h=8, w=8)      # n_chunks=3

print(">>> all bundles written")
