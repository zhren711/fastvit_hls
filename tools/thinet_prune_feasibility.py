"""
thinet_prune_feasibility.py - ThiNet-style channel-pruning feasibility check
for FastVIT-T8's RepMixer MLP expand/compress pairs (Phase 0 exploratory
research, no HLS/RTL code touched).

背景: 真实上板profiling显示PWConv占总推理时间92.6%（详见项目memory），
根因是10个RepMixer block每个都做恒定3x MLP扩张比(PW1 expand -> GELU ->
PW2 compress)，且FastViT官方发布的T8/T12变体本身已经是这个系列里最低的
mlp_ratio=3(没有更低比例的已发布权重可以直接借用，见调研记录)。这个脚本
回答的问题是："如果砍掉PW1输出/PW2输入的一部分通道(降低有效expansion
ratio)，在没有真正重新训练的情况下，输出会偏离多少？" —— 用来判断ThiNet
式后训练剪枝这条路是否值得投入(先在Python/ONNX层面出个粗略信号，不碰
已部署的HLS/RTL代码)。

方法(简化版ThiNet，不是逐通道贪心搜索的完整版):
1. 识别10对PW1(expand, Cout>Cin)/PW2(compress, Cout<Cin, 紧跟在PW1+GELU
   之后)的Conv节点对，用跟export_weights.py相同的判定逻辑(kernel==[1,1]
   且group!=Cin -> pwconv)。
2. 用合成校准数据(复用calibrate_activations.py的make_synthetic_calibration_
   batch，跟本项目已有校准脚本保持一致的局限性——没有真实图片)跑一次baseline
   模型，抓取每个PW1+GELU之后的中间激活张量。
3. 每个中间通道的重要性 = 该通道激活的L2范数 * 该通道在PW2里对应输入权重列
   的L2范数(重建误差贡献的一阶近似，比ThiNet论文原版"逐通道贪心搜索到局部
   最优"更粗但快得多——数量级差异：这里是O(C)量级的一次性排序，不是O(C^2)
   量级的贪心迭代)。
4. 对每个候选保留比例，选出重要性最低的那部分通道，把PW1对应输出通道的
   weight+bias直接置零(GELU(0)=0，对下游PW2等价于该通道被物理移除，但
   不改变张量形状/不做PW2权重的最小二乘重解——这是一个"只砍不补"的悲观
   下界，真正实现时ThiNet论文自己的结果显示配合最小二乘重解通常能明显
   挽回精度，这里先不做，得到的是保守估计)。
5. 分别测: (a)单独砍一个block(看各stage的敏感度差异)，(b)同时砍全部10个
   block在同一比例下(真实部署要生效必须全砍，这个数字才是"这条路到底
   值不值得走"的关键信号)。用跟verify_w8a4_accuracy.py相同的指标(cosine
   similarity + 相对L2误差)、相同的"和校准种子不同的测试种子"约定。

已知局限(如实报告，不隐藏):
- 校准/测试数据都是合成的，不是真实图片，跟本项目其他校准脚本的局限性一致。
- 通道重要性用的是激活范数*权重范数的一阶近似，不是ThiNet论文完整的逐通道
  贪心重建误差搜索，排序质量会打折扣。
- 只做"置零"，没有做PW2输入权重的最小二乘重解——真正实现时这一步通常能
  挽回不少精度，所以这里测出的数字是这条技术路线的"下界"，不是"上限"。
- 这个脚本跑在FP32原始模型上，没有叠加现有的W8A8/W8A4量化误差——真正部署
  时剪枝+量化两种误差会叠加，需要在有了初步"剪枝本身值不值得做"的信号之后，
  再去测叠加量化的组合效果。

用法:
  python thinet_prune_feasibility.py [--model <path>] [--n_calib 24] [--n_test 8]
                                      [--ratios 1.0,0.83,0.67,0.5]
"""
import onnx
import onnx.helper
import onnx.numpy_helper
import onnxruntime as ort
import numpy as np
import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from calibrate_activations import make_synthetic_calibration_batch  # reuse


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    p.add_argument("--n_calib", type=int, default=24)
    p.add_argument("--n_test", type=int, default=8)
    p.add_argument("--calib_seed", type=int, default=42)   # matches calibrate_activations.py default
    p.add_argument("--test_seed", type=int, default=999)   # matches verify_w8a4_accuracy.py default
    p.add_argument("--ratios", default="1.0,0.83,0.67,0.5",
                   help="fraction of expand-layer channels to KEEP (1.0=no pruning baseline sanity check)")
    return p.parse_args()


def find_conv_nodes(graph):
    return [(i, n) for i, n in enumerate(graph.node) if n.op_type == "Conv"]


def classify_conv(node, init_map):
    w_name = node.input[1]
    w = init_map[w_name]
    attrs = {a.name: (list(a.ints) if a.ints else a.i) for a in node.attribute}
    kernel = attrs.get("kernel_shape", [1, 1])
    group = attrs.get("group", 1)
    cout = w.shape[0]
    cin = w.shape[1] * group
    if group == cin:
        return "dwconv", cin, cout
    elif kernel == [1, 1]:
        return "pwconv", cin, cout
    else:
        return "conv", cin, cout


