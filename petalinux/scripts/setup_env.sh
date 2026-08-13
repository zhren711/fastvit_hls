#!/bin/bash
# ================================================================
# setup_env.sh — FastVIT MicroZed PetaLinux 环境检查
# 在 WSL UbuntuBuild 中运行，检查所有前提条件
# ================================================================

BSP_FILE="/mnt/d/BOYI/fpga/2024.2/2024.2/BSP/mz7020_som_base_2024_2.bsp"
XSA_FILE="/mnt/e/codes/microzed/fastvit_hls/petalinux/hardware/fastvit_hw.xsa"
BIT_FILE="/mnt/e/codes/microzed/fastvit_hls/petalinux/hardware/fastvit_bd_wrapper.bit"
SCRIPT_DIR="$(dirname "$0")"

echo "======================================================"
echo " FastVIT MicroZed PetaLinux 环境检查"
echo "======================================================"

READY=true

echo ""
echo "[1] PetaLinux 2024.2 安装"
if command -v petalinux-create &>/dev/null; then
    VER=$(petalinux-create --version 2>/dev/null | head -1)
    echo "  ✅ $VER"
else
    echo "  ❌ 未安装"
    echo "     下载: https://www.xilinx.com/support/download.html"
    echo "     → Embedded Design Tools → PetaLinux 2024.2"
    echo "     安装: chmod +x 01_install_petalinux.sh && ./01_install_petalinux.sh"
    READY=false
fi

echo ""
echo "[2] Avnet MicroZed BSP (2024.2)"
if [ -f "$BSP_FILE" ]; then
    SZ=$(du -sh "$BSP_FILE" 2>/dev/null | cut -f1)
    echo "  ✅ $BSP_FILE ($SZ)"
else
    echo "  ❌ 未找到: $BSP_FILE"
    READY=false
fi

echo ""
echo "[3] Vivado 硬件描述 (XSA)"
if [ -f "$XSA_FILE" ]; then
    SZ=$(du -sh "$XSA_FILE" 2>/dev/null | cut -f1)
    echo "  ✅ $XSA_FILE ($SZ)"
else
    echo "  ❌ 未找到: $XSA_FILE"
    echo "     在 Vivado 中运行: source export_xsa.tcl"
    READY=false
fi

echo ""
echo "[4] FPGA Bitstream"
if [ -f "$BIT_FILE" ]; then
    SZ=$(du -sh "$BIT_FILE" 2>/dev/null | cut -f1)
    echo "  ✅ $BIT_FILE ($SZ)"
else
    echo "  ❌ 未找到: $BIT_FILE"
    READY=false
fi

echo ""
echo "[5] WSL Ubuntu 版本"
OS=$(lsb_release -rs 2>/dev/null)
if [[ "$OS" == "22.04" ]]; then
    echo "  ✅ Ubuntu 22.04 (PetaLinux 2024.2 支持)"
else
    echo "  ⚠️  Ubuntu $OS (推荐 22.04)"
fi

echo ""
echo "======================================================"
if $READY; then
    echo " ✅ 所有前提条件满足，可以开始创建项目"
    echo ""
    echo " 运行顺序:"
    echo "   source /opt/petalinux/2024.2/settings.sh"
    echo "   bash $SCRIPT_DIR/02_create_project.sh"
    echo "   cd ~/fastvit_microzed"
    echo "   bash $SCRIPT_DIR/03_build_and_package.sh"
else
    echo " ❌ 请先解决上述问题，再运行项目脚本"
    echo ""
    echo " 最重要的缺失项:"
    echo "   PetaLinux 2024.2 安装文件需要从 AMD 官网下载"
    echo "   文件名: petalinux-v2024.2-final-installer.run (~8GB)"
fi
echo "======================================================"
