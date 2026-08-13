# FastVIT MicroZed 部署计划

**目标平台：** MicroZed xc7z020clg400-1  
**任务：** FastVIT-SA36 (c0_l4.onnx) 完整推理部署，int8量化  
**最后更新：** 2026-03-27

---

## 一、网络结构速览

```
输入 [1, 3, 256, 256]
  ↓ Stem (Conv 3→64, 3×3, s=2) + 3× DWConv+GELU+PWConv
[1, 64, 128, 128]
  ↓ Stage1: 4× RepMixer Block (C=64, 64×64)
[1, 64, 64, 64]
  ↓ Stage2: Transition + 8× RepMixer Block (C=128, 32×32)
[1, 128, 32, 32]
  ↓ Stage3: SE + 12× RepMixer Block (C=256, 16×16)
[1, 256, 16, 16]
  ↓ Stage4: SE + 4× RepMixer Block (C=512, 8×8)
输出 [1, 512, 8, 8]
```

**RepMixer Block 结构（主体）：**
```
DWConv(3×3) + DWConv(7×7)
→ PWConv(expand) → GELU → PWConv(compress)
→ channel-wise scale (Mul)
→ Add (残差)
```

**算子统计（ONNX 节点 653个）：**
| 类型 | 次数 | 备注 |
|------|------|------|
| DWConv (group=C) | 84 | 3×3 和 7×7 各半 |
| PWConv (1×1) | 88 | 含SE内部conv |
| Regular Conv (3×3) | 1 | Stem第一层 |
| Add | 89 | 残差连接 |
| GELU (Div+Erf+Add+Mul+Mul) | 49 | 每次5个节点 |
| Mul (channel scale) | 30+ | SE输出scale |
| ReduceMean | 2 | SE GlobalAvgPool |
| Relu | 2 | SE中间 |
| Sigmoid | 2 | SE门控 |

---

## 二、IP核规划

### 2.1 IP核总览

| IP核 | 状态 | 覆盖算子 | HLS估算LUT | 实现LUT |
|------|------|----------|------------|---------|
| `conv_ip` | ✅ 综合完成 | Stem 3×3 Conv (1次) + 兼容DW/PW | ~34,695(旧TN=4) | ~8k(实测) |
| `dwconv_ip` | ✅ 综合完成 | 84× DWConv (3×3/7×7, group=C) | 14,674 | TBD |
| `pwconv_ip` | ⚠️ 待综合 | 88× PWConv (1×1) | TBD | TBD |
| `add_ip` | ✅ 综合完成 | 89× 残差Add | 3,710 | ~1k |
| `pool_ip` | ✅ 综合完成 | 2× ReduceMean (SE) | 17,146 | TBD |

**注意：** Vivado实现后实际LUT远低于HLS估算（实测比约3~4×），全部IP预计合计 ≤30% LUT。

### 2.2 CPU处理算子（软件端）

下列算子次数极少，在ARM Cortex-A9上处理：
- GELU (49次)：查表法 (256项int8 LUT) 或直接 `x * sigmoid(1.702x)` 近似
- channel-wise Mul (SE scale)：可折叠到后续PWConv bias，或纯软件
- Relu (2次), Sigmoid (2次)：纯软件

---

## 三、整体部署架构

```
ARM (Linux) ←→ DDR ←→ AXI HP0 ←→ FPGA IP核群
                                   ├─ conv_ip  (Stem)
                                   ├─ dwconv_ip
                                   ├─ pwconv_ip
                                   ├─ add_ip
                                   └─ pool_ip

调度方式: 软件逐层调用，每层:
  1. 写参数到AXI-Lite寄存器
  2. 启动IP (ap_start)
  3. 等待中断/轮询 ap_done
  4. 下一层
```

**数据常驻DDR，IP核直接DMA读写，无需CPU搬运中间结果。**

---

## 四、执行计划（分阶段）

---

### 阶段0：准备工作 ✅ 已完成

