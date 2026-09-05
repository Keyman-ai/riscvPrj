# M3 笔记：CLINT 机器定时器中断

> 阅读顺序：**1. 做了什么** → **2. 原理**（定时器从硬件到软件的完整链路，
> 含位级细节）→ **3. 技巧**（实现细节与坑）→ **4. 验证**（对照实验）→
> **附录**（全部源码）。
> 配套：`docs/trap-csrs.md`（mstatus/mie/mepc 详解）、`docs/asm-cheatsheet.md`。

---

## 1. 这一阶段做了什么

**一句话**：让内核每 1ms 被硬件"闹钟"打断一次——中断里只做两件小事
（重设闹钟 + tick++），主循环每秒打印一次 uptime。

### 改动清单

| 文件 | 改动 | 作用 |
|------|------|------|
| `include/kernel/timer.hpp` | 新增 | 接口：`timer_init` / `timer_handler` / `uptime_ms` |
| `kernel/timer.cpp` | 新增 | CLINT 驱动（见附录 A） |
| `kernel/trap.cpp` | 修改 | 中断分发：`mcause=7` → `timer_handler()` |
| `kernel/main.cpp` | 修改 | 调 `timer_init()`；阻塞回显改为**非阻塞** + 每秒打印 |

### 运行效果（实测）

```
> [tick] uptime = 1000 ms      ← 每 1 秒打印一次 (后台 1ms 中断在跑)
[tick] uptime = 2000 ms
[tick] uptime = 3000 ms
```

回显、非法指令 trap 演示在中断开启后全部正常。

---

## 2. 原理：定时器中断的完整链路

### 2.1 硬件：CLINT 是什么（位级）

CLINT（Core Local Interruptor）= RISC-V 的**机器级定时器/软件中断控制器**。
QEMU virt 挂在 `0x02000000`，我们只用其中两个 64 位寄存器：

```
0x0200BFF8   mtime    64 位, 只读, 全局唯一
0x02004000   mtimecmp 64 位, 读/写, 每 hart 一份 (0x02004000 = hart0)

64 位寄存器访问粒度是 32 位 → 读写要拆两次 (见技巧 3.1/3.2)
```

| 寄存器 | 谁在变 | 含义 |
|--------|--------|------|
| `mtime` | **硬件**一直自增 | 开机以来的 tick 数（秒表） |
| `mtimecmp` | **软件**设置 | "闹钟设定值"，`mtime >= mtimecmp` 时中断置起 |

**时基从哪来？** 设备树 `/cpus` 节点（QEMU 真相，不是猜的）：

```dts
cpus {
    timebase-frequency = <0x989680>;   /* = 10,000,000 = 10MHz */
};
```

10MHz = 1 tick 100ns。所以：

```
1ms = 10,000,000 tick/s × 0.001s = 10000 tick
```

**为什么 mtimecmp 是 per-hart 的而 mtime 是全局的？** 时间对所有核是
同一个（全局秒表），但每个核可以设自己的闹钟（各睡各的）。

### 2.2 闹钟怎么"响"

```
mtime 不停往前走……
        │  mtime >= mtimecmp ?
        ▼
   定时器中断挂起 (mip.MTIP 置位, 等待被允许触发)
```

**一次性的**：响过之后 `mtime >= mtimecmp` 依然成立，挂起位不会自己
清——必须软件把 `mtimecmp` 改成更大的值。所以中断处理函数的第一件事
必须是"重设闹钟"（技巧 3.1 之前说过的"排下一次"）。

### 2.3 两道开关（位级）

中断要真正进 trap，必须过两道开关（`timer_init` 里用 `csrs` 打开）：

```
mie     寄存器 (0x304):  bit 7 = MTIE   ← 定时器这一类中断的开关
mstatus 寄存器 (0x300):  bit 3 = MIE    ← 所有机器中断的总闸
```

```cpp
asm volatile("csrs mie, %0"     :: "r"(1UL << 7));  /* MTIE */
asm volatile("csrs mstatus, %0" :: "r"(1UL << 3));  /* MIE  */
```

