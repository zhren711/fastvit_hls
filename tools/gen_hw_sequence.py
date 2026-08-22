"""
gen_hw_sequence.py -- Phase A2: fold the 52-layer structural descriptor
(tools/layer_descriptor_256.json, produced by gen_layer_descriptor.py)
into the full hardware execution sequence mac_array_top actually needs
to run, and assign DRAM buffer offsets per the A2 design (reviewed on
ZHR-63/92, 2026-08-21).

Deliberately a SEPARATE script from gen_layer_descriptor.py, not a change
to it: gen_layer_descriptor.py's job is "read the ONNX graph correctly"
(structure only, no scheduling decisions); this script's job is "turn
that structure into an executable schedule with real addresses" -- same
separation-of-concerns reason gen_layer_descriptor.py itself gives for
not doing numeric folding (activation calibration) in the structural
pass. Mixing them would make it impossible to tell, if the folded
sequence is wrong, whether the bug is in ONNX extraction or scheduling.

Fold rule (derived from consumer kind tags, all confirmed against
tools/layer_dag_ground_truth.json's multi_input_nodes, not assumed):

  'gelu'                        -> insert 1 GELU after this layer         (16 direct hits)
  'add' (paired with 'conv')    -> this layer's own output goes to the
                                    RESIDUAL side-buffer instead of the
                                    ping-pong rotation (zero-copy); the
                                    pending Add is emitted when the
                                    matching 'mul' (layer_scale) is found  (10)
  'mul' (not co-occurring with
         'se_reduce')           -> insert 1 LSCALE, then 1 ADD using the
                                    pending RESIDUAL as op0                (10 lscale + 10 add)
  'se_reduce'                   -> this layer's own output goes to the
                                    SE_HOLD side-buffer; insert 1 GAP      (1)
  'other' after an SE fc1/fc2   -> insert RELU (fc1) or SIGMOID (fc2),
                                    disambiguated by node_name substring   (1 relu + 1 sigmoid)
  (tail, no consumer tag --
   found by direct ONNX trace,
   not derivable from the 52-
   layer consumer list alone)   -> after fc2->sigmoid: 1 SCALE (op0=
                                    SE_HOLD, op1=sigmoid output), then
                                    1 more GELU on Scale's output (the
                                    17th GELU -- /final_conv/act/Erf's
                                    source is se/Mul, not a conv output)   (1 scale + 1 gelu)

Total: 52 conv + 16 + 10 + 10 + 1 + 1 + 1 + 1 + 1 = 93 entries.

Buffer regions (sizes are the max tensor ever assigned to that region,
not summed -- this is the whole point of not doing "one buffer per
layer"):
  MAIN0, MAIN1  -- ping-pong, the ordinary chain (conv->act->conv->...)
  RESIDUAL      -- one block's token_mixer output, alive from its own
                   produce step until the block's Add consumes it
  SE_HOLD       -- final_conv's own output, alive from GAP until Scale
  SE_VEC        -- the tiny C-length SE intermediates (GAP/fc1/relu/fc2/
                   sigmoid outputs), reused sequentially

用法:
  python gen_hw_sequence.py [--descriptor <json>] [--out <json>]
"""
import json
import os
import argparse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--descriptor",
                    default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    p.add_argument("--weights",
                    default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded",
                    help="weight source directory. Determines whether LayerScale is already "
                         "folded into fc2's weight+bias (skip the 10 synthetic LSCALE ops, "
                         "route straight from fc2's conv output into the block's Add) or still "
                         "needs a separate LSCALE op at runtime (weights_t8_pruned, pre-fold). "
                         "Route A (fold into fc2) confirmed 2026-08-21 on ZHR-92 -- validated "
                         "to ~1e-16 in fold_layer_scale.py, including the int8-saturation risk "
                         "check (worst case 61.46% saturated before AND after folding).")
    p.add_argument("--out",
                    default=r"E:\codes\microzed\fastvit_hls\tools\layer_hw_sequence_256.json")
    return p.parse_args()


