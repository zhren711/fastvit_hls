"""
diagnose_stage1_bisect.py -- A2 cliff bisection, step 2 (ZHR-92, 2026-08-21):
diagnose_stage1_float.py showed the float diagnostic (dequantized folded
weights, real erf-GELU, zero activation quantization) reproduces the SAME
gap as real hardware (cosine 0.618 vs hardware's 0.6325) -- this rules out
scale propagation / Add-rescale entirely (there is no quantization at all
in that diagnostic), and points at weight quantization/fold or a topology
mismatch in one of the 10 conv layers spanning stem.1 through stage1
(layer_idx 1-10).

This taps EVERY one of those 10 layers' own raw conv-output tensor in a
SINGLE re-run (same weight substitution as step 1), computing per-layer
cosine against the TRUE (original, unmodified-weight) ONNX reference at
that exact tensor -- localizes the divergence to a specific layer/entry
in one shot instead of 10 separate runs.

用法:
  python diagnose_stage1_bisect.py
"""
import onnx
import onnx.numpy_helper
import onnxruntime as ort
import numpy as np
import json
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch

MODEL = r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx"
WEIGHTS_DIR = r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded"
REF_DIR = r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256"

TARGET_LAYER_IDX = list(range(1, 11))


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom < 1e-12:
        return float("nan")
    return float(a @ b / denom)


def main():
    with open(r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json") as f:
        descriptor = json.load(f)
    with open(os.path.join(WEIGHTS_DIR, "quant_config.json")) as f:
        qcfg = json.load(f)
    by_layer_idx = {e["layer_idx"]: e for e in descriptor}

    # ---- reference run: TRUE weights, tap every target layer's own output ----
    # EXCEPT the gamma-folded fc2 layers: folding makes the CONV's own raw
    # output already gamma-scaled (that's the whole point), so the correct
    # comparison point in the ORIGINAL/unfolded graph is AFTER its separate
    # Mul(fc2_out, gamma) node, not the conv's own pre-gamma output -- same
    # resolution compare_checkpoint_cosine.py already uses. Comparing the
    # conv's own output directly (what this script did on the first attempt)
    # compares a gamma-scaled tensor against a pre-gamma one BY CONSTRUCTION
    # -- caught by checking a single isolated layer's substitution against
    # its own true reference before trusting the original bisection result.
    model_ref = onnx.load(MODEL)
    graph_ref = model_ref.graph

    def is_folded_fc2(li):
        node = graph_ref.node[by_layer_idx[li]["node_idx"]]
        return "fc2" in node.name

    def conv_output_tap(graph, li):
        return graph.node[by_layer_idx[li]["node_idx"]].output[0]

    def true_reference_tap(li):
        conv_out = conv_output_tap(graph_ref, li)
        if not is_folded_fc2(li):
            return conv_out
        for n in graph_ref.node:
            if n.op_type == "Mul" and conv_out in n.input:
                return n.output[0]
        raise RuntimeError(f"layer_idx={li}: no layer_scale Mul found consuming {conv_out}")

    # ref side needs the TRUE (post-gamma, for fc2 layers) tensor; diag side
    # (weights already gamma-folded) needs the conv's OWN raw output, which
    # is already the post-gamma value there -- two different tensor names
    # for the fc2 layers, deliberately, not a shared tap_names dict.
    ref_tap = {li: true_reference_tap(li) for li in TARGET_LAYER_IDX}
    diag_tap = {li: conv_output_tap(graph_ref, li) for li in TARGET_LAYER_IDX}

    existing = {o.name for o in graph_ref.output}
    for name in ref_tap.values():
        if name not in existing:
            graph_ref.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    ref_tmp = os.path.join(REF_DIR, "_diag_bisect_ref.onnx")
    onnx.save(model_ref, ref_tmp)
    sess_ref = ort.InferenceSession(ref_tmp, providers=["CPUExecutionProvider"])
    input_name = sess_ref.get_inputs()[0].name
    img = make_synthetic_calibration_batch(1, h=256, w=256, seed=20260813)
    ref_out_names = [ref_tap[li] for li in TARGET_LAYER_IDX]
    ref_outs = dict(zip(ref_out_names, sess_ref.run(ref_out_names, {input_name: img})))
    os.remove(ref_tmp)

    # ---- diagnostic run: dequantized-from-int8 (gamma-folded) weights,
    # same taps ----
    model_diag = onnx.load(MODEL)
    graph_diag = model_diag.graph
    init_map = {init.name: init for init in graph_diag.initializer}
    for li in TARGET_LAYER_IDX:
        d = by_layer_idx[li]
        tag = d["tag"]
        qc = qcfg[tag]
        node = graph_diag.node[d["node_idx"]]

        w_int8 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["weight_file"]), dtype=np.int8)
        w_scale = np.array(qc["weight_scale"])
        cout = qc["Cout"]
        orig_shape = onnx.numpy_helper.to_array(init_map[node.input[1]]).shape
        w_float = (w_int8.astype(np.float64).reshape(cout, -1) * w_scale[:, None]).reshape(orig_shape).astype(np.float32)

        b_int32 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["bias_file"]), dtype=np.int32)
        b_scale = (1.0 / 127.0) * w_scale
        b_float = (b_int32.astype(np.float64) * b_scale).astype(np.float32)

        init_map[node.input[1]].CopyFrom(onnx.numpy_helper.from_array(w_float, node.input[1]))
        if len(node.input) > 2:
            init_map[node.input[2]].CopyFrom(onnx.numpy_helper.from_array(b_float, node.input[2]))

    existing = {o.name for o in graph_diag.output}
    for name in diag_tap.values():
        if name not in existing:
            graph_diag.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    diag_tmp = os.path.join(REF_DIR, "_diag_bisect_diag.onnx")
    onnx.save(model_diag, diag_tmp)
    sess_diag = ort.InferenceSession(diag_tmp, providers=["CPUExecutionProvider"])
    diag_out_names = [diag_tap[li] for li in TARGET_LAYER_IDX]
    diag_outs = dict(zip(diag_out_names, sess_diag.run(diag_out_names, {input_name: img})))
    os.remove(diag_tmp)

    print(f"{'layer_idx':10s} {'tag':20s} {'cosine':>8s}   note")
    print("-" * 80)
    for li in TARGET_LAYER_IDX:
        cs = cosine_sim(diag_outs[diag_tap[li]], ref_outs[ref_tap[li]])
        tag = by_layer_idx[li]["tag"]
        note = "(vs post-gamma Mul)" if is_folded_fc2(li) else ""
        print(f"{li:<10d} {tag:20s} {cs:8.4f}   {note}")


if __name__ == "__main__":
    main()
