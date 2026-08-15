"""
fold_layer_scale.py -- Phase A step 2b-1: fold LayerScale gamma into fc2.

Implements the fold confirmed numerically safe earlier this session: under
this project's existing PER-CHANNEL int8 weight quantization (see
export_weights.py's quantize_weight_per_channel), multiplying an entire
output channel by a scalar gamma[c] is exactly compensated by that
channel's own scale = max(|w|)/127 recomputed after the fold -- the int8
*codes* come out nearly identical (up to sign flip for negative gamma),
verified empirically across all 10 blocks (worst case: a channel already
61.46% saturated near {-1,0,1} before folding, identically so after).

Does NOT re-derive the pruning channel selection (thinet_prune_feasibility
keep_idx) for the 3 already-pruned fc2 layers -- that would risk silently
picking a DIFFERENT channel subset than what's actually deployed. Instead
dequantizes the EXISTING (already-correctly-pruned) int8 weight file using
its own recorded weight_scale to recover the pruned fp32 weight to
quantization precision, then folds gamma on top of that. This is safe
regardless of whether a given fc2 was pruned.

gamma applies to fc2's OUTPUT channel dim (= the block's main channel C,
not the expanded C_expand) -- dimensionally this is exactly "scale the
whole ConvFFN branch's output by gamma before it hits the residual add",
which is what the real ONNX graph does (Mul(fc2_out, gamma) -> Add).
Bias gets the same per-channel gamma scale, and out_shift is recomputed
from the new (smaller, since |gamma|<1 typically) weight_scale using the
same log2-based formula export_weights.py already uses (still assumes
output_scale==input_scale=default_act_scale=1/127 -- calibration is step
2b-2, not done here).

Input weights unchanged for every layer except the 10 fc2 layers; input_scale
for those layers is unaffected (gamma only scales fc2's OWN output, not
what feeds into it).

用法:
  python fold_layer_scale.py [--model <onnx>] [--src-weights <dir>]
                              [--out <dir>]
"""
import onnx
import onnx.numpy_helper
import numpy as np
import json
import os
import shutil
import argparse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--src-weights", default=r"E:\codes\microzed\fastvit_hls\weights_t8_pruned")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded")
    return p.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    model = onnx.load(args.model)
    graph = model.graph
    init_map = {i.name: onnx.numpy_helper.to_array(i) for i in graph.initializer}

    with open(os.path.join(args.src_weights, "quant_config.json")) as f:
        qcfg = json.load(f)

    # map block prefix ("stages.0.blocks.0") -> its fc2 tag in quant_config,
    # via the ONNX node name that contains "<prefix>.mlp.fc2"
    node_idx_to_tag = {v["node_idx"]: k for k, v in qcfg.items()}
    gamma_names = sorted(n for n in init_map if "layer_scale.gamma" in n)

    # initializer names use "stages.<s>.blocks.<b>.layer_scale.gamma" (dots
    # throughout); node names use "/stages.<s>/blocks/blocks.<b>/mlp/fc2/Conv"
    # ("blocks" appears as its own path segment, then fused with the index) --
    # different ONNX export naming conventions for initializers vs nodes, not
    # a simple substring relationship.
    import re
    fold_targets = {}  # tag -> gamma array
    for gname in gamma_names:
        m = re.match(r"stages\.(\d+)\.blocks\.(\d+)\.layer_scale\.gamma", gname)
        assert m, f"unexpected gamma name format: {gname}"
        stage_idx, block_idx = m.group(1), m.group(2)
        target_node_name = f"/stages.{stage_idx}/blocks/blocks.{block_idx}/mlp/fc2/Conv"
        for i, node in enumerate(graph.node):
            if node.op_type == "Conv" and node.name == target_node_name:
                tag = node_idx_to_tag.get(i)
                assert tag is not None, f"no quant_config entry for node_idx={i} ({node.name})"
                fold_targets[tag] = init_map[gname].reshape(-1)
                break
        else:
            raise RuntimeError(f"could not find node {target_node_name!r} for gamma {gname}")

    assert len(fold_targets) == 10, f"expected 10 fc2 layers to fold, found {len(fold_targets)}"
    print(f">>> folding gamma into {len(fold_targets)} fc2 layers:")
    for tag in sorted(fold_targets, key=lambda t: int(t.split("_")[1])):
        print(f"    {tag}")

    # copy everything unchanged first, then overwrite the 10 fc2 layers
    for fname in os.listdir(args.src_weights):
        shutil.copy2(os.path.join(args.src_weights, fname), os.path.join(args.out, fname))

    new_qcfg = dict(qcfg)
    default_act_scale = 1.0 / 127.0  # unchanged -- calibration is step 2b-2

    for tag, gamma in fold_targets.items():
        entry = qcfg[tag]
        w_scale_old = np.array(entry["weight_scale"])  # [Cout]
        cout = entry["Cout"]

        w_int8_old = np.fromfile(os.path.join(args.src_weights, entry["weight_file"]), dtype=np.int8)
        w_int8_old = w_int8_old.reshape(cout, -1).astype(np.float64)
        w_fp32 = w_int8_old * w_scale_old[:, None]

        assert gamma.shape[0] == cout, f"{tag}: gamma channels {gamma.shape[0]} != Cout {cout}"
        w_folded_fp32 = w_fp32 * gamma[:, None]

        w_scale_new = np.max(np.abs(w_folded_fp32), axis=1) / 127.0
        w_scale_new = np.maximum(w_scale_new, 1e-12)  # guard near-zero-gamma channels
        w_int8_new = np.round(w_folded_fp32 / w_scale_new[:, None]).clip(-128, 127).astype(np.int8)
        w_int8_new.tofile(os.path.join(args.out, entry["weight_file"]))

        new_b_int32 = None
        if entry["bias_file"]:
            b_int32_old = np.fromfile(os.path.join(args.src_weights, entry["bias_file"]), dtype=np.int32)
            b_scale_old = default_act_scale * w_scale_old
            b_fp32 = b_int32_old.astype(np.float64) * b_scale_old
            b_folded_fp32 = b_fp32 * gamma
            b_scale_new = default_act_scale * w_scale_new
            new_b_int32 = np.round(b_folded_fp32 / b_scale_new).clip(-2**31, 2**31 - 1).astype(np.int32)
            new_b_int32.tofile(os.path.join(args.out, entry["bias_file"]))

        shift_per_channel = np.round(np.log2(1.0 / (w_scale_new + 1e-30))).clip(0, 31).astype(int)
        out_shift_new = int(np.round(np.mean(shift_per_channel)))

        new_qcfg[tag] = dict(entry)
        new_qcfg[tag]["weight_scale"] = w_scale_new.tolist()
        new_qcfg[tag]["out_shift"] = out_shift_new
        new_qcfg[tag]["gamma_folded"] = True

        print(f"[{tag}] old_shift={entry['out_shift']} new_shift={out_shift_new}  "
              f"mean|w_scale| old={w_scale_old.mean():.6f} new={w_scale_new.mean():.6f}")

    with open(os.path.join(args.out, "quant_config.json"), "w") as f:
        json.dump(new_qcfg, f, indent=2)

    print(f"\n>>> wrote {args.out} (all 52 layers' weight/bias files, quant_config.json updated for the 10 folded fc2 layers)")


if __name__ == "__main__":
    main()
