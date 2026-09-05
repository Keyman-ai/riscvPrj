/* riscvPrj S 模式 trap 入口 (M5, 直接模式, stvec 低 2 位 = 0)。
 * 进入时把全部 GPR + sepc/scause/stval/sstatus 保存进 trap_frame (35 * 8 字节),
 * 调 C 处理函数 trap_handler(TrapFrame*), 返回后恢复现场并 sret。
 *
 * 与 M 模式版的唯一区别是 CSR 名字:
 *   mscratch -> sscratch, mepc->sepc, mcause->scause, mtval->stval,
 *   mstatus->sstatus, mret -> sret。保存/恢复逻辑一字不变 —— trap
 * 现场处理是"模式无关"的套路, 这正是 RISC-V 特权设计漂亮的地方。
 *
 * 关键技巧: 用 sscratch 与 t0 做一次 csrrw 交换 —— 进 trap 时 t0 变成
 * 帧指针, 原 t0 暂存进 sscratch; 退出时反向交换, 同时把 sscratch 恢复成
 * 帧指针, 供下一次 trap 使用。 */
.section .text
.globl trap_entry
.align 2                            /* stvec 直接模式要求 4 字节对齐 */

trap_entry:
    /* ---- 保存现场 ---- */
    csrrw t0, sscratch, t0          /* t0 = &trap_frame, sscratch = 原 t0 */
    sd x1,  0*8(t0)                 /* ra */
    sd x2,  1*8(t0)                 /* sp */
    sd x3,  2*8(t0)                 /* gp */
    sd x4,  3*8(t0)                 /* tp */
    /* x5 (t0) 的原值在 sscratch 里, 稍后补存 */
    sd x6,  5*8(t0)                 /* t1 */
    sd x7,  6*8(t0)                 /* t2 */
    sd x8,  7*8(t0)                 /* s0/fp */
    sd x9,  8*8(t0)                 /* s1 */
    sd x10, 9*8(t0)                 /* a0 */
    sd x11, 10*8(t0)                /* a1 */
    sd x12, 11*8(t0)                /* a2 */
    sd x13, 12*8(t0)                /* a3 */
    sd x14, 13*8(t0)                /* a4 */
    sd x15, 14*8(t0)                /* a5 */
    sd x16, 15*8(t0)                /* a6 */
    sd x17, 16*8(t0)                /* a7 */
    sd x18, 17*8(t0)                /* s2 */
    sd x19, 18*8(t0)                /* s3 */
    sd x20, 19*8(t0)                /* s4 */
    sd x21, 20*8(t0)                /* s5 */
    sd x22, 21*8(t0)                /* s6 */
    sd x23, 22*8(t0)                /* s7 */
    sd x24, 23*8(t0)                /* s8 */
    sd x25, 24*8(t0)                /* s9 */
    sd x26, 25*8(t0)                /* s10 */
    sd x27, 26*8(t0)                /* s11 */
    sd x28, 27*8(t0)                /* t3 */
    sd x29, 28*8(t0)                /* t4 */
    sd x30, 29*8(t0)                /* t5 */
    sd x31, 30*8(t0)                /* t6 */
    csrr t1, sscratch
    sd   t1, 4*8(t0)                /* 原 t0 */
    csrr t1, sepc
    sd   t1, 31*8(t0)
    csrr t1, scause
    sd   t1, 32*8(t0)
    csrr t1, stval
    sd   t1, 33*8(t0)
    csrr t1, sstatus
    sd   t1, 34*8(t0)

    /* ---- 调 C 处理函数 ---- */
    mv   a0, t0                     /* 参数: TrapFrame* */
    call trap_handler

    /* handler 可能修改了 sepc (如异常恢复 sepc+=4) 或 sstatus, 写回 CSR */
    ld   t1, 31*8(t0)               /* sepc */
    csrw sepc, t1
    ld   t1, 34*8(t0)               /* sstatus */
    csrw sstatus, t1

    /* ---- 恢复现场 ---- */
    ld x1,  0*8(t0)
    ld x2,  1*8(t0)
    ld x3,  2*8(t0)
    ld x4,  3*8(t0)
    /* x5 (t0) 最后恢复 */
    ld x6,  5*8(t0)
    ld x7,  6*8(t0)
    ld x8,  7*8(t0)
    ld x9,  8*8(t0)
    ld x10, 9*8(t0)
    ld x11, 10*8(t0)
    ld x12, 11*8(t0)
    ld x13, 12*8(t0)
    ld x14, 13*8(t0)
    ld x15, 14*8(t0)
    ld x16, 15*8(t0)
    ld x17, 16*8(t0)
    ld x18, 17*8(t0)
    ld x19, 18*8(t0)
    ld x20, 19*8(t0)
    ld x21, 20*8(t0)
    ld x22, 21*8(t0)
    ld x23, 22*8(t0)
    ld x24, 23*8(t0)
    ld x25, 24*8(t0)
    ld x26, 25*8(t0)
    ld x27, 26*8(t0)
    ld x28, 27*8(t0)
    ld x29, 28*8(t0)
    ld x30, 29*8(t0)
    ld x31, 30*8(t0)

    /* 反向交换: t0 = 原 t0 (sscratch 里), sscratch = &trap_frame (下次用) */
    csrrw t0, sscratch, t0
    sret

/* trap 现场保存区: 35 个 u64 (x1..x31 + sepc/scause/stval/sstatus) */
.section .bss
.align 3
.globl trap_frame
trap_frame:
    .skip 35 * 8
