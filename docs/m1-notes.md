# M1 笔记：riscvPrj 启机记（从零点亮第一行字）

> 阅读顺序：**1. 做了什么** → **2. 原理**（加电到 C 世界的每一步，
> 含逐行指令解读）→ **3. 技巧**（两个大坑 + 细节）→ **4. 验证**
> （对照实验）→ **附录**（全部源码）。
> 配套：`docs/asm-cheatsheet.md`（指令词典）、`docs/trap-csrs.md`（trap CSR）。

---

## 1. 这一阶段做了什么

**一句话**：让 riscvPrj 在 QEMU 上"从零到有"——加电后打印启动横幅、
回显键盘输入、捕获非法指令并打印现场后恢复。

### 改动清单（M1 全是新增）

| 文件 | 作用 | 现状 |
|------|------|------|
| `linker.ld` | 链接脚本：内核放哪、内存怎么分 | 沿用至今 |
| `arch/boot.S` | 启动汇编：加电 → C 世界 | 沿用至今 |
| `arch/trap.S` | trap 入口：现场保存/恢复 | 沿用至今 |
| `drivers/uart.cpp` | 16550 串口驱动（轮询） | M4 加了中断接收 |
| `lib/printf.cpp` | 迷你 printf | 沿用至今 |
| `kernel/main.cpp` | M1 主程序：横幅 + 阻塞回显 + trap 演示 | M3 改非阻塞，M4 改中断驱动 |
| `kernel/trap.cpp` | trap 处理：异常分类打印 | M3/M4 加中断分发 |

### 运行效果（M1 实测）

```
==============================================
  riscvPrj - riscv64 bare-metal OS (M1)
==============================================
machine : QEMU virt (-bios none, M-mode)
cpu     : hartid = 0
...
> abc                          ← 回显
> e                            ← 触发非法指令
[trap] exception: Illegal instruction (cause=2)
  mepc = 0x80000192  mstatus = 0x1800
  [trap] recoverable: skipping illegal instruction (mepc += 4)
[riscvPrj] back from trap (mepc advanced, context intact)
```

---

## 2. 原理：内核是怎么"活"过来的

### 2.1 加电到 C 世界：boot.S 逐行解读

QEMU 把 ELF 按链接地址载入内存（内核在 `0x80000000`），所有 hart
以 **M 模式**从 `0x80000000` 开始执行。逐行看 `_start`：

```asm
_start:
    csrr t0, mhartid          # ① 读"我是第几个核" → 盒子 t0
    bnez t0, .Lpark           #    不是 0 号核 → 跳去停车 (WFI 死循环)
                              #    只有 0 号核继续 —— 多核启动, 单核引导

    la   t0, __bss_start      # ② 清 .bss: t0 = BSS 起点
    la   t1, __bss_end        #    t1 = BSS 终点
1:  bgeu t0, t1, 2f           #    t0 >= t1 (无符号) → 跳出循环
    sd   zero, 0(t0)          #    把 0 写 8 字节到 t0 指向的地址
    addi t0, t0, 8            #    t0 += 8 (指针前进一个 u64)
    j    1b                   #    跳回 1: 再查条件 —— 这就是 while 循环

2:  la   sp, __stack_top      # ③ 设栈指针 sp (没栈不能调 C 函数!)
    la   gp, __global_pointer$  #   设全局指针 gp (小数据寻址锚点)

    la   t0, trap_entry       # ④ 配 trap: mtvec = trap_entry
    csrw mtvec, t0            #    之后任何时刻出异常/中断都能接住
    la   t0, trap_frame
    csrw mscratch, t0         #    mscratch = 现场帧指针 (trap 入口交换用)

    call kernel_main          # ⑤ 跳进 C++ 世界 (extern "C" 符号)

.Lhalt:  wfi; j .Lhalt        # 兜底: kernel_main 若返回则停机
.Lpark:  wfi; j .Lpark        # 非 boot hart 停车
```

**每步的顺序为什么不能乱**：

| 顺序 | 为什么 |
|------|--------|
| ① 先分核 | 两个 hart 都执行这段代码会互相踩（清同一个 BSS、抢同一个栈） |
| ② 清 BSS | 不清的话 C 的全局变量初始值是内存残留垃圾 |
| ③ 设栈 | `call` 是函数调用，返回地址要压栈——没有栈直接崩 |
| ④ 配 trap | 越早越好：万一启动早期就出异常，有地方去 |
| ⑤ 跳 C | 前面全部就绪，才进入"文明世界" |

