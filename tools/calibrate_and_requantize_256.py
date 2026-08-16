"""
calibrate_and_requantize_256.py -- Phase A step 2b-2: 256x256 variant of
calibrate_and_requantize.py, run on top of the gamma-folded weights
(weights_t8_gamma_folded/, step 2b-1) rather than weights_t8_pruned/.
Same method, different target resolution (ZHR-63 revision 5, confirmed
2026-08-15: 256x256, not 128x128) and different weight source. Order
matters: gamma folding first, then calibration measures the POST-fold
dynamic range -- calibrating before folding would measure the wrong range
and need redoing (see fea504d's commit message).

Kept as a separate file rather than a --resolution flag on the 128x128
script, matching this project's existing convention (run_accuracy_harness.py
/ run_accuracy_harness_256.py) of not letting a single parameterized script
silently regress the already-validated 128x128 path.

calibrate_and_requantize.py -- Phase 0.8 fix 1: replace the never-calibrated
default_act_scale=1/127 placeholder with real per-layer/per-tensor
activation scales, and recompute out_shift/bias accordingly.

Root cause (see ZHR-8 2026-08-15, Phase 0.8 step 5): export_weights.py /
export_weights_pruned.py hardcode input_scale=output_scale=1/127 for EVERY
one of the 52 layers, and compute_out_shift() additionally assumes
output_scale==input_scale (so out_shift is derived from weight_scale alone).
Direct measurement at the Stem checkpoint showed the real activation range
there is [-37.56, 3.93] -- the scale that would actually fit it is ~0.296,
~37.6x larger than the placeholder. This was never calibrated: it's exactly
Phase 0.7's own step 3 ("run real-image activation calibration"), deferred
through 9+ rounds. See CLAUDE.md's working-method note added 2026-08-15:
verify an admitted placeholder first when the symptom matches it.

Architectural constraint this script must respect (NOT touched -- would
require HLS/ROM changes, out of scope): both the GELU op (gelu_lut.c,
scale=1/127 baked into the FPGA IP's ROM at synthesis time, see
gelu_lut.h's docstring) and the plain fv_run_add (no rescale capability at
all) force specific tensors in the graph to share a scale:
  - Any conv whose output feeds a GELU: output_scale is FORCED to 1/127
    (matches the ROM's assumed input range).
  - Any conv whose input comes directly from a GELU: input_scale is
    FORCED to 1/127 (matches the ROM's assumed output range).
  - The two producers feeding a RepMixerBlock's residual Add (token_mixer
    chain's DW7, and the ConvFFN branch's PW2) must share the exact same
    output_scale (Add has no rescale) -- this shared value is calibrated
    from the Add's OWN output tensor, not chosen independently per side.
Everywhere else (Stem's own 2nd/3rd conv boundary, each Transition's final
PW, each RepMixerBlock's DW3, FinalDW, SE fc1/fc2) is a free conv-to-conv
edge and gets its own real calibrated scale.

This classification is derived programmatically from the ONNX graph (which
nodes actually feed a GELU-decomposition Div / a genuine residual Add),
not hand-typed, and printed for verification before use.

Calibration method (method details, for the paper reproduction writeup):
  - N_CALIB real-image-equivalent samples: this repo has no photos (search
    already documented in prior comments), so calibration uses the same
    structured-synthetic generator as calibrate_activations.py (gradients +
    smoothed noise + edge stripes, NOT pure random noise, seed=42 -- same
    seed/generator as the existing W8A4 calibration script, DIFFERENT seed
    from the accuracy-test images (20260813) and W8A4 verification (999),
    so calibration and evaluation data never overlap).
  - Per-tensor (not per-channel) symmetric scale: scale = percentile(|x|,
    P) / 127, P=99.9 by default -- percentile clipping, not raw min/max,
    to avoid a handful of outlier activations blowing up the whole range
    (same convention as calibrate_activations.py). Symmetric (scale-only,
    zero-point=0) by construction: percentile is taken over |x| directly,
    so an asymmetric distribution like the Stem tensor's real
    [-37.56, 3.93] (negative extent ~9.5x the positive one) is still
    handled correctly -- the scale is sized by whichever side has larger
    magnitude (the negative side here), matching what the driver's
    existing clip-to-[-128,127]-int8 arithmetic actually requires. This
    intentionally wastes int8 codebook resolution on the smaller-magnitude
    side (a known, accepted cost of symmetric/zero-point-0 quantization,
    not a bug) -- true asymmetric (affine, with a zero-point) quantization
    would need every fv_run_* conv/add/gelu call to carry a zero-point
    term, which is an HLS IP change and out of scope here.

Weight quantization itself is UNCHANGED (still per-output-channel symmetric
off the real fp32 weight values, independent of activation scale) --
this script reuses the exact int8 weight .bin files already exported by
export_weights_pruned.py and only recomputes bias (re-quantized against
the new input_scale) and out_shift (recomputed against real input_scale
AND output_scale, not assuming they're equal).

用法:
  python calibrate_and_requantize.py [--model <path>]
      [--src-weights weights_t8_pruned] [--out weights_t8_calibrated]
      [--n_calib 24] [--percentile 99.9] [--seed 42]
"""
import onnx
import onnx.numpy_helper
import onnx.shape_inference
import onnxruntime as ort
import numpy as np
import json
import os
import shutil
import argparse
import sys

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch  # reuse, seed=42 default


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--src-weights", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\weights_t8_calibrated_256")
    p.add_argument("--h", type=int, default=256)
    p.add_argument("--w", type=int, default=256)
    p.add_argument("--n_calib", type=int, default=24)
    p.add_argument("--percentile", type=float, default=99.9)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--gamma_folded", type=lambda x: x.lower() != "false", default=True,
                   help="set false if --src-weights weights do NOT have LayerScale gamma folded into fc2")
    p.add_argument("--gelu_scale", type=float, default=1.0 / 127.0,
                   help="fixed scale baked into gelu_lut.c's ROM -- NOT calibratable without an HLS change")
    return p.parse_args()


