# riscvPrj 项目从头到尾逐行讲读报告

> 目的：把 riscvPrj 项目从第一行代码讲到最后，边讲代码边讲 RISC-V 架构。
> 读法：按顺序，一关一关过。每关 = 一段真实代码 + 逐行人话解释 + 涉及
> 的 RISC-V 概念就地讲清。不跳关。
> 术语从简：寄存器=CPU 内部的"存东西的小格子"，内存=外面的大仓库。
> 写这份报告时内核在 S 模式（M5 之后版本），但讲解按代码执行顺序走，
> M4→M5 的差异在最后一章专门讲。

---

## 第一章：内核文件是怎么变成"能跑的东西"的

### 1.1 你写的东西最终是三种文件

```
你的源代码                      工具链处理                产物
─────────────────────────    ──────────────────    ───────────────
arch/boot.S  arch/trap.S  →  gcc 编译(汇编)   →  build/arch/*.o
kernel/*.cpp drivers/*.cpp →  g++ 编译        →  build/kernel/*.o
lib/*.cpp                   (编译=把 C++ 翻成汇编,
                            再翻成机器码字节)
                                        │
                                        ▼
       linker.ld (链接脚本) ──► 链接器把碎片拼成整体
                                        │
                                        ▼
                              build/riscvPrj.elf  ← 最终内核
```

`riscvPrj.elf` 是一个 **ELF 文件** —— 一种"带说明书"的二进制格式：
里面装着机器码（.text）、只读数据（.rodata）、变量（.data/.bss），
还有一张"段表"告诉加载器：每块内容应该放到内存的哪个地址。

### 1.2 linker.ld：告诉链接器"拼好后放哪、谁在前"

打开 `linker.ld` 逐行看（这是理解一切的起点）：

```ld
OUTPUT_FORMAT(elf64-littleriscv)   /* 产物格式: 64 位小端 RISC-V ELF */
OUTPUT_ARCH(riscv)                 /* 目标架构 */
ENTRY(_start)                      /* ★入口: 告诉加载器/调试器从哪开始执行 */

SECTIONS
{
    . = 0x80200000;          /* ★当前位置指针 = 0x80200000 (内存门牌) */

    . = ALIGN(16);           /* 后续内容按 16 字节对齐 */
    .text : {                /* "代码段": 所有机器码放这里 */
        *(.text.boot)        /* 先放 boot.S 的启动代码 */
        *(.text*)            /* 再放其余所有函数的代码 */
    }

    .rodata : { *(.rodata*) }   /* 只读数据: 字符串常量等 */

    .data : {
        __global_pointer$ = . + 0x800;  /* 记一个锚点位置 */
        *(.data*)            /* 已初始化的全局变量 */
    }

    .bss : {                 /* 未初始化全局变量(初值全是0) */
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = . + 0x10000;         /* 再往后留 64KB 当内核栈 */
    __stack_top = .;         /* 栈顶地址记成符号 */

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }  /* 调试垃圾丢弃 */
}
```

三个概念必须搞懂（这是 RISC-V/链接的通用知识）：

**① 为什么起始地址是 0x80200000？**
内存仓库从门牌 0x80000000 开始（QEMU virt 机器的 DRAM 起点）。
M5 之前内核直接住 0x80000000；M5 之后 OpenSBI 固件占了前 2MB
(0x80000000~0x80200000)，内核搬到 0x80200000。2MB 间隔是约定俗成
的"固件之后第一块"。
（你学 RISC-V 要知道的重点：**地址是内存的门牌号，链接脚本决定
你代码里每个符号住几号门牌。**）

**② 段（section）是什么？**
`.text` = 机器码，`.rodata` = 只读常量，`.data` = 有初值的变量，
`.bss` = 没初值的变量（ELF 里不占空间，运行时才存在）。
把代码、常量、变量分开放是 ELF/操作系统普遍规矩。

**③ `__bss_start`/`__bss_end`/`__stack_top` 这些符号哪来的？**
链接器在"拼图"时记下的位置标记，`boot.S` 里用 `la` 指令直接引用。
**链接脚本 = 给汇编/C 代码提供"关键门牌号"的途径。**

### 1.3 Makefile：编译参数里就有 RISC-V 知识

```make
CXX     = riscv64-unknown-elf-g++    /* 交叉编译器: 目标不是 x86 而是 riscv64 */
ARCH    = rv64gc
ABI     = lp64
CXXFLAGS = -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany ...
```

