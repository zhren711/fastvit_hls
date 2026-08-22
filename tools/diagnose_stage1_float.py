"""
diagnose_stage1_float.py -- A2 cliff bisection, step 1 (ZHR-92, 2026-08-21):
same technique that got Stem's own diagnosis right (cosine=0.999989 pure
float) -- but for the whole stem.1..stage1 span (entries 1-16 of the real
hardware sequence: stem.1, stem.2, both of stage0's RepMixerBlocks,
including the residual Add and the gamma-folded fc2 weights).

Deliberately does NOT hand-reimplement conv/GELU math in numpy (that would
risk introducing a NEW bug into the diagnostic itself, exactly the kind of
mistake that cost two cycles chasing Stem's scale). Instead: take the REAL
ONNX graph, REPLACE only the 10 conv layers' (layer_idx 1-10) weight/bias
initializers with their DEQUANTIZED-FROM-INT8 values (int8 code * scale,
same values the real hardware pipeline actually uses, including the
gamma-fold), and let onnxruntime run its own correct Conv/Erf/Add
semantics on the real image. This isolates exactly one variable: does
int8 weight quantization (with the LayerScale fold baked in) alone,
with NO activation quantization anywhere and the REAL erf-based GELU
(not hardware's crude placeholder -- the round's own framing groups the
GELU LUT's approximation under "quantization", not "wiring/topology"),
reproduce the ONNX reference at stage1?

  cosine ~= 1.0  -> weight quantization/fold is fine; the whole gap is in
                    activation-side machinery (scale propagation, shift,
                    or the GELU placeholder's numeric approximation)
  cosine still low -> a real topology/wiring/fold bug, narrows the next
                    entry-level bisection immediately

用法:
  python diagnose_stage1_float.py
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
STAGE1_TENSOR = "/stages.0/blocks/blocks.1/Add_output_0"

# layer_idx 1..10: stem.1, stem.2, both RepMixerBlocks (token_mixer, mlp/conv,
# fc1, fc2) x2 -- exactly the span entries 1-16 of the real hw sequence cover
TARGET_LAYER_IDX = list(range(1, 11))


def main():
    with open(r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json") as f:
        descriptor = json.load(f)
    with open(os.path.join(WEIGHTS_DIR, "quant_config.json")) as f:
        qcfg = json.load(f)
    by_layer_idx = {e["layer_idx"]: e for e in descriptor}

    model = onnx.load(MODEL)
    graph = model.graph
    init_map = {init.name: init for init in graph.initializer}

    replaced = []
    for li in TARGET_LAYER_IDX:
        d = by_layer_idx[li]
        tag = d["tag"]
        qc = qcfg[tag]
        node = graph.node[d["node_idx"]]
        assert node.op_type == "Conv", f"layer_idx={li} node {node.name} is not Conv"

        w_int8 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["weight_file"]), dtype=np.int8)
        w_scale = np.array(qc["weight_scale"])
        cout = qc["Cout"]
        w_float = (w_int8.astype(np.float64).reshape(cout, -1) * w_scale[:, None]).reshape(
            onnx.numpy_helper.to_array(init_map[node.input[1]]).shape).astype(np.float32)

        b_int32 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["bias_file"]), dtype=np.int32)
        b_scale = (1.0 / 127.0) * w_scale  # this project's uniform bias-scale convention
        b_float = (b_int32.astype(np.float64) * b_scale).astype(np.float32)

        w_init_name = node.input[1]
        b_init_name = node.input[2] if len(node.input) > 2 else None

        new_w = onnx.numpy_helper.from_array(w_float, w_init_name)
        init_map[w_init_name].CopyFrom(new_w)
        if b_init_name is not None:
            new_b = onnx.numpy_helper.from_array(b_float, b_init_name)
            init_map[b_init_name].CopyFrom(new_b)

        replaced.append((li, tag, node.name))

    print(f">>> replaced {len(replaced)} conv layers' weight/bias with dequantized-from-int8 "
          f"(gamma-folded where applicable) values:")
    for li, tag, name in replaced:
        print(f"    layer_idx={li:2d} {tag:20s} {name}")

    # 2026-08-21 fix: the graph's own layer_scale/Mul(conv_out, gamma) node
    # is STILL PRESENT and unmodified -- for fc2 layers substituted with
    # GAMMA-FOLDED weights (conv_out already = gamma*true_conv_out), this
    # node would apply gamma a SECOND time (gamma^2 * true_conv_out),
    # silently double-scaling exactly the 2 fc2 layers in this span. This
    # is why the diagnostic read 0.618 instead of ~1.0 like Stem's pure-
    # float check -- neutralize by setting that Mul's gamma input to 1.0
    # (keeps the graph structure, and the tap points diagnose_stage1_bisect.py
    # already uses, intact) for exactly the fc2 layers whose weight was
    # replaced with a gamma-folded value.
    neutralized = []
    for li, tag, name in replaced:
        if "fc2" not in name:
            continue
        conv_out = graph.node[by_layer_idx[li]["node_idx"]].output[0]
        mul_node = next(n for n in graph.node if n.op_type == "Mul" and conv_out in n.input)
        gamma_name = [i for i in mul_node.input if i != conv_out][0]
        gamma_arr = onnx.numpy_helper.to_array(init_map[gamma_name])
        init_map[gamma_name].CopyFrom(onnx.numpy_helper.from_array(
            np.ones_like(gamma_arr, dtype=np.float32), gamma_name))
        neutralized.append((li, mul_node.name, gamma_name))
    print(f">>> neutralized {len(neutralized)} redundant layer_scale Mul nodes "
          f"(gamma already folded into the substituted weight):")
    for li, mname, gname in neutralized:
        print(f"    layer_idx={li:2d} {mname} (gamma init {gname} set to 1.0)")

    existing_outputs = {o.name for o in graph.output}
    if STAGE1_TENSOR not in existing_outputs:
        graph.output.append(onnx.helper.make_tensor_value_info(STAGE1_TENSOR, onnx.TensorProto.FLOAT, None))

    tmp_path = os.path.join(REF_DIR, "_diag_stage1_tapped.onnx")
    onnx.save(model, tmp_path)
    sess = ort.InferenceSession(tmp_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    img = make_synthetic_calibration_batch(1, h=256, w=256, seed=20260813)
    (diag_out,) = sess.run([STAGE1_TENSOR], {input_name: img})

    ref = np.load(os.path.join(REF_DIR, "ckpt_ref_stage1_0000.npy"))

    a = diag_out.ravel().astype(np.float64)
    b = ref.ravel().astype(np.float64)
    cos = float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
    rel_l2 = float(np.linalg.norm(a - b) / np.linalg.norm(b))
    print(f"\n>>> DIAGNOSTIC (dequantized folded weights, real erf-GELU, no activation "
          f"quantization anywhere): cosine={cos:.6f} rel_L2={rel_l2:.4f}")
    print(f"    diag range=[{a.min():.4f},{a.max():.4f}]  ref range=[{b.min():.4f},{b.max():.4f}]")

    os.remove(tmp_path)


if __name__ == "__main__":
    main()
