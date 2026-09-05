/* CLINT 机器定时器驱动 (QEMU virt) */
#pragma once
#include "types.hpp"

void timer_init();          /* 设置第一次 mtimecmp 并开启 MTIE + MIE */
void timer_handler();       /* 定时器中断处理: 排下一次 + 计数 (在 trap 上下文调用) */
u64  uptime_ms();           /* 开机以来的毫秒数 */
