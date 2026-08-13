"""
verify_w8a4_accuracy.py - W8A4 fake-quantization accuracy check (Phase 1
step 4 of the W8A4 redesign plan)

方法: 对每个已校准的激活张量 (layer_idx 0-49 的 tap tensor, 与
calibrate_activations.py 里 tap 的是同一批张量名) 做图级手术 (graph
surgery)：在该张量产生的地方插入 fake-quant 节点
(round(x/scale)*scale, clip 到 [-7*scale, 7*scale])，并把这个张量的
**所有**下游消费者 (不只是下一个 conv，也包括残差 Add 等分支) 都重新接线
到量化后的版本 —— 这与真实硬件的行为一致：一个激活值只在 DRAM 里存一份
(4-bit 精度)，无论后面被哪个 op 读取，读到的都是同一份量化后的值，不存在
"给 conv 用低精度、给 skip connection 用全精度" 这种分裂。

然后分别跑 float32 原始模型 和 fake-quantized 模型 (相同的合成测试输入，
和校准时用的种子不同，避免"校准集上看起来准但换个输入就崩"的假象)，
比较 FPGA 范围的最终输出张量 (FinalDW 之后、SE block 之前，
node 49 /final_conv/reparam_conv/Conv_output_0) 的 cosine similarity 和
相对 L2 误差，作为"这个 W8A4 方案值不值得往下做 HLS"的第一个量化信号。

已知局限: 这仍然是 fake-quant (浮点模拟量化网格)，不是逐 bit 精确的整数
定点运算模拟 (真正的 bit-exact 验证要等 Phase 2 的 fastvit_ip_tb.cpp
参考函数更新后，在 HLS csim 阶段做)。这里只回答"这个量化方案在数值上
是否合理"这一更粗粒度的问题。

用法:
  python verify_w8a4_accuracy.py [--model <path>] [--calib weights_t8_w8a4/activation_calib.json]
                                  [--n_test 8] [--seed 999]
"""
import onnx
import onnx.helper
import onnxruntime as ort
import numpy as np
import json
import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch, find_conv_nodes  # reuse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--calib", default=r"E:\codes\microzed\fastvit_hls\weights_t8_w8a4\activation_calib.json")
    p.add_argument("--n_test", type=int, default=8)
    p.add_argument("--seed", type=int, default=999)  # different from calibration's default seed=42
    p.add_argument("--last_layer_idx", type=int, default=49)
    return p.parse_args()


def make_fake_quant_model(model, tap_scales):
    """tap_scales: dict tensor_name -> int4_scale. Inserts, for each tap
    tensor, a Div->Round->Clip->Mul chain producing `<name>_fq4`, then
    rewires every node input in the ENTIRE graph that references the
    original tensor name to the new fake-quantized name instead (all
    consumers, not just the immediate next conv)."""
    new_model = onnx.ModelProto()
    new_model.CopyFrom(model)
    graph = new_model.graph

    new_nodes = []
    rename_map = {}  # original_name -> fq_name
    for i, (name, scale) in enumerate(tap_scales.items()):
        fq_name = f"{name}__fq4"
        scale_name = f"{name}__scale4"
        clip_lo_name = f"{name}__clip_lo4"
        clip_hi_name = f"{name}__clip_hi4"
        divided = f"{name}__div4"
        rounded = f"{name}__round4"
        clipped = f"{name}__clipped4"

        scale_t = onnx.helper.make_tensor(scale_name, onnx.TensorProto.FLOAT, [], [scale])
        clip_lo_t = onnx.helper.make_tensor(clip_lo_name, onnx.TensorProto.FLOAT, [], [-7.0])
        clip_hi_t = onnx.helper.make_tensor(clip_hi_name, onnx.TensorProto.FLOAT, [], [7.0])
        graph.initializer.extend([scale_t, clip_lo_t, clip_hi_t])

        new_nodes.append(onnx.helper.make_node("Div", [name, scale_name], [divided], name=f"fq_div_{i}"))
        new_nodes.append(onnx.helper.make_node("Round", [divided], [rounded], name=f"fq_round_{i}"))
        new_nodes.append(onnx.helper.make_node("Clip", [rounded, clip_lo_name, clip_hi_name], [clipped], name=f"fq_clip_{i}"))
        new_nodes.append(onnx.helper.make_node("Mul", [clipped, scale_name], [fq_name], name=f"fq_mul_{i}"))
        rename_map[name] = fq_name

    # rewire every existing node's inputs (in new_model's OWN copy of the
    # nodes, mutated in place) to read the fake-quantized tensor instead of
    # the original wherever that name is used -- this naturally covers ALL
    # consumers of a tap tensor (the next conv, a residual Add, whatever),
    # since we rewrite by tensor name rather than touching one specific edge
    for node in graph.node:
        for k, inp in enumerate(node.input):
            if inp in rename_map:
                node.input[k] = rename_map[inp]

    # splice the new fake-quant nodes in; ONNX Runtime topologically sorts
    # the graph itself so exact list ordering doesn't matter for
    # correctness, only readability
    graph.node.extend(new_nodes)

    return new_model


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = (np.linalg.norm(a) * np.linalg.norm(b))
    if denom < 1e-12:
        return float("nan")
    return float(np.dot(a, b) / denom)