def weights_are_gamma_folded(weights_dir):
    """Read the fold status from the weight source itself rather than
    inferring it from a directory name, so pointing --weights at the
    wrong place fails loudly instead of silently picking the wrong fold
    rule."""
    qcfg_path = os.path.join(weights_dir, "quant_config.json")
    with open(qcfg_path) as f:
        qcfg = json.load(f)
    folded_tags = [k for k, v in qcfg.items() if v.get("gamma_folded")]
    if not folded_tags:
        return False
    assert len(folded_tags) == 10, (
        f"expected exactly 10 gamma_folded fc2 tags (one per RepMixerBlock) in "
        f"{weights_dir}, found {len(folded_tags)}: {folded_tags}"
    )
    return True


def out_hw(e):
    h_out = (e["h_in"] + 2 * e["pad"] - e["k"]) // e["stride"] + 1
    w_out = (e["w_in"] + 2 * e["pad"] - e["k"]) // e["stride"] + 1
    return h_out, w_out


def build_hw_sequence(descriptor, gamma_folded):
    seq = []
    regions = {}

    def bump(name, nbytes):
        regions[name] = max(regions.get(name, 0), nbytes)

    # Route C (ZHR-92, 2026-08-21): layer 0 (Stem's plain K=3 conv, the
    # ONLY hardware-capability violation gen_layer_descriptor.py's
    # check_hw_capability() finds in the real 52-layer model -- PWCONV's
    # hardware is structurally K=1-only) is computed off-chip (ARM,
    # int32-accumulate single pass, see tools/compute_stem_arm.py) instead
    # of dispatched into this sequence. Its quantized int8 output is what
    # the ARM DMAs into MAIN0 before hardware dispatch starts -- same
    # buffer role the raw-image INPUT region used to play; there is no
    # "INPUT" region in the hardware's own DRAM footprint anymore, since
    # hardware never touches the raw image (Stem's conv+quantization both
    # happen ARM-side). Rejected Route B (3x DW fpg=48 + 2x Add, verified
    # feasible) specifically because it forces an int8 round-trip on 3
    # partial sums before combining -- precision loss concentrated in the
    # FIRST checkpoint of the very cosine table this exists to produce,
    # exactly the "cliff read as scale-placeholder error" trap this round
    # was started to avoid. Declared reproduction deviation (paper's
    # detector is fully on-chip); revisit once the array widens back
    # toward 512 and Stem's ARM latency becomes the dominant bottleneck.
    d0 = descriptor[0]
    assert d0["op_type"] == "conv" and d0["layer_idx"] == 0, (
        "expected layer 0 to be Stem's plain conv (Route C assumption) -- "
        "if this fires, check_hw_capability()'s violation list changed and "
        "this whole Route C wiring needs to be re-derived, not patched"
    )
    h0_out, w0_out = out_hw(d0)
    stem_osz = d0["cout"] * h0_out * w0_out

    toggle = [0]

    def next_main():
        name = f"MAIN{toggle[0]}"
        toggle[0] ^= 1
        return name

    # Same bump()/toggle bookkeeping as if a hardware entry had produced
    # Stem's output -- just no seq.append() for the CONV itself, since
    # nothing dispatches it. Stem's own activation (GELU) still runs in
    # hardware -- only the conv moves to the ARM -- so its consumer-driven
    # insertion has to happen here too, mirroring the default branch below
    # instead of being silently skipped along with the conv (caught by
    # the entry-count assertion at the bottom of main(): first attempt at
    # this produced 81 entries / gelu=16, not 82/17, because this block
    # was originally missing entirely).
    cur = next_main()
    bump(cur, stem_osz)
    stem_kinds = {c["kind"] for c in d0["consumers"]}
    assert stem_kinds == {"gelu"}, (
        f"expected Stem's only consumer to be 'gelu' (Route C assumption), got {stem_kinds} "
        f"-- this fast-path only handles that case, re-derive the wiring if it changed"
    )
    gelu_out = next_main()
    bump(gelu_out, stem_osz)
    seq.append({"kind": "gelu", "in_region": cur, "out_region": gelu_out,
                "cin": d0["cout"], "h": h0_out, "w": w0_out})
    cur = gelu_out

    pending_residual = [None]
    final_conv_shape = {}

    i = 1  # skip descriptor[0] (Stem) -- already accounted for above
    n = len(descriptor)
    while i < n:
        e = descriptor[i]
        h_out, w_out = out_hw(e)
        osz = e["cout"] * h_out * w_out
        kinds = {c["kind"] for c in e["consumers"]}

        if "se_reduce" in kinds:
            out_region = "SE_HOLD"
            bump(out_region, osz)
            seq.append({"kind": "conv", "layer_idx": e["layer_idx"],
                        "in_region": cur, "out_region": out_region,
                        "cin": e["cin"], "cout": e["cout"], "h_out": h_out, "w_out": w_out})
            cur = out_region
            final_conv_shape.update(cout=e["cout"], h_out=h_out, w_out=w_out)
            gap_out = "SE_VEC"
            bump(gap_out, e["cout"])
            seq.append({"kind": "gap", "in_region": cur, "out_region": gap_out,
                        "cin": e["cout"], "h": h_out, "w": w_out})
            cur = gap_out
            i += 1
            continue

        if "add" in kinds:
            out_region = "RESIDUAL"
            bump(out_region, osz)
            seq.append({"kind": "conv", "layer_idx": e["layer_idx"],
                        "in_region": cur, "out_region": out_region,
                        "cin": e["cin"], "cout": e["cout"], "h_out": h_out, "w_out": w_out})
            cur = out_region
            assert pending_residual[0] is None, f"nested residual at layer {e['layer_idx']}, not supported"
            pending_residual[0] = out_region
            i += 1
            continue

        out_region = next_main()
        bump(out_region, osz)
        seq.append({"kind": "conv", "layer_idx": e["layer_idx"],
                    "in_region": cur, "out_region": out_region,
                    "cin": e["cin"], "cout": e["cout"], "h_out": h_out, "w_out": w_out})
        cur = out_region

        if "gelu" in kinds:
            gelu_out = next_main()
            bump(gelu_out, osz)
            seq.append({"kind": "gelu", "in_region": cur, "out_region": gelu_out,
                        "cin": e["cout"], "h": h_out, "w": w_out})
            cur = gelu_out

        elif "mul" in kinds:
            # 'mul' here is layer_scale (disambiguated from the SE gate mul
            # by classify_consumers() -- se_reduce co-occurrence). If the
            # weight source already has gamma folded into fc2 (Route A,
            # ZHR-92 2026-08-21), this conv's own output IS the scaled
            # value already -- no separate hardware op, go straight to Add.
            # If not, gamma still needs applying at runtime via LSCALE.
            if not gamma_folded:
                ls_out = next_main()
                bump(ls_out, osz)
                seq.append({"kind": "lscale", "in_region": cur, "out_region": ls_out,
                            "cin": e["cout"], "h": h_out, "w": w_out,
                            "weight_layer_idx": e["layer_idx"]})
                cur = ls_out
            assert pending_residual[0] is not None, f"layer_scale at layer {e['layer_idx']} with no pending residual"
            add_out = next_main()
            bump(add_out, osz)
            seq.append({"kind": "add", "op0_region": pending_residual[0], "op1_region": cur,
                        "out_region": add_out, "cin": e["cout"], "h": h_out, "w": w_out})
            cur = add_out
            pending_residual[0] = None

        elif "other" in kinds:
            act_out = next_main()
            bump(act_out, osz)
            if "fc1" in e["node_name"]:
                act_kind = "relu"
            elif "fc2" in e["node_name"]:
                act_kind = "sigmoid"
            else:
                raise ValueError(f"unexpected 'other' consumer on {e['node_name']!r}, layer {e['layer_idx']}")
            seq.append({"kind": act_kind, "in_region": cur, "out_region": act_out,
                        "cin": e["cout"], "h": h_out, "w": w_out})
            cur = act_out

        i += 1

    assert pending_residual[0] is None, "unresolved residual at end of sequence"
    assert final_conv_shape, "never found the se_reduce (final_conv) layer"

    # ---- SE tail, found by direct ONNX trace, not derivable from the 52
    # conv layers' own consumer tags: Scale (op0=SE_HOLD, the ORIGINAL
    # final_conv output, op1=the sigmoid gate just computed) then one more
    # GELU on Scale's output -- /final_conv/act/Erf's source is se/Mul,
    # confirmed 2026-08-21, this is the 17th GELU the 16-hit consumer scan
    # can't see. ----
    scale_out = next_main()
    bump(scale_out, regions["SE_HOLD"])
    seq.append({"kind": "scale", "op0_region": "SE_HOLD", "op1_region": cur,
                "out_region": scale_out, **final_conv_shape})
    cur = scale_out
    final_gelu_out = next_main()
    bump(final_gelu_out, regions["SE_HOLD"])
    seq.append({"kind": "gelu", "in_region": cur, "out_region": final_gelu_out,
                "cin": final_conv_shape["cout"], "h": final_conv_shape["h_out"], "w": final_conv_shape["w_out"]})

    return seq, regions