def find_conv_nodes(graph):
    return [(i, n) for i, n in enumerate(graph.node) if n.op_type == "Conv"]


def is_constant_input(name, init_names):
    return name in init_names


def classify_conv_edges(graph, conv_nodes, init_names, gamma_folded=False):
    """For each conv (node_idx, node), determine:
       - output_role: 'gelu_forced' | 'add_shared:<add_output_name>' | 'free'
       - input_role:  'gelu_forced' | 'add_shared:<add_output_name>' | 'free' | 'network_input'
       by walking actual graph consumers/producers -- not hand-typed.

    gamma_folded: True when calibrating scales for weights that already have
    LayerScale gamma folded into fc2 (step 2b-1, fold_layer_scale.py). The
    ONNX graph being measured here is the UN-folded original (fc2 -> Mul
    (gamma) -> Add), but the DEPLOYED fc2 output already includes the gamma
    multiply -- so its real magnitude matches the un-folded graph's
    post-Mul tensor (which feeds the Add), not fc2's own raw pre-Mul
    output. With gamma_folded=True, output_role treats a Conv -> Mul(const)
    -> Add chain the same as a direct Conv -> Add (walks through the
    constant Mul). With gamma_folded=False (un-folded weights, e.g. the
    128x128 script), this is left off deliberately: bug 2 means the
    deployed fc2 output there really is the raw un-scaled value, and
    forcing it to share the Add's scale would calibrate against a
    magnitude the un-fixed hardware never actually produces."""
    name_to_producer = {}
    for n in graph.node:
        for o in n.output:
            name_to_producer[o] = n

    def output_role(conv_out_name):
        consumers = [n for n in graph.node if conv_out_name in n.input]
        for c in consumers:
            if c.op_type == "Div":
                return "gelu_forced"
        for c in consumers:
            if c.op_type == "Add":
                other_inputs = [x for x in c.input if x != conv_out_name]
                if other_inputs and not is_constant_input(other_inputs[0], init_names):
                    return f"add_shared:{c.output[0]}"
        if gamma_folded:
            for c in consumers:
                if c.op_type != "Mul":
                    continue
                mul_other = [x for x in c.input if x != conv_out_name]
                if not (mul_other and is_constant_input(mul_other[0], init_names)):
                    continue  # not a Mul-by-constant (gamma) -- e.g. GELU's Mul, leave alone
                mul_out = c.output[0]
                for c2 in graph.node:
                    if mul_out not in c2.input or c2.op_type != "Add":
                        continue
                    other2 = [x for x in c2.input if x != mul_out]
                    if other2 and not is_constant_input(other2[0], init_names):
                        return f"add_shared:{c2.output[0]}"
        return "free"

    def input_role(conv_in_name):
        producer = name_to_producer.get(conv_in_name)
        if producer is None:
            return "network_input"
        if producer.op_type == "Mul" and producer.name.endswith("Mul_1"):
            # tail of the Erf-GELU decomposition: Div->Erf->Add->Mul->Mul_1
            return "gelu_forced"
        if producer.op_type == "Add":
            other_inputs = [x for x in producer.input if x != conv_in_name]
            # only a genuine residual add if going further back doesn't land on GELU's Erf chain
            if other_inputs and not is_constant_input(other_inputs[0], init_names):
                return f"add_shared:{producer.output[0]}"
        # bug-4 fan-out (ZHR-8 2026-08-15): token_mixer(DW3)'s output feeds
        # BOTH the next conv (DW7) AND the block's residual Add directly.
        # If this tensor is ALSO consumed by an Add elsewhere, this conv is
        # reading the exact same physical add-shared buffer as its input --
        # not a value this producer computes independently of the Add.
        # output_role() answers exactly that ("do any of this tensor's
        # consumers include an Add") from the consumer side; reuse it here
        # instead of re-deriving it, so the two calls can't drift apart.
        fanout_role = output_role(conv_in_name)
        if fanout_role != "free":
            return fanout_role
        return "free"

    roles = {}
    for node_idx, node in conv_nodes:
        out_name = node.output[0]
        in_name = node.input[0]
        roles[node_idx] = {
            "name": node.name,
            "out": out_name,
            "in": in_name,
            "out_role": output_role(out_name),
            "in_role": input_role(in_name),
        }
    return roles


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    model = onnx.load(args.model)
    graph = model.graph
    init_names = {i.name for i in graph.initializer}
    init_map = {i.name: onnx.numpy_helper.to_array(i) for i in graph.initializer}

    with open(os.path.join(args.src_weights, "quant_config.json")) as f:
        src_cfg = json.load(f)
    layer_tags = sorted(src_cfg.keys(), key=lambda k: int(k.split("_")[1]))
    assert len(layer_tags) == 52

    conv_nodes = find_conv_nodes(graph)
    roles = classify_conv_edges(graph, conv_nodes, init_names, gamma_folded=args.gamma_folded)

    n_gelu_forced_out = sum(1 for r in roles.values() if r["out_role"] == "gelu_forced")
    n_add_shared_out = sum(1 for r in roles.values() if r["out_role"].startswith("add_shared"))
    print(f">>> classified {len(roles)} conv nodes: {n_gelu_forced_out} GELU-forced outputs, "
          f"{n_add_shared_out} Add-shared outputs, "
          f"{len(roles) - n_gelu_forced_out - n_add_shared_out} free outputs")
    print(">>> sanity check: FastViT-T8 has 1 (Stem) + 10 (RepMixer blocks) + 3 (Transitions) = 14 GELUs,")
    print(">>>                and 10 RepMixerBlock residual Adds -- expect 14 gelu_forced, 10 add_shared (x2 producers = 20 conv-output refs, but add_shared counts unique target names)")
    add_targets = {r["out_role"].split(":", 1)[1] for r in roles.values() if r["out_role"].startswith("add_shared")}
    print(f">>>   -> got {n_gelu_forced_out} gelu_forced, {len(add_targets)} unique Add targets from {n_add_shared_out} producer edges")

    # ---- tap every distinct tensor we need calibration data for ----
    tap_names = set()
    for r in roles.values():
        if r["out_role"] == "free":
            tap_names.add(r["out"])
        elif r["out_role"].startswith("add_shared"):
            tap_names.add(r["out_role"].split(":", 1)[1])  # tap the Add's own output
        if r["in_role"] == "network_input":
            pass  # handled separately via raw calibration images
        elif r["in_role"] == "free":
            # producer isn't necessarily a Conv this loop already taps (e.g.
            # SE's fc1 reads a ReduceMean output) -- tap the input tensor
            # itself so resolve_scale's tap_scale[r["in"]] fallback always
            # has an entry. Redundant (not missing) when the producer IS a
            # Conv already tapped via its own out_role above.
            tap_names.add(r["in"])
    tap_names = sorted(tap_names)
    print(f">>> tapping {len(tap_names)} distinct tensors for calibration")

    tapped = onnx.ModelProto()
    tapped.CopyFrom(model)
    existing_out = {o.name for o in tapped.graph.output}
    for name in tap_names:
        if name not in existing_out:
            tapped.graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    tapped_path = os.path.join(args.out, "_calib_tapped_tmp.onnx")
    onnx.save(tapped, tapped_path)
    sess = ort.InferenceSession(tapped_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    calib_imgs = make_synthetic_calibration_batch(args.n_calib, h=args.h, w=args.w, seed=args.seed)
    print(f">>> running {args.n_calib} synthetic calibration images (seed={args.seed}, "
          f"structured gradients/noise/stripes, NOT pure random -- same generator as "
          f"calibrate_activations.py, different seed from accuracy-eval(20260813)/W8A4-verify(999))")

    per_tap_abs = {name: [] for name in tap_names}
    per_tap_minmax = {name: [1e30, -1e30] for name in tap_names}
    input_abs = []
    for i in range(args.n_calib):
        img = calib_imgs[i:i + 1]
        input_abs.append(np.abs(img).ravel())
        outs = sess.run(tap_names, {input_name: img})
        for name, val in zip(tap_names, outs):
            per_tap_abs[name].append(np.abs(val).ravel())
            per_tap_minmax[name][0] = min(per_tap_minmax[name][0], float(val.min()))
            per_tap_minmax[name][1] = max(per_tap_minmax[name][1], float(val.max()))
    os.remove(tapped_path)

    def calc_scale(abs_list):
        all_abs = np.concatenate(abs_list)
        p_val = float(np.percentile(all_abs, args.percentile))
        max_val = float(np.max(all_abs))
        clip_val = p_val if p_val > 1e-8 else max(max_val, 1e-3)
        return clip_val / 127.0, max_val, p_val

    tap_scale = {}
    print(f"\n>>> per-tensor calibrated scale (percentile={args.percentile}, symmetric via |x|):")
    for name in tap_names:
        scale, max_val, p_val = calc_scale(per_tap_abs[name])
        mn, mx = per_tap_minmax[name]
        tap_scale[name] = scale
        print(f"    {name:55s} range=[{mn:8.3f},{mx:8.3f}]  max|x|={max_val:8.3f}  "
              f"p{args.percentile}|x|={p_val:8.3f}  scale={scale:.6f}  "
              f"(vs placeholder 1/127={1/127.0:.6f}, ratio={scale/(1/127.0):.2f}x)")

    net_input_scale, net_input_max, net_input_p = calc_scale(input_abs)
    print(f"\n>>> network raw input scale: {net_input_scale:.6f} "
          f"(max|x|={net_input_max:.3f}, p{args.percentile}={net_input_p:.3f}, "
          f"vs placeholder 1/127={1/127.0:.6f}, ratio={net_input_scale/(1/127.0):.2f}x)")

    # ---- resolve each conv's actual input_scale / output_scale ----
    def resolve_scale(role_str):
        if role_str == "gelu_forced":
            return args.gelu_scale
        if role_str == "network_input":
            return net_input_scale
        if role_str.startswith("add_shared"):
            return tap_scale[role_str.split(":", 1)[1]]
        return None  # 'free' resolved via tap_names keyed by tensor name below

    layer_scales = {}
    for node_idx, r in roles.items():
        out_scale = resolve_scale(r["out_role"])
        if out_scale is None:
            out_scale = tap_scale[r["out"]]
        in_scale = resolve_scale(r["in_role"])
        if in_scale is None:
            in_scale = tap_scale[r["in"]]
        layer_scales[node_idx] = (in_scale, out_scale)

    # ---- recompute bias + out_shift per layer, reusing existing int8 weights ----
    shutil.copy2(os.path.join(args.src_weights, "quant_config.json"),
                 os.path.join(args.out, "_src_quant_config.json"))
    new_cfg = {}
    for tag in layer_tags:
        entry = src_cfg[tag]
        node_idx = entry["node_idx"]
        in_scale, out_scale = layer_scales[node_idx]
        w_scale = np.array(entry["weight_scale"])
        old_b_scale = (1.0 / 127.0) * w_scale  # every prior layer used the 1/127 placeholder uniformly

        # re-derive fp32 bias from the existing correctly-quantized int32 bias file
        new_bias_file = None
        if entry["bias_file"]:
            b_int32_old = np.fromfile(os.path.join(args.src_weights, entry["bias_file"]), dtype=np.int32)
            b_fp32 = b_int32_old.astype(np.float64) * old_b_scale
            new_b_scale = in_scale * w_scale
            b_int32_new = np.round(b_fp32 / new_b_scale).clip(-2**31, 2**31 - 1).astype(np.int32)
            new_bias_file = entry["bias_file"]
            b_int32_new.tofile(os.path.join(args.out, new_bias_file))

        # copy weight file unchanged (weight quantization is activation-scale-independent)
        shutil.copy2(os.path.join(args.src_weights, entry["weight_file"]),
                     os.path.join(args.out, entry["weight_file"]))

        shift_per_channel = np.round(np.log2(out_scale / (in_scale * w_scale + 1e-30))).clip(0, 31).astype(int)
        out_shift = int(np.round(np.mean(shift_per_channel)))

        new_cfg[tag] = dict(entry)
        new_cfg[tag]["input_scale"] = float(in_scale)
        new_cfg[tag]["output_scale"] = float(out_scale)
        new_cfg[tag]["out_shift"] = out_shift
        new_cfg[tag]["calib_role_out"] = roles.get(node_idx, {}).get("out_role", "n/a")
        new_cfg[tag]["calib_role_in"] = roles.get(node_idx, {}).get("in_role", "n/a")

        print(f"[{tag}] in_scale={in_scale:.6f} out_scale={out_scale:.6f} "
              f"old_shift={entry['out_shift']} new_shift={out_shift}")

    with open(os.path.join(args.out, "quant_config.json"), "w") as f:
        json.dump(new_cfg, f, indent=2)

    print(f"\n>>> wrote {args.out}/quant_config.json + re-quantized bias files "
          f"(weight files copied unchanged from {args.src_weights})")
    print(f">>> network input_scale for re-quantizing test images: {net_input_scale:.6f} "
          f"(was 1/127={1/127.0:.6f})")


if __name__ == "__main__":
    main()