def rel_l2_error(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a)
    if denom < 1e-12:
        return float("nan")
    return float(np.linalg.norm(a - b) / denom)


def main():
    args = parse_args()

    with open(args.calib) as f:
        calib = json.load(f)

    model = onnx.load(args.model)
    graph = model.graph
    conv_nodes = find_conv_nodes(graph)

    in_scope = [(layer_idx, node_idx, node) for layer_idx, (node_idx, node) in enumerate(conv_nodes)
                if layer_idx <= args.last_layer_idx]
    tap_scales = {}
    for layer_idx, _, node in in_scope:
        tap_scales[node.input[0]] = calib[str(layer_idx)]["int4_scale"]

    final_output_name = conv_nodes[args.last_layer_idx][1].output[0]
    print(f">>> comparing final FPGA-scope tensor: {final_output_name} "
          f"(node_idx {conv_nodes[args.last_layer_idx][0]}, layer_idx {args.last_layer_idx})")

    fq_model = make_fake_quant_model(model, tap_scales)

    # both models need final_output_name as an explicit output to compare
    for m in (model, fq_model):
        existing = {o.name for o in m.graph.output}
        if final_output_name not in existing:
            m.graph.output.append(onnx.helper.make_tensor_value_info(
                final_output_name, onnx.TensorProto.FLOAT, None))

    fp32_path = os.path.join(os.path.dirname(args.calib), "verify_fp32_model.onnx")
    fq_path = os.path.join(os.path.dirname(args.calib), "verify_w8a4_fq_model.onnx")
    onnx.save(model, fp32_path)
    onnx.save(fq_model, fq_path)

    sess_fp32 = ort.InferenceSession(fp32_path, providers=["CPUExecutionProvider"])
    sess_fq = ort.InferenceSession(fq_path, providers=["CPUExecutionProvider"])
    input_name = sess_fp32.get_inputs()[0].name

    test_imgs = make_synthetic_calibration_batch(args.n_test, seed=args.seed)

    cos_sims, rel_errs = [], []
    for i in range(args.n_test):
        img = test_imgs[i:i + 1]
        out_fp32 = sess_fp32.run([final_output_name], {input_name: img})[0]
        out_fq = sess_fq.run([final_output_name], {input_name: img})[0]
        cs = cosine_sim(out_fp32, out_fq)
        re = rel_l2_error(out_fp32, out_fq)
        cos_sims.append(cs)
        rel_errs.append(re)
        print(f"  test image {i}: cosine_sim={cs:.4f}  rel_l2_error={re:.4f}  "
              f"fp32_range=[{out_fp32.min():.3f},{out_fp32.max():.3f}]  "
              f"w8a4_range=[{out_fq.min():.3f},{out_fq.max():.3f}]")

    print()
    print(f">>> mean cosine_sim  = {np.mean(cos_sims):.4f}  (1.0 = identical direction)")
    print(f">>> mean rel_l2_error = {np.mean(rel_errs):.4f}  (0.0 = identical magnitude)")
    print(">>> NOTE: this uses SYNTHETIC test images (no real photos in this repo,")
    print(">>> same limitation as calibration) -- treat as a coarse sanity signal,")
    print(">>> not a real accuracy benchmark, until real images are available.")


if __name__ == "__main__":
    main()
