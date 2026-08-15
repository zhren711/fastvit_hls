"""
compare_checkpoint_cosine.py -- Phase 0.8 step 5.

Segmented hardware-vs-ONNX cosine comparison at 6 checkpoints (after Stem,
after each Stage, after FinalDW), using the FULL int8 tensors dumped by
fastvit_infer_checkpoint_dump.c (not min/max/mean) against the matching
ONNX float32 intermediate tensors. This project has never done a
layer-by-layer hardware-vs-float comparison before -- only aggregate
end-to-end (run_accuracy_harness.py) and hardware-vs-hardware (Phase 0.7
step 9's full_trace diff). Directly answers where the accuracy gap starts,
and as a side effect tells us whether the "3 pruned layers' weight content
doesn't affect final output" anomaly (see ZHR-8 2026-08-15) already shows
up by Stage2/Stage3 (where those layers live) or only appears later.

Checkpoint -> ONNX tensor resolution: "stem" and "finaldw" are Conv node
outputs (looked up directly by node_idx from quant_config.json); the 4
stage checkpoints are RepMixerBlock outputs, i.e. the Add node that sums
token_mixer(x) and convffn(x) -- found by searching forward from the
block's last conv (mlp.fc2) node_idx for the next Add node consuming its
output.

用法:
  python compare_checkpoint_cosine.py --dump-dir <board dump dir> [--n 8]
"""
import onnx
import onnx.numpy_helper
import onnxruntime as ort
import numpy as np
import json
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch  # reuse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--quant-config",
                   default=r"E:\codes\microzed\fastvit_hls\weights_t8_pruned\quant_config.json")
    p.add_argument("--dump-dir", required=True,
                   help="directory containing ckpt_stem.bin .. ckpt_finaldw.bin per image, "
                        "named ckpt_<tag>_<NNNN>.bin (see fetch step)")
    p.add_argument("--n", type=int, default=8)
    p.add_argument("--seed", type=int, default=20260813)  # matches run_accuracy_harness.py
    p.add_argument("--scale", type=float, default=1.0 / 127.0,
                   help="uniform default_act_scale used for every layer's output_scale")
    return p.parse_args()


CHECKPOINTS = ["stem", "stage1", "stage2", "stage3", "stage4", "finaldw"]
# tag -> quant_config key of the last conv in that segment
LAST_CONV_TAG = {
    "stem":    "layer_0002_pwconv",
    "stage1":  "layer_0010_pwconv",
    "stage2":  "layer_0020_pwconv",
    "stage3":  "layer_0038_pwconv",
    "stage4":  "layer_0048_pwconv",
    "finaldw": "layer_0049_dwconv",
}
IS_DIRECT_CONV_OUTPUT = {"stem", "finaldw"}  # others need the following Add node


def find_tensor_name(graph, qcfg, tag):
    node_idx = qcfg[LAST_CONV_TAG[tag]]["node_idx"]
    conv_node = graph.node[node_idx]
    conv_out = conv_node.output[0]
    if tag in IS_DIRECT_CONV_OUTPUT:
        return conv_out
    # RepMixerBlock's fc2 output does NOT feed the residual Add directly --
    # it passes through a layer_scale.gamma Mul first (fc2_out -> Mul(gamma)
    # -> Add(token_mixer_out, that)). The driver currently skips this Mul
    # entirely (see ZHR-8 2026-08-15), but the checkpoint we want is still
    # "the block's real final output", i.e. the Add's output -- so follow
    # the dependency chain: conv_out -> (Mul consuming it) -> (Add consuming
    # that Mul's output).
    mul_out = None
    for n in graph.node[node_idx:]:
        if n.op_type == "Mul" and conv_out in n.input:
            mul_out = n.output[0]
            break
    if mul_out is None:
        raise RuntimeError(f"could not find Mul(layer_scale) node consuming {conv_out} for checkpoint {tag}")
    for n in graph.node[node_idx:]:
        if n.op_type == "Add" and mul_out in n.input:
            return n.output[0]
    raise RuntimeError(f"could not find Add node consuming {mul_out} for checkpoint {tag}")


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom < 1e-12:
        return float("nan")
    return float(np.dot(a, b) / denom)


def main():
    args = parse_args()

    model = onnx.load(args.model)
    graph = model.graph
    with open(args.quant_config) as f:
        qcfg = json.load(f)

    tensor_names = {tag: find_tensor_name(graph, qcfg, tag) for tag in CHECKPOINTS}
    print(">>> checkpoint -> ONNX tensor:")
    for tag in CHECKPOINTS:
        print(f"    {tag:10s} -> {tensor_names[tag]}")

    existing = {o.name for o in graph.output}
    for name in tensor_names.values():
        if name not in existing:
            graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))

    tmp_path = os.path.join(os.path.dirname(args.dump_dir.rstrip("/\\")) or ".", "_ckpt_tapped_tmp.onnx")
    onnx.save(model, tmp_path)
    sess = ort.InferenceSession(tmp_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    out_names = [tensor_names[t] for t in CHECKPOINTS]

    imgs = make_synthetic_calibration_batch(args.n, seed=args.seed)  # [N,3,128,128] float32

    per_ckpt_cos = {tag: [] for tag in CHECKPOINTS}
    for i in range(args.n):
        img = imgs[i:i + 1]
        refs = sess.run(out_names, {input_name: img})
        tag_to_ref = dict(zip(CHECKPOINTS, refs))

        for tag in CHECKPOINTS:
            dump_path = os.path.join(args.dump_dir, f"{i:04d}", f"ckpt_{tag}.bin")
            if not os.path.exists(dump_path):
                print(f"  img {i} {tag}: MISSING {dump_path}")
                continue
            board_int8 = np.fromfile(dump_path, dtype=np.int8)
            board_float = board_int8.astype(np.float64) * args.scale
            ref = tag_to_ref[tag]
            if board_float.size != ref.size:
                print(f"  img {i} {tag}: SHAPE MISMATCH board={board_float.size} ref={ref.size}")
                continue
            cs = cosine_sim(board_float, ref)
            per_ckpt_cos[tag].append(cs)

    print()
    print(">>> mean cosine per checkpoint (hardware int8 dequant vs ONNX float32):")
    for tag in CHECKPOINTS:
        vals = per_ckpt_cos[tag]
        if vals:
            print(f"    {tag:10s}  mean cos = {np.mean(vals):7.4f}   (n={len(vals)})  "
                  f"per-image: {['%.3f' % v for v in vals]}")
        else:
            print(f"    {tag:10s}  NO DATA")

    os.remove(tmp_path)


if __name__ == "__main__":
    main()
