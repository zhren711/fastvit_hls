"""
calibrate_all_layers_256.py -- A2 exit measurement (ZHR-92, 2026-08-21):
extends calibrate_stem_256.py's methodology (real batch, N images,
99.9th-percentile-of-|abs|, not max) from Stem alone to all 52 conv
layers' RAW OUTPUT tensors (node.output[0], pre-activation -- same
distinction from calibrate_activations.py's input-tapping noted in
calibrate_stem_256.py: what's needed here is what each conv itself
produces before GELU/Add/etc, since that's what out_shift converts the
accumulator into and what the NEXT stage reads as its input scale).

Only computes the "ideal" (independently-optimal) per-layer output scale
from real data -- does NOT decide which layers must share a scale (e.g.
Add's two operands). That's gen_hw_sequence.py's job (it already walks
the exact DAG topology this decision depends on -- token_mixer/Add
pairing, mul-consumer disambiguation -- duplicating that walk here would
risk the two falling out of sync, exactly the failure mode this project's
"generator decides" principle exists to prevent).

用法:
  python calibrate_all_layers_256.py [--n 16] [--percentile 99.9]
"""
import onnx
import onnx.helper
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
    p.add_argument("--descriptor", default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\layer_calib_256.json")
    p.add_argument("--n", type=int, default=16)
    p.add_argument("--percentile", type=float, default=99.9)
    p.add_argument("--seed", type=int, default=20260813)
    return p.parse_args()


def main():
    args = parse_args()

    model = onnx.load(args.model)
    graph = model.graph
    with open(args.descriptor) as f:
        descriptor = json.load(f)
    assert len(descriptor) == 52

    tap_names = []
    layer_idx_by_tap = {}
    for e in descriptor:
        node = graph.node[e["node_idx"]]
        tap = node.output[0]
        tap_names.append(tap)
        layer_idx_by_tap[tap] = e["layer_idx"]

    existing_outputs = {o.name for o in graph.output}
    for name in tap_names:
        if name not in existing_outputs:
            graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
            existing_outputs.add(name)

    tmp_path = args.out.replace(".json", "_tapped.onnx")
    onnx.save(model, tmp_path)
    sess = ort.InferenceSession(tmp_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    output_names = [o.name for o in sess.get_outputs() if o.name in layer_idx_by_tap]

    print(f">>> calibrating {len(output_names)} conv-output taps over {args.n} real images "
          f"(seed={args.seed}), p{args.percentile}")

    imgs = make_synthetic_calibration_batch(args.n, h=256, w=256, seed=args.seed)

    # also collect the input image's own distribution -- same batch, same
    # framework, folds the 14.1%-saturation input_scale fix in here too
    input_abs_all = []

    per_tap_abs = {name: [] for name in output_names}
    for i in range(args.n):
        img = imgs[i:i + 1]
        input_abs_all.append(np.abs(img).ravel())
        outputs = sess.run(output_names, {input_name: img})
        for name, val in zip(output_names, outputs):
            per_tap_abs[name].append(np.abs(val).ravel())

    input_abs = np.concatenate(input_abs_all)
    input_clip = float(np.percentile(input_abs, args.percentile))
    input_scale = input_clip / 127.0
    print(f">>> input image: max={input_abs.max():.4f} p{args.percentile}={input_clip:.4f} "
          f"-> input_scale={input_scale:.6f}")

    result = {"input_scale": input_scale, "n_images": args.n, "percentile": args.percentile,
              "seed": args.seed, "layers": {}}
    for name in output_names:
        layer_idx = layer_idx_by_tap[name]
        abs_vals = np.concatenate(per_tap_abs[name])
        clip_val = float(np.percentile(abs_vals, args.percentile))
        max_val = float(np.max(abs_vals))
        if clip_val <= 1e-8:
            clip_val = max(max_val, 1e-3)
        ideal_scale = clip_val / 127.0
        result["layers"][str(layer_idx)] = {
            "tap_tensor": name,
            "max_abs": max_val,
            "p_abs": clip_val,
            "ideal_output_scale": ideal_scale,
        }

    with open(args.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f">>> wrote {args.out} ({len(result['layers'])} layers)")

    # summary: how far each layer's ideal scale is from the old uniform placeholder
    old = 1.0 / 127.0
    ratios = [result["layers"][k]["ideal_output_scale"] / old for k in result["layers"]]
    print(f">>> ideal_output_scale / old placeholder(1/127): min={min(ratios):.2f}x "
          f"max={max(ratios):.2f}x median={sorted(ratios)[len(ratios)//2]:.2f}x")

    os.remove(tmp_path)


if __name__ == "__main__":
    main()