- [x] Vivado BD 搭建：conv_ip + dwconv_ip + pwconv_ip + add_ip + pool_ip → AXI 互联 → PS7 HP0
- [x] conv_ip 综合 + IP打包
- [x] add_ip 综合 + IP打包
- [x] pool_ip 综合 + IP打包
- [x] dwconv_ip 综合（LUT=14,674, DSP=62）
- [x] ONNX解析完成，算子分类清楚

---

### 阶段1：完成剩余IP核综合 🔄 进行中

#### 1.1 pwconv_ip 综合 + IP打包
- 文件已存在：`pwconv_ip/pwconv_ip.cpp`, `pwconv_ip.h`
- 执行HLS综合，验证资源
- 打包为Vivado IP

#### 1.2 dwconv_ip IP打包
- 综合已完成，需执行 `export_design -format ip_catalog`
- 生成 `.zip` IP包供Vivado使用

#### 1.3 资源汇总验证
- 所有IP合计 LUT/DSP/BRAM 确认在板子上限以内

---

### 阶段2：Vivado Block Design 更新 & 实现

#### 2.1 更新 run_impl.tcl
- 加入 dwconv_ip + pwconv_ip IP路径
- 实例化5个IP：conv_ip, dwconv_ip, pwconv_ip, add_ip, pool_ip
- AXI-Lite master 数量更新（当前6，需扩展到 5×2=10 个slave口）
- AXI HP0 数据总线 slave 数量更新
  - conv_ip: 4路 (gmem0~3)
  - dwconv_ip: 3路 (feat_in, weight, feat_out)
  - pwconv_ip: 4路 (feat_in, weight, bias, feat_out)
  - add_ip: 3路 (a, b, out)
  - pool_ip: 2路 (in, out)
  - 合计：约16路 → hp0_ic NUM_SI=16

#### 2.2 综合 + 实现
- 运行 `vivado -mode batch -source run_impl.tcl -nolog -nojournal`
- 查看实现后 utilization report
- 目标：LUT < 80%, DSP < 90%

#### 2.3 生成 bitstream
- `write_bitstream fastvit_bd_wrapper.bit`

---

### 阶段3：权重量化 & 导出

#### 3.1 ONNX 权重提取脚本
- 文件：`tools/export_weights.py`
- 从 `c0_l4.onnx` 提取每层权重（float32）
- 量化为 int8：`w_int8 = round(w / scale).clip(-128, 127)`
- 计算 per-channel scale 和 bias
- 输出：二进制 `.bin` 文件（按层命名）

#### 3.2 量化配置文件
- 文件：`tools/quant_config.json`
- 每层：input_scale, weight_scale, output_scale, out_shift

#### 3.3 激活量化
- 用校准数据集（100张图）统计 activation range
- 或直接用对称量化：`scale = max(|feat|) / 127`

---

### 阶段4：ARM 软件驱动开发

#### 4.1 裸机驱动（Vitis / Baremetal）
- 文件：`arm_driver/fastvit_driver.c`
- 功能：
  - 映射 AXI-Lite 寄存器地址
  - `run_conv_ip()` / `run_dwconv_ip()` / `run_pwconv_ip()` / `run_add_ip()` / `run_pool_ip()`
  - 每函数：写参数 → ap_start → 轮询 ap_done

#### 4.2 推理调度器
- 文件：`arm_driver/fastvit_infer.c`
- 按 ONNX 节点顺序，依次调用对应 IP 核函数
- 管理 DDR 内存地址：feature map 双缓冲（ping-pong buffer）

#### 4.3 GELU 软件实现
- 文件：`arm_driver/gelu_lut.c`
- 256项 int8 LUT，输入 int8 → 输出 int8
- 公式：`gelu(x) = x * Φ(x)` ≈ `x * sigmoid(1.702 * x)`

#### 4.4 SE模块软件实现
- ReduceMean → pool_ip
- Conv(squeeze) → pwconv_ip（也可软件做，只2次）
- Relu → 软件
- Conv(excite) → pwconv_ip
- Sigmoid → 软件
- Mul(scale) → 软件（channel-wise乘法）

---

### 阶段5：集成测试

#### 5.1 单IP核测试（已有testbench，扩展到硬件）
- 用 Vitis 裸机工程，逐一测试每个IP
- 验证输入/输出 vs. Python float参考

