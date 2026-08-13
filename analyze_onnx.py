"""
analyze_onnx.py - 详细分析 FastVIT ONNX，输出推理流水线规划报告
"""
import onnx
from collections import defaultdict

model = onnx.load(r"E:\codes\microzed\c0_l4.onnx")
graph = model.graph

init_map = {init.name: list(init.dims) for init in graph.initializer}
shape_map = {}
for inp in graph.input:
    t = inp.type.tensor_type
    shape_map[inp.name] = [d.dim_value for d in t.shape.dim]
for vi in graph.value_info:
    t = vi.type.tensor_type
    shape_map[vi.name] = [d.dim_value for d in t.shape.dim]
for out in graph.output:
    t = out.type.tensor_type
    shape_map[out.name] = [d.dim_value for d in t.shape.dim]

# ── 1. Conv 分类 ─────────────────────────────────────────────────────────────
print("=" * 70)
print("1. CONV 算子分类（按 group / kernel）")
print("=" * 70)

conv_regular = []   # group=1, kernel>1x1 (stem conv)
conv_dw = []        # group=C (depthwise)
conv_pw = []        # group=1, kernel=1x1 (pointwise)
conv_se = []        # SE块内的conv (group=1, 1x1, 特殊in/out比例)

for node in graph.node:
    if node.op_type != "Conv":
        continue
    attrs = {a.name: list(a.ints) if a.ints else a.i for a in node.attribute}
    kernel = attrs.get("kernel_shape", [1,1])
    group  = attrs.get("group", 1)
    # 找权重维度 [Cout, Cin/group, kH, kW]
    w_shape = None
    for inp in node.input:
        if inp in init_map:
            w_shape = init_map[inp]
            break
    if w_shape is None:
        continue
    cout = w_shape[0]
    cin_per_group = w_shape[1]
    cin = cin_per_group * group

    entry = {"cout": cout, "cin": cin, "group": group,
             "kernel": kernel, "attrs": attrs}
    
    if group == 1 and kernel == [1,1]:
        conv_pw.append(entry)
    elif group == cin:  # depthwise
        conv_dw.append(entry)
    else:  # regular (group=1, kernel>1)
        conv_regular.append(entry)

print(f"Regular Conv (group=1, k>1x1): {len(conv_regular)}")
for c in conv_regular:
    print(f"  Cin={c['cin']:4d} Cout={c['cout']:4d} kernel={c['kernel']} group={c['group']}")

print(f"\nDepthwise Conv (group=C):       {len(conv_dw)}")
dw_summary = defaultdict(int)
for c in conv_dw:
    key = (c['cin'], tuple(c['kernel']))
    dw_summary[key] += 1
for (cin, k), cnt in sorted(dw_summary.items()):
    print(f"  C={cin:4d} kernel={str(list(k)):10s}  x{cnt}")

print(f"\nPointwise Conv (group=1, 1x1):  {len(conv_pw)}")
pw_summary = defaultdict(int)
for c in conv_pw:
    key = (c['cin'], c['cout'])
    pw_summary[key] += 1
for (cin, cout), cnt in sorted(pw_summary.items()):
    print(f"  Cin={cin:5d} Cout={cout:5d}  x{cnt}")

# ── 2. 其他算子 ──────────────────────────────────────────────────────────────
print("\n" + "=" * 70)
print("2. 非Conv算子（逐类）")
print("=" * 70)

# GELU = Div+Erf+Add+Mul+Mul (pattern)
gelu_count = 0
i = 0
nodes = list(graph.node)
while i < len(nodes):
    if (i+4 < len(nodes) and
        nodes[i].op_type == "Div" and
        nodes[i+1].op_type == "Erf" and
        nodes[i+2].op_type == "Add" and
        nodes[i+3].op_type == "Mul" and
        nodes[i+4].op_type == "Mul"):
        gelu_count += 1
        i += 5
    else:
        i += 1

print(f"GELU (Div+Erf+Add+Mul+Mul 模式):  {gelu_count}")
print(f"Add (残差连接):                    {sum(1 for n in nodes if n.op_type=='Add')}")  
print(f"Mul (scale/SE注意力):              {sum(1 for n in nodes if n.op_type=='Mul')}")
print(f"ReduceMean (SE GlobalAvgPool):     {sum(1 for n in nodes if n.op_type=='ReduceMean')}")
print(f"Relu (SE中间激活):                 {sum(1 for n in nodes if n.op_type=='Relu')}")
print(f"Sigmoid (SE门控):                  {sum(1 for n in nodes if n.op_type=='Sigmoid')}")

# ── 3. 网络阶段划分 ───────────────────────────────────────────────────────────
print("\n" + "=" * 70)
print("3. 网络阶段划分（基于权重通道数变化）")
print("=" * 70)

