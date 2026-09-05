/* trap 现场与处理函数声明 (arch/trap.S 保存现场, kernel/trap.cpp 处理) */
#pragma once
#include "types.hpp"

/* 与 arch/trap.S 的保存顺序严格对应: x1..x31 + sepc/scause/stval/sstatus
 * M5: CSR 全部换成 S 前缀 (sepc/scause/stval/sstatus), 布局不变 */
struct TrapFrame {
    u64 ra;      /* x1  */
    u64 sp;      /* x2  */
    u64 gp;      /* x3  */
    u64 tp;      /* x4  */
    u64 t0;      /* x5  */
    u64 t1;      /* x6  */
    u64 t2;      /* x7  */
    u64 s0;      /* x8  */
    u64 s1;      /* x9  */
    u64 a0;      /* x10 */
    u64 a1;      /* x11 */
    u64 a2;      /* x12 */
    u64 a3;      /* x13 */
    u64 a4;      /* x14 */
    u64 a5;      /* x15 */
    u64 a6;      /* x16 */
    u64 a7;      /* x17 */
    u64 s2;      /* x18 */
    u64 s3;      /* x19 */
    u64 s4;      /* x20 */
    u64 s5;      /* x21 */
    u64 s6;      /* x22 */
    u64 s7;      /* x23 */
    u64 s8;      /* x24 */
    u64 s9;      /* x25 */
    u64 s10;     /* x26 */
    u64 s11;     /* x27 */
    u64 t3;      /* x28 */
    u64 t4;      /* x29 */
    u64 t5;      /* x30 */
    u64 t6;      /* x31 */
    u64 sepc;    /* 32: 出错/中断点的 PC (原 mepc) */
    u64 scause;  /* 33: trap 原因 (原 mcause) */
    u64 stval;   /* 34: 附加信息, 如出错地址 (原 mtval) */
    u64 sstatus; /* 35: S 模式状态, SPP=来源模式 (原 mstatus) */
};
static_assert(sizeof(TrapFrame) == 35 * 8, "TrapFrame layout mismatch");

/* trap 处理函数: arch/trap.S 调用 (extern "C", 无名字修饰) */
extern "C" void trap_handler(TrapFrame* frame);
