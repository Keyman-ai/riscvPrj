/* 迷你 printf: %d %i %u %x %X %c %s %p %%
 * 与 myos 同款实现, 但 M1 阶段还没有信号量, 暂时去掉打印互斥锁;
 * 等移植调度器/信号量时再加回来。 */
#pragma once

void printf(const char* fmt, ...);