### 2.2 链接脚本：内核住哪、内存怎么分

`linker.ld`（附录 A）把内核按以下布局排进 `0x80000000` 起的内存：

```
0x80000000  .text    代码 (boot.S 的 .text.boot 排最前)
            .rodata  只读数据 (字符串常量等)
            .data    已初始化全局变量  ← __global_pointer$ 锚点在这
            .bss     未初始化全局变量 (运行时清零)
            . += 0x10000   64KB 栈
0x80010xxx  __stack_top = .
```

**为什么 `.text.boot` 要排最前？** 链接脚本按顺序排放节，
`*(.text.boot)` 写在第一个，保证 `_start`（入口）落在 `0x80000000`——
QEMU 复位后第一条指令必须在那里。

**导出给汇编的 4 个符号**：

| 符号 | 值 | 谁用 |
|------|-----|------|
| `__bss_start` / `__bss_end` | BSS 段边界 | boot.S 清 BSS |
| `__stack_top` | 栈顶 | boot.S 设 sp |
| `__global_pointer$` | data 段中间 + 0x800 | boot.S 设 gp |

**为什么 `__global_pointer$ = . + 0x800`？** gp 相对寻址范围是
`gp ± 2048` 字节。把锚点放在 data 段起点**往后 0x800（2048）**处，
小数据变量无论排在锚点前还是后，都能落在 ±2KB 窗口内（见技巧 3.3）。

### 2.3 UART 16550：怎么把字显示到终端

**内存映射 I/O**：外设寄存器就是内存地址，读写地址 = 操作外设。
QEMU virt 串口在 `0x10000000`（字节寄存器，位级）：

| 偏移 | 寄存器 | 位级含义 |
|------|--------|---------|
| +0 | THR(写)/RBR(读) | 发送/接收字符 |
| +1 | IER | bit0=RX 中断使能（M4 用） |
| +2 | FCR | bit0=0 关 FIFO（轮询模式） |
| +3 | LCR | 0x03 = 8 数据位/无校验/1 停止位 |
| +5 | LSR | bit0=DR 有数据；bit5=THRE 发送器空 |

**轮询发送的核心**：

```cpp
while (!(*reg(REG_LSR) & (1 << 5)))   /* 发送器忙就原地等 */
    ;
*reg(REG_THR) = c;                     /* 空了才写, 字符才发得出去 */
```

**为什么 `'\n'` 要补 `'\r'`？** 终端协议里 CR（回车）才回行首，
LF（换行）只下移一行。串口只认 CR，所以 `uart_putc('\n')` 先发 `\r`。

### 2.4 printf：变参与进制转换

**变参怎么工作**（体系结构视角）：
- 调用者按 ABI 把参数放进 `a0-a7`（前 8 个）和栈（更多时）
- 被调用者用 `__builtin_va_arg(ap, int)` 按类型大小**逐个取**
- 我们的实现不依赖 libc，直接用编译器内建的 `__builtin_va_*`

**数字转字符串的核心**（`print_unsigned`）：

```
对 num 循环:  digit = num % base   → 从"个位"开始取
              num   = num / base
存进 buf(倒序) → 最后反着打出来      → 得到正序字符串
```

例：`0xABCDEF` 转 16 进制：取 `F`、`E`、`D`…… 倒着打 = `ABCDEF`。
`%p` = 先打 `0x` 再调同一个函数打 64 位地址。

### 2.5 trap 现场：非法指令怎么被"抓住"（位级）

硬件检测到非法指令时自动：

```
mcause ← 2 (异常: 非法指令)
mepc   ← 出错指令的地址
PC     ← mtvec (trap_entry)
```

`trap_entry` 把现场存进 `trap_frame`——**31 个 GPR + 4 个 CSR**，
布局与 `TrapFrame` 结构严格对应：

| 帧偏移 | 内容 | | 帧偏移 | 内容 |
|--------|------|-|--------|------|
| 0×8 | ra (x1) | | 16×8 | a7 (x17) |
| 1×8 | sp (x2) | | 17×8 | s2 (x18) |
| 2×8 | gp (x3) | | … | … |
| 3×8 | tp (x4) | | 30×8 | t6 (x31) |
| 4×8 | t0 (x5) | | 31×8 | mepc |
| 5×8 | t1 (x6) | | 32×8 | mcause |
| … | … | | 33×8 | mtval |
| 9×8 | a0 (x10) | | 34×8 | mstatus |

处理函数打印 → `f->mepc += 4` 跳过非法指令 → 恢复现场 → `mret`
回到出错指令的**下一条**继续执行（完整机制见 trap-csrs.md）。