def resolve_offsets(seq, regions):
    """Assign concrete byte offsets to the symbolic region names, in a
    fixed, deterministic order. Region SIZE is the max tensor ever
    assigned to it (computed in build_hw_sequence) -- NOT a sum, that's
    the whole point of ping-pong + side-buffers over "one buffer per
    layer"."""
    order = ["MAIN0", "MAIN1", "RESIDUAL", "SE_HOLD", "SE_VEC"]
    offsets = {}
    cursor = 0
    for name in order:
        if name not in regions:
            continue
        offsets[name] = cursor
        cursor += regions[name]
    total_bytes = cursor

    def resolve(entry):
        out = dict(entry)
        for key in ("in_region", "out_region", "op0_region", "op1_region"):
            if key in out:
                region = out[key]
                out[key.replace("_region", "_off")] = offsets[region]
        return out

    resolved = [resolve(e) for e in seq]
    return resolved, offsets, total_bytes


def summarize(seq):
    from collections import Counter
    c = Counter(e["kind"] for e in seq)
    return dict(c)


def main():
    args = parse_args()
    with open(args.descriptor) as f:
        descriptor = json.load(f)
    assert len(descriptor) == 52, f"expected 52 conv-type layers, got {len(descriptor)}"

    gamma_folded = weights_are_gamma_folded(args.weights)
    print(f">>> weight source: {args.weights}")
    print(f">>> gamma_folded: {gamma_folded}")

    seq, regions = build_hw_sequence(descriptor, gamma_folded)
    resolved, offsets, total_bytes = resolve_offsets(seq, regions)

    counts = summarize(seq)
    print(">>> op kind counts:", counts)
    print(">>> total hw-sequence entries:", len(seq))
    print(">>> region sizes (bytes):", regions)
    print(">>> region offsets (bytes):", offsets)
    print(f">>> total DRAM footprint: {total_bytes} bytes ({total_bytes/1024/1024:.3f} MB)")

    # conv=51, not 52: Stem (layer 0) is computed ARM-side (Route C,
    # ZHR-92 2026-08-21), never dispatched into this sequence -- its own
    # GELU still is (17 stays 17, not 16).
    expected = {"conv": 51, "gelu": 17, "add": 10, "gap": 1, "relu": 1, "sigmoid": 1, "scale": 1}
    expected_total = 82
    if not gamma_folded:
        expected["lscale"] = 10
        expected_total = 92
    mismatches = {k: (counts.get(k, 0), v) for k, v in expected.items() if counts.get(k, 0) != v}
    if mismatches:
        print(">>> FOLD RULE MISMATCH (expected vs actual):", mismatches)
    else:
        print(">>> fold rule counts match design doc exactly:", expected)
    assert sum(counts.values()) == expected_total, f"expected {expected_total} total entries, got {sum(counts.values())}"

    with open(args.out, "w") as f:
        json.dump({"sequence": resolved, "regions": regions, "offsets": offsets,
                   "total_bytes": total_bytes}, f, indent=2)
    print(f">>> wrote {args.out}")


if __name__ == "__main__":
    main()
