"""
run_accuracy_harness.py - Phase 0.5 end-to-end accuracy harness (PC side).

用法:
  1. 先跑本脚本 (PC 上): 生成 N 张测试图片，量化成板子要的 int8 [3,128,128]
     格式存成 .bin，同时跑 ONNX float32 参考模型算出对照输出，存成 .npy。
  2. 把生成的 accuracy_test_imgs/ 目录 scp 到板子，对每张 img_XXXX.bin 跑
     board 端的 accuracy_infer 程序 (见 petalinux/software/fastvit_app/src/
     accuracy_infer.c)，把每张图对应的 out_XXXX.bin 传回来。
  3. 跑 compare_accuracy_results.py 做最终对比。

已知局限 (和本项目 verify_w8a4_accuracy.py / calibrate_activations.py 一样):
仓库里没有真实照片/数据集，用的是结构化合成图像 (渐变+平滑噪声+条纹，模拟
真实图像的空间相关性，不是纯随机噪声)。如果之后拿到真实图片，应换成读图片
目录而不是合成生成 —— 但作为"当前部署 bitstream 的数值输出到底对不对"这个
问题的第一个真实端到端信号 (而不是逐层/fake-quant 的间接信号)，这已经比
project 之前有过的任何精度验证都更接近真实场景 (真实硬件，不是 csim/fake-quant)。

用法:
  python run_accuracy_harness.py [--n 8] [--out accuracy_test_imgs] [--seed 20260813]
"""
import onnx
import onnxruntime as ort
import numpy as np
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch, find_conv_nodes  # reuse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "accuracy_test_imgs"))
    p.add_argument("--n", type=int, default=8)
    p.add_argument("--seed", type=int, default=20260813,  # distinct from calibration(42) and verify_w8a4(999)
                   help="different seed from calibration/verify_w8a4 scripts -- genuinely held-out images")
    p.add_argument("--input_scale", type=float, default=1.0 / 127.0,
                   help="matches quant_config.json's layer_0000_conv input_scale exactly")
    return p.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    print(f">>> loading ONNX model: {args.model}")
    model = onnx.load(args.model)
    graph = model.graph
    conv_nodes = find_conv_nodes(graph)

    # Two reference points: (a) right after the SE gate's Mul (what the
    # DEPLOYED C driver's fastvit_t8_infer() actually computes -- se_block()
    # ends with the scale-multiply, no further activation), and (b) the
    # true ONNX graph final "output" (which the deployed driver does NOT
    # match -- the ONNX graph applies one more GELU after the SE multiply
    # that fastvit_infer.c never calls). Comparing against BOTH tells us
    # whether that's a real behavioral gap or a red herring.
    se_mul_tensor = "/final_conv/se/Mul_output_0"
    final_tensor = graph.output[0].name  # "output"
    print(f">>> comparing against TWO tensors:")
    print(f"    (a) {se_mul_tensor}  <- matches what fastvit_t8_infer() in fastvit_infer.c actually computes")
    print(f"    (b) {final_tensor}  <- true ONNX graph output (has one more GELU the C driver never applies)")

    existing_outputs = {o.name for o in graph.output}
    if se_mul_tensor not in existing_outputs:
        graph.output.append(onnx.helper.make_tensor_value_info(se_mul_tensor, onnx.TensorProto.FLOAT, None))

    tmp_model_path = os.path.join(args.out, "_ref_model_tapped.onnx")
    onnx.save(model, tmp_model_path)
    sess = ort.InferenceSession(tmp_model_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    print(f">>> generating {args.n} synthetic test images (seed={args.seed}, held-out from calibration)")
    imgs = make_synthetic_calibration_batch(args.n, seed=args.seed)  # [N,3,128,128] float32

    for i in range(args.n):
        img = imgs[i:i + 1]  # [1,3,128,128]

        # quantize for the board: int8 = clip(round(img / input_scale), -127, 127)
        img_q = np.clip(np.round(img / args.input_scale), -127, 127).astype(np.int8)
        img_q.tofile(os.path.join(args.out, f"img_{i:04d}.bin"))

        out_se_mul, out_final = sess.run([se_mul_tensor, final_tensor], {input_name: img})
        np.save(os.path.join(args.out, f"ref_se_mul_{i:04d}.npy"), out_se_mul)
        np.save(os.path.join(args.out, f"ref_final_{i:04d}.npy"), out_final)
        print(f"  img {i}: quantized range=[{img_q.min()},{img_q.max()}]  "
              f"ref_se_mul shape={out_se_mul.shape} range=[{out_se_mul.min():.3f},{out_se_mul.max():.3f}]  "
              f"ref_final range=[{out_final.min():.3f},{out_final.max():.3f}]")

    print()
    print(f">>> wrote {args.n} test images + references to {args.out}")
    print(f">>> next: scp {args.out} to the board, run accuracy_infer on each img_XXXX.bin,")
    print(f">>> scp the out_XXXX.bin results back, then run compare_accuracy_results.py")


if __name__ == "__main__":
    main()