- `csrs` = CSR Set：把指定位**置 1**，其他位不动（`csrc` 是清零）
- 顺序上两个开关都要开，缺一不可——这就是 M4 排障时"只开分闸不开总闸"
  会导致中断永远不来的原因

**完整的响应条件链**：

```
mip.MTIP = 1   (闹钟响, 硬件置位)
  且 mie.MTIE = 1  (分闸)
  且 mstatus.MIE = 1 (总闸)
  → 当前指令结束后进 trap
```

### 2.4 进 trap：硬件自动做的 4 件事

1. `mstatus.MIE` 清零（关总闸——处理中断期间不再被中断，**禁止嵌套**）
2. 旧 MIE 值存入 `mstatus.MPIE`（记下"刚才开着"）
3. `mepc` ← 被中断的指令地址；`mcause` ← `0x80000007`
   （bit63=1 表示中断，低 12 位 7 = 机器定时器）
4. `PC` ← `mtvec`（我们的 `trap_entry`）

### 2.5 处理：trap_handler 分发

```cpp
bool is_interrupt = (f->mcause >> 63) != 0;    /* 最高位: 1=中断 0=异常 */
u64 cause = f->mcause & 0x7FFFFFFFFFFFFFFFULL; /* 低 63 位: 具体原因 */

if (is_interrupt) {
    if (cause == 7) {
        timer_handler();   /* 重设闹钟 + tick++, 然后 return 恢复现场 */
        return;
    }
    ...
}
```

### 2.6 返回：mret 的语义

`mret` 让硬件做两件事：

```
① PC ← mepc               (回到被打断的那条指令)
② mstatus.MIE ← MPIE      (恢复总闸, 继续允许中断)
   特权级 ← MPP
```

### 2.7 完整时序图

```
主循环某条指令 (比如 uart 轮询的读内存)
  → mtime >= mtimecmp, 两道开关都开
  → 硬件: 关 MIE → 存 MPIE → mepc/mcause 记录 → 跳 mtvec
  → trap.S: csrrw 交换 → 保存 31 GPR + 4 CSR 到 trap_frame
  → trap_handler: mcause=0x80000007 → timer_handler()
      ├─ set_mtimecmp(mtime + 10000)   ← 重设闹钟 (下一个 1ms)
      └─ tick_ms++                      ← 记账 (只做小事!)
  → trap.S: mepc/mstatus 写回 CSR → 恢复 31 GPR → 反向交换
  → mret: 恢复 MIE, PC 回 mepc
  → 主循环继续, 像什么都没发生
```

### 2.8 中断优先级（为什么定时器不会和外设打架）

规范定义固定优先级（从高到低）：

```
MEI(外设) > MSI(软件) > MTI(定时器) > SEI > SSI > STI > ...
```

- M3 只有定时器，无所谓；M4 加了外设中断后，**同时挂起时外设先处理**
- 本工程两个中断源都"做完就 return"，互不阻塞，优先级影响不大

---

## 3. 技巧：实现细节与坑

### 3.1 防撕裂读 `mtime`（64 位 MMIO 的经典问题）

**问题**：`mtime` 是 64 位寄存器，但访问粒度是 32 位——必须分两次读
低 32 位和高 32 位。两次读之间如果低 32 位**进位**（如
`0xFFFFFFFF` → `0x00000000`），拼出来的值就错了（高位还是旧的）。

**解法**：高位读两遍，变了就重读：

```cpp
static u64 read_mtime() {
    volatile u32* lo = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIME);
    volatile u32* hi = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIME + 4);
    u32 h, l;
    do {
        h = *hi;              /* 先读高位 */
        l = *lo;              /* 再读低位 */
    } while (h != *hi);       /* 高位变了 = 低位刚进位, 重来 */
    return (static_cast<u64>(h) << 32) | l;
}
```

**为什么循环能保证正确？** 进位只会让"读到的低位"偏小，而高位变了
就说明发生过进位；重读一遍必然拿到进位后的低位——数学上保证一致。