---

## 3. 技巧：两个大坑 + 细节

### 3.1 坑 1：`la` 被链接器"偷懒优化"坑了（gp 松弛）

**反汇编证据**（当时 `_start` 的真实样子）：

```asm
# 我写的:                        # 反汇编出来的:
la t0, __bss_start               auipc t0, 0x1          ← 正常 2 条
                                 addi  t0, t0, -744
la t1, __bss_end                 addi  t1, gp, -1760    ← 被偷懒成 1 条!
la gp, __global_pointer$         mv    gp, gp           ← 变成 no-op!
```

**为什么**：RISC-V 链接器默认做 **relaxation（松弛）**——`la` 的目标
符号若落在 `gp ± 2KB` 内，可省成一条 `addi rd, gp, offset`。代价是
**要求 gp 已初始化**。而启动时 gp 还是垃圾值 → t1 拿到错误地址 →
清 BSS 循环的结束条件永远不满足 → **死循环，什么都不打印**。

连 `la gp, __global_pointer$` 也被松弛成 `mv gp, gp`：链接器知道
"gp 的目标就是 gp 自己 + 0"，于是认为不需要干活——gp 永远是垃圾值。

**修复**（boot.S 第一行）：

```asm
.option norelax    # 关闭松弛: 启动代码的 la 一律 auipc + addi, 不依赖 gp
```

**要点**：凡是在 gp 初始化**之前**执行的汇编（boot 代码、trap 入口
早期），要么 `norelax`，要么先设好 gp——所有 RISC-V 裸机工程的共同规矩。
C 代码里 gp 已初始化，可以放心用松弛（小数据访问更快、代码更小）。

### 3.2 坑 2：trap 返回忘了把 mepc 写回 CSR

**现象**：非法指令 trap 打印现场正确，`f->mepc += 4` 也执行了，
但 `mret` 后**又回到同一条非法指令**，无限 trap 刷屏。

**原因**（用 M3 学的 trap 流程讲）：

```
进 trap:  硬件把 mepc 存进 CSR (导航仪)
trap.S:   mepc 拷进 trap_frame (笔记本)
handler:  在笔记本上改 mepc += 4
mret:     用的是【导航仪里的 mepc】, 不是笔记本上的!  ← 没同步
```

**修复**（trap.S，handler 返回后、恢复 GPR 前）：

```asm
ld   t1, 31*8(t0)     # 从笔记本读 handler 改过的 mepc
csrw mepc, t1         # 写回导航仪 (mret 用这个)
ld   t1, 34*8(t0)     # mstatus 同理 (中断开关状态也要同步)
csrw mstatus, t1
```

**要点**：trap 入口完整流程 = 保存现场 → 调 C handler →
**把 handler 改过的 CSR 写回** → 恢复 GPR → `mret`。少了"写回"，
任何"改 mepc 恢复执行"的机制（异常恢复、任务切换、系统调用返回）
都会失效——这是 RISC-V trap 编程的第一大坑。

### 3.3 gp 相对寻址的机制（坑 1 的深层理解）

```
数据段:  [.........__global_pointer$....小数据....]
                  ↑ ( = .data起点 + 0x800 )
                  └─ gp 指向这里
小数据变量 (≤8 字节, 走 .sdata) 用 addi rd, gp, 偏移 访问
范围: gp ± 2048 字节 → 锚点放中间, 前后都能覆盖
```

这就是 `__global_pointer$ = . + 0x800` 的来历——**锚点放在数据段中间**，
让尽可能多的小数据落在 gp 的 ±2KB 窗口里。

### 3.4 其他细节

- **volatile MMIO**：`reinterpret_cast<volatile u8*>(地址)`——告诉编译器
  "这个地址随时会变，读写别优化掉"（不加 volatile，`while(!(*p))` 可能
  被优化成只读一次）
- **mtvec 4 字节对齐**：直接模式下 mtvec 低 2 位是 MODE 字段（00），
  所以 `trap_entry` 必须 `.align 2`（见 trap-csrs.md §3.1）
- **非 boot hart 停车**：`bnez t0, .Lpark` + WFI——为多核启动留好位置
  （M7 之后真正唤醒它们）
- **64KB 内核栈**：链接脚本 `. += 0x10000`——启动早期、trap 处理、
  printf 递归（uart_putc 的 `\n` 分支）都在这一个栈上，给足余量

---

## 4. 怎么验证（对照实验）

