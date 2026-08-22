"""
calibrate_stem_256.py -- A2 exit measurement follow-up (ZHR-92, 2026-08-21):
calibrate_activations.py has existed since Phase 0.7 (flagged there as
"step 3", deferred 10+ rounds, never actually run against real data) --
this reuses its exact methodology (percentile-of-abs-value over a real
batch, not max, not a single image) rather than inventing a new one, but
adapted for what THIS round needs, which differs in two ways the original
script doesn't cover:

  1. Resolution: 256x256 (this project's confirmed target), not 128x128.
  2. Tap point: the RAW CONV OUTPUT (node.output[0]), not each conv's own
     INPUT (node.input[0], which is POST-activation). calibrate_activations.py
     taps inputs because it's calibrating what each conv's OWN MAC array
     consumes; this script needs Stem's raw pre-GELU output specifically,
     because that's exactly what compute_stem_arm.py computes and what
     the hardware sequence's first GELU entry reads as in_off.

Also calibrates the INPUT IMAGE's own distribution (not a conv tap at all)
-- folds the 14.1%-saturation input_scale fix into the SAME batch-based,
percentile-based framework instead of leaving it as a separate one-image
max-based patch.

Scope explicitly bounded (ZHR-92 2026-08-21): this calibrates Stem's own
input and output scale only, verified against Stem's checkpoint. Full
52-layer calibration + per-channel out_shift support in hardware
(mac_array.cpp's WRITEOUT currently takes one scalar shift per layer) are
NOT done here -- confirmed next step, not silently expanded into this
round. W8A4 / CLIP-distillation retraining / mixed-precision allocation
strategy remain out of scope entirely (Phase C proper).

用法:
  python calibrate_stem_256.py [--n 16] [--percentile 99.9]
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
    p.add_argument("--quant-config", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded\quant_config.json")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\stem_calib_256.json")
    p.add_argument("--n", type=int, default=16,
                    help="batch size for calibration -- 16 chosen as a middle point of the user's "
                         "suggested 8-32 range: large enough that a single unusual image doesn't "
                         "set the scale (the original single-image diagnostic used n=1, which is "
                         "exactly the failure mode 'too few images underestimates the range' warns "
                         "against), small enough to run in seconds, not minutes")
    p.add_argument("--percentile", type=float, default=99.9,
                    help="99.9th percentile of abs value, not max -- matches calibrate_activations.py's "
                         "existing convention. max is a single-sample statistic (one outlier pixel in "
                         "one image sets the scale for everything); the 99.9th percentile over a pooled "
                         "batch is far more stable and is what W8A4 calibration in this repo already uses")
    p.add_argument("--seed", type=int, default=20260813)  # matches run_accuracy_harness_256.py
    return p.parse_args()


def main():
    args = parse_args()

    model = onnx.load(args.model)
    graph = model.graph
    with open(args.quant_config) as f:
        qcfg = json.load(f)

    stem_node_idx = qcfg["layer_0000_conv"]["node_idx"]
    stem_node = graph.node[stem_node_idx]
    stem_out_tensor = stem_node.output[0]

    existing_outputs = {o.name for o in graph.output}
    if stem_out_tensor not in existing_outputs:
        graph.output.append(onnx.helper.make_tensor_value_info(stem_out_tensor, onnx.TensorProto.FLOAT, None))
    tmp_path = args.out.replace(".json", "_tapped.onnx")
    onnx.save(model, tmp_path)

    sess = ort.InferenceSession(tmp_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    print(f">>> calibrating over {args.n} real images (seed={args.seed}), "
          f"p{args.percentile} of pooled |abs value|")

    imgs = make_synthetic_calibration_batch(args.n, h=256, w=256, seed=args.seed)

    input_abs_all = []
    stem_out_abs_all = []
    for i in range(args.n):
        img = imgs[i:i + 1]
        input_abs_all.append(np.abs(img).ravel())
        (stem_out,) = sess.run([stem_out_tensor], {input_name: img})
        stem_out_abs_all.append(np.abs(stem_out).ravel())

    input_abs = np.concatenate(input_abs_all)
    stem_out_abs = np.concatenate(stem_out_abs_all)

    input_clip = float(np.percentile(input_abs, args.percentile))
    stem_out_clip = float(np.percentile(stem_out_abs, args.percentile))

    # symmetric int8: -127..127
    input_scale = input_clip / 127.0
    output_scale = stem_out_clip / 127.0

    input_sat_frac_old = float(np.mean(input_abs > (1.0)))  # old scale=1/127 representable max=1.0
    print(f">>> input image:  max={input_abs.max():.4f}  p{args.percentile}={input_clip:.4f}  "
          f"-> input_scale={input_scale:.6f}  (old default_act_scale=1/127={1/127:.6f})")
    print(f"    old scale (1/127) would saturate {input_sat_frac_old*100:.2f}% of pixels "
          f"across this {args.n}-image batch")
    print(f">>> Stem raw output:  max={stem_out_abs.max():.4f}  p{args.percentile}={stem_out_clip:.4f}  "
          f"-> output_scale={output_scale:.6f}")

    result = {
        "n_images": args.n,
        "percentile": args.percentile,
        "seed": args.seed,
        "input_scale": input_scale,
        "input_max_abs": float(input_abs.max()),
        "input_p_abs": input_clip,
        "stem_output_scale": output_scale,
        "stem_output_max_abs": float(stem_out_abs.max()),
        "stem_output_p_abs": stem_out_clip,
    }
    with open(args.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f">>> wrote {args.out}")

    os.remove(tmp_path)


if __name__ == "__main__":
    main()
