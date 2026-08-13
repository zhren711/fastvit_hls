"""
export_weights_w8a4.py - FastVIT-T8 ONNX 权重提取 + W8A4 量化 + 导出二进制
(W8A4 = 权重 8-bit int8 不变，激活 4-bit int4，量化范围来自
 calibrate_activations.py 产出的 activation_calib.json，不再用固定 1/127 占位值)

模型: fastvit_t8_processed_128x128.onnx
范围: 只处理 layer_idx 0-49 (stem+Stage1-4+FinalDW)，SE block 的 fc1/fc2
(layer_idx 50-51) 保持原 INT8 (继续跑在 ARM 上，不受这次 FPGA 端 W8A4
redesign 影响)，本脚本对这两层原样按 export_weights.py 的旧逻辑处理
(固定 1/127 激活 scale)，方便 ARM 侧驱动代码不用为这两层单独分叉逻辑。

输出目录: weights_t8_w8a4/
  layer_NNNN_<type>_weight.bin   int8 权重 (与现有 weights_t8/ 格式相同)
  layer_NNNN_<type>_bias.bin     int32 偏置
  quant_config_w8a4.json         量化参数 (新增 act_bits 字段区分 4/8)

前置条件: 先跑 calibrate_activations.py 生成 activation_calib.json

用法:
  python export_weights_w8a4.py [--model <path>] [--calib weights_t8_w8a4/activation_calib.json]
                                 [--out weights_t8_w8a4/]
"""
import onnx
import numpy as np
import json
import os
import argparse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--calib", default=r"E:\codes\microzed\fastvit_hls\weights_t8_w8a4\activation_calib.json")
    p.add_argument("--out",   default=r"E:\codes\microzed\fastvit_hls\weights_t8_w8a4")
    p.add_argument("--last_w8a4_layer_idx", type=int, default=49,
                   help="layer_idx <= this uses calibrated INT4 activation scale; "
                        "layers after this (SE fc1/fc2) fall back to the old fixed "
                        "1/127 INT8 activation scale, unchanged from export_weights.py")
    return p.parse_args()


def quantize_weight_per_channel(w_fp32, nbits=8):
    """Per-output-channel 对称量化权重, 返回 (w_intN, scales) -- unchanged
    from export_weights.py, weights stay 8-bit regardless of activation bits."""
    cout = w_fp32.shape[0]
    w_flat = w_fp32.reshape(cout, -1)
    max_val = np.max(np.abs(w_flat), axis=1) + 1e-8
    scale = max_val / (2 ** (nbits - 1) - 1)
    w_int = np.round(w_flat / scale[:, None]).clip(-(2**(nbits-1)), 2**(nbits-1) - 1).astype(np.int8)
    return w_int.reshape(w_fp32.shape), scale


def compute_out_shift(input_scale, weight_scale_arr):
    """Unchanged from export_weights.py (see that file's docstring for the
    caveat that this is a simplified mean-shift approximation)."""
    shift = np.round(np.log2(1.0 / (weight_scale_arr + 1e-30))).clip(0, 31).astype(int)
    return int(np.mean(shift))


def save_bin(arr, path):
    arr.tofile(path)


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    with open(args.calib) as f:
        calib = json.load(f)
    print(f">>> loaded calibrated activation scales for {len(calib)} layers from {args.calib}")

    model = onnx.load(args.model)
    graph = model.graph

    init_map = {}
    for init in graph.initializer:
        init_map[init.name] = onnx.numpy_helper.to_array(init).copy()

    quant_config = {}
    layer_idx = 0
    default_act_scale_int8 = 1.0 / 127.0  # only used for layer_idx > last_w8a4_layer_idx (SE block)

    conv_count = 0
    n_int4_layers = 0
    for node_i, node in enumerate(graph.node):
        if node.op_type != "Conv":
            continue

        w_name = node.input[1] if len(node.input) > 1 else None
        b_name = node.input[2] if len(node.input) > 2 else None

        if w_name is None or w_name not in init_map:
            print(f"  [SKIP] node {node_i} Conv: no weight found")
            continue

        w_fp32 = init_map[w_name].astype(np.float32)
        b_fp32 = init_map[b_name].astype(np.float32) if (b_name and b_name in init_map) else None

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

        # weights: always 8-bit, unchanged regardless of W8A4 vs old-INT8 scope
        w_int8, w_scale = quantize_weight_per_channel(w_fp32, nbits=8)
        w_path = os.path.join(args.out, f"{tag}_weight.bin")
        save_bin(w_int8, w_path)

        is_w8a4 = layer_idx <= args.last_w8a4_layer_idx
        if is_w8a4:
            act_bits = 4
            key = str(layer_idx)
            if key not in calib:
                raise RuntimeError(
                    f"layer_idx {layer_idx} is in W8A4 scope (<= {args.last_w8a4_layer_idx}) "
                    f"but missing from {args.calib} -- rerun calibrate_activations.py with a "
                    f"matching --last_layer_idx")
            input_scale = calib[key]["int4_scale"]
            n_int4_layers += 1
        else:
            act_bits = 8
            input_scale = default_act_scale_int8

        if b_fp32 is not None:
            b_scale = input_scale * w_scale
            b_int32 = np.round(b_fp32 / b_scale).clip(-2**31, 2**31 - 1).astype(np.int32)
            b_path = os.path.join(args.out, f"{tag}_bias.bin")
            save_bin(b_int32, b_path)
        else:
            b_int32 = None
            b_path = None

        out_shift = compute_out_shift(input_scale, w_scale)

        quant_config[tag] = {
            "node_idx":      node_i,
            "op_type":       op_type,
            "kernel":        kernel,
            "group":         group,
            "Cout":          int(cout),
            "Cin":           int(cin),
            "weight_shape":  list(w_fp32.shape),
            "act_bits":      act_bits,
            "input_scale":   float(input_scale),
            "weight_scale":  w_scale.tolist(),
            "output_scale":  float(input_scale),
            "out_shift":     out_shift,
            "weight_file":   os.path.basename(w_path),
            "bias_file":     os.path.basename(b_path) if b_path else None,
        }

        print(f"[{layer_idx:4d}] {op_type:8s} A{act_bits}  W{list(w_fp32.shape)}  "
              f"act_scale={input_scale:.6f}  w_scale_mean={np.mean(w_scale):.4f}  shift={out_shift}")
        layer_idx += 1
        conv_count += 1

    cfg_path = os.path.join(args.out, "quant_config_w8a4.json")
    with open(cfg_path, "w") as f:
        json.dump(quant_config, f, indent=2)

    print()
    print(f"完成: {conv_count} 个 Conv 层已量化 ({n_int4_layers} 层 W8A4, "
          f"{conv_count - n_int4_layers} 层保留 W8A8 [SE block, ARM])")
    print(f"权重目录: {args.out}")
    print(f"量化配置: {cfg_path}")
    total_bytes = sum(
        os.path.getsize(os.path.join(args.out, f))
        for f in os.listdir(args.out) if f.endswith(".bin")
    )
    print(f"总权重大小: {total_bytes/1024/1024:.2f} MB")


if __name__ == "__main__":
    main()