def find_repmixer_pw_pairs(graph, init_map):
    """Return list of (pw1_node_idx, pw2_node_idx, pw1_node, pw2_node) for
    the 10 RepMixer expand(Cout>Cin)/compress(Cout<Cin) pairs. Excludes the
    Cin==Cout transition PW convs and the ARM-side SE block's fc1/fc2
    (those are also pwconv-shaped but are not part of the MLP sandwich --
    the SE ones are identified by not immediately preceding a Gelu-fed
    compress conv of matching Cin, see the sequential scan below)."""
    conv_nodes = find_conv_nodes(graph)
    pairs = []
    i = 0
    while i < len(conv_nodes):
        node_idx, node = conv_nodes[i]
        op_type, cin, cout = classify_conv(node, init_map)
        if op_type == "pwconv" and cout > cin:
            # candidate PW1 (expand). The very next Conv node in program
            # order should be PW2 (compress) with matching Cin==cout.
            if i + 1 < len(conv_nodes):
                node_idx2, node2 = conv_nodes[i + 1]
                op_type2, cin2, cout2 = classify_conv(node2, init_map)
                if op_type2 == "pwconv" and cin2 == cout and cout2 < cin2:
                    pairs.append((node_idx, node_idx2, node, node2, cin, cout, cout2))
                    i += 2
                    continue
        i += 1
    return pairs


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
    ratios = [float(x) for x in args.ratios.split(",")]

    model = onnx.load(args.model)
    graph = model.graph
    init_map = {init.name: onnx.numpy_helper.to_array(init).copy() for init in graph.initializer}

    pairs = find_repmixer_pw_pairs(graph, init_map)
    print(f">>> found {len(pairs)} RepMixer PW1(expand)/PW2(compress) pairs:")
    for (idx1, idx2, n1, n2, cin, cexp, cout) in pairs:
        print(f"    node {idx1:3d}->{idx2:3d}  {cin:4d} -> {cexp:4d} -> {cout:4d}  "
              f"(mlp_ratio={cexp/cin:.2f})")
    assert len(pairs) == 10, f"expected 10 RepMixer pairs, found {len(pairs)} -- architecture assumption violated, stop and investigate"

    # tap the GELU output feeding each PW2 (i.e. PW2's own input tensor)
    pw2_input_names = [n2.input[0] for (_, _, _, n2, _, _, _) in pairs]

    tapped = onnx.ModelProto()
    tapped.CopyFrom(model)
    existing_out = {o.name for o in tapped.graph.output}
    for name in pw2_input_names:
        if name not in existing_out:
            tapped.graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    tapped_path = os.path.join(os.path.dirname(__file__), "_thinet_tapped_model_tmp.onnx")
    onnx.save(tapped, tapped_path)

    sess_tap = ort.InferenceSession(tapped_path, providers=["CPUExecutionProvider"])
    input_name = sess_tap.get_inputs()[0].name

    calib_imgs = make_synthetic_calibration_batch(args.n_calib, seed=args.calib_seed)
    print(f"\n>>> running {args.n_calib} synthetic calibration images to collect channel importance...")
    # accumulate sum of squared activation per channel, per pair
    act_sq_sums = [np.zeros(cexp, dtype=np.float64) for (_, _, _, _, _, cexp, _) in pairs]
    for i in range(args.n_calib):
        outs = sess_tap.run(pw2_input_names, {input_name: calib_imgs[i:i + 1]})
        for p_idx, act in enumerate(outs):
            # act shape: [1, C, H, W]
            act_sq_sums[p_idx] += (act.astype(np.float64) ** 2).sum(axis=(0, 2, 3))

    # channel importance = sqrt(mean squared activation) * L2 norm of PW2's
    # corresponding input weight column (cheap 1st-order proxy for
    # reconstruction-error contribution, see module docstring)
    importances = []
    for p_idx, (idx1, idx2, n1, n2, cin, cexp, cout) in enumerate(pairs):
        act_rms = np.sqrt(act_sq_sums[p_idx] / (args.n_calib))  # [cexp]
        w2 = init_map[n2.input[1]]  # [cout, cexp, 1, 1]
        w2_norm = np.linalg.norm(w2.reshape(cout, cexp), axis=0)  # [cexp]
        importance = act_rms * w2_norm
        importances.append(importance)

    # baseline (unpruned) reference run, for comparison
    final_node = find_conv_nodes(graph)[49][1]  # layer_idx 49 = FinalDW, FPGA-scope boundary (matches verify_w8a4_accuracy.py)
    final_output_name = final_node.output[0]

    base_model = onnx.ModelProto()
    base_model.CopyFrom(model)
    if final_output_name not in {o.name for o in base_model.graph.output}:
        base_model.graph.output.append(onnx.helper.make_tensor_value_info(final_output_name, onnx.TensorProto.FLOAT, None))
    base_path = os.path.join(os.path.dirname(__file__), "_thinet_base_model_tmp.onnx")
    onnx.save(base_model, base_path)
    sess_base = ort.InferenceSession(base_path, providers=["CPUExecutionProvider"])

    test_imgs = make_synthetic_calibration_batch(args.n_test, seed=args.test_seed)
    base_outs = [sess_base.run([final_output_name], {input_name: test_imgs[i:i + 1]})[0] for i in range(args.n_test)]

    def build_pruned_model(keep_frac, pair_indices):
        """Zero out the lowest-importance (1-keep_frac) channels' PW1
        weight+bias for the given pair indices (list of ints into `pairs`,
        or 'all' handled by caller). Returns a saved model path."""
        m = onnx.ModelProto()
        m.CopyFrom(model)
        w_map = {init.name: init for init in m.graph.initializer}
        for p_idx in pair_indices:
            idx1, idx2, n1, n2, cin, cexp, cout = pairs[p_idx]
            n_prune = int(round(cexp * (1.0 - keep_frac)))
            if n_prune <= 0:
                continue
            prune_ch = np.argsort(importances[p_idx])[:n_prune]  # lowest importance first
            w1_name = n1.input[1]
            b1_name = n1.input[2] if len(n1.input) > 2 else None
            w1 = onnx.numpy_helper.to_array(w_map[w1_name]).copy()
            w1[prune_ch, :, :, :] = 0.0
            new_w1 = onnx.numpy_helper.from_array(w1, name=w1_name)
            w_map[w1_name].CopyFrom(new_w1)
            if b1_name and b1_name in w_map:
                b1 = onnx.numpy_helper.to_array(w_map[b1_name]).copy()
                b1[prune_ch] = 0.0
                new_b1 = onnx.numpy_helper.from_array(b1, name=b1_name)
                w_map[b1_name].CopyFrom(new_b1)
        if final_output_name not in {o.name for o in m.graph.output}:
            m.graph.output.append(onnx.helper.make_tensor_value_info(final_output_name, onnx.TensorProto.FLOAT, None))
        path = os.path.join(os.path.dirname(__file__), "_thinet_pruned_model_tmp.onnx")
        onnx.save(m, path)
        return path

    print("\n" + "=" * 78)
    print(" PER-BLOCK SENSITIVITY (prune ONE pair at a time, others untouched)")
    print("=" * 78)
    for ratio in ratios:
        if ratio >= 0.999:
            continue
        print(f"\n--- keep_frac={ratio:.2f} (effective mlp_ratio {3.0*ratio:.2f}) ---")
        for p_idx, (idx1, idx2, n1, n2, cin, cexp, cout) in enumerate(pairs):
            path = build_pruned_model(ratio, [p_idx])
            sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
            cos_sims, rel_errs = [], []
            for i in range(args.n_test):
                out = sess.run([final_output_name], {input_name: test_imgs[i:i + 1]})[0]
                cos_sims.append(cosine_sim(base_outs[i], out))
                rel_errs.append(rel_l2_error(base_outs[i], out))
            print(f"  pair[{p_idx}] node{idx1:3d}->{idx2:3d} ({cin}->{cexp}->{cout}): "
                  f"cos_sim={np.mean(cos_sims):.4f}  rel_l2={np.mean(rel_errs):.4f}")

    print("\n" + "=" * 78)
    print(" COMBINED (prune ALL 10 pairs simultaneously -- the real deployment case)")
    print("=" * 78)
    all_idx = list(range(len(pairs)))
    for ratio in ratios:
        if ratio >= 0.999:
            cos_sims = [cosine_sim(base_outs[i], base_outs[i]) for i in range(args.n_test)]
            rel_errs = [0.0 for _ in range(args.n_test)]
        else:
            path = build_pruned_model(ratio, all_idx)
            sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
            cos_sims, rel_errs = [], []
            for i in range(args.n_test):
                out = sess.run([final_output_name], {input_name: test_imgs[i:i + 1]})[0]
                cos_sims.append(cosine_sim(base_outs[i], out))
                rel_errs.append(rel_l2_error(base_outs[i], out))
        print(f"  keep_frac={ratio:.2f} (mlp_ratio {3.0*ratio:.2f}): "
              f"mean cos_sim={np.mean(cos_sims):.4f}  mean rel_l2={np.mean(rel_errs):.4f}  "
              f"(min cos_sim over test set={np.min(cos_sims):.4f})")

    print("\n>>> NOTE: synthetic calibration/test data (no real photos in this repo,")
    print(">>> same limitation as calibrate_activations.py). Zero-out only, no")
    print(">>> least-squares re-fit of PW2's remaining weights -- this is a")
    print(">>> pessimistic lower bound on achievable accuracy, not the ceiling.")
    print(">>> No quantization error included (pure FP32) -- real deployment would")
    print(">>> stack this with existing W8A8/W8A4 error on top.")

    for p in ("_thinet_tapped_model_tmp.onnx", "_thinet_base_model_tmp.onnx", "_thinet_pruned_model_tmp.onnx"):
        fp = os.path.join(os.path.dirname(__file__), p)
        if os.path.exists(fp):
            os.remove(fp)


if __name__ == "__main__":
    main()