```bash
# 实验 1: 正常启动
make run
# 预期: 横幅 + 回显 + e 触发 trap 恢复 + Ctrl+C 停机

# 实验 2: 复现坑 1 (去掉 .option norelax, 重新 make clean && make)
#   预期: 内核无任何输出, 卡死 —— 清 BSS 死循环
#   诊断: objdump -d build/riscvPrj.elf | head -20 看 la 是否变 gp 相对

# 实验 3: 复现坑 2 (注释掉 trap.S 里两行 csrw mepc/mstatus)
#   预期: 按 e 后 trap 现场无限刷屏
#   诊断: 对比 mepc 一直不变 (0x80000192), 说明 mret 总回同一条指令

# 实验 4: 看入口反汇编 (学指令编码)
make disasm && sed -n '1,40p' build/disasm.txt
```

---

## 5. 下一步

- **M3（已完成）**：CLINT 定时器——trap 机制的第一次实战，见 `docs/m3-notes.md`
- **M2**：异常分类完善（断点 ebreak / 访存错误单独处理）——trap_handler
  的分支从"非法指令特判"变成完整分发表
- **M4（已完成）**：PLIC 外设中断，见 `docs/m4-notes.md`

---

## 附录 A：linker.ld（全文）

```ld
OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;                  /* QEMU virt DRAM 起始地址 */

    . = ALIGN(16);
    .text : {
        *(.text.boot)                /* boot.S 代码排最前 (入口必须在这) */
        *(.text*)
    }

    . = ALIGN(16);
    .rodata : { *(.rodata*) }

    . = ALIGN(16);
    .data : {
        __global_pointer$ = . + 0x800;  /* gp 锚点: 数据段中间 (3.3) */
        *(.data*)
    }

    . = ALIGN(16);
    .bss : {
        __bss_start = .;             /* 给 boot.S 清 BSS 用 */
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    . = . + 0x10000;                 /* 64KB 内核栈 */
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
```

## 附录 B：arch/boot.S（全文逐行注释）

```asm
/* 启动汇编: 所有 hart 从 0x80000000 以 M 模式启动 */
.option norelax                   /* 坑 1 修复: 关掉 gp 松弛 */

.section .text.boot
.globl _start
_start:
    csrr t0, mhartid              /* ① 读 hart id */
    bnez t0, .Lpark               /*    非 0 号 hart: 停车 */

    /* ② 清零 .bss */
    la   t0, __bss_start
    la   t1, __bss_end
1:  bgeu t0, t1, 2f               /*    t0 >= t1 则跳出 */
    sd   zero, 0(t0)              /*    写 8 字节 0 */
    addi t0, t0, 8
    j    1b

    /* ③ 设栈 + gp */
2:  la   sp, __stack_top
    la   gp, __global_pointer$

    /* ④ 配 trap */
    la   t0, trap_entry
    csrw mtvec, t0                /*    异常/中断都进 trap_entry */
    la   t0, trap_frame
    csrw mscratch, t0             /*    trap 入口交换用 */

    /* ⑤ 跳进 C 世界 */
    call kernel_main

.Lhalt:                           /* 兜底停机 */
    wfi
    j    .Lhalt

.Lpark:                           /* 非 boot hart 停车 */
    wfi
    j    .Lpark
```

## 附录 C：arch/trap.S（全文注释）

```asm
/* M 模式 trap 入口 (直接模式, mtvec 低 2 位 = 0) */
.section .text
.globl trap_entry
.align 2                            /* 4 字节对齐: mtvec MODE=00 */

trap_entry:
    /* ---- 保存现场 ---- */
    csrrw t0, mscratch, t0          /* t0 = &trap_frame, mscratch = 原 t0 */
    sd x1,  0*8(t0)                 /* ra */
    sd x2,  1*8(t0)                 /* sp */
    sd x3,  2*8(t0)                 /* gp */
    sd x4,  3*8(t0)                 /* tp */
    /* x5 (t0) 原值在 mscratch, 稍后补存 */
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
    csrr t1, mscratch
    sd   t1, 4*8(t0)                /* 原 t0 补存 */
    csrr t1, mepc;    sd t1, 31*8(t0)   /* 4 个 CSR 入帧 */
    csrr t1, mcause;  sd t1, 32*8(t0)
    csrr t1, mtval;   sd t1, 33*8(t0)
    csrr t1, mstatus; sd t1, 34*8(t0)

    /* ---- 调 C 处理函数 ---- */
    mv   a0, t0                     /* 参数: TrapFrame* */
    call trap_handler

    /* ---- 坑 2 修复: handler 改过的 CSR 写回 ---- */
    ld   t1, 31*8(t0); csrw mepc, t1
    ld   t1, 34*8(t0); csrw mstatus, t1

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

    /* 反向交换: 原 t0 回 t0, mscratch 复原 */
    csrrw t0, mscratch, t0
    mret

.section .bss
.align 3
.globl trap_frame
trap_frame:
    .skip 35 * 8                    /* 31 GPR + 4 CSR */
```