**`-march=rv64gc` 拆开就是一门课**：

| 片段 | 含义 |
|------|------|
| rv64 | 64 位基础指令集 RV64I |
| g | **通用扩展缩写** = IMAFD + 若干小项 |
| I | 整数指令（加减、加载、存储、跳转、移位） |
| M | 乘除指令 mul/div |
| A | 原子指令 amoswap（boot.S 里用过！） |
| F/D | 单/双精度浮点 |
| C | 压缩指令（16 位短指令，比如 c.ebreak） |

**RISC-V 最核心的架构思想在这里**：它不卖"一款 CPU"，而卖"一份
基础指令集 + 可选扩展清单"。`rv64gc` 就是"我要这套配置"。
Linux 的 riscv64 发行版全是 rv64gc —— 所以我们这个裸机内核用同一套
参数编译，将来跑 Linux 兼容。

---

## 第二章：arch/boot.S —— 上电后 CPU 执行的第一段代码

### 2.1 前提：上电后世界是什么样

QEMU 启动 → 所有 hart（CPU 核）从复位向量开始 → OpenSBI（M5 起）
完成初始化后把**每个 hart**都带到我们的 `_start`，进入时约定：

- `a0` 寄存器 = 本 hart 的编号（hartid）
- `a1` 寄存器 = 设备树 dtb 地址
- 机器此时处于 S 模式（M5 起；M5 前是 M 模式）

boot.S 干的事（按顺序，一步步来）：

### 2.2 逐行讲解

```asm
.option norelax          /* ★让链接器别做"松弛优化", 见下方专题 */
.section .text.boot      /* 这段代码放进 .text.boot 段(链接脚本保证它在最前) */
.globl _start            /* 导出符号, 链接器 ENTRY(_start) 用它 */
_start:
```

```asm
    la   t0, boot_flag   /* t0 = boot_flag 的门牌号 (la=load address) */
    li   t1, 1           /* t1 = 1 (li=load immediate, 立即数装入) */
    amoswap.w.aqrl t2, t1, (t0)  /* 原子交换: 把 boot_flag 和 1 互换,
                                     旧值进 t2 */
    bnez t2, .Lpark      /* t2 != 0 → 已经有人捷足先登, 本 hart 去休息 */
```

**这段是"多 hart 抢内核"的锁**。QEMU 配了 2 个 hart（-smp 2），
OpenSBI 会把两个都送到 `_start`。若两个 hart 都往下走，会互相踩踏
（比如同时清 BSS、共用一块栈）。所以：

1. `amoswap.w` = "原子交换一个 32 位字"：一条指令完成
   [读旧值 → 写新值]，期间别的 hart 插不进来（A 扩展）。
2. 第一个到的 hart：boot_flag 原是 0 → t2=0 → 不跳，继续往下；
   后到的 hart：t2=1 → bnez 成立 → 跳去 `.Lpark` 永远 `wfi` 睡觉。
3. `.aqrl` 后缀是内存序控制（acquire/release），多核场景防止
   读写乱序。现在记住"这是保证原子性的完整写法"即可。

```asm
    la   t0, __bss_start /* 清 BSS 开始门牌 */
    la   t1, __bss_end   /* 清 BSS 结束门牌 */
1:  bgeu t0, t1, 2f      /* 若 t0 >= t1 说明清完, 跳到标签 2f */
    sd   zero, 0(t0)     /* 往 t0 处写 8 字节 0 (sd=store doubleword) */
    addi t0, t0, 8       /* 门牌前进 8 字节 */
    j    1b              /* 回到标签 1b 继续循环 */
2:
```

**这段是"清 BSS"** —— 链接脚本里 `.bss` 段装的是"未初始化全局变量"
（比如 trap 的现场缓冲区 `trap_frame`）。这些变量不占 ELF 文件空间，
但运行时必须存在且初值为 0 —— 谁来把内存写成 0？没人，必须 CPU 自己
动手。这就是清 BSS：把 `__bss_start` 到 `__bss_end` 之间全部写 0。

**RISC-V 指令要点（顺便认识四条指令）**：

