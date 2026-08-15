"""
run_accuracy_harness_256.py -- Phase A checkpoint (2026-08-15), 256x256 variant.

Same method as run_accuracy_harness.py (structured synthetic images, not
random noise; same generator; held-out seed 20260813, distinct from
calibration(42) and W8A4-verify(999)), just at 256x256 -- the target
resolution confirmed this session (ZHR-63 revision 5). Quantizes with the
SAME default_act_scale=1/127 placeholder as before -- calibration hasn't
been redone yet (step 2b), this harness is for the "structural fix only"
checkpoint.

用法:
  python run_accuracy_harness_256.py [--n 8] [--out accuracy_test_imgs_256]
"""
import onnx
import onnxruntime as ort
import numpy as np
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "accuracy_test_imgs_256"))
    p.add_argument("--n", type=int, default=8)
    p.add_argument("--seed", type=int, default=20260813)
    p.add_argument("--input_scale", type=float, default=1.0 / 127.0)
    return p.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    model = onnx.load(args.model)
    graph = model.graph

    se_mul_tensor = "/final_conv/se/Mul_output_0"
    final_tensor = graph.output[0].name

    existing_outputs = {o.name for o in graph.output}
    if se_mul_tensor not in existing_outputs:
        graph.output.append(onnx.helper.make_tensor_value_info(se_mul_tensor, onnx.TensorProto.FLOAT, None))

    tmp_model_path = os.path.join(args.out, "_ref_model_tapped.onnx")
    onnx.save(model, tmp_model_path)
    sess = ort.InferenceSession(tmp_model_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    print(f">>> generating {args.n} synthetic 256x256 test images (seed={args.seed})")
    imgs = make_synthetic_calibration_batch(args.n, h=256, w=256, seed=args.seed)

    for i in range(args.n):
        img = imgs[i:i + 1]
        img_q = np.clip(np.round(img / args.input_scale), -127, 127).astype(np.int8)
        img_q.tofile(os.path.join(args.out, f"img_{i:04d}.bin"))

        out_se_mul, out_final = sess.run([se_mul_tensor, final_tensor], {input_name: img})
        np.save(os.path.join(args.out, f"ref_se_mul_{i:04d}.npy"), out_se_mul)
        np.save(os.path.join(args.out, f"ref_final_{i:04d}.npy"), out_final)
        print(f"  img {i}: quantized range=[{img_q.min()},{img_q.max()}]  "
              f"ref_se_mul shape={out_se_mul.shape} range=[{out_se_mul.min():.3f},{out_se_mul.max():.3f}]")

    os.remove(tmp_model_path)
    print(f"\n>>> wrote {args.n} test images + references to {args.out}")


if __name__ == "__main__":
    main()
