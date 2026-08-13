#!/bin/bash
# ================================================================
# 01_install_petalinux.sh
# 在 WSL Ubuntu 22.04 (UbuntuBuild) 中安装 PetaLinux 2024.2
#
# 前提:
#   1. 从 AMD 网站下载 petalinux-v2024.2-final-installer.run
#      https://www.xilinx.com/support/download/index.html
#      → Embedded Design Tools → PetaLinux 2024.2
#   2. 将安装文件放到 /mnt/d/BOYI/fpga/ 或指定路径
#
# 用法:
#   wsl -d UbuntuBuild
#   chmod +x 01_install_petalinux.sh
#   ./01_install_petalinux.sh
# ================================================================

set -e

INSTALLER="${1:-/mnt/d/BOYI/fpga/petalinux-v2024.2-final-installer.run}"
INSTALL_DIR="/opt/petalinux/2024.2"

echo "================================================"
echo " PetaLinux 2024.2 Installation"
echo " Installer: $INSTALLER"
echo " Install to: $INSTALL_DIR"
echo "================================================"

if [ ! -f "$INSTALLER" ]; then
    echo "ERROR: Installer not found: $INSTALLER"
    echo ""
    echo "Download from:"
    echo "  https://www.xilinx.com/support/download/index.html"
    echo "  → Embedded Design Tools → PetaLinux 2024.2"
    echo ""
    echo "Usage: $0 /path/to/petalinux-v2024.2-final-installer.run"
    exit 1
fi

# 安装依赖 (Ubuntu 22.04)
echo ">>> Installing dependencies..."
sudo apt-get update -qq
sudo apt-get install -y \
    gawk wget git diffstat unzip texinfo gcc build-essential \
    chrpath socat xterm autoconf libtool python3 python3-pip \
    python3-pexpect python3-git python3-jinja2 libegl1-mesa \
    libsdl1.2-dev pylint xterm zlib1g-dev openssl \
    libssl-dev libglib2.0-dev libgobject-2.0-dev \
    libpixman-1-dev lzop libncurses5-dev libncursesw5-dev \
    tftpd-hpa tftp iproute2 net-tools \
    libmpc-dev libmpfr-dev libgmp-dev \
    ca-certificates cpio bc rsync \
    u-boot-tools device-tree-compiler

# 接受 EULA 并安装
echo ">>> Running installer (requires ~30min)..."
sudo mkdir -p $INSTALL_DIR
sudo chmod 755 $INSTALL_DIR
chmod +x "$INSTALLER"
"$INSTALLER" --dir $INSTALL_DIR

echo ""
echo "================================================"
echo " Installation complete: $INSTALL_DIR"
echo " Add to ~/.bashrc:"
echo "   source $INSTALL_DIR/settings.sh"
echo "================================================"