#### 5.2 逐层精度测试
- 在 PC 上跑 `tools/run_reference.py`（numpy float32）
- 在 MicroZed 上逐层执行，对比中间结果
- 容忍误差：int8量化误差 ≤ 1 LSB

#### 5.3 端到端测试
- 输入：一张 256×256 RGB 图像
- 输出：[1, 512, 8, 8] 特征图
- 验证与 ONNX Runtime CPU 推理结果的 cosine similarity ≥ 0.99

#### 5.4 性能测试
- 记录每层耗时（用 Zynq 定时器）
- 统计整网 latency (ms)
- 目标：< 500ms/帧（100MHz）

---

### 阶段6：优化（可选）

- [ ] 双缓冲 DMA：下一层权重预取，与当前层计算并行
- [ ] 合并 DWConv+GELU+PWConv 调用（减少 AXI 启动开销）
- [ ] Tiling 参数调优（更大的 TN/TM 提升吞吐）
- [ ] Pipeline 多层并发（资源允许时）

---

## 五、文件结构规划

```
E:\codes\microzed\fastvit_hls\
├── conv_ip/          ✅ 综合完成
├── dwconv_ip/        ✅ 综合完成，待IP打包
├── pwconv_ip/        ⚠️ 待综合
├── add_ip/           ✅ 综合 + IP打包完成
├── pool_ip/          ✅ 综合完成
│
├── tools/
│   ├── export_weights.py    # 权重提取 + 量化
│   ├── quant_config.json    # 量化参数
│   └── run_reference.py     # Python float32参考推理
│
├── arm_driver/
│   ├── fastvit_driver.h     # IP核寄存器宏定义
│   ├── fastvit_driver.c     # 各IP驱动函数
│   ├── fastvit_infer.c      # 推理调度器（层序列）
│   ├── gelu_lut.c           # GELU查找表
│   └── main.c               # 测试主程序
│
├── vivado_impl/
│   └── run_impl.tcl         # Vivado BD脚本（待更新5IP版）
│
├── DEPLOY_PLAN.md           # 本文件
├── DEPLOY_PROGRESS.md       # 实时进度记录
└── parse_onnx.py / analyze_onnx.py
```

---

## 六、资源预算

| IP | HLS估算LUT | 实现LUT(估) |
|----|------------|------------|
| conv_ip (TN=2,TM=2) | ~15,000 | ~4,000 |
| dwconv_ip | 14,674 | ~4,000 |
| pwconv_ip | TBD (~15,000) | ~4,000 |
| add_ip | 3,710 | ~1,000 |
| pool_ip | 17,146 | ~4,500 |
| **合计** | **~66,000** | **~17,500 (33%)** |
| 板子上限 | 53,200 | 53,200 |

*注：Vivado实现时LUT packing效率高，实测约为HLS估算的1/3~1/4*

---

## 七、当前状态（2026-03-27）

| 任务 | 状态 |
|------|------|
| ONNX解析，算子分类 | ✅ 完成 |
| conv_ip 综合 + IP打包 | ✅ 完成 |
| add_ip 综合 + IP打包 | ✅ 完成 |
| pool_ip 综合 | ✅ 完成 |
| dwconv_ip 综合 | ✅ 完成 |
| dwconv_ip IP打包 | ⬜ 待执行 |
| pwconv_ip 综合 | ⬜ 待执行 |
| pwconv_ip IP打包 | ⬜ 待执行 |
| Vivado BD更新（5IP版） | ⬜ 待执行 |
| Vivado 实现 + bitstream | ⬜ 待执行 |
| 权重量化脚本 | ⬜ 待执行 |
| ARM驱动开发 | ⬜ 待执行 |
| 端到端测试 | ⬜ 待执行 |

---

## 八、立即执行步骤（Next Actions）

1. **pwconv_ip HLS综合** → 确认资源
2. **dwconv_ip IP打包** → export_design
3. **更新 run_impl.tcl** → 5IP版BD
4. **Vivado实现** → 生成bitstream
5. **权重量化脚本**
6. **ARM驱动骨架**
