# riscvPrj — 从零手写 riscv64 裸机操作系统

一个用于学习 RISC-V 的操作系统内核：**无 libc、无标准头文件、无 C++ 运行时**，
全部手写，跑在 QEMU 的 `virt` 机器上（`qemu-system-riscv64 -M virt`），
与同目录的 [myos](../myos)（aarch64）同风格、同方法论。

学习路线见 [docs/learning-plan.md](docs/learning-plan.md)（12 个月 RISC-V 专家计划）。

## 当前进度: M5（OpenSBI + S 模式）+ 中断体系完整 + 异常分类

- **启动**：`arch/boot.S` — OpenSBI（M 模式固件）完成初始化后把内核放进
  **S 模式**（入口 `0x80200000`，a0=hartid a1=dtb）；`amoswap` 原子守卫保证
  多 hart 只有一个跑内核；清 BSS、设栈、设 `stvec`/`sscratch`
- **异常分类**：`kernel/trap.cpp` — 三档处理：可恢复（非法指令经 OpenSBI
  重定向 / 断点由 medeleg 直接委派，按指令长度跳过）、ecall 占位、
  不可恢复（访存/页错误）打印现场后停机
- **UART 16550**：`drivers/uart.cpp` — 基址 `0x10000000`；**中断驱动接收**
  （环形缓冲区，ISR 生产 / 主循环消费）
- **printf**：`lib/printf.cpp` — `%d %i %u %x %X %c %s %p %%`
- **Trap 现场**：`arch/trap.S` + `kernel/trap.cpp` — `csrrw t0, sscratch, t0`
  技巧保存全部 GPR + `sepc/scause/stval/sstatus`，`sret` 返回
- **SBI 定时器**：`kernel/timer.cpp` + `lib/sbi.cpp` — `rdtime` 读时间，
  `sbi_set_timer()` ecall 排闹钟（OpenSBI 代写 mtimecmp → 注入 STIP），1ms tick
- **PLIC 外设中断**：`kernel/plic.cpp` — S-context（hartid*2+1），
  UART 接收中断（中断源 10），claim/complete 协议
  ★ QEMU 的 PLIC stride 与 SiFive 真实硬件不同（0x80/0x1000），详见 m5-notes

## 构建与运行

```bash
make            # 编译 → build/riscvPrj.elf
make run        # 启动 QEMU（-M virt -smp 2 -m 128M -nographic，OpenSBI → S 模式内核）
make disasm     # 反汇编内核 → build/disasm.txt（学指令编码用）
make asm-formats  # Phase 1 练习: 汇编 asm/ex1-formats.S 并反汇编(六种指令格式)
make asm-c2asm    # Phase 1 练习: 把 asm/ex2-c-to-asm.c 编译成汇编(-O0/-O2 对比)
make clean      # 删除 build/
```

或者直接跑脚本：`bash run.sh`

启动后你会看到 OpenSBI 横幅（M 模式固件）→ riscvPrj 横幅（S 模式内核），然后：

- 任意输入会被回显（走 **UART 中断路径**，PLIC S-context 投递）
- 按 `i` 查看 RX 中断统计（证明输入是中断驱动的）
- 按 `s` 读 `sstatus/sie/sip` —— 看 S 模式中断开关状态
- 按 `e` 触发非法指令（经 **OpenSBI 重定向**回来）、按 `b` 触发断点
  （`ebreak`，由 **medeleg 直接委派**）——观察异常分类与 `sepc`
  按指令长度跳过恢复（32 位指令 `+=4`，压缩指令 `+=2`）
- `Ctrl+C`（字节 `0x03`）停机；`Ctrl+A X` 退出 QEMU

## 里程碑路线（对应学习计划 Phase 2–4）

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M1 | 启动 + UART + printf + trap 现场 | ✅ |
| M2 | 异常处理完善（非法指令/断点等分类处理） | ✅ |
| M3 | CLINT 机器定时器中断 + uptime | ✅ |
| M4 | PLIC + UART 接收中断（中断驱动 echo） | ✅ |
| M5 | OpenSBI 启动 + S 模式运行（SBI 定时器 / S-context PLIC / 委派） | ✅ |
| M6 | Sv39 虚拟内存 + 页分配器 | |
| M7 | 移植 myos 调度器/信号量/Shell/kmalloc | |
| M8 | QEMU 启动 Linux riscv64 内核 | |

## 目录结构

```
riscvPrj/
├── arch/          # boot.S (S 模式启动) / trap.S (S 模式 trap 入口)
├── kernel/        # main / trap / timer (SBI 定时器) / plic (S-context)
├── drivers/       # uart.cpp (16550, 中断驱动接收)
├── lib/           # printf.cpp + sbi.cpp (SBI ecall 封装)
├── asm/           # Phase 1 指令集练习 (ex1 格式 / ex2 C↔汇编)
├── include/       # 头文件 (types/uart/printf/trap/timer/plic/sbi)
├── docs/          # 学习计划 + M1-M5 笔记 + trap-csrs + 汇编小词典
├── linker.ld      # 链接脚本 (0x80200000, OpenSBI 之后)
├── Makefile / run.sh
```
