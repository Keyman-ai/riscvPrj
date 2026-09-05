/* riscvPrj S 模式启动汇编 (M5)
 *
 * 启动路径变了: QEMU 先跑 OpenSBI 固件 (M 模式, 0x80000000), 它完成
 * M 模式初始化后把控制权交给 0x80200000 处的内核 —— 此时 CPU 已经是
 * S 模式。进入约定: a0 = hartid, a1 = dtb 物理地址。
 *
 * 与 M 模式版的两个本质区别:
 *   1. S 模式读不到 mhartid (M 模式专用 CSR) —— hartid 只能从 a0 拿;
 *   2. 一切 M 前缀 CSR 换成 S 前缀: mtvec->stvec, mscratch->sscratch。
 *
 * hart 策略: OpenSBI (fw_dynamic, HSM) 默认只把冷启动 hart 送进 payload,
 * 这里用原子交换守卫保证"谁先到谁跑内核", 其余 hart 原地 WFI。
 *
 * 注意: 必须 .option norelax 关闭链接器松弛 —— 否则 "la" 可能被改写成
 * gp 相对寻址 (addi x, gp, off), 而启动时 gp 尚未初始化, 会拿到错误地址。 */
.option norelax

.section .text.boot
.globl _start

_start:
    /* 0. 谁先到谁跑内核: amoswap.w 原子交换 boot_flag (A 扩展预览)。
     *    默认 fw_dynamic 下只有冷启动 hart 会进 payload (其余 hart 在
     *    OpenSBI 的 HSM 里停着, 等 sbi_hsm_hart_start —— M7 再学);
     *    这个守卫保证即使 OpenSBI 把多个 hart 都放进来, 也只有一个跑。 */
    la   t0, boot_flag
    li   t1, 1
    amoswap.w.aqrl t2, t1, (t0)     /* t2 = boot_flag 旧值, boot_flag = 1 */
    bnez t2, .Lpark                 /* 已有 hart 在跑内核: 去停机循环 */

    /* 1. 清零 .bss */
    la   t0, __bss_start
    la   t1, __bss_end
1:  bgeu t0, t1, 2f                 /* t0 >= t1 则跳出 */
    sd   zero, 0(t0)                /* 写 8 字节 0 */
    addi t0, t0, 8
    j    1b

    /* 2. 设置栈指针与全局指针 */
2:  la   sp, __stack_top            /* sp = 栈顶 (向下增长) */
    la   gp, __global_pointer$      /* gp = 小数据锚点 */

    /* 3. 设置 trap 向量: stvec 指向 trap_entry (直接模式, 4 字节对齐)。
     *    OpenSBI 委派给 S 模式的中断/异常 + 它重定向过来的 trap 都从这里进 */
    la   t0, trap_entry
    csrw stvec, t0

    /* 4. sscratch = &trap_frame: trap 入口用 csrrw 交换, 保存原始 t0 */
    la   t0, trap_frame
    csrw sscratch, t0

    /* 5. 跳进 C++ 世界 (extern "C"): a0 = hartid, a1 = dtb 原样传下去 */
    call kernel_main

.Lhalt:                             /* 兜底: kernel_main 若返回则停机 */
    wfi
    j    .Lhalt

.Lpark:                             /* 非 boot hart 停机等待 */
    wfi
    j    .Lpark

/* "谁先到谁跑内核" 原子守卫 (在 .bss, 启动时为 0; 被 BSS 清零覆盖也无妨,
 * 因为清零只发生一次, 在守卫通过之后) */
.section .bss
.align 2
boot_flag:
    .skip 4
