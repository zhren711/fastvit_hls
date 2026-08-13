"""
export_weights.py - FastVIT-T8 ONNX 权重提取 + int8 量化 + 导出二进制
模型: fastvit_t8_processed_128x128.onnx (52 Conv 层, 128×128 输入)
输出目录: weights_t8/
  layer_NNNN_<type>_weight.bin   int8 权重
  layer_NNNN_<type>_bias.bin     int32 偏置
  quant_config.json              量化参数

用法:
  python export_weights.py [--model <path>] [--out weights_t8/]
"""
import onnx
import numpy as np
import json
import os
import struct
import argparse
from collections import defaultdict

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--out",   default=r"E:\codes\microzed\fastvit_hls\weights_t8")
    return p.parse_args()

def quantize_weight_per_channel(w_fp32, nbits=8):
    """Per-output-channel 对称量化权重, 返回 (w_int8, scales)"""
    # w_fp32: [Cout, ...], 对 Cout 维做 per-channel
    cout = w_fp32.shape[0]
    w_flat = w_fp32.reshape(cout, -1)
    max_val = np.max(np.abs(w_flat), axis=1) + 1e-8   # [Cout]
    scale = max_val / (2 ** (nbits - 1) - 1)          # [Cout]
    w_int8 = np.round(w_flat / scale[:, None]).clip(-128, 127).astype(np.int8)
    return w_int8.reshape(w_fp32.shape), scale

def quantize_weight_per_tensor(w_fp32, nbits=8):
    """Per-tensor 对称量化权重"""
    max_val = np.max(np.abs(w_fp32)) + 1e-8
    scale = max_val / (2 ** (nbits - 1) - 1)
    w_int8 = np.round(w_fp32 / scale).clip(-128, 127).astype(np.int8)
    return w_int8, np.array([scale])

def compute_out_shift(input_scale, weight_scale_arr):
    """
    计算量化后右移位数
    累加器 = input_int8 * weight_int8, 精度 = input_scale * weight_scale_arr
    输出需要缩放回 input_scale (假设 output_scale ≈ input_scale)
    right_shift = round(log2(weight_scale_arr / 1.0))
    实际使用时: output_int8 = (acc_int32 * weight_scale) >> shift
    """
    # 简化: 固定输出 scale = input_scale, 右移 = log2(1/weight_scale_normalized)
    # 这里先输出原始 scale 值，ARM 驱动再做精确处理
    shift = np.round(np.log2(1.0 / (weight_scale_arr + 1e-30))).clip(0, 31).astype(int)
    return int(np.mean(shift))

def save_bin(arr, path):
    arr.tofile(path)

def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    model = onnx.load(args.model)
    graph = model.graph

    # 初始化器（权重/偏置 float32）
    init_map = {}
    for init in graph.initializer:
        init_map[init.name] = onnx.numpy_helper.to_array(init).copy()

    # 统计节点
    quant_config = {}
    layer_idx = 0

    # 默认激活 scale（假设 uint8 范围，这里先用统一 scale=1/127，后续可通过校准更新）
    default_act_scale = 1.0 / 127.0

    conv_count = 0
    for node_i, node in enumerate(graph.node):
        if node.op_type != "Conv":
            continue

        # 找权重和偏置
        w_name = node.input[1] if len(node.input) > 1 else None
        b_name = node.input[2] if len(node.input) > 2 else None

        if w_name is None or w_name not in init_map:
            print(f"  [SKIP] node {node_i} Conv: no weight found")
            continue

        w_fp32 = init_map[w_name].astype(np.float32)
        b_fp32 = init_map[b_name].astype(np.float32) if (b_name and b_name in init_map) else None

        # 判断类型
        attrs = {a.name: list(a.ints) if a.ints else a.i for a in node.attribute}
        kernel = attrs.get("kernel_shape", [1, 1])
        group  = attrs.get("group", 1)
        cout   = w_fp32.shape[0]
        cin    = w_fp32.shape[1] * group

        if group == cin:
            op_type = "dwconv"
        elif kernel == [1, 1]:
            op_type = "pwconv"
        else:
            op_type = "conv"

        tag = f"layer_{layer_idx:04d}_{op_type}"

        # 权重量化 (per-channel)
        w_int8, w_scale = quantize_weight_per_channel(w_fp32)
        w_path = os.path.join(args.out, f"{tag}_weight.bin")
        save_bin(w_int8, w_path)

        # 偏置量化: bias_int32 = bias_fp32 / (act_scale * weight_scale_per_channel)
        if b_fp32 is not None:
            # 对于 per-channel weight scale，偏置也要 per-channel 缩放
            b_scale = default_act_scale * w_scale  # [Cout]
            b_int32 = np.round(b_fp32 / b_scale).clip(-2**31, 2**31 - 1).astype(np.int32)
            b_path = os.path.join(args.out, f"{tag}_bias.bin")
            save_bin(b_int32, b_path)
        else:
            b_int32 = None
            b_path = None

        # out_shift 估算
        out_shift = compute_out_shift(default_act_scale, w_scale)

        # 记录 quant config
        quant_config[tag] = {
            "node_idx":      node_i,
            "op_type":       op_type,
            "kernel":        kernel,
            "group":         group,
            "Cout":          int(cout),
            "Cin":           int(cin),
            "weight_shape":  list(w_fp32.shape),
            "input_scale":   float(default_act_scale),
            "weight_scale":  w_scale.tolist(),   # per-channel [Cout]
            "output_scale":  float(default_act_scale),
            "out_shift":     out_shift,
            "weight_file":   os.path.basename(w_path),
            "bias_file":     os.path.basename(b_path) if b_path else None,
        }

        print(f"[{layer_idx:4d}] {op_type:8s}  W{list(w_fp32.shape)}  "
              f"w_scale_mean={np.mean(w_scale):.4f}  shift={out_shift}")
        layer_idx += 1
        conv_count += 1

    # 保存 quant_config.json
    cfg_path = os.path.join(args.out, "quant_config.json")
    with open(cfg_path, "w") as f:
        json.dump(quant_config, f, indent=2)

    print()
    print(f"完成: {conv_count} 个 Conv 层已量化")
    print(f"权重目录: {args.out}")
    print(f"量化配置: {cfg_path}")
    total_bytes = sum(
        os.path.getsize(os.path.join(args.out, f))
        for f in os.listdir(args.out) if f.endswith(".bin")
    )
    print(f"总权重大小: {total_bytes/1024/1024:.2f} MB")

if __name__ == "__main__":
    main()
