FastVIT-T8 FPGA 硬件文件
===========================

需要的文件:
-----------
1. fastvit_hw.xsa   — Vivado 硬件描述文件 (Petalinux 配置用)
2. fastvit_bd_wrapper.bit — FPGA bitstream

生成方法:
---------
# 步骤 1: 重新生成 working bitstream (dwconv_ip v7.0 TN=1)
#   先恢复 dwconv_ip 为原始代码，然后:
cd E:\codes\microzed\fastvit_hls\vivado_impl
vivado -mode batch -source update_impl.tcl -nolog -nojournal

# 步骤 2: 导出 XSA
vivado -mode batch -source export_xsa.tcl -nolog -nojournal
# → 输出: petalinux/hardware/fastvit_hw.xsa

# 步骤 3: 复制 bitstream
copy fastvit_util_check\fastvit_util_check.runs\impl_1\fastvit_bd_wrapper.bit .

硬件规格:
---------
- 目标器件: xc7z020clg400-1 (MicroZed)
- 时钟: 200MHz (FCLK_CLK0)
- 最后成功实现: Jun 24 16:28, WNS=+0.617ns
- LUT: 40,211 (75.6%), FF: 61,749 (57.7%), DSP: 112 (50.9%)

HLS IP 版本:
-----------
- conv_ip     v1.0
- dwconv_ip   v7.0 (TN=1, K=3/7, stride=1/2)
- pwconv_ip   v17.0 (TM=8, TN=4, TS=8, 32-bit AXI)
- add_ip      v1.0
- pool_ip     v1.0

AXI-Lite 地址映射:
------------------
add_ip   ctrl:  0x40000000  param: 0x40010000
conv_ip  ctrl:  0x40020000  param: 0x40030000
dwconv   ctrl:  0x40040000  param: 0x40050000
pool_ip  ctrl:  0x40060000  param: 0x40070000
pwconv   ctrl:  0x40080000  param: 0x40090000

Petalinux 配置注意事项:
-----------------------
1. device tree 中保留 DMA 内存:
   /memreserve/ 0x10000000 0x06000000;  /* 96MB for weights+buffers */

2. 或使用 kernel cmdline:
   mem=256M memmap=96M$256M

3. 启用 /dev/mem 访问 (Petalinux 默认可能受限):
   CONFIG_STRICT_DEVMEM=n  (in kernel config)
