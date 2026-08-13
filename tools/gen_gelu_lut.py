"""
gen_gelu_lut.py - 生成 GELU int8 查找表
gelu(x) = x * Phi(x) ≈ x * sigmoid(1.702 * x)
输入 int8 [-128,127], scale = 1/127
输出 int8 [-128,127]
"""
import numpy as np

def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))

def gelu_approx(x):
    return x * sigmoid(1.702 * x)

scale = 1.0 / 127.0
lut = []
for i in range(-128, 128):
    x_fp = i * scale
    y_fp = gelu_approx(x_fp)
    y_int = int(np.round(y_fp / scale))
    y_int = max(-128, min(127, y_int))
    lut.append(y_int)

# 生成 C 代码
print('const int8_t gelu_lut[256] = {')
for i in range(0, 256, 8):
    vals = ', '.join(f'{v:4d}' for v in lut[i:i+8])
    comment = f'/* {i-128:4d} */'
    print(f'{comment} {vals},')
print('};')
print()

# 验证几个关键点
print("/* Verification:")
for test in [-127, -64, -32, 0, 32, 64, 127]:
    idx = test + 128
    x_fp = test * scale
    print(f"   gelu({test:4d}) = gelu({x_fp:6.3f}) -> {lut[idx]:4d} ({lut[idx]*scale:6.3f})")
print("*/")
