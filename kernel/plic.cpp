/* PLIC (Platform-Level Interrupt Controller) 驱动
 *
 * QEMU virt 的 PLIC 内存布局 (32 位寄存器, 按中断源/上下文编址):
 *   0x0c000000  基址
 *   0x0c000000 + 4×src   每个中断源的优先级 (0=禁用, 1..7 有效)
 *   0x0c001000 + ...     挂起位 (硬件写, 只读)
 *   0x0c002000 + ctx×0x80          每个 context 的使能位   ★ stride=0x80
 *   0x0c200000 + ctx×0x1000        每个 context 的优先级阈值 ★ stride=0x1000
 *   0x0c200004 + ctx×0x1000        每个 context 的 claim(读)/complete(写)
 *
 * context 编号 = hart 在前模式在后: 每个 hart 占两个 context,
 *   ctx = hartid*2 + 0 (M 模式), ctx = hartid*2 + 1 (S 模式)。
 * M4 时内核自己就是 M 模式, 用 context 0; M5 内核在 S 模式,
 * 用 hart 0 的 S-context = 1。M 模式那份留给 OpenSBI, 互不干扰。
 *
 * 协议: 中断来了 → claim 读到源号(同时清挂起) → 处理 → complete 写回源号。
 * 不 complete 的话, PLIC 认为还没处理完, 该源的中断不再上报。
 *
 * ★ M5 踩坑实录: stride 别照抄 SiFive FU540 手册 (enable 0x100 /
 *   context 0x2000)! QEMU virt 的 sifive_plic 模型是 0x80 / 0x1000
 *   (qtree 实测 enable-stride=128, context-stride=4096; Linux 内核
 *   plic.c 的 ENABLE_PER_HART=0x80 / CONTEXT_PER_HART=0x1000 同款)。
 *   写错 stride 的症状极隐蔽: 寄存器读回值"看起来对", 但全写进了
 *   别的 context —— PLIC pending 有, sip.SEIP 永远不来。 */
#include "kernel/plic.hpp"

constexpr u64 PLIC_BASE = 0x0c000000;
constexpr u64 PLIC_PRIORITY = 0x000000;   /* 源优先级区 (与 context 无关) */
constexpr u64 CTX_ENABLE_STRIDE = 0x80;   /* 使能区: 每 context 间隔 0x80 (QEMU!) */
constexpr u64 CTX_CONTEXT_STRIDE = 0x1000;/* 阈值/claim 区: 每 context 间隔 0x1000 (QEMU!) */

/* 本 hart 的 S 模式 context 号 (plic_init 里算好) */
static u32 s_context;

static inline volatile u32* plic_reg(u64 offset) {
    return reinterpret_cast<volatile u32*>(PLIC_BASE + offset);
}

void plic_init(u32 hartid) {
    s_context = hartid * 2 + 1;     /* hart 的 S-context: M 在前 S 在后 */

    /* ① UART 源优先级 = 1 (0 表示禁用该源; 与 context 无关, 写一次即可) */
    *plic_reg(PLIC_PRIORITY + 4 * PLIC_UART_SOURCE) = 1;

    /* ② 使能 UART 源: S-context 使能区 = 0x2000 + ctx*0x80 */
    *plic_reg(0x2000 + static_cast<u64>(s_context) * CTX_ENABLE_STRIDE)
        |= (1u << PLIC_UART_SOURCE);

    /* ③ 阈值 = 0: 所有优先级 > 0 的中断都放行 */
    *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE) = 0;

    /* ④ 开 S 外设中断: sie.SEIE (bit 9) —— 总闸 sstatus.SIE 由 timer_init 开 */
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));
}

u32 plic_claim() {
    return *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE + 4);
}

void plic_complete(u32 source) {
    *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE + 4) = source;
}