### 3.2 `mtimecmp` 先写低 32 位、再写高 32 位

**问题**：如果先写高 32 位，中间态可能是"新高位 + 旧低位"——比如把
`mtimecmp` 从 100 改成 100000 时，先写高字会让比较值瞬间变成一个
**巨大或错误的数**，可能小于当前 mtime 而**提前触发一次中断**。

**解法**：低字先写、高字后写（高字写入才真正生效）：

```cpp
static void set_mtimecmp(u64 value) {
    volatile u32* lo = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIMECMP);
    volatile u32* hi = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIMECMP + 4);
    *lo = static_cast<u32>(value);          /* 先低 */
    *hi = static_cast<u32>(value >> 32);    /* 后高 (生效点) */
}
```

### 3.3 ISR 只做最小的事，打印永远放主循环

**为什么？** 定时器中断可以打断**主循环的任何一条指令，包括 printf
自己**。若 ISR 里也 printf：

- 主循环 printf 打到一半 → 中断 → ISR printf 插入 → 输出交错
- printf 若用了不可重入的锁 → 主循环拿着锁被中断 → ISR 等锁 → **死锁**

**设计**：ISR 只做两件小事（重设闹钟 + `tick_ms++`），打印放主循环：

```cpp
void timer_handler() {
    set_mtimecmp(read_mtime() + TICKS_PER_MS);   /* 排下一次 */
    tick_ms += TICK_MS;                          /* 只记账 */
}
```

主循环检测 tick 变化再打印：

```cpp
if (uptime_ms() - last_print >= 1000) {
    last_print = uptime_ms();
    printf("[tick] uptime = %u ms\n", (u32)last_print);
}
```

这是"**中断里少干活**"原则的第一次实践——M7 做任务切换时，
ISR（上下文切换）也会保持精简；M4 的 UART ISR 同样只入队不打印。

### 3.4 64 位 `tick_ms` 为什么不用加锁

单 hart 上：
- 读/写一个 64 位对齐变量各是**一条指令**
- 中断只能插在**两条指令之间**，不会撕开一条指令

所以主循环读、ISR 写，无需锁。（多核就需要原子操作了——Phase 3 的事。）

### 3.5 trap.S 现场保存：mscratch 交换（M1 的坑 2 修复版）

进 trap 要一个寄存器当"帧指针"，但不能覆盖被打断代码的寄存器。
用 `mscratch`（boot.S 预置为 `&trap_frame`）：

```asm
trap_entry:
    csrrw t0, mscratch, t0   # 一行两件事: t0 = &trap_frame
                             #            mscratch = 原 t0 (先藏起来)
    sd x1, 0*8(t0)           # 用 t0 当基址存全部 GPR...
    ...
    csrr t1, mscratch
    sd   t1, 4*8(t0)         # 原 t0 补存进帧 (x5 槽位)
    csrr t1, mepc;    sd t1, 31*8(t0)   # 4 个 CSR 也入帧
    csrr t1, mcause;  sd t1, 32*8(t0)
    csrr t1, mtval;   sd t1, 33*8(t0)
    csrr t1, mstatus; sd t1, 34*8(t0)
    mv   a0, t0
    call trap_handler
    # 返回后:
    ld   t1, 31*8(t0); csrw mepc, t1    # 关键: 写回 (M1 坑 2)
    ld   t1, 34*8(t0); csrw mstatus, t1
    # ... 恢复 GPR ...
    csrrw t0, mscratch, t0   # 反向交换: 原 t0 回 t0, mscratch 复原
    mret
```

**为什么必须写回 mepc？** 见 M1 笔记坑 2——mret 用的是**硬件 CSR**
里的值，不是帧里的副本。

### 3.6 用 mstatus 验证中断真的开了（0x1880 vs 0x1800）

按 `e` 触发异常，打印 `mstatus = 0x1880`，解码：

| 位 | 值 | 含义 |
|----|----|------|
| [12:11] MPP | `11` | trap 来自 M 模式 |
| [7] MPIE | `1` | **进 trap 前中断是开着的** ← 定时器在跑的证据 |
| [3] MIE | `0` | 进 trap 后被硬件清零（禁止嵌套） |

