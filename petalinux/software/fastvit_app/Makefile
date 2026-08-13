# Makefile for FastVIT-T8 Petalinux Application
# Cross-compile for ARM Cortex-A9 (Zynq-7000)
#
# 用法:
#   source /opt/petalinux/<ver>/environment-setup-cortexa9t2hf-neon-poky-linux-gnueabi
#   make
#   # 或指定交叉编译器:
#   make CROSS_COMPILE=arm-linux-gnueabihf-

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

TARGET  = fastvit_infer

SRC_DIR = src
INC_DIR = include

# gelu_lut.c/.h are no longer linked into the ARM binary -- GELU now
# runs on the FPGA (fastvit_ip OP_GELU, see fastvit_ip/gelu_worker.cpp).
# The files stay in src/ as the canonical reference table (regenerated
# by tools/gen_gelu_lut.py) that gelu_worker.cpp's ROM was copied from.
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/fastvit_driver.c \
       $(SRC_DIR)/fastvit_infer.c

CFLAGS  = -O2 -Wall -mfpu=neon -mfloat-abi=hard \
          -I$(INC_DIR) -I$(SRC_DIR) \
          -DLINUX_BUILD
LDFLAGS = -lm -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built: $(TARGET)"
	@file $(TARGET)

clean:
	rm -f $(TARGET)

# 上传到板子 (需要配置 BOARD_IP)
BOARD_IP ?= 192.168.1.100
upload: $(TARGET)
	scp $(TARGET) root@$(BOARD_IP):/home/root/
	scp -r ../weights root@$(BOARD_IP):/home/root/

.PHONY: all clean upload
