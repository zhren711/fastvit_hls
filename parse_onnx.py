"""
parse_onnx.py - 解析 FastVIT ONNX 模型，输出完整算子序列
"""
import onnx
from onnx import numpy_helper
import numpy as np
from collections import defaultdict

model = onnx.load(r"E:\codes\microzed\c0_l4.onnx")
graph = model.graph

# 建立 value_info 字典（含 input/output/initializer shape）
shape_map = {}
for inp in graph.input:
    t = inp.type.tensor_type
    shape = [d.dim_value for d in t.shape.dim]
    shape_map[inp.name] = shape
for vi in graph.value_info:
    t = vi.type.tensor_type
    shape = [d.dim_value for d in t.shape.dim]
    shape_map[vi.name] = shape
for out in graph.output:
    t = out.type.tensor_type
    shape = [d.dim_value for d in t.shape.dim]
    shape_map[out.name] = shape

# 初始化器（权重）字典
init_map = {}
for init in graph.initializer:
    init_map[init.name] = list(init.dims)

# 统计算子类型
op_counts = defaultdict(int)
for node in graph.node:
    op_counts[node.op_type] += 1

print("=" * 60)
print(f"总节点数: {len(graph.node)}")
print("算子类型统计:")
for op, cnt in sorted(op_counts.items(), key=lambda x: -x[1]):
    print(f"  {op:20s}: {cnt}")
print("=" * 60)
print()

# 逐节点输出
print(f"{'#':>4} {'OpType':20s} {'输入Shape':35s} {'输出Shape':35s} {'属性'}")
print("-" * 120)
for i, node in enumerate(graph.node):
    op = node.op_type
    
    # 输入 shape（跳过权重 initializer，只显示 tensor）
    in_shapes = []
    for inp in node.input:
        if inp == "":
            continue
        if inp in init_map:
            in_shapes.append(f"W{init_map[inp]}")
        elif inp in shape_map:
            in_shapes.append(str(shape_map[inp]))
        else:
            in_shapes.append(f"?({inp[:12]})")
    
    out_shapes = []
    for out in node.output:
        if out in shape_map:
            out_shapes.append(str(shape_map[out]))
        else:
            out_shapes.append(f"?")
    
    # 关键属性
    attrs = {}
    for attr in node.attribute:
        if attr.name in ("kernel_shape", "strides", "pads", "group", "dilations", "axis"):
            if attr.ints:
                attrs[attr.name] = list(attr.ints)
            elif attr.i:
                attrs[attr.name] = attr.i
    
    in_str  = ", ".join(in_shapes)[:33]
    out_str = ", ".join(out_shapes)[:33]
    attr_str = str(attrs)[:40]
    print(f"{i:>4} {op:20s} {in_str:35s} {out_str:35s} {attr_str}")

print()
print("=" * 60)
print("模型输入:")
for inp in graph.input:
    t = inp.type.tensor_type
    shape = [d.dim_value for d in t.shape.dim]
    print(f"  {inp.name}: {shape}")
print("模型输出:")
for out in graph.output:
    t = out.type.tensor_type
    shape = [d.dim_value for d in t.shape.dim]
    print(f"  {out.name}: {shape}")