| 指令 | 全称含义 | 人话 |
|------|----------|------|
| `bgeu t0,t1,2f` | branch if ≥ unsigned | 无符号比较 t0≥t1 就跳。**b=分支=有条件跳** |
| `sd zero, 0(t0)` | store doubleword | 把 8 字节写进内存门牌 t0+0。**只有 ld/sd 这类指令能碰内存** |
| `addi t0,t0,8` | add immediate | t0 = t0 + 8。**i 后缀=带立即数(常量)** |
| `j 1b` | jump | 无条件跳。`1b`=往前找标签 1，`2f`=往后找标签 2 |

```asm
    la   sp, __stack_top    /* sp = 栈顶 (sp=x2, 盒子2, 专职栈指针) */
    la   gp, __global_pointer$
```

**为什么 sp 必须现在立好？** 因为下一行 `call kernel_main` 会往栈上
压返回地址、函数里局部变量也要用栈。栈 = 仓库里从高往低长的一块区域，
sp 盒子记"当前栈顶在哪"。**任何函数调用之前必须先把 sp 指到安全区域**，
否则 call 一压栈就写到未知内存 → 死机。

gp（x3）= 全局指针：编译器用它访问小范围的全局变量（±2KB 内用
一条指令搞定，不用两条）。`__global_pointer$` 是链接脚本里那个
`.+0x800` 锚点。

```asm
    la   t0, trap_entry
    csrw stvec, t0        /* ★把 trap_entry 的门牌写进 stvec 寄存器 */
    la   t0, trap_frame
    csrw sscratch, t0     /* ★把现场缓冲区门牌写进 sscratch */
```

**这里是 RISC-V 特权架构的第一个关键点 —— CSR 与 stvec**：

- `stvec`（Supervisor Trap Vector Base）是一个 **CSR**：控制状态寄存器。
  它和 x0-x31 通用格子不同：专门管 CPU 的"控制功能"，要用专用指令
  `csrr`（读）/`csrw`（写）访问。
- stvec 存的是"**门铃响起时 CPU 跳去哪**"的地址。现在填好
  `trap_entry`，将来任何中断/异常发生，CPU 自动跳到 trap_entry 开始跑。

`sscratch` 也是 CSR，存的是一块内存缓冲区的地址（trap_frame），
trap 入口用它保存现场（第 5 章细讲）。先记住：**进 trap 前先把
两样东西交给硬件 —— 处理门铃的代码地址(stvec)、保存现场用的
仓库地址(sscratch)**。

```asm
    call kernel_main       /* 调用 C++ 主函数 (a0/a1 原样带过去) */
.Lhalt:
    wfi                    /* wait for interrupt: 睡到下一个中断 */
    j    .Lhalt            /* 万一 kernel_main 返回(不该发生), 死循环 */
.Lpark:
    wfi
    j    .Lpark
```

**`call kernel_main` = 汇编喊话 C++ 世界**。`kernel_main` 是 C++
函数，`call` 指令先往 `ra`（x1）写返回地址，再跳进函数。
从那以后就是 C++ 代码接管 —— 汇编的使命到此完成（大约 25 行）。

### 2.3 专题：`.option norelax` —— 曾经杀死过项目的坑

```asm
.option norelax
```

没有这行的话，`la t0, __bss_start` 这种"取符号地址"的指令会被链接器
**松弛优化**成 gp 相对寻址：`addi t0, gp, 偏移`。gp 此刻还没初始化
（下一行才 `la gp, ...`），于是 t0 拿到一个垃圾地址 → 清 BSS 清到
未知内存 → 死机或跑飞。

**这行的本质**：告诉链接器"启动早期 gp 不可用，别做这个优化"。
链接器优化看着无害（少一条指令），但破坏了"先立 gp 再用"的顺序。
这种"工具链聪明反被聪明误"的坑，是嵌入式起步的第一课。

---

## 第三章：kernel/main.cpp —— C++ 世界开门

### 3.1 从"入口函数"看函数怎么被调用

```cpp
extern "C" void kernel_main(u64 hartid, u64 dtb) {
```

`extern "C"` = 告诉 g++ 别给函数名做 C++ 改名（mangling），这样
`call kernel_main` 在链接时能找到它。boot.S 用 a0/a1 递进来的
hartid/dtb 在这里被当成**前两个参数**接住 —— 还记得汇编调用约定吗：
参数放 a0 起。**boot.S 和 kernel_main 之间没有"特殊机制"，
就是一次普通函数调用**，只是调用者用汇编写的。

### 3.2 初始化顺序有讲究

