/* PLIC (Platform-Level Interrupt Controller) 驱动 (QEMU virt) */
#pragma once
#include "types.hpp"

/* QEMU virt: UART 接在 PLIC 中断源 10 上 (设备树 uart@10000000 的
 * interrupts 属性 = <0x0a>; 旁边的 goldfish-rtc 是 11) */
constexpr u32 PLIC_UART_SOURCE = 10;

/* M5: S 模式下用本 hart 的 S-context (hartid*2+1), 开 SEIE */
void plic_init(u32 hartid);     /* 配置 UART 源优先级 + 使能 + 阈值, 开 SEIE */
u32  plic_claim();              /* 取当前最高优先级挂起源 (读取即清除挂起) */
void plic_complete(u32 source); /* 处理完必须写回, 否则中断不再触发 */