M1 时是 `0x1800`（MPIE=0，当时根本没开中断）。同一份打印两版不同——
**这就是"中断系统生效了"的验证手段**。

### 3.7 用 QEMU 日志观察定时器中断

```bash
qemu ... -d int -D /tmp/qemu-int.log
head /tmp/qemu-int.log
# riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000007, ...
#                                        ↑ async=1 中断  ↑ 7 = 定时器
```

M4 排障时正是靠它发现"只有 m_timer、没有 m_external"。

---

## 4. 怎么验证（对照实验）

```bash
# 实验 1: 正常运行
timeout 7 qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic \
    -bios none -kernel build/riscvPrj.elf < /dev/null
# 预期: 每秒一行 [tick] uptime = NNNN ms

# 实验 2: 注释掉 timer_init 里的 "csrs mie" (MTIE 分闸)
#   预期: 完全没有 uptime 输出 —— 开关①不开, 闹钟响了也进不来

# 实验 3: 注释掉 timer_handler 里的 set_mtimecmp (不重设闹钟)
#   预期: 只打印一次 uptime(1ms 左右) 然后中断不再触发 —— 一次性闹钟的证据

# 实验 4: 只开 MTIE 不开 MIE (总闸)
#   预期: 依旧无 uptime —— 两道开关缺一不可

# 实验 5: -d int 看日志
qemu ... -d int -D /tmp/qemu-int.log; grep -c m_timer /tmp/qemu-int.log
```

---

## 5. 下一步

- **M4（已完成）**：PLIC + UART 外设中断——第二个中断源接入同一 trap 入口，
  见 `docs/m4-notes.md`
- **M5**：同一套定时器逻辑从 M 模式搬到 S 模式（`stimecmp` + SBI 调用）

---

## 附录 A：kernel/timer.cpp（全文）

```cpp
/* CLINT 机器定时器驱动
 * QEMU virt: 0x0200BFF8 mtime (64 位只读, 10MHz)
 *            0x02004000 mtimecmp (64 位, hart0) */
#include "kernel/timer.hpp"

constexpr u64 CLINT_BASE     = 0x02000000;
constexpr u64 CLINT_MTIME    = 0xBFF8;
constexpr u64 CLINT_MTIMECMP = 0x4000;

constexpr u64 TICKS_PER_MS = 10000;   /* 10MHz → 1ms = 10000 tick */
constexpr u64 TICK_MS      = 1;

static volatile u64 tick_ms = 0;

static u64 read_mtime() {              /* 防撕裂读 (3.1) */
    volatile u32* lo = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIME);
    volatile u32* hi = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIME + 4);
    u32 h, l;
    do { h = *hi; l = *lo; } while (h != *hi);
    return (static_cast<u64>(h) << 32) | l;
}

static void set_mtimecmp(u64 value) {  /* 先低后高 (3.2) */
    volatile u32* lo = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIMECMP);
    volatile u32* hi = reinterpret_cast<volatile u32*>(CLINT_BASE + CLINT_MTIMECMP + 4);
    *lo = static_cast<u32>(value);
    *hi = static_cast<u32>(value >> 32);
}

void timer_init() {                    /* 两道开关 (2.3) */
    set_mtimecmp(read_mtime() + TICKS_PER_MS);
    asm volatile("csrs mie, %0"     :: "r"(1UL << 7));   /* MTIE */
    asm volatile("csrs mstatus, %0" :: "r"(1UL << 3));   /* MIE  */
}

void timer_handler() {                 /* ISR: 只做小事 (3.3) */
    set_mtimecmp(read_mtime() + TICKS_PER_MS);
    tick_ms += TICK_MS;
}

u64 uptime_ms() { return tick_ms; }
```

## 附录 B：kernel/trap.cpp（M3 版全文）