```cpp
    uart_init();           /* 先让"对讲机"能说话 */
    plic_init((u32)hartid);/* 配门铃总机(第7章) */
    uart_enable_rx_irq();  /* 打开收字符中断 */
    timer_init();          /* 配闹钟, 开全局中断总闸 */
```

顺序逻辑：所有硬件设备配置完，**最后**才开全局中断 —— 避免设备
刚配一半就来中断。开总闸在 `timer_init()` 里（sstatus.SIE），
见第 6 章。

### 3.3 主循环：一个裸机内核的全部"业务"

```cpp
    for (;;) {
        int c = uart_rx_pop();      /* 有字符吗?(中断后台塞进环形缓冲) */
        if (c >= 0) { ... 处理命令 a/b/e/i/s ... }
        if (uptime_ms() - last_print >= 1000) {   /* 每1秒 */
            printf("[tick] uptime = %u ms\n", ...);
        }
    }
```

裸机内核（没有操作系统概念时）就是**一个死循环**：
1. 有没有输入 → 处理
2. 时间到没 → 报 tick
3. 回到 1
没有"退出"，内核跑完启动就永不结束。这个循环结构你要记住：
**将来写调度器（M7），只是在这个循环里插"切换任务"而已。**

### 3.4 UART：怎么让一个字节出现在屏幕上

`drivers/uart.cpp` —— 访问的是 MMIO（内存映射外设），前面第 0 课
讲过：设备被投影到内存门牌上，用普通读写指令就能操作。

```cpp
constexpr u64 UART_BASE = 0x10000000;   /* UART 设备门牌区起点 */
constexpr u64 REG_THR = 0x00;   /* 发送寄存器(写=把字节发出去) */
constexpr u64 REG_LSR = 0x05;   /* 线路状态寄存器(读状态) */

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');          /* 终端要 \r\n 才换行 */
    while (!(*reg(REG_LSR) & (1 << 5)))       /* LSR bit5 = "发送口空了没" */
        ;                                     /* 没空就死等(轮询) */
    *reg(REG_THR) = static_cast<u8>(c);       /* 往发送口写字节 → 屏幕出字 */
}
```

**为什么发送要"等"**：UART 是慢速设备（每字节要时间在线上移位送出），
发送口一次只能装一个字节。bit5=1 表示"口空了，可以写下一个"。
这是最朴素的**轮询**（polling）方式：一直看状态位。

**volatile 关键字**：`reg()` 返回 `volatile u8*`。告诉编译器
"这个地址每次都要真的去读/写，别优化掉"。因为设备门牌不像普通内存，
读它可能有副作用（比如读接收口会把字节取走）。

### 3.5 printf 怎么工作（lib/printf.cpp 思想）

`printf("%d", n)` 内部 = 把数字 n 转成字符 '1''2''3'，然后对每个字符
调用 `uart_putc`。没有操作系统、没有库函数 —— 一切从零写。
mini printf 只支持 `%d %u %x %X %c %s %p`，所以写 `%02X` 会原样
打印（还记得 M5 被它坑过吗）。

---

## 第四章：arch/trap.S —— 门铃响起的处理入口（项目灵魂）

### 4.1 先搞清楚：trap 是什么，硬件自动做了什么

**trap（门铃）= 中断 + 异常的统称**。RISC-V 手册规定，任何 trap 发生时，
**硬件自动**做这些事（其他什么都不做）：

1. 把当前特权级存进 mstatus 的 MPP/SPP 位（记下"我是从哪层被打断的"）
2. 关闭中断（把 MIE/SIE 清零 —— 防止处理中又被打断）
3. 把"被打断时执行到哪条指令"的地址写进 mepc/sepc
4. 把"为什么被打断"的原因码写进 mcause
5. **跳转到 stvec/mtvec 存的地址**（就是我们 boot.S 里填的 trap_entry）

**关键**：硬件只管这 5 件事。**被打断现场的 31 个通用寄存器怎么保存，
硬件一概不管** —— 全是我们的事。因为 trap 是"单方面闯入"（见第一章
讲过的观点），处理代码和被打断代码素不相识，没有约定可用，所以
**最保险的做法：31 个寄存器全部保存，处理完全部恢复**。

### 4.2 trap_frame：现场保存区的布局

trap.S 末尾声明了保存区：

