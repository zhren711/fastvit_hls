#!/bin/bash
# ================================================================
# 02_create_project.sh
# 使用 Avnet MicroZed BSP 创建 PetaLinux 2024.2 项目
# 并配置自定义 FPGA 硬件 (fastvit_hw.xsa)
#
# 用法 (在 WSL UbuntuBuild 中):
#   source /opt/petalinux/2024.2/settings.sh
#   chmod +x 02_create_project.sh
#   ./02_create_project.sh
# ================================================================

set -e

BSP_FILE="/mnt/d/BOYI/fpga/2024.2/2024.2/BSP/mz7020_som_base_2024_2.bsp"
XSA_FILE="/mnt/e/codes/microzed/fastvit_hls/petalinux/hardware/fastvit_hw.xsa"
PROJECT_NAME="fastvit_microzed"
PROJ_DIR="$HOME/$PROJECT_NAME"
HW_DIR="/mnt/e/codes/microzed/fastvit_hls/petalinux"

echo "================================================"
echo " FastVIT-T8 PetaLinux Project Setup"
echo " BSP:  $BSP_FILE"
echo " XSA:  $XSA_FILE"
echo " Proj: $PROJ_DIR"
echo "================================================"

# 检查前提
if [ ! -f "$BSP_FILE" ]; then
    echo "ERROR: BSP not found: $BSP_FILE"; exit 1
fi
if [ ! -f "$XSA_FILE" ]; then
    echo "ERROR: XSA not found: $XSA_FILE"; exit 1
fi
if ! command -v petalinux-create &>/dev/null; then
    echo "ERROR: petalinux not in PATH. Run: source /opt/petalinux/2024.2/settings.sh"
    exit 1
fi

# 1. 从 BSP 创建项目
echo ""
echo ">>> Step 1: Creating project from Avnet MicroZed BSP..."
if [ -d "$PROJ_DIR" ]; then
    echo "  Project already exists at $PROJ_DIR, skipping creation"
else
    cd "$HOME"
    petalinux-create -t project -s "$BSP_FILE" -n "$PROJECT_NAME"
fi
cd "$PROJ_DIR"

# 2. 配置自定义硬件
echo ""
echo ">>> Step 2: Configuring with fastvit_hw.xsa..."
petalinux-config --get-hw-description "$XSA_FILE" --silentconfig

# 3. 配置内核: 保留 DMA 内存
echo ""
echo ">>> Step 3: Configuring device tree for DMA memory reservation..."
DTSI_FILE="$PROJ_DIR/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi"

# 写入自定义 device tree
cat > "$DTSI_FILE" << 'DTSI_EOF'
/include/ "system-conf.dtsi"

/ {
    /* Reserve 96MB for FastVIT weight+feature buffers */
    /* Address: 0x10000000 - 0x15FFFFFF */
    reserved-memory {
        #address-cells = <1>;
        #size-cells = <1>;
        ranges;

        fastvit_reserved: buffer@10000000 {
            reg = <0x10000000 0x06000000>;
            no-map;
        };
    };
};

/* Allow /dev/mem access to PL registers */
&amba {
    /* PL AXI-Lite slaves accessible through GP0 */
};
DTSI_EOF

echo "  device tree written: $DTSI_FILE"

# 4. 创建 FastVIT 应用
echo ""
echo ">>> Step 4: Creating fastvit_infer application..."
APP_NAME="fastvit_infer"

# 检查是否已存在
if [ ! -d "$PROJ_DIR/project-spec/meta-user/recipes-apps/$APP_NAME" ]; then
    petalinux-create -t apps --template c -n $APP_NAME --enable
fi

APP_SRC_DIR="$PROJ_DIR/project-spec/meta-user/recipes-apps/$APP_NAME/files"
mkdir -p "$APP_SRC_DIR"

# 复制源文件
echo "  Copying source files..."
cp "$HW_DIR/software/fastvit_app/src/"*.c  "$APP_SRC_DIR/"
cp "$HW_DIR/software/fastvit_app/src/"*.h  "$APP_SRC_DIR/"
cp "$HW_DIR/software/fastvit_app/include/"*.h "$APP_SRC_DIR/"

# 写入 recipe Makefile
cat > "$APP_SRC_DIR/Makefile" << 'MAKEFILE_EOF'
APP = fastvit_infer
SRCS = main.c fastvit_driver.c fastvit_infer.c
CFLAGS = -O2 -Wall -I. -DLINUX_BUILD
LDFLAGS = -lm

all: $(APP)
$(APP): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
install:
	install -m 0755 $(APP) $(DESTDIR)$(bindir)
MAKEFILE_EOF

echo "  Source files copied to: $APP_SRC_DIR"

echo ""
echo "================================================"
echo " Project created successfully!"
echo " Location: $PROJ_DIR"
echo ""
echo " Next steps:"
echo "   cd $PROJ_DIR"
echo "   ./$(basename $0 .sh | sed 's/02/03/')_build.sh"
echo "   # Or manually:"
echo "   petalinux-build"
echo "================================================"
