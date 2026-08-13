"""
export_weights_pruned.py - FastVIT-T8 ONNX 权重提取 + int8 量化 + 导出二进制,
在导出前对3个RepMixer expand/compress pair做真正的通道剪枝(物理切片，不是
置零)。

背景: 见 tools/thinet_prune_feasibility.py 的可行性预研(cosine similarity
测量)和项目memory里的PWConv优化调研记录。这次剪枝目标是保守的"top3"子集
(三个敏感度最低的block)，每个砍17%通道(保留比例0.83，取整到8的倍数以
匹配PW_TM=PW_TN=8的tiling边界)：
  layer_0015/16 (Stage2 blk0, node 49->55):   96->288->96  剪成 96->240->96
  layer_0029/30 (Stage3 blk1, node 89->95):  192->576->192 剪成 192->480->192
  layer_0047/48 (Stage4 blk1, node140->146): 384->1152->384 剪成 384->960->384
Python层面测过的组合效果: cosine相似度0.978(最差测试样本0.963)，PW总算力
约省5.1%。这次导出用真正的通道选择+切片(不是zero-out)，理论上精度应该
不差于(可能优于)预研时的zero-out数字。

跟export_weights.py的区别: 量化方案(per-channel int8权重、default_act_scale
占位符、out_shift算法)完全不变，只是在量化之前，对这3个PW1层的输出通道
(和对应PW2层的输入通道)先做重要性排序+物理切片。通道重要性计算复用
thinet_prune_feasibility.py里已经验证过的方法(激活RMS x PW2权重列L2范数，
用同一批合成校准数据)。

输出目录: weights_t8_pruned/ (跟 weights_t8/ 平行，不覆盖原始baseline)。

用法:
  python export_weights_pruned.py [--model <path>] [--out weights_t8_pruned/]
"""
import onnx
import onnx.numpy_helper
import onnxruntime as ort
import numpy as np
import json
import os
import argparse
import sys

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch  # reuse
from thinet_prune_feasibility import find_conv_nodes, find_repmixer_pw_pairs  # reuse
from export_weights import quantize_weight_per_channel, compute_out_shift, save_bin  # reuse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\weights_t8_pruned")
    p.add_argument("--n_calib", type=int, default=24)
    p.add_argument("--calib_seed", type=int, default=42)
    # target pairs to prune, identified by which pair index (0-9, in the order
    # find_repmixer_pw_pairs returns them) and the target KEPT channel count
    # (must be a multiple of 8, matching PW_TM=PW_TN=8). pair 2/5/9 are the
    # "top3" least-sensitive blocks found by thinet_prune_feasibility.py.
    p.add_argument("--prune_spec", default="2:240,5:480,9:960",
                   help="comma-separated pair_idx:keep_channels")
    return p.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    prune_spec = {}
    for tok in args.prune_spec.split(","):
        pidx, keep = tok.split(":")
        prune_spec[int(pidx)] = int(keep)

    model = onnx.load(args.model)
    graph = model.graph
    init_map = {init.name: onnx.numpy_helper.to_array(init).copy() for init in graph.initializer}

    pairs = find_repmixer_pw_pairs(graph, init_map)
    assert len(pairs) == 10, f"expected 10 RepMixer pairs, found {len(pairs)}"

    print(">>> RepMixer PW1/PW2 pairs and prune targets:")
    for p_idx, (idx1, idx2, n1, n2, cin, cexp, cout) in enumerate(pairs):
        tag = f" -> PRUNE to {prune_spec[p_idx]}" if p_idx in prune_spec else ""
        print(f"    pair[{p_idx}] node{idx1:3d}->{idx2:3d}  {cin}->{cexp}->{cout}{tag}")

    # ---- compute channel importance for pruned pairs (same method as
    # thinet_prune_feasibility.py: activation RMS x PW2 input-weight L2 norm,
    # using the same synthetic calibration batch/seed for reproducibility) ----
    pw2_input_names = [n2.input[0] for (_, _, _, n2, _, _, _) in pairs]
    tapped = onnx.ModelProto()
    tapped.CopyFrom(model)
    existing_out = {o.name for o in tapped.graph.output}
    for name in pw2_input_names:
        if name not in existing_out:
            tapped.graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    tapped_path = os.path.join(args.out, "_tapped_model_tmp.onnx")
    onnx.save(tapped, tapped_path)
    sess_tap = ort.InferenceSession(tapped_path, providers=["CPUExecutionProvider"])
    input_name = sess_tap.get_inputs()[0].name

    calib_imgs = make_synthetic_calibration_batch(args.n_calib, seed=args.calib_seed)
    print(f"\n>>> running {args.n_calib} synthetic calibration images for channel importance...")
    act_sq_sums = {p_idx: np.zeros(pairs[p_idx][5], dtype=np.float64) for p_idx in prune_spec}
    for i in range(args.n_calib):
        outs = sess_tap.run([pw2_input_names[p_idx] for p_idx in prune_spec], {input_name: calib_imgs[i:i + 1]})
        for out_i, p_idx in enumerate(prune_spec):
            act_sq_sums[p_idx] += (outs[out_i].astype(np.float64) ** 2).sum(axis=(0, 2, 3))
    os.remove(tapped_path)

    keep_channels = {}  # p_idx -> sorted array of channel indices to KEEP
    for p_idx in prune_spec:
        idx1, idx2, n1, n2, cin, cexp, cout = pairs[p_idx]
        act_rms = np.sqrt(act_sq_sums[p_idx] / args.n_calib)
        w2 = init_map[n2.input[1]]
        w2_norm = np.linalg.norm(w2.reshape(cout, cexp), axis=0)
        importance = act_rms * w2_norm
        n_keep = prune_spec[p_idx]
        assert n_keep % 8 == 0, f"keep count {n_keep} for pair {p_idx} must be a multiple of 8 (PW_TM=PW_TN=8)"
        keep_idx = np.sort(np.argsort(importance)[-n_keep:])  # keep highest-importance, restore ascending order
        keep_channels[p_idx] = keep_idx
        print(f"    pair[{p_idx}]: kept {n_keep}/{cexp} channels (importance range kept: "
              f"[{importance[keep_idx].min():.4f}, {importance[keep_idx].max():.4f}], "
              f"pruned range: [{np.min(np.delete(importance, keep_idx)):.4f}, "
              f"{np.max(np.delete(importance, keep_idx)):.4f}])")

    # map node_idx (PW1 and PW2) -> (p_idx, is_pw1) for the actual export loop
    node_idx_to_prune = {}
    for p_idx, (idx1, idx2, n1, n2, cin, cexp, cout) in enumerate(pairs):
        if p_idx in prune_spec:
            node_idx_to_prune[idx1] = (p_idx, True)   # PW1: slice Cout
            node_idx_to_prune[idx2] = (p_idx, False)  # PW2: slice Cin

    # ---- main export loop, adapted from export_weights.py ----
    quant_config = {}
    layer_idx = 0
    default_act_scale = 1.0 / 127.0
    conv_count = 0

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
        group = attrs.get("group", 1)

        # ---- apply pruning slice, if this node is one of the 3 targets ----
        if node_i in node_idx_to_prune:
            p_idx, is_pw1 = node_idx_to_prune[node_i]
            keep_idx = keep_channels[p_idx]
            if is_pw1:
                w_fp32 = w_fp32[keep_idx, :, :, :]           # slice Cout
                if b_fp32 is not None:
                    b_fp32 = b_fp32[keep_idx]
            else:
                w_fp32 = w_fp32[:, keep_idx, :, :]           # slice Cin (bias untouched, indexed by Cout)

        cout = w_fp32.shape[0]
        cin = w_fp32.shape[1] * group

        if group == cin:
            op_type = "dwconv"
        elif kernel == [1, 1]:
            op_type = "pwconv"
        else:
            op_type = "conv"

        tag = f"layer_{layer_idx:04d}_{op_type}"

        w_int8, w_scale = quantize_weight_per_channel(w_fp32)
        w_path = os.path.join(args.out, f"{tag}_weight.bin")
        save_bin(w_int8, w_path)

        if b_fp32 is not None:
            b_scale = default_act_scale * w_scale
            b_int32 = np.round(b_fp32 / b_scale).clip(-2**31, 2**31 - 1).astype(np.int32)
            b_path = os.path.join(args.out, f"{tag}_bias.bin")
            save_bin(b_int32, b_path)
        else:
            b_int32 = None
            b_path = None

        out_shift = compute_out_shift(default_act_scale, w_scale)

        quant_config[tag] = {
            "node_idx":      node_i,
            "op_type":       op_type,
            "kernel":        kernel,
            "group":         group,
            "Cout":          int(cout),
            "Cin":           int(cin),
            "weight_shape":  list(w_fp32.shape),
            "input_scale":   float(default_act_scale),
            "weight_scale":  w_scale.tolist(),
            "output_scale":  float(default_act_scale),
            "out_shift":     out_shift,
            "weight_file":   os.path.basename(w_path),
            "bias_file":     os.path.basename(b_path) if b_path else None,
            "pruned":        node_i in node_idx_to_prune,
        }

        pruned_note = "  [PRUNED]" if node_i in node_idx_to_prune else ""
        print(f"[{layer_idx:4d}] {op_type:8s}  W{list(w_fp32.shape)}  "
              f"w_scale_mean={np.mean(w_scale):.4f}  shift={out_shift}{pruned_note}")
        layer_idx += 1
        conv_count += 1

    cfg_path = os.path.join(args.out, "quant_config.json")
    with open(cfg_path, "w") as f:
        json.dump(quant_config, f, indent=2)

    print()
    print(f"完成: {conv_count} 个 Conv 层已量化 (含3个剪枝pair)")
    print(f"权重目录: {args.out}")
    print(f"量化配置: {cfg_path}")
    total_bytes = sum(
        os.path.getsize(os.path.join(args.out, f))
        for f in os.listdir(args.out) if f.endswith(".bin")
    )
    print(f"总权重大小: {total_bytes/1024/1024:.2f} MB "
          f"(baseline weights_t8/ 是 3.19 MB，剪枝后应该略小)")


if __name__ == "__main__":
    main()
