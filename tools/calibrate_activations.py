"""
calibrate_activations.py - W8A4 activation-range calibration for FastVIT-T8
模型: fastvit_t8_processed_128x128.onnx (52 Conv 层, 128x128 输入)

背景: export_weights.py 现有的 INT8 流程用固定 default_act_scale=1/127
(未校准的占位值)。W8A4 只有 16 个电平 (对称 -7..7)，对裁剪范围敏感得多，
必须做真正的校准，而不是继续用占位常数。

范围: 只校准 stem+Stage1-4+FinalDW 这 50 层 (layer_idx 0-49, 对应 ONNX
Conv 节点 0-49 的输入激活)，这是本次 W8A4 redesign 的 FPGA 范围；SE block
的 fc1/fc2 (layer_idx 50-51) 保持不变、继续跑在 ARM 上，不在本脚本处理。

方法: 由于仓库里没有任何真实校准图片/数据集 (已确认搜索过)，本脚本生成
一组结构化的合成校准输入 (渐变 + 平滑噪声色块 + 边缘图案，而不是纯随机噪声，
更接近真实图像的激活统计分布)，跑 onnxruntime 前向推理，抓取每个目标层的
输入张量，收集统计后计算每层的 INT4 对称裁剪范围/scale。

局限性 (已知，向用户报告): 合成校准数据不能替代真实图片的代表性；如果后续
拿到真实样例图片，应该重新跑一遍本脚本 (只需把 --calib_mode 换成读图片目录)。

用法:
  python calibrate_activations.py [--model <path>] [--out weights_t8_w8a4/activation_calib.json]
                                   [--n_calib 24] [--percentile 99.9]
"""
import onnx
import onnx.helper
import onnxruntime as ort
import numpy as np
import json
import os
import argparse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\weights_t8_w8a4\activation_calib.json")
    p.add_argument("--n_calib", type=int, default=24)
    p.add_argument("--percentile", type=float, default=99.9,
                   help="clip percentile (of abs value) to guard against single-pixel outliers blowing up the scale")
    p.add_argument("--seed", type=int, default=42)
    # last in-scope Conv node index (inclusive) -- stem+Stage1-4+FinalDW.
    # SE block's fc1/fc2 (node_idx 50,51) stay on ARM, unaffected by W8A4.
    p.add_argument("--last_layer_idx", type=int, default=49)
    return p.parse_args()


