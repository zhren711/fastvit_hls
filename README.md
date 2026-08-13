# FastVIT HLS 加速器工程

**平台:** MicroZed xc7z020clg400-1  
**工具:** Vitis HLS / Vivado 2024.2  
**量化:** int8 激活 + int8 权重 → int32 累加 → int8 输出  
**参考模型:** `E:\codes\microzed\c0_l4.onnx` (FastVIT-SA36 变体, 173 个 Conv)  
**设计原则:** 每个算子类型独立成一个 IP 核，不合并模式

---

## 工程结构

```
fastvit_hls/
├── conv_ip/          ← 标准卷积 (Standard Conv, K=3/7, group=1)
│   ├── conv_ip.h
│   ├── conv_ip.cpp
│   ├── conv_ip_tb.cpp
│   └── run_hls.tcl
├── dwconv_ip/        ← Depthwise Conv (group=CH, K=3/7)
│   ├── dwconv_ip.h
│   ├── dwconv_ip.cpp
│   ├── tb_dwconv_ip.cpp
│   └── run_hls.tcl
├── pwconv_ip/        ← Pointwise Conv (1×1, group=1)
│   ├── pwconv_ip.h
│   ├── pwconv_ip.cpp
│   ├── tb_pwconv_ip.cpp
│   └── run_hls.tcl
├── pool_ip/          ← MaxPool / AvgPool / GlobalAvgPool
│   ├── pool_ip.h
│   ├── pool_ip.cpp
│   ├── pool_ip_tb.cpp
│   └── run_hls.tcl
├── add_ip/           ← 残差加法 (Element-wise Add, int8)
│   ├── add_ip.h
│   ├── add_ip.cpp
│   ├── tb_add_ip.cpp
│   └── run_hls.tcl
└── README.md
```

---

## IP 核一览

| IP | 顶层函数 | 功能 | FastVIT 对应层 |
|---|---|---|---|
| **conv_ip** | `conv_ip` | 标准卷积 K=3/7 | Stem 第一层 `[64,3,3,3]` 等 |
| **dwconv_ip** | `dwconv_ip` | Depthwise Conv K=3/7 | Token mixer `[C,1,3,3]`, MLP `[C,1,7,7]` |
| **pwconv_ip** | `pwconv_ip` | Pointwise 1×1 Conv | fc1/fc2 `[Cout,Cin,1,1]` |
| **pool_ip** | `pool_ip` / `global_avgpool_ip` | MaxPool / AvgPool / GAP | 下采样, SE 块 |
| **add_ip** | `add_ip` | Element-wise Add (残差) | 89 个残差连接 |

---

## 接口规范（统一）

- **数据接口:** AXI4 Master（4 个 gmem 端口：feat_in, weight, bias, feat_out）
- **控制接口:** AXI4-Lite Slave（参数寄存器）
- **量化:** `out = clip((acc + bias) >> out_shift, -128, 127)`
- **激活:** `ACT_NONE=0` / `ACT_RELU=1`（通过控制寄存器选择）

---

## Tiling 参数

| IP | 并行参数 | 说明 |
|---|---|---|
| conv_ip | TN=2, TM=2, TR=4, TC=4 | 标准卷积，资源消耗最大 |
| dwconv_ip | DW_TN=4, DW_TR=4, DW_TC=4 | DW 无 CHout 维，只展开 CH |
| pwconv_ip | PW_TN=4, PW_TM=4, PW_TS=16 | 空间维展平，纯 channel GEMM |
| pool_ip | — | 轻量级，无需 tiling |
| add_ip | ADD_TN=8, ADD_TR=16, ADD_TC=16 | 简单逐元素运算 |

---

## 综合进度

| IP | C 仿真 | HLS 综合 | IP 打包 | 资源 (LUT/FF/DSP/BRAM) |
|---|---|---|---|---|
| conv_ip | ✅ | ✅ | ✅ | 34695 / 36378 / 114 / 7 |
| dwconv_ip | ⏳ 待运行 | ⏳ | ⏳ | — |
| pwconv_ip | ⏳ 待运行 | ⏳ | ⏳ | — |
| pool_ip | ⏳ | 🔄 进行中 | ⏳ | — |
| add_ip | ⏳ 待运行 | ⏳ | ⏳ | — |

> xc7z020 资源上限: 220 DSP / 140 BRAM18K / 53200 LUT / 106400 FF  
> conv_ip 实际占用: LUT 65% / FF 34% / DSP 52% — **资源充裕，无需调小 Tiling**

---

## 快速综合

```powershell
$env:PATH = "E:\Xilinx\Vitis_HLS\2024.2\bin;" + $env:PATH

# 依次综合各 IP（pool_ip 已在后台运行）
cd E:\codes\microzed\fastvit_hls\dwconv_ip; vitis_hls -f run_hls.tcl
cd E:\codes\microzed\fastvit_hls\pwconv_ip; vitis_hls -f run_hls.tcl
cd E:\codes\microzed\fastvit_hls\add_ip;   vitis_hls -f run_hls.tcl
```

---

## 量化说明

```
output_int8 = clip( (acc_int32 + bias) >> out_shift, -128, 127 )
```

- `out_shift`: 每层的量化右移位数，从量化工具导出
- BN 层已 **fuse** 进 Conv 的 bias 和 scale
- GELU 激活（Erf-based）暂由 ARM CPU 处理，后续可加入 LUT-based GELU IP

---

## 下一步计划

- [x] conv_ip 综合 + IP 打包
- [x] 拆分 DW/PW/Standard 为独立 IP（dwconv_ip, pwconv_ip）
- [x] add_ip 源码实现
- [ ] pool_ip 综合完成（进行中）
- [ ] dwconv_ip / pwconv_ip / add_ip 综合
- [ ] 解析 ONNX 模型结构，建立层调度表
- [ ] 在 Vivado 中集成所有 IP，连接 AXI Interconnect
- [ ] 写 ARM 端驱动（Linux userspace, /dev/mem 或 UIO）

---

## 参考

- 参考项目: `E:\codes\microzed\CNN_Accelerator` (VGG16 int16, xc7z020)
- 模型结构: 173×Conv, 89×Add(残差), 2×ReduceMean(SE 块), GELU 激活
- DPU 基准: KV260 DPU4096 约 19ms (FastVIT 推理)