```asm
.section .bss
.align 3
.globl trap_frame
trap_frame:
    .skip 35 * 8        /* 35 个 8 字节 = 31 个寄存器 + 4 个 CSR */
```

35 个字 = 31 个通用寄存器(x1..x31) + sepc + scause + stval + sstatus。
跳过 x0？x0 恒为 0，不用存（存了也是 0）。

### 4.3 核心技巧：sscratch 与 t0 的一次交换

```asm
trap_entry:
    csrrw t0, sscratch, t0
```

这是全项目最精妙的一条指令，拆开看：

- `csrrw rd, csr, rs`：原子地 [rd = csr旧值; csr = rs]。
- 执行后：**t0 = 原来的 sscratch = trap_frame 地址**（boot.S 里填的）；
  **sscratch = 被打断现场的 t0 原值**（暂时寄存在抽屉里）。
- 从此 t0 变成"指向保存区的指针"，其余 30 个寄存器原封不动。

问题：为什么不直接用一个固定寄存器当帧指针？因为 trap 闯入时
**所有寄存器都是被打断现场的财产**，不能随便挑一个占用 ——
唯一可以动的只有 x0（恒零，没有信息）和通过 sscratch 换来用的 t0。
csrrw 交换后 t0 原值存在 sscratch，没丢 —— 处理完再换回去。

### 4.4 保存 31 个寄存器 + 4 个 CSR

```asm
    sd x1,  0*8(t0)     /* ra  存到 帧指针+0 */
    sd x2,  1*8(t0)     /* sp */
    ...
    sd x31, 30*8(t0)    /* t6 */
    csrr t1, sscratch
    sd   t1, 4*8(t0)    /* ★补存 t0 的原值(刚才寄存在 sscratch 里) */
    csrr t1, sepc       /* 读"被打断位置" */
    sd   t1, 31*8(t0)
    csrr t1, scause     /* 读"原因码" */
    sd   t1, 32*8(t0)
    csrr t1, stval      /* 读"附加信息"(如出错地址) */
    sd   t1, 33*8(t0)
    csrr t1, sstatus    /* 读"被打断前状态" */
    sd   t1, 34*8(t0)
```

注意顺序：x1,x2,x3,x4 先存，x5(t0) 的原值从 sscratch 补回 ——
因为 t0 正忙着当"帧指针"，它的旧值在交换时被塞进了 sscratch。

### 4.5 调用 C 处理函数并返回

```asm
    mv   a0, t0             /* 参数1 = TrapFrame* (指向保存区) */
    call trap_handler       /* 进 C++ 世界处理(下一章) */

    ld   t1, 31*8(t0)       /* 把可能被改过的 sepc 读回来 */
    csrw sepc, t1           /* 写回硬件 —— handler 可能改了它(如跳过坏指令) */
    ld   t1, 34*8(t0)
    csrw sstatus, t1
    /* 恢复 31 个寄存器(顺序与保存对称) */
    ld x1, 0*8(t0) ... ld x31, 30*8(t0)
    csrrw t0, sscratch, t0  /* 反向交换: t0=原值, sscratch=帧指针(下次用) */
    sret                    /* ★返回被打断的地方 */
```

两个要点：

1. **为什么 sepc/sstatus 要写回**：C 层 handler 可能修改保存在
   trap_frame 里的 sepc（例如"跳过非法指令"= sepc+=4）。修改只写在
   仓库里，必须 `csrw` 写回真实 CSR，`sret` 才会跳到新位置。
2. **sret 是什么**：恢复执行的特权指令 —— CPU 从 CSR 里拿回被打断
   时的位置（sepc）和状态，继续跑。sret 是"门铃处理完回家"的指令，
   与 mret 对应（S 模式用 sret，M 模式用 mret）。

---

## 第五章：kernel/trap.cpp —— 门铃响了，处理什么

### 5.1 读现场：是中断还是异常？

```cpp
extern "C" void trap_handler(TrapFrame* f) {
    bool is_interrupt = (f->scause >> 63) != 0;   /* scause 最高位 */
    u64 cause = f->scause & 0x7FFFFFFFFFFFFFFFULL; /* 去掉最高位 */
```

**scause 的编码规则**（RISC-V 手册）：

```
scause = 0x8000000000000005 这样的数
         最高位(bit63)=1 → 是"中断"(异步,设备喊的)
         最高位=0        → 是"异常"(同步,自己踩坑)
         低12位 = 具体原因码
```

