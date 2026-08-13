#!/bin/bash
# ================================================================
# 03_build_and_package.sh
# 编译 PetaLinux 并打包 SD 卡镜像
#
# 用法:
#   source /opt/petalinux/2024.2/settings.sh
#   cd ~/fastvit_microzed
#   ../petalinux/scripts/03_build_and_package.sh
# ================================================================

set -e

PROJECT_NAME="fastvit_microzed"
PROJ_DIR="$HOME/$PROJECT_NAME"
OUT_DIR="/mnt/e/codes/microzed/fastvit_hls/petalinux/output"
BIT_FILE="/mnt/e/codes/microzed/fastvit_hls/petalinux/hardware/fastvit_bd_wrapper.bit"

echo "================================================"
echo " FastVIT-T8 PetaLinux Build"
echo "================================================"

cd "$PROJ_DIR"

# 1. 编译所有组件
echo ""
echo ">>> Step 1: Building PetaLinux project (takes ~30-60 min)..."
petalinux-build

# 2. 检查编译结果
echo ""
echo ">>> Step 2: Checking build outputs..."
ls -lh "$PROJ_DIR/images/linux/"

# 3. 打包 BOOT.BIN (包含 bitstream)
echo ""
echo ">>> Step 3: Packaging BOOT.BIN with FPGA bitstream..."
petalinux-package --boot \
    --fsbl "$PROJ_DIR/images/linux/zynq_fsbl.elf" \
    --fpga "$BIT_FILE" \
    --u-boot "$PROJ_DIR/images/linux/u-boot.elf" \
    --force

# 4. 复制输出文件
echo ""
echo ">>> Step 4: Copying outputs to $OUT_DIR..."
mkdir -p "$OUT_DIR"
cp "$PROJ_DIR/images/linux/BOOT.BIN"      "$OUT_DIR/"
cp "$PROJ_DIR/images/linux/image.ub"      "$OUT_DIR/"  2>/dev/null || true
cp "$PROJ_DIR/images/linux/boot.scr"      "$OUT_DIR/"  2>/dev/null || true

# 复制应用程序
APP_PATH=$(find "$PROJ_DIR/build" -name "fastvit_infer" -type f 2>/dev/null | head -1)
if [ -n "$APP_PATH" ]; then
    cp "$APP_PATH" "$OUT_DIR/"
    echo "  Application binary: $OUT_DIR/fastvit_infer"
fi

echo ""
echo "================================================"
echo " Build complete! SD card files:"
ls -lh "$OUT_DIR/"
echo ""
echo " SD Card layout:"
echo "   Partition 1 (FAT32): BOOT.BIN, image.ub, boot.scr"
echo "   Partition 2 (ext4):  Linux rootfs"
echo ""
echo " After booting on MicroZed:"
echo "   cp /mnt/e/.../weights_t8/*.bin /home/root/weights_t8/"
echo "   ./fastvit_infer /home/root/weights_t8"
echo "================================================"