```cpp
#include "kernel/trap.hpp"
#include "kernel/timer.hpp"
#include "drivers/uart.hpp"
#include "lib/printf.hpp"

static const char* exception_name(u64 cause) { /* 异常原因码表 ... */ }
static const char* interrupt_name(u64 cause) { /* 中断原因码表 ... */ }

extern "C" void trap_handler(TrapFrame* f) {
    bool is_interrupt = (f->mcause >> 63) != 0;
    u64 cause = f->mcause & 0x7FFFFFFFFFFFFFFFULL;

    if (is_interrupt) {
        if (cause == 7) {              /* 机器定时器中断 */
            timer_handler();           /* 重设闹钟 + tick++ */
            return;
        }
        printf("[trap] interrupt: %s (cause=%d), mepc=%p\n",
               interrupt_name(cause), (int)cause, (void*)f->mepc);
        return;
    }

    /* 异常: 打印完整现场 */
    printf("\n[trap] exception: %s (cause=%d)\n", exception_name(cause), (int)cause);
    printf("  mepc    = %p\n", (void*)f->mepc);
    printf("  mtval   = %p\n", (void*)f->mtval);
    printf("  mstatus = 0x%X\n", (u32)f->mstatus);
    printf("  ra=%p sp=%p gp=%p tp=%p\n",
           (void*)f->ra, (void*)f->sp, (void*)f->gp, (void*)f->tp);

    if (cause == 2) {                  /* 非法指令: 跳过继续 */
        f->mepc += 4;
        return;
    }
    printf("  [trap] unrecoverable, halting\n");
    for (;;) asm volatile("wfi");
}
```

## 附录 C：kernel/main.cpp（M3 版核心）

```cpp
extern "C" void kernel_main() {
    uart_init();
    timer_init();              /* 开定时器中断 (最后开总闸) */

    printf("... 横幅 ...\n> ");

    u64 last_print = 0;
    for (;;) {
        int c = uart_getc_nonblock();          /* 非阻塞收字符 */
        if (c >= 0) { /* 回显 / 'e' trap 演示 / Ctrl+C 处理 */ }

        if (uptime_ms() - last_print >= 1000) {   /* 每秒打印 */
            last_print = uptime_ms();
            printf("[tick] uptime = %u ms\n", (u32)last_print);
        }
    }
}
```

## 附录 D：arch/trap.S（全文注释，M3 状态）

```asm
.section .text
.globl trap_entry
.align 2                            /* mtvec 直接模式要求 4 字节对齐 */

trap_entry:
    /* ---- 保存现场 ---- */
    csrrw t0, mscratch, t0          /* t0 = &trap_frame, mscratch = 原 t0 */
    sd x1,  0*8(t0)                 /* ra */
    sd x2,  1*8(t0)                 /* sp */
    sd x3,  2*8(t0)                 /* gp */
    sd x4,  3*8(t0)                 /* tp */
    sd x6,  5*8(t0)                 /* t1 ... (x7..x31 同理, 见 trap.hpp) */
    ...
    sd x31, 30*8(t0)                /* t6 */
    csrr t1, mscratch
    sd   t1, 4*8(t0)                /* 原 t0 补存 */
    csrr t1, mepc;    sd t1, 31*8(t0)
    csrr t1, mcause;  sd t1, 32*8(t0)
    csrr t1, mtval;   sd t1, 33*8(t0)
    csrr t1, mstatus; sd t1, 34*8(t0)

    /* ---- 调 C 处理函数 ---- */
    mv   a0, t0
    call trap_handler

    /* ---- handler 改过的 CSR 写回 (M1 坑 2 的修复) ---- */
    ld   t1, 31*8(t0); csrw mepc, t1
    ld   t1, 34*8(t0); csrw mstatus, t1

    /* ---- 恢复 GPR ---- */
    ld x1, 0*8(t0)
    ...
    /* ---- 反向交换 + 返回 ---- */
    csrrw t0, mscratch, t0          /* 原 t0 回 t0, mscratch 复原 */
    mret

.section .bss
.align 3
.globl trap_frame
trap_frame:
    .skip 35 * 8                    /* 31 GPR + 4 CSR */
```
