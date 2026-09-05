/* S 模式定时器驱动 (M5)
 *
 * M 模式版直接摸 CLINT MMIO (mtime/mtimecmp); S 模式必须换路子:
 *   读时间: rdtime 指令 —— time CSR 是 mtime 的 S 模式只读视图 (10MHz, 同源);
 *   排闹钟: sbi_set_timer() ecall —— mtimecmp 是 M 模式专用 MMIO, S 模式写入
 *           会直接非法指令, 只能请 OpenSBI 代写。
 *
 * 中断到达路径 (两跳, 这是理解 S 模式定时器的关键):
 *   sbi_set_timer(t) -> OpenSBI 写本 hart mtimecmp = t
 *   -> mtime >= mtimecmp 时 MTIP 置起, trap 进 OpenSBI (M 模式)
 *   -> OpenSBI 把 STIP 注入本 hart 的 ip 寄存器 (软件模拟"转发")
 *   -> S 模式看到 scause=5 (Supervisor timer interrupt) -> 我们的 stvec */
#include "kernel/timer.hpp"
#include "lib/sbi.hpp"

/* QEMU virt 时基 10MHz: 1 tick = 100ns → 1ms = 10000 tick */
constexpr u64 TICKS_PER_MS = 10000;
constexpr u64 TICK_MS      = 1;          /* 每个中断代表 1ms */

/* 中断里 ++, 主循环里读 (单 hart 有效跑内核, 64 位读是原子的, 无需加锁) */
static volatile u64 tick_ms = 0;

/* rdtime: 读 time CSR (= mtime), 防撕裂读法不再需要 ——
 * CSR 读取是单条指令原子完成, 这也是 M 模式版读 CLINT MMIO 做不到的 */
static inline u64 read_time() {
    u64 t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

void timer_init() {
    sbi_set_timer(read_time() + TICKS_PER_MS);          /* 第一个闹钟: 1ms 后 */
    asm volatile("csrs sie, %0"     :: "r"(1UL << 5));  /* STIE: 开 S 定时器中断 */
    asm volatile("csrs sstatus, %0" :: "r"(1UL << 1));  /* SIE : 开全局中断 */
    /* 对照 M 模式版: mie.MTIE(bit7)+mstatus.MIE(bit3) -> sie.STIE(bit5)+sstatus.SIE(bit1)
     * 注意 sstatus 的位编排和 mstatus 不同! SIE 在 bit1, 不在 bit3 */
}

void timer_handler() {
    sbi_set_timer(read_time() + TICKS_PER_MS);          /* 排下一次闹钟 */
    tick_ms += TICK_MS;
}

u64 uptime_ms() {
    return tick_ms;
}
