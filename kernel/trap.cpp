/* S 模式 trap 处理 (M5): 中断分发 + 异常打印现场
 *
 * 与 M 模式版相比的三处实质变化:
 *   1. 中断号换班: M 模式的 7(timer)/11(external) 变成 S 模式的 5/9
 *      —— 因为 OpenSBI 通过 mideleg=0x222 把 SSIP/STIP/SEIP 委派给了 S 模式;
 *   2. 现场字段改名: mepc/mcause/mtval/mstatus -> sepc/scause/stval/sstatus;
 *   3. 非法指令(cause=2)不在 medeleg=0xb109 里, 先落进 OpenSBI 的 M 模式
 *      trap 处理, 它模拟不了就把 trap "重定向"回我们的 stvec (scause 不变)
 *      —— 所以处理逻辑不变, 但路径多了一跳 (m5-notes.md 里有图)。 */
#include "kernel/trap.hpp"
#include "kernel/timer.hpp"
#include "kernel/plic.hpp"
#include "drivers/uart.hpp"
#include "lib/printf.hpp"

/* 异常原因码 (scause 低 12 位) 的常见取值, 用于打印 */
static const char* exception_name(u64 cause) {
    switch (cause) {
        case 0:  return "Instruction address misaligned";
        case 1:  return "Instruction access fault";
        case 2:  return "Illegal instruction";
        case 3:  return "Breakpoint";
        case 4:  return "Load address misaligned";
        case 5:  return "Load access fault";
        case 6:  return "Store/AMO address misaligned";
        case 7:  return "Store/AMO access fault";
        case 8:  return "Environment call from U-mode";
        case 9:  return "Environment call from S-mode";
        case 11: return "Environment call from M-mode";
        case 12: return "Instruction page fault";
        case 13: return "Load page fault";
        case 15: return "Store/AMO page fault";
        default: return "unknown";
    }
}

/* 中断原因码: scause 最高位为 1, 低 12 位对应 sip 的 bit。
 * 注意与 M 模式版不同: S 模式只看得到委派下来的这三种 (mideleg=0x222) */
static const char* interrupt_name(u64 cause) {
    switch (cause) {
        case 1:  return "Supervisor software interrupt";  /* OpenSBI IPI 用, 我们不碰 */
        case 5:  return "Supervisor timer interrupt";     /* 原 M 模式 cause=7 */
        case 9:  return "Supervisor external interrupt";  /* 原 M 模式 cause=11, PLIC */
        default: return "unknown interrupt";
    }
}

/* 判断 sepc 处指令的长度:
 * RISC-V 指令按最低 2 位区分宽度 —— 11 = 32 位, 其他 = 16 位压缩指令
 * (IALIGN=16, 即 rv64gc 允许 2 字节对齐的压缩指令) */
static u32 instr_len(const void* addr) {
    u16 half = *(const volatile u16*)addr;
    return (half & 0x3) == 0x3 ? 4 : 2;
}

/* ecall 来源模式名字 (cause 8/9/11 用; S 模式内核只会遇到 8/9) */
static const char* ecall_mode(u64 cause) {
    switch (cause) {
        case 8:  return "U-mode";
        case 9:  return "S-mode";
        default: return "M-mode";
    }
}

extern "C" void trap_handler(TrapFrame* f) {
    bool is_interrupt = (f->scause >> 63) != 0;
    u64 cause = f->scause & 0x7FFFFFFFFFFFFFFFULL;

    if (is_interrupt) {
        /* M5: S 定时器中断 (scause 最高位=1, 低 12 位 = 5) */
        if (cause == 5) {
            timer_handler();   /* SBI 排下一次闹钟 + tick++ (中断上下文执行) */
            return;
        }
        /* M5: S 外设中断 (scause = 9) —— 来自 PLIC 的 S-context 1 */
        if (cause == 9) {
            u32 source = plic_claim();          /* 拿到最高优先级挂起源 (同时清挂起) */
            if (source == PLIC_UART_SOURCE) {
                uart_rx_irq_handler();          /* UART 有字符: 塞进环形缓冲区 */
            }
            if (source != 0) {
                plic_complete(source);          /* 必须写回, 否则该源中断不再上报 */
            }
            return;
        }
        /* 其他中断 (如 cause=1 软件 IPI, OpenSBI 核间通信用, 我们不使能) */
        printf("[trap] interrupt: %s (cause=%d), sepc=%p\n",
               interrupt_name(cause), (int)cause, (void*)f->sepc);
        return;
    }

    /* 异常: 打印完整现场 */
    printf("\n[trap] exception: %s (cause=%d)\n", exception_name(cause), (int)cause);
    printf("  sepc    = %p\n", (void*)f->sepc);
    printf("  stval   = %p\n", (void*)f->stval);
    printf("  sstatus = 0x%X\n", (u32)f->sstatus);
    printf("  ra=%p sp=%p gp=%p tp=%p\n",
           (void*)f->ra, (void*)f->sp, (void*)f->gp, (void*)f->tp);
    printf("  a0=%p a1=%p a2=%p a3=%p\n",
           (void*)f->a0, (void*)f->a1, (void*)f->a2, (void*)f->a3);

    /* M2 的异常三档分类在 S 模式下原样适用 */

    /* ① 可恢复: 跳过出错指令继续执行 (sepc += 指令长度) */
    switch (cause) {
        case 2: {   /* 非法指令: 演示用, 跳过 (经 OpenSBI 重定向进来) */
            u32 len = instr_len((void*)f->sepc);
            printf("  [trap] recoverable: skipping illegal instruction (sepc += %u)\n", len);
            f->sepc += len;
            return;
        }
        case 3: {   /* 断点 ebreak: medeleg bit3=1, 直接委派进我们 */
            u32 len = instr_len((void*)f->sepc);   /* ebreak 4B / c.ebreak 2B 通吃 */
            printf("  [trap] recoverable: breakpoint hit, skipping (sepc += %u)\n", len);
            f->sepc += len;
            return;
        }
    }

    /* ② ecall: 我们自己的 SBI ecall 由 OpenSBI 在 M 模式处理完才返回,
     *    不会走到这里; 这条分支留着兜底 (将来 U-mode 应用系统调用入口) */
    if (cause == 8 || cause == 9) {
        printf("  [trap] ecall from %s: no syscalls implemented, skipping\n",
               ecall_mode(cause));
        f->sepc += 4;   /* ecall 恒为 32 位指令, 无压缩版本 */
        return;
    }

    /* ③ 不可恢复: 访存错误/对齐错误/页错误等, 打印后停机 */
    printf("  [trap] unrecoverable, halting\n");
    for (;;) {
        asm volatile("wfi");
    }
}
