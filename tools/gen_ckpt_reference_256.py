"""
gen_ckpt_reference_256.py -- A2 exit measurement (ZHR-92, 2026-08-21):
regenerate the float32 ONNX checkpoint reference tensors at 256x256, using
the EXACT SAME input image compute_stem_arm.py used (same generator
function, same n, same seed -- make_synthetic_calibration_batch's RNG is
consumed sequentially per image index, independent of n, confirmed by
reading the code, not assumed -- so n=1 here reproduces img[0] from the
n=8 batch that produced accuracy_test_imgs_256/img_0000.bin bit-for-bit).

Phase 0.8's checkpoint reference batch (compare_checkpoint_cosine.py) was
generated at 128x128 and is INVALID for this comparison -- different
resolution changes every intermediate tensor's shape. This script is a
fresh 256x256 regeneration, not a reuse.

Float32, not quantized simulation: this taps the ORIGINAL onnx graph's
real float computation (Div->Erf->Add->Mul for GELU, the real Mul(gamma)
->Add chain for RepMixerBlock, etc.) with no quantization involved at all
-- comparing hardware's int8 output against this measures "hardware vs
real math", not "hardware vs our own quantization model" (which would
hide bugs in the quantization scheme itself, not just the datapath).

用法:
  python gen_ckpt_reference_256.py [--out <dir>]
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
from calibrate_activations import make_synthetic_calibration_batch


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--quant-config", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded\quant_config.json")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256")
    p.add_argument("--seed", type=int, default=20260813)  # matches run_accuracy_harness_256.py
    return p.parse_args()


CHECKPOINTS = ["stem", "stage1", "stage2", "stage3", "stage4", "finaldw", "se"]
LAST_CONV_TAG = {
    "stem":    "layer_0000_conv",
    "stage1":  "layer_0010_pwconv",
    "stage2":  "layer_0020_pwconv",
    "stage3":  "layer_0038_pwconv",
    "stage4":  "layer_0048_pwconv",
    "finaldw": "layer_0049_dwconv",
}
IS_DIRECT_CONV_OUTPUT = {"stem", "finaldw"}
SE_MUL_TENSOR = "/final_conv/se/Mul_output_0"


def find_tensor_name(graph, qcfg, tag):
    if tag == "se":
        return SE_MUL_TENSOR
    node_idx = qcfg[LAST_CONV_TAG[tag]]["node_idx"]
    conv_node = graph.node[node_idx]
    conv_out = conv_node.output[0]
    if tag in IS_DIRECT_CONV_OUTPUT:
        return conv_out
    # RepMixerBlock: fc2_out -> Mul(layer_scale.gamma) -> Add(token_mixer_out, that).
    # This is the REAL ONNX graph's actual computation, independent of
    # whatever our own hardware does with gamma (folded into fc2's weight
    # or not) -- the float reference must follow the true graph exactly.
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


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

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

    tmp_path = os.path.join(args.out, "_ckpt_ref_tapped_256.onnx")
    onnx.save(model, tmp_path)
    sess = ort.InferenceSession(tmp_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    img = make_synthetic_calibration_batch(1, h=256, w=256, seed=args.seed)  # [1,3,256,256], == img_0000's float32 source
    out_names = [tensor_names[t] for t in CHECKPOINTS]
    refs = sess.run(out_names, {input_name: img})

    for tag, ref in zip(CHECKPOINTS, refs):
        np.save(os.path.join(args.out, f"ckpt_ref_{tag}_0000.npy"), ref)
        print(f"  {tag:10s} shape={ref.shape} range=[{ref.min():.4f},{ref.max():.4f}]")

    os.remove(tmp_path)
    print(f"\n>>> wrote {len(CHECKPOINTS)} checkpoint reference tensors to {args.out}")


if __name__ == "__main__":
    main()