| 原因码 | 类型 | 人话 |
|--------|------|------|
| 2 | 异常 | 非法指令（CPU 不认识这条指令） |
| 3 | 异常 | 断点（执行了 ebreak） |
| 9 | 中断 | S 模式外设中断（PLIC 门铃，UART 字符来了） |
| 5 | 中断 | S 模式定时器中断（闹钟到点） |

### 5.2 中断处理：定时器和外设

```cpp
    if (is_interrupt) {
        if (cause == 5) {            /* 定时器 */
            timer_handler();
            return;
        }
        if (cause == 9) {            /* 外设中断: 查 PLIC 是谁喊的 */
            u32 source = plic_claim();          /* 问总机: 谁? */
            if (source == PLIC_UART_SOURCE)     /* 10 = UART */
                uart_rx_irq_handler();          /* 收字符进环形缓冲 */
            if (source != 0) plic_complete(source); /* 答: 处理完 */
            return;
        }
```

注意中断处理里**只做必要的事、快速返回**，不 printf —— 防止
重入和长时间占着中断上下文。

### 5.3 异常分类：三档处理（M2 的核心成果）

```cpp
    /* 打印现场... */
    switch (cause) {
        case 2:  /* 非法指令: 跳过它继续跑 */
            f->sepc += instr_len((void*)f->sepc);
            return;
        case 3:  /* 断点 ebreak: 同样跳过 */
            f->sepc += instr_len((void*)f->sepc);
            return;
    }
    if (cause == 8 || cause == 9) { /* ecall: 跳过 */
        f->sepc += 4;
        return;
    }
    /* 其余: 不可恢复, 打印后停机 */
    for (;;) asm volatile("wfi");
```

**为什么 sepc+len 就能"跳过"**：异常发生时 sepc 停在**出问题的指令**
上。`instr_len` 判断这条指令多长（RISC-V 指令可能是 2 字节压缩指令
或 4 字节普通指令），把 sepc 往后挪一条，sret 回来就直接执行下一条
指令 —— 程序"假装"没发生过。这是调试器、模拟器的常见戏法。

---

## 第六章：闹钟 —— 定时器与"两层中断开关"（M3）

### 6.1 CLINT 与 mtime

QEMU virt 机器内置 CLINT（Core Local Interruptor），提供：

- `mtime`：一个一直在+1 的 64 位计数器（10MHz → 每 1ms 加 10000）
- `mtimecmp`：比较值寄存器。**mtime ≥ mtimecmp 时触发定时器中断**

M 模式时代直接读 MMIO；S 模式时代（M5）用 `rdtime` 指令读
（time CSR 是 mtime 的 S 模式视图）。

### 6.2 定时器的"排下一个闹钟"模式

```cpp
void timer_handler() {
    sbi_set_timer(read_time() + TICKS_PER_MS);  /* 排下一次闹钟 */
    tick_ms += TICK_MS;                          /* 软件 tick 计数 */
}
```

**内核定时器不是"周期性自动响"的**，而是"一次一响"：
每次响完，处理函数立刻把 mtimecmp 往后推 1ms —— 形成周期。
tick_ms 是纯软件计数（uptime_ms() 读它返回毫秒数）。

### 6.3 ★两层中断开关（必考级知识）

为什么"开了定时器中断却收不到"？因为有**两级开关，都要开**：

```
第一级: 具体某种中断的类型开关   mstatus/sie 的某一位
第二级: 全局总开关               sstatus.SIE (bit1)

门铃要响必须: 类型开关(如 STIE) 开 AND 全局总开关(SIE) 开
```

代码：

```cpp
void timer_init() {
    sbi_set_timer(read_time() + TICKS_PER_MS);
    asm volatile("csrs sie, %0"     :: "r"(1UL << 5));  /* 类型开关 STIE */
    asm volatile("csrs sstatus, %0" :: "r"(1UL << 1));  /* 全局 SIE */
}
```

- `csrs` = CSR set：把指定 CSR 的某些位置 1（不动其他位）
- `1UL << 5` = bit5 → STIE（S 定时器中断使能）
- `1UL << 1` = bit1 → SIE（全局）

| CSR | 位 | 含义 |
|-----|----|----|
| sie | 1 (SSIE) / 5 (STIE) / 9 (SEIE) | 软件/定时器/外设 中断类型开关 |
| sstatus | 1 (SIE) | 全局开关 |
| sip | 1/5/9 | 对应中断的"挂起"标志（只读观察） |

