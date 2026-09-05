#!/bin/bash
# riscvPrj 一键运行: QEMU virt, OpenSBI 固件 (M 模式) -> 内核
# M5: 去掉 -bios none, QEMU 自动加载自带 OpenSBI (fw_dynamic),
#     它完成 M 模式初始化后跳到 0x80200000 处的内核。
qemu-system-riscv64 \
    -M virt \
    -smp 2 \
    -m 128M \
    -nographic \
    -kernel build/riscvPrj.elf
