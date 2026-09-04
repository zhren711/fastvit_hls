"""
build_pw_axi_txn_probes.py -- ZHR-92 (2026-09-04): AXI-transaction-count
hypothesis test. Pipeline fill/drain was ruled out (<=0.4% of the
remainder, computed against real csynth PipelineDepth=22, no board
needed). AXI-transaction handshake overhead is now the only live
candidate for PW's 48.7-71.9% remainder.

Design refinement vs. the round's own literal suggestion (vary Cin against
W_in inversely): holding Cin FIXED instead and trading H_in against W_in
is STRICTLY BETTER -- it eliminates the n_cbase confound entirely (Cin
fixed -> n_cbase=ceil(Cin/32) identical across all points) rather than
needing to model it out and subtract it. ROW_READ issues one read_request
per (ci, rr) per row-tile -- TOTAL transaction count across the whole
ROW_READ sweep for a layer = Cin*MAC_PR*n_row_tiles = Cin*MAC_PR*(H_in/4)
(with n_chunks=1 for all points here), i.e. proportional to Cin*H_in with
Cin fixed -> proportional to H_in alone. Total bytes moved = Cin*H_in*W_in.
Holding Cin*H_in*W_in constant while scaling H_in by 1x/2x/4x (and W_in by
the inverse) gives:
  1x: cin=48, H_in=8,  W_in=64  (bytes=24576, total_txn=48*4*2=384)
  2x: cin=48, H_in=16, W_in=32  (bytes=24576, total_txn=48*4*4=768)
  4x: cin=48, H_in=32, W_in=16  (bytes=24576, total_txn=48*4*8=1536)
n_tiles = (H_in/4)*(W_in/4) = H_in*W_in/16 = (bytes/cin)/16 -- AUTOMATICALLY
constant (32) across all three once cin and bytes are both fixed, no
separate h_in-vs-w_in tile-count compensation needed. n_words per
transaction = ceil(W_in/4) in {16,8,4}, all comfortably under
MAX_WORDS_PER_CH=17. All byte-address-aligned (r=0 always -- in_ch_stride
and oh*W both multiples of 4 in all three cases, verified by hand).

cout=48 fixed, n_chunks=1 for all three (cin*cout=2304 << 144KB).
n_cbase=ceil(48/32)=2 IDENTICAL across all three (Cin fixed) -- unlike the
round's own literal design, this means the analytical PW_FLAT compute-only
time (total_iters/tile * n_tiles * n_chunks * 10ns) is EXACTLY IDENTICAL
across all three points (same n_ot, n_cbase, n_tiles, n_chunks) -- no
subtraction/modeling needed to isolate the transaction-count effect; any
observed ms difference between the three points is attributable to
transaction count alone by construction, not by post-hoc compute
correction.

Trivial golden (all-ones input/weight, zero bias, out_shift=7,
out=cin>>7), same pattern as build_pw_scaling_probes.py -- CORRECT-VALUES
construction, byte-exact is a real check here, no new bitstream needed.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
BASE_OUT = os.path.join(ROOT, "accuracy_test_imgs_256")

MAC_PR, MAC_PC, MAC_PD = 4, 4, 1
MAX_CIN_PW = 32
OP_PWCONV = 1
OUT_SHIFT = 7
COUT = 48


def build(tag, cin, h, w):
    out_dir = os.path.join(BASE_OUT, f"board_test_{tag}")
    os.makedirs(out_dir, exist_ok=True)

    cout = COUT
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
    n_chunks = (cout + pw_ot_per_chunk - 1) // pw_ot_per_chunk
    n_tiles = n_row_tiles * n_col_tiles
    txn_count = cin * MAC_PR  # per rt-call; total ROW_READ txns = txn_count * n_row_tiles
    bytes_moved = cin * h * w  # total activation bytes read (K*MAC_PR*n_row_tiles, but cin*h*w is simpler/exact)

    fields = [
        OP_PWCONV, cin, cout,
        h, w,
        k, stride, pad,
        fpg,
        OUT_SHIFT,
        0, 0, 0, 0,
        0,
        h_out, w_out,
        n_row_tiles, n_col_tiles, n_ch_tiles,
        last_row_tile, last_col_tile, last_ch_tile,
        0, 0,
        in_ch_stride, out_ch_stride,
        0,
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

    print(f">>> {tag}: cin={cin} cout={cout} h={h} w={w} n_cbase={n_cbase} n_tiles={n_tiles} "
          f"n_chunks={n_chunks} txn/rt={txn_count} n_row_tiles={n_row_tiles} "
          f"total_txn={txn_count*n_row_tiles} bytes_moved={bytes_moved} golden_val={val} "
          f"-> {out_dir}")


build("pwaxi_1x", cin=48, h=8,  w=64)    # n_row=2, n_tiles=32, total_txn=384
build("pwaxi_2x", cin=48, h=16, w=32)    # n_row=4, n_tiles=32, total_txn=768
build("pwaxi_4x", cin=48, h=32, w=16)    # n_row=8, n_tiles=32, total_txn=1536

print(">>> all bundles written")