**检查中断为什么不来，顺序就是看这三样**：sie 类型开没开 →
sstatus.SIE 总闸开没开 → sip 挂起位亮没亮（'s' 命令就是干这个的）。

---

## 第七章：PLIC —— 一堆设备抢一个门铃（M4）

### 7.1 为什么需要 PLIC

一个 UART、一个定时器、以后还有键盘/网卡……中断源很多，CPU 只有
一根"外部中断"线。PLIC 就是**门铃总机**：收集所有设备的中断请求，
按优先级选一个上报给 CPU。UART 是第 10 号源。

### 7.2 PLIC 的配置三件套

```cpp
void plic_init(u32 hartid) {
    s_context = hartid * 2 + 1;   /* S 模式 context 编号 */
    /* ① 优先级: 源 10 的优先级 = 1 (0 表示禁用) */
    *plic_reg(PLIC_PRIORITY + 4 * PLIC_UART_SOURCE) = 1;
    /* ② 使能: 该 context 的使能位图 bit10 = 1 */
    *plic_reg(0x2000 + s_context*CTX_ENABLE_STRIDE) |= (1u << PLIC_UART_SOURCE);
    /* ③ 阈值: 0 = 所有优先级都放行 */
    *plic_reg(0x200000 + s_context*CTX_CONTEXT_STRIDE) = 0;
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));  /* SEIE 类型开关 */
}
```

**context 概念**：PLIC 按"哪个 hart 的哪个模式"分别记录中断配置。
编号 = hartid×2 + (0=M, 1=S)。context 1 = hart 0 的 S 模式（我们）。

### 7.3 claim/complete 协议（必须成对）

```cpp
u32 plic_claim()    { return *plic_reg(... + 4); }   /* 问总机: 谁喊的? */
void plic_complete(u32 s) { *plic_reg(... + 4) = s; }/* 答: 处理完了 */
```

- **claim（读）**：返回当前最高优先级挂起的源号，同时"领取"它
- **complete（写）**：把源号写回去表示处理完毕
- 忘了 complete → 总机认为你还没处理完 → 这个源永远不再报上来

### 7.4 环形缓冲区：ISR 与主循环的"信箱"

UART 收字符中断可能很密集，ISR 里最好把能收的一次收完放进缓冲区，
主循环慢慢取。`drivers/uart.cpp` 里的 rxbuf 环形缓冲：

- ISR（生产者）写 rx_tail
- 主循环（消费者）读/写 rx_head
- 单生产者单消费者 → 天然无锁（各自只改自己的下标）
- 满了丢新字符（tail 追上 head 就不写）

**这是嵌入式/OS 里最常用的无锁数据结构之一**。

---

## 第八章：M5 —— 搬到 S 模式（整个项目最大的一次重构）

### 8.1 发生了什么变化（一句话）

内核从"机器的主人(M 模式)"变成"物业 OpenSBI 管着的住户(S 模式)"：
机器级资源（mtimecmp、mhartid、mtvec...）碰不到了，改用 **SBI
调用(ecall)** 请 OpenSBI 代劳。

### 8.2 改动分三类（对代码）

| 文件 | 改了什么 | 本质 |
|------|---------|------|
| run.sh/linker.ld | 启动 OpenSBI；内核搬 0x80200000 | 地址/固件 |
| boot.S/trap.S/trap.hpp | mtvec→stvec 等所有 CSR 换名 | 前缀 m→s |
| sbi.cpp（新）/timer.cpp | 定时器改 rdtime+SBI ecall | 新模式 |
| plic.cpp | context 0→1 | 换插座 |
| main.cpp | kernel_main(hartid,dtb) | 接收信封 |

### 8.3 CSR 换名对照（M5 全部"换名"就这 7 个）

| M4 (M 模式) | M5 (S 模式) | 作用 |
|-------------|-------------|------|
| mtvec | stvec | trap 入口地址 |
| mscratch | sscratch | 现场区指针暂存 |
| mepc | sepc | 被打断位置 |
| mcause | scause | 原因码 |
| mtval | stval | 附加信息 |
| mstatus | sstatus | 状态(注意位编排不同!) |
| mret | sret | 返回指令 |
| mie.MTIE(bit7) | sie.STIE(bit5) | 定时器中断类型开关 |
| mstatus.MIE(bit3) | sstatus.SIE(bit1) | 全局总开关(位也变了!) |

