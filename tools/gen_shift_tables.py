"""
gen_shift_tables.py -- A2 exit measurement (ZHR-92, 2026-08-21): propagate
calibrated per-layer output scales through the REAL hardware sequence
(layer_hw_sequence_256.json), and derive per-channel out_shift for every
dispatched conv layer.

Walks tools/layer_hw_sequence_256.json's OWN sequence in execution order,
tracking which scale currently occupies each live address -- deliberately
NOT re-deriving topology from the original consumer tags (that decision
already lives in gen_hw_sequence.py; re-parsing it here would risk the
two falling out of sync, the same reason gen_ckpt_harness.py only ever
*consumes* the sequence, never re-derives it).

The one real design choice (flagged on ZHR-92 before writing this):
run_add has no rescale capability -- both operands must already share a
scale. Every residual merge point is [token_mixer conv] -> RESIDUAL ->
... -> [fc2 conv] -> Add(RESIDUAL, fc2_out). token_mixer's own ideal
calibrated scale becomes the block's shared scale; fc2's TARGET output
scale is forced to match it (not fc2's own independently-ideal scale),
detected by looking one entry ahead for kind=='add' whose op1_off equals
this conv's out_off. Every other conv uses its own ideal scale.

All other op kinds (gelu/relu/gap/sigmoid/scale/add) are scale-preserving
by construction (see mac_array.cpp's out_shift derivations: add=0 exact
sum of same-scale operands, gap=0 exact average, gelu/scale=7 the
sigmoid-gate-family multiply that targets the SAME scale as its feature-
map operand) -- so scale propagation through them is just "unchanged",
no separate calibration needed for them.

用法:
  python gen_shift_tables.py [--out-dir <dir>]
"""
import json
import os
import argparse
import numpy as np


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--hw-seq", default=r"E:\codes\microzed\fastvit_hls\tools\layer_hw_sequence_256.json")
    p.add_argument("--descriptor", default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    p.add_argument("--calib", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\layer_calib_256.json")
    p.add_argument("--weights", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded")
    p.add_argument("--out-dir", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256")
    return p.parse_args()


def compute_shift_per_channel(input_scale, output_scale, weight_scale_arr):
    ratio = input_scale / output_scale
    return np.round(np.log2(1.0 / (weight_scale_arr * ratio + 1e-30))).clip(0, 31).astype(int)


def main():
    args = parse_args()
    with open(args.hw_seq) as f:
        hwdata = json.load(f)
    seq = hwdata["sequence"]
    with open(args.descriptor) as f:
        descriptor = json.load(f)
    with open(args.calib) as f:
        calib = json.load(f)
    with open(os.path.join(args.weights, "quant_config.json")) as f:
        qcfg = json.load(f)

    by_layer_idx = {e["layer_idx"]: e for e in descriptor}
    ideal_scale = {int(k): v["ideal_output_scale"] for k, v in calib["layers"].items()}
    stem_output_scale = ideal_scale[0]

    addr_scale = {}
    # seed: Stem's ARM-computed output lands in MAIN0 (offset 0) before
    # hardware dispatch starts -- the first entry's in_off reads exactly
    # this, matching verify_buffer_liveness.py's own "first dangling read
    # is the external handoff" model.
    first_in_off = None
    for e in seq:
        for key in ("in_off", "op0_off"):
            if key in e:
                first_in_off = e[key]
                break
        if first_in_off is not None:
            break
    addr_scale[first_in_off] = stem_output_scale

    per_layer_shift = {}   # layer_idx -> np.array shift per channel
    per_layer_scales = {}  # layer_idx -> (input_scale, output_scale)
    scale_at_seq_index = {}  # seq_index -> the scale of what THIS entry writes to out_off
                              # (lets any consumer of the hw sequence -- e.g. the checkpoint
                              # table -- dequantize correctly without re-deriving the DAG walk)

    for i, e in enumerate(seq):
        kind = e["kind"]

        if kind == "conv":
            d = by_layer_idx[e["layer_idx"]]
            tag = d["tag"]
            weight_scale = np.array(qcfg[tag]["weight_scale"])
            in_scale = addr_scale[e["in_off"]]

            # detect the fc2-under-Add case: next entry is 'add' and its
            # op1_off is THIS conv's out_off -> target scale forced to
            # match the OTHER operand (the residual branch), already known
            forced = None
            if i + 1 < len(seq) and seq[i + 1]["kind"] == "add" and seq[i + 1]["op1_off"] == e["out_off"]:
                forced = addr_scale[seq[i + 1]["op0_off"]]

            out_scale = forced if forced is not None else ideal_scale[e["layer_idx"]]

            shift = compute_shift_per_channel(in_scale, out_scale, weight_scale)
            per_layer_shift[e["layer_idx"]] = shift
            per_layer_scales[e["layer_idx"]] = (in_scale, out_scale)
            addr_scale[e["out_off"]] = out_scale

        elif kind == "add":
            s0 = addr_scale[e["op0_off"]]
            s1 = addr_scale[e["op1_off"]]
            assert abs(s0 - s1) < 1e-9 * max(abs(s0), abs(s1), 1e-30), (
                f"entry {i}: Add operands at different scales ({s0} vs {s1}) -- "
                f"the fc2-forced-scale-match logic didn't fire where it should have, "
                f"re-check the lookahead condition"
            )
            addr_scale[e["out_off"]] = s0

        elif kind in ("gelu", "relu", "gap", "sigmoid"):
            addr_scale[e["out_off"]] = addr_scale[e["in_off"]]

        elif kind == "scale":
            # feature-map operand (op0) carries the scale forward, op1 is
            # the fixed-range [0,127] gate (see mac_array.cpp's run_scale)
            addr_scale[e["out_off"]] = addr_scale[e["op0_off"]]

        else:
            raise ValueError(f"entry {i}: unhandled kind {kind!r}")

        scale_at_seq_index[i] = addr_scale[e["out_off"]]

    print(f">>> propagated scale through {len(seq)} entries, {len(per_layer_shift)} conv layers calibrated")

    # sanity: how far did any conv's forced (Add-matched) target scale
    # diverge from its own independently-ideal scale? Large divergence
    # here would mean the residual branch and processed branch have very
    # different natural dynamic ranges, worth knowing.
    divergences = []
    for e in seq:
        if e["kind"] != "conv":
            continue
        li = e["layer_idx"]
        in_s, out_s = per_layer_scales[li]
        own_ideal = ideal_scale[li]
        if abs(out_s - own_ideal) > 1e-9:
            divergences.append((li, by_layer_idx[li]["tag"], own_ideal, out_s))
    print(f">>> {len(divergences)} conv layers had their target scale forced to match a residual "
          f"branch (not their own ideal):")
    for li, tag, own_ideal, forced in divergences:
        print(f"    layer_idx={li:2d} {tag:20s} own_ideal={own_ideal:.6f} forced_to={forced:.6f} "
              f"ratio={forced/own_ideal:.3f}x")

    # ---- export: flat per-channel shift table (int8, fits [0,31] trivially),
    # one contiguous array per conv layer in layer_idx order, plus an
    # offset map gen_ckpt_harness.py can use directly. ----
    flat_shift = bytearray()
    shift_off_by_layer = {}
    for li in sorted(per_layer_shift.keys()):
        shift_off_by_layer[li] = len(flat_shift)
        flat_shift += bytes((int(v) & 0xFF) for v in per_layer_shift[li])

    shift_bin_path = os.path.join(args.out_dir, "shift_table_flat.bin")
    with open(shift_bin_path, "wb") as f:
        f.write(flat_shift)
    print(f">>> wrote {shift_bin_path} ({len(flat_shift)} bytes, {len(per_layer_shift)} layers)")

    meta = {
        "shift_off_by_layer": shift_off_by_layer,
        "scales_by_layer": {str(li): {"input_scale": s[0], "output_scale": s[1]}
                             for li, s in per_layer_scales.items()},
        "stem_output_scale": stem_output_scale,
        "stem_input_scale": calib["input_scale"],
        "scale_at_seq_index": {str(i): s for i, s in scale_at_seq_index.items()},
    }
    meta_path = os.path.join(args.out_dir, "shift_table_meta.json")
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
    print(f">>> wrote {meta_path}")


if __name__ == "__main__":
    main()
