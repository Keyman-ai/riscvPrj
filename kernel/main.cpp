/* riscvPrj M5 主程序: S 模式内核 (OpenSBI 载入)
 *
 * M5 变化: 入口从"QEMU 直接扔进 M 模式"变成"OpenSBI 固件跳进 S 模式"。
 *   - hartid/dtb 来自 OpenSBI 的 a0/a1 (S 模式读不了 mhartid);
 *   - 定时器走 SBI ecall, PLIC 用 S-context, trap 全家桶换 S 前缀 CSR;
 *   - 新增 's' 命令: 直接读 sstatus/sie/sip 看 S 模式中断开关状态。 */
#include "drivers/uart.hpp"
#include "lib/printf.hpp"
#include "kernel/trap.hpp"
#include "kernel/timer.hpp"
#include "kernel/plic.hpp"

/* 链接脚本导出的符号 */
extern char __stack_top[];

/* 读 S 模式 CSR (trap 之外也能随时看状态) */
static inline u64 read_sstatus() {
    u64 v; asm volatile("csrr %0, sstatus" : "=r"(v)); return v;
}
static inline u64 read_sie() {
    u64 v; asm volatile("csrr %0, sie" : "=r"(v)); return v;
}
static inline u64 read_sip() {
    u64 v; asm volatile("csrr %0, sip" : "=r"(v)); return v;
}

extern "C" void kernel_main(u64 hartid, u64 dtb) {
    uart_init();
    plic_init((u32)hartid);  /* M5: 配置本 hart 的 S-context = hartid*2+1 */
    uart_enable_rx_irq();    /* 打开 UART 接收中断 (IER bit0) */
    timer_init();            /* SBI 排第一个闹钟, 开 STIE, 最后开总闸 SIE */

    printf("\n");
    printf("==============================================\n");
    printf("  riscvPrj - riscv64 OS (M1-M5, S-mode)\n");
    printf("==============================================\n");
    printf("boot    : OpenSBI (M-mode @ 0x80000000) -> S-mode payload\n");
    printf("cpu     : hartid = %d, PLIC S-context = %d\n",
           (int)hartid, (int)(hartid * 2 + 1));
    printf("dtb     : %p (a1 from OpenSBI)\n", (void*)dtb);
    printf("uart    : 16550 @ 0x%X (IRQ-driven RX)\n", 0x10000000);
    printf("plic    : 0x%X, source %d = UART\n", 0x0c000000, PLIC_UART_SOURCE);
    printf("timer   : SBI set_timer + rdtime, 1ms tick\n");
    printf("stack   : top = %p\n", (void*)__stack_top);
    printf("----------------------------------------------\n");
    printf("type to echo (via IRQ);  'e' = illegal-instruction;\n");
    printf("'b' = breakpoint.  'i' = rx stats.  's' = S-mode csrs.\n");
    printf("Ctrl+C = halt.\n");
    printf("> ");

    u64 last_print = 0;
    for (;;) {
        /* 从环形缓冲区取字符 (UART 中断在后台塞进来) */
        int c = uart_rx_pop();
        if (c >= 0) {
            char ch = (char)c;
            if (ch == '\r') {
                uart_putc('\n');
                printf("> ");
            } else if (ch == 0x03) {
                printf("\n[riscvPrj] bye, halting (uptime %u ms)\n", (u32)uptime_ms());
                for (;;) {
                    asm volatile("wfi");
                }
            } else if (ch == 'e') {
                printf("\n[riscvPrj] triggering illegal instruction...\n");
                asm volatile(".word 0xffffffff");   /* 未定义指令 -> scause=2 (经 OpenSBI 重定向) */
                printf("[riscvPrj] back from trap (sepc advanced, context intact)\n");
                printf("> ");
            } else if (ch == 'b') {
                /* ebreak 断点: medeleg bit3=1, 直接委派 -> scause=3 */
                printf("\n[riscvPrj] triggering breakpoint (ebreak)...\n");
                asm volatile("ebreak");             /* 0x00100073 -> scause=3 */
                printf("[riscvPrj] back from breakpoint\n");
                printf("> ");
            } else if (ch == 'i') {
                /* 验证: 中断次数 > 输入字符数 说明真的走中断路径 */
                printf("[info] rx_irq=%u (IRQ-driven RX works)\n",
                       (u32)uart_rx_irq_count());
                printf("> ");
            } else if (ch == 's') {
                /* M5: 看 S 模式中断开关 —— 对照位定义自己解码 */
                printf("[csrs] sstatus=0x%X (SIE=bit1, SPP=bit8)\n", (u32)read_sstatus());
                printf("       sie=0x%X (SSIE=1 STIE=5 SEIE=9)\n", (u32)read_sie());
                printf("       sip=0x%X (pending bits)\n", (u32)read_sip());
                printf("> ");
            } else {
                uart_putc(ch);
            }
        }

        /* 每 1000ms 打印一次 uptime */
        if (uptime_ms() - last_print >= 1000) {
            last_print = uptime_ms();
            printf("[tick] uptime = %u ms\n", (u32)last_print);
        }
    }
}