def make_synthetic_calibration_batch(n, h=128, w=128, seed=42):
    """结构化合成校准图像: 渐变 + 平滑色块噪声 + 边缘/条纹图案的混合，
    比纯高斯/均匀随机噪声更接近真实图像的空间相关性和激活统计分布。
    输出范围大致模拟 ImageNet 标准化后的分布 (均值0, 大部分值落在 [-2.5, 2.5])。"""
    rng = np.random.default_rng(seed)
    imgs = np.zeros((n, 3, h, w), dtype=np.float32)
    yy, xx = np.meshgrid(np.linspace(-1, 1, h), np.linspace(-1, 1, w), indexing="ij")

    for i in range(n):
        kind = i % 4
        if kind == 0:
            # 线性渐变 (随机方向/幅度)，模拟大尺度光照变化
            angle = rng.uniform(0, 2 * np.pi)
            grad = np.cos(angle) * xx + np.sin(angle) * yy
            base = grad[None, :, :] * rng.uniform(0.5, 2.0) + rng.normal(0, 0.3, size=(3, 1, 1))
        elif kind == 1:
            # 低频平滑噪声色块 (box-blur 近似) 模拟纹理/物体边界
            raw = rng.normal(0, 1, size=(3, h // 8, w // 8)).astype(np.float32)
            base = np.repeat(np.repeat(raw, 8, axis=1), 8, axis=2)[:, :h, :w]
            base += rng.normal(0, 0.2, size=(3, h, w))
        elif kind == 2:
            # 条纹/边缘图案，模拟结构化边缘响应
            freq = rng.uniform(2, 8)
            phase = rng.uniform(0, 2 * np.pi)
            stripes = np.sin(freq * xx + phase)[None, :, :]
            base = stripes * rng.uniform(0.8, 1.8) + rng.normal(0, 0.4, size=(3, h, w))
        else:
            # 纯高斯噪声打底 (仍保留一部分，覆盖"最坏情况"高频输入)
            base = rng.normal(0, 1.0, size=(3, h, w)).astype(np.float32)

        imgs[i] = base.astype(np.float32)

    return imgs


def find_conv_nodes(graph):
    return [(i, n) for i, n in enumerate(graph.node) if n.op_type == "Conv"]


def build_tapped_model(model, tap_tensor_names):
    """返回一个新的 ModelProto，在原有输出基础上，把 tap_tensor_names
    里每个中间张量名都加成额外的 graph output（形状未知，交给 onnxruntime
    自己在运行时推断，不需要提前做 shape inference）。"""
    new_model = onnx.ModelProto()
    new_model.CopyFrom(model)
    existing_outputs = {o.name for o in new_model.graph.output}
    for name in tap_tensor_names:
        if name in existing_outputs:
            continue
        vi = onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None)
        new_model.graph.output.append(vi)
        existing_outputs.add(name)
    return new_model


def main():
    args = parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    model = onnx.load(args.model)
    graph = model.graph
    conv_nodes = find_conv_nodes(graph)
    print(f">>> total Conv nodes in model: {len(conv_nodes)}")

    in_scope = [(layer_idx, node_idx, node) for layer_idx, (node_idx, node) in enumerate(conv_nodes)
                if layer_idx <= args.last_layer_idx]
    print(f">>> calibrating layer_idx 0..{args.last_layer_idx} "
          f"({len(in_scope)} layers: stem+Stage1-4+FinalDW, SE fc1/fc2 excluded)")

    # tap tensor = each in-scope conv's INPUT activation (what actually gets
    # quantized to INT4 before entering that conv's MAC array)
    tap_names = [node.input[0] for (_, _, node) in in_scope]
    layer_idx_by_tap = {node.input[0]: layer_idx for (layer_idx, _, node) in in_scope}

    tapped_model = build_tapped_model(model, tap_names)
    tapped_model_path = args.out.replace(".json", "_tapped_model.onnx")
    onnx.save(tapped_model, tapped_model_path)
    print(f">>> tapped model (with {len(tap_names)} extra outputs) saved to {tapped_model_path}")

    sess = ort.InferenceSession(tapped_model_path, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    output_names = [o.name for o in sess.get_outputs()]

    calib_imgs = make_synthetic_calibration_batch(args.n_calib, seed=args.seed)
    print(f">>> running {args.n_calib} synthetic calibration images through the model...")

    # per-tap running collection of abs values (concatenate then percentile at
    # the end -- fine at this scale: 50 layers x 24 images x <=1152 channels x
    # up to 64x64 spatial is a few hundred MB total, acceptable for a one-off
    # offline calibration script)
    per_tap_abs_values = {name: [] for name in tap_names}

    for i in range(args.n_calib):
        img = calib_imgs[i:i + 1]
        outputs = sess.run(output_names, {input_name: img})
        for name, val in zip(output_names, outputs):
            if name in per_tap_abs_values:
                per_tap_abs_values[name].append(np.abs(val).ravel())

    calib_result = {}
    for name in tap_names:
        layer_idx = layer_idx_by_tap[name]
        all_abs = np.concatenate(per_tap_abs_values[name])
        clip_val = float(np.percentile(all_abs, args.percentile))
        max_val = float(np.max(all_abs))
        if clip_val <= 1e-8:
            clip_val = max(max_val, 1e-3)  # degenerate all-zero tap guard
        int4_scale = clip_val / 7.0  # symmetric signed INT4: -7..7 (matches
        # existing INT8 convention of 2**(nbits-1)-1, not -8..7, for symmetry)
        calib_result[str(layer_idx)] = {
            "tap_tensor": name,
            "n_samples": int(all_abs.size),
            "max_abs": max_val,
            f"p{args.percentile}_abs": clip_val,
            "int4_scale": int4_scale,
        }
        print(f"[{layer_idx:4d}] {name:60s}  max={max_val:8.4f}  "
              f"p{args.percentile}={clip_val:8.4f}  int4_scale={int4_scale:.6f}")

    with open(args.out, "w") as f:
        json.dump(calib_result, f, indent=2)

    print()
    print(f">>> calibration done: {len(calib_result)} layers -> {args.out}")
    print(">>> KNOWN LIMITATION: calibration inputs are synthetic (no real sample")
    print(">>> images exist in this repo). If real photos become available, rerun")
    print(">>> this script with a real image-loading calib_mode before trusting")
    print(">>> these scales for anything beyond bring-up/bit-exactness testing.")


if __name__ == "__main__":
    main()