**保存/恢复的逻辑一行没变** —— 换名字而已。trap 处理套路是
"模式无关"的，这是 RISC-V 特权设计优雅的地方。

### 8.4 SBI：住户填申请单（也是"系统调用"的原理）

```cpp
/* lib/sbi.cpp */
SbiRet sbi_ecall(u64 ext, u64 fid, u64 a0,...) {
    register u64 r_a0 asm("a0") = a0;  /* 参数放 a0-a5 */
    ...
    register u64 r_a6 asm("a6") = fid; /* 单号 */
    register u64 r_a7 asm("a7") = ext; /* 部门号 */
    asm volatile("ecall" ...);          /* 递单! CPU 进 M 模式给 OpenSBI */
    return {r_a0, r_a1};               /* 回执: error, value */
}
void sbi_set_timer(u64 stime) {
    sbi_ecall(0x54494D45 /*"TIME"*/, 0, stime);
}
```

**ecall 会触发 trap**（scause=9，从 S 模式提出），OpenSBI 在 M 模式
收到后查"部门号(扩展ID) + 单号(功能ID)"执行，再返回 —— 等于住户
填单、物业办事。**用户态程序找内核办事 = 同样的 ecall 机制**，
这就是系统调用。

### 8.5 定时器两跳（为什么 M5 后 tick 还准）

```
sbi_set_timer(t) → OpenSBI 写本 hart mtimecmp
→ 到点, MTIP 在 M 模式触发 → OpenSBI 把 STIP 注入 S 模式
→ 我们收到 scause=5 → timer_handler 再填单排下一个
```

### 8.6 委派（delegation）：物业把哪些门铃线接给你

OpenSBI 启动时通过 medeleg/mideleg 两个 CSR 决定哪些 trap 直接
转给 S 模式。mideleg=0x222 → 软件/定时器/外设中断(位1/5/9)直接进
我们的 stvec，不打扰物业。

**'e' 和 'b' 命令的区别正好演示委派**：
- 'b'（ebreak）：medeleg bit3=1 → 直接委派 → 我们直接收 scause=3
- 'e'（非法指令）：medeleg bit2=0 → 先响物业 → 物业模拟不了 →
  重定向（填好 scause 等 CSR 后把返回地址改 stvec）→ 我们收到 scause=2

两条路径结果一样：我们都能处理。差别只在于中间是否绕物业一跳。

---

## 第九章：怎么自己动手验证（每章的"实验台"）

```bash
make clean && make                    # 编译
riscv64-unknown-elf-objdump -d build/riscvPrj.elf | head   # 看反汇编
riscv64-unknown-elf-objdump -S build/riscvPrj.elf          # C++↔汇编对照

# 跑起来(设备在远程主机, 需要 ssh 过去)
# (sleep 4; printf 'abis\x03') | timeout 12 qemu-system-riscv64 \
#   -M virt -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf

# 改一处, 看效果(强烈建议亲手试):
#  1. 注释掉 timer_init 里 csrs sstatus 那行 → tick 消失(总闸没开)
#  2. 把 plic 里使能行去掉 → 输入不再回显(外设中断没使能)
#  3. trap.cpp 里删掉 case 2 → 按 'e' 会停机(非法指令不再被跳过)
```

---

## 结束语：学完这份报告，你手里有什么

一份能回答这些问题的地图：

1. 内核文件从源码到 ELF 发生了什么？（第一章）
2. 上电后第一段汇编在干嘛，为什么要 .option norelax？（第二章）
3. C++ 世界的入口长什么样，主循环为什么永不退出？（第三章）
4. trap 时硬件只做 5 件事，保存现场为什么是我们的事？（第四章）
5. scause 怎么区分中断和异常，异常怎么"跳过"？（第五章）
6. 两级中断开关怎么查（sie/sstatus.SIE/sip）？（第六章）
7. PLIC 的 claim/complete 为什么必须成对？（第七章）
8. M5 搬家到 S 模式，哪些是换名、哪些是新机制？（第八章）
9. 每条知识都有一个能亲手实验验证的方法（第九章）

**如果哪一章读的时候卡住，把那一章编号和卡住的那句话发给我，
我针对那一句展开，不再整篇返工。**