print("""
输入: [1, 3, 256, 256]

■ Stem (节点 0-27) — 4层Conv + GELU，输出 [1,64,H,W]
  Conv(3→64, 3x3, s=2)  → [1,64,128,128]
  DWConv(64, 3x3)        → GELU
  PWConv(64→64)          → GELU
  DWConv(64, 3x3)        → [1,64,128,128]

■ Stage1 (节点 28-82) — 4× RepMixer Block, C=64, [1,64,64,64]
  每块: DWConv(64,3x3) + DWConv(64,7x7)
        PWConv(64→192) + GELU + PWConv(192→64)
        SE-scale(Mul) + Add

■ Stage2 (节点 83-268) — 2× Transition + 8× RepMixer Block, C=128
  Transition: DWConv(128,7x7) + GELU + PWConv(128)
  每块: DWConv(128,3x3) + DWConv(128,7x7)
        PWConv(128→384) + GELU + PWConv(384→128)
        SE-scale(Mul) + Add
  输出: [1,128,32,32]

■ Stage3 (节点 269-572) — SE×1 + 12× RepMixer Block, C=256
  Transition: DWConv(256,7x7) + SE块 + GELU + PWConv(256)
  SE块: ReduceMean → Conv(256→64) → Relu → Conv(64→256) → Sigmoid → Mul
  每块: DWConv(256,3x3) + DWConv(256,7x7)
        PWConv(256→768) + GELU + PWConv(768→256)
        SE-scale(Mul) + Add
  输出: [1,256,16,16]

■ Stage4 (节点 573-652) — SE×1 + 4× RepMixer Block, C=512
  Transition: DWConv(512,7x7) + SE块 + GELU + PWConv(512)
  SE块: ReduceMean → Conv(512→128) → Relu → Conv(128→512) → Sigmoid → Mul
  每块: DWConv(512,3x3) + DWConv(512,7x7)
        PWConv(512→1536) + GELU + PWConv(1536→512)
        SE-scale(Mul) + Add
  输出: [1,512,8,8]

输出: [1, 512, 8, 8]
""")

# ── 4. IP核对应关系 ───────────────────────────────────────────────────────────
print("=" * 70)
print("4. IP核 → 算子 对应关系分析")
print("=" * 70)
print("""
✅ conv_ip   → Regular Conv (stem, 3x3, group=1)          ×4
⚠️  conv_ip   → DWConv 当前是否支持 group=C？需确认
               DWConv(64,3x3)×9, DWConv(64,7x7)×5
               DWConv(128,3x3)×9, DWConv(128,7x7)×9
               DWConv(256,3x3)×12, DWConv(256,7x7)×13
               DWConv(512,3x3)×4, DWConv(512,7x7)×5
               合计 DWConv: ~66次

⚠️  conv_ip   → PWConv (1x1, group=1)                     ~103次
               （SE内的pwconv + RepMixer内的expand/compress）

✅ add_ip    → Add (残差连接)                              ×89
✅ pool_ip   → ReduceMean (SE GlobalAvgPool)               ×2

❌ 缺IP:
   - GELU (49次) → 需要 gelu_ip 或 LUT近似（可折叠到conv后）
   - Mul (scale, SE attention) → 需要 eltwise_mul_ip 或 CPU处理
   - Relu, Sigmoid → 可LUT / CPU处理
""")

# ── 5. 关键数据流尺寸 ─────────────────────────────────────────────────────────
print("=" * 70)
print("5. 各阶段 feature map 大小（内存带宽估算）")
print("=" * 70)
print("""
Stage   | C    | H×W      | Feature Map (int8) | 权重量级
--------|------|----------|--------------------|---------
Stem    |  64  | 128×128  |  1.0 MB            | 小
Stage1  |  64  |  64×64   |  0.25 MB           | 小
Stage2  | 128  |  32×32   |  0.13 MB           | 中
Stage3  | 256  |  16×16   |  0.06 MB           | 大 (768×256 pwconv)
Stage4  | 512  |   8×8    |  0.03 MB           | 最大 (1536×512 pwconv)
""")

print("=" * 70)
print("6. 下一步行动建议")
print("=" * 70)
print("""
优先级1: 确认 conv_ip 是否支持 depthwise (group=C)
         → 如不支持需新增 dwconv_ip

优先级2: GELU 处理策略
         → 方案A: 查找表(LUT)近似, 256项int8 LUT
         → 方案B: CPU处理(影响延迟但实现简单)
         → 方案C: 多项式近似 gelu(x)≈x*sigmoid(1.702x)

优先级3: Mul(scale/SE) 处理策略  
         → SE注意力的Mul是channel-wise缩放，可合并到下一个PWConv偏置

优先级4: 整体调度规划
         → 确定 conv_ip 能否兼容 DW+PW，统一复用
         → 规划DDR权重布局，减少AXI传输开销
""")