## 附录 D：drivers/uart.cpp（M1 版全文）

```cpp
#include "drivers/uart.hpp"

constexpr u64 UART_BASE = 0x10000000;   /* QEMU virt 串口基址 */

constexpr u64 REG_THR = 0x00;   /* 发送 (写) */
constexpr u64 REG_RBR = 0x00;   /* 接收 (读) */
constexpr u64 REG_IER = 0x01;   /* 中断使能 */
constexpr u64 REG_FCR = 0x02;   /* FIFO 控制 */
constexpr u64 REG_LCR = 0x03;   /* 线路控制 */
constexpr u64 REG_LSR = 0x05;   /* 线路状态 */

static inline volatile u8* reg(u64 offset) {
    return reinterpret_cast<volatile u8*>(UART_BASE + offset);
}

void uart_init() {
    *reg(REG_LCR) = 0x03;   /* 8N1 */
    *reg(REG_FCR) = 0x00;   /* 关 FIFO (轮询模式) */
    *reg(REG_IER) = 0x00;   /* 先关中断 (M4 再开) */
}

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');       /* 换行先补回车 */
    while (!(*reg(REG_LSR) & (1 << 5)))   /* 轮询 THRE */
        ;
    *reg(REG_THR) = static_cast<u8>(c);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

char uart_getc() {
    while (!(*reg(REG_LSR) & 0x01))       /* 轮询 DR */
        ;
    return static_cast<char>(*reg(REG_RBR));
}

int uart_getc_nonblock() {
    if (!(*reg(REG_LSR) & 0x01)) return -1;
    return *reg(REG_RBR) & 0xFF;
}
```

## 附录 E：lib/printf.cpp（核心部分）

```cpp
#include "lib/printf.hpp"
#include "drivers/uart.hpp"

static constexpr u64 INT64_MIN_ABS = (u64)1 << 63;

/* 数字 → 字符串: 从个位取, 倒序存, 反着打 */
static void print_unsigned(u64 num, u32 base, bool upper) {
    char buf[32];
    int i = 0;
    do {
        u64 digit = num % base;
        if (digit < 10)      buf[i++] = static_cast<char>('0' + digit);
        else                 buf[i++] = upper ? static_cast<char>('A' + digit - 10)
                                              : static_cast<char>('a' + digit - 10);
        num /= base;
    } while (num > 0);
    while (--i >= 0) uart_putc(buf[i]);
}

void printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') { uart_putc(*fmt++); continue; }
        fmt++;
        switch (*fmt) {
            case 'd': case 'i': {
                int val = __builtin_va_arg(ap, int);
                if (val < 0) { uart_putc('-'); val = -val; }
                print_unsigned((u64)val, 10, false);
                break;
            }
            case 'u': {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                print_unsigned(val, 10, false);
                break;
            }
            case 'x': case 'X': {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                print_unsigned(val, 16, *fmt == 'X');
                break;
            }
            case 'c': {
                int val = __builtin_va_arg(ap, int);
                uart_putc(static_cast<char>(val));
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                while (*s) uart_putc(*s++);
                break;
            }
            case 'p': {
                void* p = __builtin_va_arg(ap, void*);
                uart_puts("0x");
                print_unsigned(reinterpret_cast<u64>(p), 16, false);
                break;
            }
            default: { uart_putc('%'); uart_putc(*fmt); break; }
        }
        fmt++;
    }
    __builtin_va_end(ap);
}
```

## 附录 F：kernel/main.cpp（M1 版核心）

```cpp
extern "C" void kernel_main() {
    uart_init();
    printf("... 横幅 ...\n> ");

    for (;;) {
        char c = uart_getc();            /* 阻塞读 (M1 版) */
        if (c == '\r')      { uart_putc('\n'); printf("> "); }
        else if (c == 0x03) { printf("\nbye, halting\n");
                              for (;;) asm volatile("wfi"); }
        else if (c == 'e')  { printf("\ntriggering illegal instruction...\n");
                              asm volatile(".word 0xffffffff");
                              printf("back from trap\n> "); }
        else                { uart_putc(c); }
    }
}
```
