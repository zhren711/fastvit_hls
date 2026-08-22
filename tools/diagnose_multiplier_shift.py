"""
diagnose_multiplier_shift.py -- A2 closeout accounting (ZHR-92, 2026-08-21),
Python-only, no hardware touched: does replacing pure power-of-2 out_shift
with a multiplier+shift ((acc*M)>>S, standard TFLite/TensorRT-style
fixed-point requantization) close the gap between idealized quantization
(0.90, diagnose_stage1_progressive.py) and real hardware (0.6325)?

Two curves, same progressive-quantization harness as diagnose_stage1_
progressive.py, differing only in how each layer's fake-quantize divisor
is computed per channel:

  shift_only:    effective_scale[oc] = output_scale / mult_err[oc], where
                 mult_err[oc] = 2**round(log2(1/(weight_scale[oc]*ratio)))
                 * weight_scale[oc] * ratio -- the exact per-channel
                 multiplicative bias INTEGER shift rounding introduces
                 (measured directly last round: up to +-41%, mean 15-19%
                 per layer). This is a SANITY CHECK first: if this curve
                 reproduces real hardware's 0.6325, it confirms the shift-
                 rounding theory is COMPLETE (not just plausible), before
                 trusting whatever number the alternative scheme gets.

  mult_shift:    effective_scale[oc] = weight_scale[oc]*ratio (the exact,
                 continuous target factor) rounded only to B=16-bit
                 fixed-point multiplier precision (~1/65536 relative
                 error, i.e. no shift-only rounding bias) -- what a
                 (acc*M)>>S scheme would achieve.

用法:
  python diagnose_multiplier_shift.py
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
MULT_BITS = 16


def per_channel_scales(li, descriptor_by_idx, qcfg, scales_by_layer, scheme):
    """This diagnostic works on the ALREADY-COMPUTED FLOAT conv output
    (onnxruntime's real Conv, not a simulated int32 accumulator), so the
    divisor applied to that float value must be derived carefully --
    first attempt at this function used `weight_scale*ratio` directly as
    a float-domain divisor, which is the ACCUMULATOR-domain factor
    (acc_int32 -> output_int8), not the float-domain one; caught because
    the resulting eff_scale values were ~100-1000x too large relative to
    output_scale, an obvious red flag on inspection before trusting the
    run. Correct derivation: acc_int32 = conv_float_output /
    (input_scale*weight_scale[oc]) (inverting the real int32-accumulate
    math), then shift-only hardware computes
    floor(acc_int32 / 2**shift[oc]) = floor(conv_float_output /
    (input_scale*weight_scale[oc]*2**shift[oc])) -- so the correct
    float-domain effective scale for shift-only is
    input_scale*weight_scale[oc]*2**shift[oc]. For the ideal/mult_shift
    case, a B=16-bit multiplier approximates 1/(weight_scale[oc]*ratio)
    to ~1/2**B relative precision, converging to the plain uniform
    output_scale (exactly what diagnose_stage1_progressive.py already
    used) -- no separate per-channel formula needed there.
    """
    d = descriptor_by_idx[li]
    tag = d["tag"]
    ws = np.array(qcfg[tag]["weight_scale"])
    in_s = scales_by_layer[str(li)]["input_scale"]
    out_s = scales_by_layer[str(li)]["output_scale"]
    ratio = in_s / out_s

    if scheme == "mult_shift":
        # B-bit fixed-point multiplier of 1/(weight_scale*ratio) converges
        # to output_scale essentially exactly (relative error ~1/2**B) --
        # uniform per layer, same as the earlier idealized progressive run.
        effective_scale = np.full_like(ws, out_s)
    elif scheme == "shift_only":
        shift = np.round(np.log2(1.0 / (ws * ratio + 1e-30)))
        effective_scale = in_s * ws * (2.0 ** shift)
    else:
        raise ValueError(scheme)

    return effective_scale, out_s


def build_model(quantize_up_to_idx, descriptor, qcfg, scales_by_layer, scheme):
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

        if "fc2" in node.name:
            conv_out = node.output[0]
            mul_node = next(n for n in graph.node if n.op_type == "Mul" and conv_out in n.input)
            gamma_name = [i for i in mul_node.input if i != conv_out][0]
            gamma_arr = onnx.numpy_helper.to_array(init_map[gamma_name])
            init_map[gamma_name].CopyFrom(onnx.numpy_helper.from_array(
                np.ones_like(gamma_arr, dtype=np.float32), gamma_name))

        original_name = node.output[0]
        prequant_name = original_name + "_prequant"
        node.output[0] = prequant_name

        eff_scale, _ = per_channel_scales(li, by_layer_idx, qcfg, scales_by_layer, scheme)
        # broadcast shape [C,1,1] to match NCHW per-channel application
        scale_arr = eff_scale.reshape(-1, 1, 1).astype(np.float32)
        scale_const_name = f"fq_scale_{scheme}_{li}"
        graph.initializer.append(onnx.numpy_helper.from_array(scale_arr, scale_const_name))

        div_out, round_out, clip_out = f"fq_div_{scheme}_{li}", f"fq_round_{scheme}_{li}", f"fq_clip_{scheme}_{li}"
        clip_min_name, clip_max_name = f"fq_clipmin_{scheme}_{li}", f"fq_clipmax_{scheme}_{li}"
        graph.initializer.append(onnx.numpy_helper.from_array(np.array(-128.0, dtype=np.float32), clip_min_name))
        graph.initializer.append(onnx.numpy_helper.from_array(np.array(127.0, dtype=np.float32), clip_max_name))
        graph.node.append(onnx.helper.make_node("Div", [prequant_name, scale_const_name], [div_out], name=f"fq_div_node_{scheme}_{li}"))
        graph.node.append(onnx.helper.make_node("Round", [div_out], [round_out], name=f"fq_round_node_{scheme}_{li}"))
        graph.node.append(onnx.helper.make_node("Clip", [round_out, clip_min_name, clip_max_name], [clip_out], name=f"fq_clip_node_{scheme}_{li}"))
        graph.node.append(onnx.helper.make_node("Mul", [clip_out, scale_const_name], [original_name], name=f"fq_mul_node_{scheme}_{li}"))

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

    print(f"{'boundary':10s} {'shift_only':>12s} {'mult_shift':>12s}")
    print("-" * 40)
    for boundary in ALL_LAYERS:
        results = {}
        for scheme in ("shift_only", "mult_shift"):
            model = build_model(boundary, descriptor, qcfg, scales_by_layer, scheme)
            graph = model.graph
            existing = {o.name for o in graph.output}
            if STAGE1_TENSOR not in existing:
                graph.output.append(onnx.helper.make_tensor_value_info(STAGE1_TENSOR, onnx.TensorProto.FLOAT, None))
            tmp = os.path.join(REF_DIR, f"_diag_ms_{scheme}_{boundary}.onnx")
            onnx.save(model, tmp)
            sess = ort.InferenceSession(tmp, providers=["CPUExecutionProvider"])
            input_name = sess.get_inputs()[0].name
            (out,) = sess.run([STAGE1_TENSOR], {input_name: img})
            a = out.ravel().astype(np.float64)
            cos = float(a @ ref_flat / (np.linalg.norm(a) * np.linalg.norm(ref_flat)))
            results[scheme] = cos
            os.remove(tmp)
        print(f"{boundary:<10d} {results['shift_only']:12.4f} {results['mult_shift']:12.4f}")


if __name__ == "__main__":
    main()
