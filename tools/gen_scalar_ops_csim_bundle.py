"""
gen_scalar_ops_csim_bundle.py -- ZHR-92 (2026-09-12): csim coverage for the
four scalar ops that had NONE (GAP / RELU / SIGMOID / SCALE), written the
same day the SCALAR_OP_SIZE_HOIST round changed all six scalar ops'
signatures without any test ever exercising these four.

Takes the REAL descriptors (all 28 fields, real in_off/in2_off/out_off,
real shapes) straight from the deployed full-network bundle's
desc_all.bin -- entries 75 (GAP), 77 (RELU), 79 (SIGMOID), 80 (SCALE),
i.e. the SE block plus the final per-channel scale, the only real
dispatches of these op types in the whole 82-entry network -- and
produces, per entry:

    csim_scalar_ops/desc_NN.bin   28 x int32, verbatim from desc_all.bin
    csim_scalar_ops/in_NN.bin     cin*h_in*w_in int8, seeded PRNG,
                                  full int8 range (negatives included --
                                  GAP's truncate-toward-zero division and
                                  RELU's sign test both need them)
    csim_scalar_ops/in2_NN.bin    SCALE only: the per-channel gate (cin
                                  int8), placed at the real in2_off
    csim_scalar_ops/ref_NN.bin    the expected output, computed HERE in
                                  Python, independently of any C++

The reference deliberately replicates the HARDWARE's own arithmetic, not
the mathematical function -- in particular quantized_sigmoid is the
placeholder `clamp(x + 64, 0, 127)` (mac_array_raster_integrated.cpp),
NOT a real sigmoid, so a "true" sigmoid reference would fail for the wrong
reason. Integer-division direction is handled explicitly (C++ acc_t
`sum / HW` truncates toward zero; Python `//` floors) per this project's
own recorded lesson on exactly this trap (2026-08-26). Right shifts are
arithmetic in both (Python int >> matches ap_int >>).

Consumed by fastvit_ip_v2/scalar_ops_real_desc_tb.cpp.
"""
import json
import os
import struct
import subprocess

ROOT = r"E:\codes\microzed\fastvit_hls"
DESC_ALL = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_full_network", "desc_all.bin")
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "csim_scalar_ops")
os.makedirs(OUT_DIR, exist_ok=True)

N_FIELDS = 28
FIELD_NAMES = ("op_type cin cout h_in w_in k stride pad fpg out_shift in_off w_off b_off "
               "out_off in2_off h_out w_out n_row_tiles n_col_tiles n_ch_tiles last_row_tile "
               "last_col_tile last_ch_tile use_shift_table shift_off in_ch_stride "
               "out_ch_stride use_wide_path").split()
OP_GAP, OP_RELU, OP_SIGMOID, OP_SCALE = 3, 4, 5, 6
ENTRIES = {75: OP_GAP, 77: OP_RELU, 79: OP_SIGMOID, 80: OP_SCALE}


def clip_shift(acc, shift):
    v = acc >> shift          # arithmetic shift, same as ap_int<32> >>
    return max(-128, min(127, v))


def trunc_div(a, b):
    """C/ap_int integer division: truncate toward zero."""
    q = abs(a) // b
    return q if a >= 0 else -q


def quantized_sigmoid(x):
    return max(0, min(127, x + 64))


class Lcg:
    """Tiny deterministic PRNG so the bundle is reproducible without numpy."""
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next_i8(self):
        self.s = (self.s * 1103515245 + 12345) & 0xFFFFFFFF
        v = (self.s >> 16) & 0xFF
        return v - 256 if v >= 128 else v


raw = open(DESC_ALL, "rb").read()
assert len(raw) % (N_FIELDS * 4) == 0, len(raw)
n_entries = len(raw) // (N_FIELDS * 4)
assert n_entries == 82, n_entries

meta = {"generator": os.path.basename(__file__),
        "source_desc": DESC_ALL,
        "commit": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                 capture_output=True, text=True).stdout.strip(),
        "entries": {}}

for idx, expect_op in ENTRIES.items():
    fields = struct.unpack_from("<%di" % N_FIELDS, raw, idx * N_FIELDS * 4)
    d = dict(zip(FIELD_NAMES, fields))
    assert d["op_type"] == expect_op, (idx, d["op_type"], expect_op)
    HW = d["h_in"] * d["w_in"]
    total = d["cin"] * HW

    rng = Lcg(0x5EED0000 + idx)
    x = [rng.next_i8() for _ in range(total)]
    gate = None

    if expect_op == OP_GAP:
        ref = []
        for c in range(d["cin"]):
            s = sum(x[c * HW:(c + 1) * HW])
            ref.append(clip_shift(trunc_div(s, HW), d["out_shift"]))
    elif expect_op == OP_RELU:
        ref = [v if v > 0 else 0 for v in x]
    elif expect_op == OP_SIGMOID:
        ref = [quantized_sigmoid(v) for v in x]
    elif expect_op == OP_SCALE:
        gate = [rng.next_i8() for _ in range(d["cin"])]
        ref = []
        for c in range(d["cin"]):
            g = gate[c]
            for i in range(HW):
                ref.append(clip_shift(x[c * HW + i] * g, d["out_shift"]))
    else:
        raise AssertionError(expect_op)

    def i8bytes(vals):
        return bytes((v + 256) if v < 0 else v for v in vals)

    open(os.path.join(OUT_DIR, "desc_%d.bin" % idx), "wb").write(struct.pack("<%di" % N_FIELDS, *fields))
    open(os.path.join(OUT_DIR, "in_%d.bin" % idx), "wb").write(i8bytes(x))
    open(os.path.join(OUT_DIR, "ref_%d.bin" % idx), "wb").write(i8bytes(ref))
    if gate is not None:
        open(os.path.join(OUT_DIR, "in2_%d.bin" % idx), "wb").write(i8bytes(gate))

    meta["entries"][idx] = {k: d[k] for k in ("op_type", "cin", "h_in", "w_in", "out_shift",
                                             "in_off", "in2_off", "out_off")}
    meta["entries"][idx].update({"in_bytes": total, "ref_bytes": len(ref),
                                 "gate_bytes": (len(gate) if gate else 0),
                                 "prng_seed": hex(0x5EED0000 + idx)})
    print(">>> entry %d op=%d cin=%d h=%d w=%d shift=%d in_off=%d in2_off=%d out_off=%d -> in %d B, ref %d B"
          % (idx, d["op_type"], d["cin"], d["h_in"], d["w_in"], d["out_shift"],
             d["in_off"], d["in2_off"], d["out_off"], total, len(ref)))

json.dump(meta, open(os.path.join(OUT_DIR, "bundle.meta.json"), "w"), indent=1)
print(">>> bundle written to", OUT_DIR)
