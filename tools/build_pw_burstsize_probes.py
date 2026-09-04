"""
build_pw_burstsize_probes.py -- ZHR-92 (2026-09-04): burst-size hypothesis
test, isolated this time (the prior round's compensating variable, burst
size, turned out to correlate better with the remainder than the intended
variable, transaction count -- this round makes burst size the ONLY
variable, with an explicit h_in-only control to catch the same trap
happening again).

PREREQUISITE CHECK (done first, no board time, ~2 min): computed n_words
for all 26 real PW layers via tools/layer_descriptor_256.json. Distribution:
w_in=64->n_words=16 (5 layers), w_in=32->8 (5 layers), w_in=16->4 (8
layers), w_in=8->2 (6 layers, INCLUDING layers 43/44/47/48 -- the exact
layers this whole ZHR-92 arc has been optimizing), w_in=1->1 (2 layers,
SE block). Roughly half the real network sits at n_words<=4 -- this is
NOT a synthetic-only corner case, so the experiment below is warranted.

Group A (isolate w_in / n_words, cin=cout=48 fixed -> n_cbase=2 fixed,
h_in compensated so n_tiles=64 fixed throughout -> analytical PW_FLAT
compute time IDENTICAL by construction across all 5 points, 2.4576ms):
  A1: w=64,h=16  n_words=16  n_row=4  n_col=16  n_tiles=64
  A2: w=32,h=32  n_words=8   n_row=8  n_col=8   n_tiles=64  (== the
      already-measured pwscale_3b_t64 point, reused, not rebuilt)
  A3: w=16,h=64  n_words=4   n_row=16 n_col=4   n_tiles=64
  A4: w=8, h=128 n_words=2   n_row=32 n_col=2   n_tiles=64
  A5: w=4, h=256 n_words=1   n_row=64 n_col=1   n_tiles=64
(n_words=32 -- the round's own first suggestion -- was checked and
REJECTED: it needs w_in=128, giving n_words=ceil(128/4)=32 >
MAX_WORDS_PER_CH=17, which would silently truncate ROW_READ_FILL's real
loop bound and corrupt data, not just slow it down. Capped the sweep at
n_words=16 instead, extending one step further at the LOW end (n_words=1,
matching the real network's own SE-block layers) rather than the high end.)

Group B (h_in-only control, w_in FIXED at 32 -> n_words=8 fixed
throughout, isolating whatever effect h_in/n_row_tiles has on its own,
independent of burst size):
  B1: w=32,h=8   n_row=2  n_col=8  n_tiles=16
  B2: w=32,h=32  n_row=8  n_col=8  n_tiles=64  (== A2, reused)
  B3: w=32,h=128 n_row=32 n_col=8  n_tiles=256

Every point's own varying quantities are listed explicitly in the build()
call site below (not just the target variable), per this round's own
explicit instruction to avoid repeating the prior round's confound trap.

Trivial golden (all-ones input/weight, zero bias, out_shift=7,
out=cin>>7), same pattern as the prior two scaling-probe builders --
CORRECT-VALUES construction, byte-exact is a real check, no new
bitstream needed.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
BASE_OUT = os.path.join(ROOT, "accuracy_test_imgs_256")

MAC_PR, MAC_PC, MAC_PD = 4, 4, 1
MAX_CIN_PW = 32
MAX_WORDS_PER_CH = 17
OP_PWCONV = 1
OUT_SHIFT = 7
CIN = 48
COUT = 48


def build(tag, h, w):
    out_dir = os.path.join(BASE_OUT, f"board_test_{tag}")
    os.makedirs(out_dir, exist_ok=True)

    cin, cout = CIN, COUT
    k, stride, pad, fpg = 1, 1, 0, 1

    n_words = (w + 3) // 4
    assert n_words <= MAX_WORDS_PER_CH, f"{tag}: n_words={n_words} exceeds MAX_WORDS_PER_CH={MAX_WORDS_PER_CH}"
    assert cin * w <= 9216, f"{tag}: cin*w={cin*w} exceeds MAX_CIN_TIMES_W=9216"

    in_buf = bytes([1]) * (cin * h * w)
    w_buf = bytes([1]) * (cout * cin)
    b_buf = struct.pack("<%di" % cout, *([0] * cout))

    h_out, w_out = h, w
    val = cin >> OUT_SHIFT
    assert -128 <= val <= 127
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
    n_tiles = n_row_tiles * n_col_tiles

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
    assert len(fields) == 28
    desc_bytes = struct.pack("<28i", *fields)

    with open(os.path.join(out_dir, "desc.bin"), "wb") as f: f.write(desc_bytes)
    with open(os.path.join(out_dir, "in.bin"), "wb") as f: f.write(in_buf)
    with open(os.path.join(out_dir, "w.bin"), "wb") as f: f.write(w_buf)
    with open(os.path.join(out_dir, "b.bin"), "wb") as f: f.write(b_buf)
    with open(os.path.join(out_dir, "ref_out.bin"), "wb") as f: f.write(ref_out)

    print(f">>> {tag}: cin={cin} cout={cout} h={h} w={w} n_words={n_words} n_cbase={n_cbase} "
          f"n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_tiles={n_tiles} -> {out_dir}")


# Group A: vary w_in (n_words), h_in compensated -> n_tiles=64 fixed throughout.
build("pwburst_a1_nw16", h=16,  w=64)
# a2 (h=32,w=32,n_words=8,n_tiles=64) == pwscale_3b_t64, reused, not rebuilt
build("pwburst_a3_nw4",  h=64,  w=16)
build("pwburst_a4_nw2",  h=128, w=8)
build("pwburst_a5_nw1",  h=256, w=4)

# Group B: vary h_in only, w_in=32 (n_words=8) fixed throughout.
build("pwburst_b1_t16",  h=8,   w=32)
# b2 (h=32,w=32) == a2 == pwscale_3b_t64, reused, not rebuilt
build("pwburst_b3_t256", h=128, w=32)

print(">>> all bundles written")
