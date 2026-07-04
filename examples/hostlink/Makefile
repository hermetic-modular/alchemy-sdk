# Daisy-bootloader workflow for this example.
#
# Usage from this directory:
#   make              — build firmware for the Daisy Seed2 DFM
#   make program-dfu  — flash via DFU to QSPI at 0x90040000
#   make size         — arm-none-eabi-size summary for this example
#   make clean        — clean the cross-compile build tree
#
# The cross-compile build directory defaults to ../../build-arm. Override with:
#   make BUILD_DIR=/path/to/build program-dfu
#
# First-time setup from the repo root:
#   git submodule update --init --recursive
#   cmake -B build-arm -G "Unix Makefiles" \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/stm32h750xx.cmake

BUILD_DIR ?= $(CURDIR)/../../build-arm
TARGET    := hostlink

.DEFAULT_GOAL := all
.PHONY: all program-dfu size clean

all:
	@cmake --build $(BUILD_DIR) --target $(TARGET)

program-dfu: all
	@cmake --build $(BUILD_DIR) --target $(TARGET)-flash

size:
	@cmake --build $(BUILD_DIR) --target $(TARGET)-size

clean:
	@cmake --build $(BUILD_DIR) --target clean
