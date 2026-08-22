"""
diagnose_stage1_progressive.py -- A2 cliff bisection, step 2 (ZHR-92,
2026-08-21), corrected float baseline (0.9948, after fixing the double-
gamma bug in diagnose_stage1_float.py -- see that file's history).

Progressively quantizes MORE of the chain (layer_idx 4, then 4-5, 4-6,
4-7, 4-8...), inserting a REAL fake-quantize (round to int8 grid at that
layer's actual calibrated output_scale, clip to [-128,127], dequantize
back to float) after each newly-included layer's output, while every
OTHER layer (both upstream of the starting point and not-yet-included
downstream) stays exact float. This is the test the earlier single-layer
isolation missed: isolation fed each tested layer a TRUE float input
(never what it actually receives in the real chain -- the PRIOR layer's
quantized output). This script feeds each layer exactly what it really
gets.

Smooth decline (per-layer cosine drops gradually) = genuine compounding,
consistent with the user's own math (independent per-layer ~0.999 should
NOT compound to 0.618 over 6 layers -- that requires amplification, not
just accumulation). A sharp single-step drop = that specific layer
mishandles a quantized (not float) input -- a real, locatable defect.

用法:
  python diagnose_stage1_progressive.py
"""
import onnx
import onnx.helper
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

ALL_LAYERS = list(range(1, 11))


def build_model(quantize_up_to_idx, descriptor, qcfg, scales_by_layer):
    """quantize_up_to_idx: layers with layer_idx <= this get their weight
    substituted (dequantized-from-int8) AND a fake-quantize inserted after
    their output. Layers with layer_idx > this keep the TRUE original
    weight and no quantization -- but still receive whatever (possibly
    quantized) tensor comes out of the last quantized layer, exactly like
    the real chain."""
    by_layer_idx = {e["layer_idx"]: e for e in descriptor}
    model = onnx.load(MODEL)
    graph = model.graph
    init_map = {init.name: init for init in graph.initializer}

    for li in ALL_LAYERS:
        if li > quantize_up_to_idx:
            continue
        d = by_layer_idx[li]
        tag = d["tag"]
        qc = qcfg[tag]
        node = graph.node[d["node_idx"]]

        w_int8 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["weight_file"]), dtype=np.int8)
        w_scale = np.array(qc["weight_scale"])
        cout = qc["Cout"]
        w_float = (w_int8.astype(np.float64).reshape(cout, -1) * w_scale[:, None]).reshape(
            onnx.numpy_helper.to_array(init_map[node.input[1]]).shape).astype(np.float32)
        b_int32 = np.fromfile(os.path.join(WEIGHTS_DIR, qc["bias_file"]), dtype=np.int32)
        b_scale = (1.0 / 127.0) * w_scale
        b_float = (b_int32.astype(np.float64) * b_scale).astype(np.float32)

        init_map[node.input[1]].CopyFrom(onnx.numpy_helper.from_array(w_float, node.input[1]))
        if len(node.input) > 2:
            init_map[node.input[2]].CopyFrom(onnx.numpy_helper.from_array(b_float, node.input[2]))

        # neutralize the redundant layer_scale Mul for gamma-folded fc2 layers
        if "fc2" in node.name:
            conv_out = node.output[0]
            mul_node = next(n for n in graph.node if n.op_type == "Mul" and conv_out in n.input)
            gamma_name = [i for i in mul_node.input if i != conv_out][0]
            gamma_arr = onnx.numpy_helper.to_array(init_map[gamma_name])
            init_map[gamma_name].CopyFrom(onnx.numpy_helper.from_array(
                np.ones_like(gamma_arr, dtype=np.float32), gamma_name))

        # fake-quantize: round(x/scale) clipped to int8, dequantized back --
        # redirect this conv's own output to a "prequant" name, insert the
        # quant/dequant chain, restore the ORIGINAL name as the chain's
        # final output so every existing downstream consumer picks it up
        # automatically without needing to be rewired individually.
        original_name = node.output[0]
        prequant_name = original_name + "_prequant"
        node.output[0] = prequant_name

        out_scale = scales_by_layer[str(li)]["output_scale"]
        scale_const_name = f"fq_scale_{li}"
        graph.initializer.append(onnx.numpy_helper.from_array(
            np.array(out_scale, dtype=np.float32), scale_const_name))

        div_out = f"fq_div_{li}"
        round_out = f"fq_round_{li}"
        clip_out = f"fq_clip_{li}"
        clip_min_name, clip_max_name = f"fq_clipmin_{li}", f"fq_clipmax_{li}"
        graph.initializer.append(onnx.numpy_helper.from_array(np.array(-128.0, dtype=np.float32), clip_min_name))
        graph.initializer.append(onnx.numpy_helper.from_array(np.array(127.0, dtype=np.float32), clip_max_name))
        graph.node.append(onnx.helper.make_node("Div", [prequant_name, scale_const_name], [div_out], name=f"fq_div_node_{li}"))
        graph.node.append(onnx.helper.make_node("Round", [div_out], [round_out], name=f"fq_round_node_{li}"))
        graph.node.append(onnx.helper.make_node("Clip", [round_out, clip_min_name, clip_max_name], [clip_out],
                                                  name=f"fq_clip_node_{li}"))
        graph.node.append(onnx.helper.make_node("Mul", [clip_out, scale_const_name], [original_name], name=f"fq_mul_node_{li}"))

    return model


def main():
    with open(r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json") as f:
        descriptor = json.load(f)
    with open(os.path.join(WEIGHTS_DIR, "quant_config.json")) as f:
        qcfg = json.load(f)
    with open(os.path.join(REF_DIR, "shift_table_meta.json")) as f:
        shift_meta = json.load(f)
    scales_by_layer = shift_meta["scales_by_layer"]

    img = make_synthetic_calibration_batch(1, h=256, w=256, seed=20260813)
    ref = np.load(os.path.join(REF_DIR, "ckpt_ref_stage1_0000.npy"))
    ref_flat = ref.ravel().astype(np.float64)

    print(f"{'quantize_up_to':16s} {'cosine':>8s} {'rel_L2':>8s}")
    print("-" * 40)
    for boundary in ALL_LAYERS:
        model = build_model(boundary, descriptor, qcfg, scales_by_layer)
        graph = model.graph
        existing = {o.name for o in graph.output}
        if STAGE1_TENSOR not in existing:
            graph.output.append(onnx.helper.make_tensor_value_info(STAGE1_TENSOR, onnx.TensorProto.FLOAT, None))
        tmp = os.path.join(REF_DIR, f"_diag_prog_{boundary}.onnx")
        onnx.save(model, tmp)
        sess = ort.InferenceSession(tmp, providers=["CPUExecutionProvider"])
        input_name = sess.get_inputs()[0].name
        (out,) = sess.run([STAGE1_TENSOR], {input_name: img})
        a = out.ravel().astype(np.float64)
        cos = float(a @ ref_flat / (np.linalg.norm(a) * np.linalg.norm(ref_flat)))
        rel_l2 = float(np.linalg.norm(a - ref_flat) / np.linalg.norm(ref_flat))
        print(f"{boundary:<16d} {cos:8.4f} {rel_l2:8.4f}")
        os.remove(tmp)


if __name__ == "__main__":
    main()
