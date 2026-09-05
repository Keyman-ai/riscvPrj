# M4 笔记：PLIC + UART 中断驱动接收

> 阅读顺序：**1. 做了什么** → **2. 原理**（从键盘到回显的完整链路，
> 含寄存器位级细节）→ **3. 技巧与排障** → **4. 验证** → **附录**（全部源码）。
> 配套：`docs/trap-csrs.md`（MEI/MEIP/claim 相关 CSR）、`docs/asm-cheatsheet.md`。

---

## 1. 这一阶段做了什么

**一句话**：键盘输入从"主循环轮询"变成"硬件中断通知"——UART 收到字符时，
16550 → PLIC → `mip.MEIP` → trap（`mcause=11`），ISR 把字符塞进
**环形缓冲区**，主循环取出来回显。M4 之前输入靠 `uart_getc_nonblock()`
轮询 LSR 寄存器；M4 之后 CPU 在输入到来之前**完全不看 UART**，
字符到达时被硬件主动打断。

### 改动清单

| 文件 | 改动 | 作用 |
|------|------|------|
| `include/kernel/plic.hpp` | 新增 | PLIC 接口 + `PLIC_UART_SOURCE=10` |
| `kernel/plic.cpp` | 新增 | PLIC 四步初始化 + claim/complete |
| `drivers/uart.cpp` | 修改 | RX 中断使能 + 排空循环 + 环形缓冲区（256B） |
| `kernel/trap.cpp` | 修改 | 中断分发：`mcause=11` → claim → 处理 → complete |
| `kernel/main.cpp` | 修改 | 初始化顺序调整；输入改用 `uart_rx_pop()`；新增 `i` 统计命令 |
| `Makefile` | 修改 | 加 plic.cpp；`-MMD -MP` 头文件依赖跟踪 |

### 运行效果（实测）

```
> abc                       ← 中断路径回显!
> i
[info] rx_irq=1 rx_irq_count > 0 = IRQ-driven ✓   ← 4 字符只触发 1 次中断
> e                        ← 非法指令 trap 演示照常
[trap] exception: Illegal instruction (cause=2)
  mstatus = 0x1880          ← MPIE=1: 中断确实是开着的
```

---

## 2. 原理：一条外设中断的完整链路

### 2.1 全景图

```
   键盘输入 (QEMU 从 stdin 读到字节)
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 16550 UART (0x10000000)                                      │
│   字节进 RBR → LSR.DR=1 → IER.ERBFI=1 时拉高中断线           │
└──────────────────────────┬──────────────────────────────────┘
                           │ 中断线 (物理连线, 源 10)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│ PLIC (0x0c000000)                                            │
│   ① 该源优先级 > 0?        (我们设 priority[10]=1)           │
│   ② 该 context 使能位=1?   (我们设 enable[0] bit10)          │
│   ③ 优先级 > 阈值?         (我们设 threshold[0]=0)           │
│   都满足 → 向 context 0 (hart0 M 模式) 上报                   │
└──────────────────────────┬──────────────────────────────────┘
                           │ 硬件置位 CPU 的 mip.MEIP (bit 11)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│ CPU                                                        │
│   ① mip.MEIP = 1 (事件发生)                                 │
│   ② mie.MEIE = 1 (分闸: 我们 plic_init 里开了)              │
│   ③ mstatus.MIE = 1 (总闸: timer_init 里开了)               │
│   全满足 → 当前指令结束后进 trap                             │
│   mcause = 0x8000000B (中断 + 11 = 机器外设中断)             │
│   PC → mtvec (trap_entry)                                   │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
              trap_handler → cause==11 → 见 2.4
```

### 2.2 第一站：16550 侧（字节怎么变成中断）

16550 是 UART 芯片，寄存器全部字节宽、挂在 `0x10000000`：

| 偏移 | 寄存器 | M4 用到的位 |
|------|--------|------------|
| +0 | THR(写)/RBR(读) | RBR = 收到的字符 |
| +1 | IER | **bit0 ERBFI**：RX 数据可用中断使能 |
| +2 | FCR(写)/IIR(读) | FCR=0 关 FIFO；IIR bit0=0 表示有中断待处理 |
| +3 | LCR | 8N1 配置 |
| +5 | LSR | **bit0 DR** 数据就绪；bit1 OE 溢出；**bit5 THRE** 发送空 |

**字节到达时的硬件行为**：
1. 字符写入 RBR（FIFO 关闭时一次只能存 1 字节）
2. `LSR.DR` 置 1
3. 若 `IER.ERBFI = 1` → 中断线拉高（**电平触发**，只要 RBR 有数据就保持）
4. 软件读 RBR → `LSR.DR` 清零 → 中断线恢复（若 IER 仍开且无新数据）

> 电平触发的意义：即使字符在中断使能**之前**就到了，只要 RBR 里还有数据，
> 打开 IER 后中断线立刻拉高——不会丢"早到的字符"。

### 2.3 第二站：PLIC（位级）

PLIC 是平台中断控制器，QEMU virt 在 `0x0c000000`，**全部 32 位寄存器**：

```
0x0c000000 + 4×N      优先级[源N]    3 位有效 (0=禁用, 1..7)
0x0c001000 + 4×(N/32) 挂起[字N]     每源 1 bit, 硬件置位
0x0c002000 + ctx×0x1000 + 4×(N/32)  使能[context][字N]  每源 1 bit
0x0c200000 + ctx×0x2000             阈值[context]       优先级须 > 阈值
0x0c200004 + ctx×0x2000             claim(读)/complete(写)
```

**context（上下文）概念**：PLIC 为"每个 hart × 每个特权模式"各准备一套
使能/阈值/claim 寄存器，叫一个 context。QEMU virt 的映射：

| context | 归属 |
|---------|------|
| 0 | hart 0 的 M 模式（我们用这个） |
| 1 | hart 0 的 S 模式（M5 用） |
| 2 | hart 1 的 M 模式 |
| 3 | hart 1 的 S 模式 |

**我们的四步配置**（`plic_init`）：

```cpp
① 优先级:  *(0x0c000000 + 4×10) = 1;   // 源 10 (UART) 优先级 1
② 使能:    *(0x0c002000) |= (1u << 10); // context 0 的使能字 bit10
③ 阈值:    *(0x0c200000) = 0;           // 优先级 1 > 0, 放行
④ 开 MEIE: csrs mie, 1<<11              // CPU 侧分闸 (总闸 mstatus.MIE 最后开)
```

**上报条件（三个"与"）**：源优先级 > 0 **且** 该 context 使能位 = 1
**且** 源优先级 > 阈值。都满足 → 置位该 context 对应 hart 的 `mip.MEIP`。

### 2.4 第三站：CPU 侧与 claim/complete 协议

`mip.MEIP` 置位只是"挂起牌"。真正进 trap 还要过两道开关
（见 trap-csrs.md §3.6/3.7）：

```
mip.MEIP = 1  且  mie.MEIE = 1  且  mstatus.MIE = 1   → 进 trap
```

进 trap 后，trap_handler 的分发（`kernel/trap.cpp`）：

```cpp
if (cause == 11) {                     /* 机器外设中断 */
    u32 source = plic_claim();         /* ① claim: 读 0x0c200004 */
    if (source == PLIC_UART_SOURCE) {
        uart_rx_irq_handler();         /* ② 处理: 字符入环形缓冲区 */
    }
    if (source != 0) {
        plic_complete(source);         /* ③ complete: 写回 0x0c200004 */
    }
    return;
}
```

**claim 的语义（关键！）**：读取 claim 寄存器会返回"当前最高优先级的
挂起源号"，**同时清除该源的挂起位**。所以：

- 同一个中断源，claim 之后、complete 之前，**不会再上报**（PLIC 认为
  还在处理中）——天然实现"同一时刻一个源只有一个中断在途"
- **必须 complete**：complete 告诉 PLIC"处理完了，可以继续收这个源的
  下一个中断"。不 complete → 该源中断永久丢失
- claim 返回 0 = 没有挂起中断（竞态兜底），此时**不要** complete 0

### 2.5 第四站：环形缓冲区（ISR 与主循环的握手）

**为什么需要它？** ISR 里处理字符有时间限制（太长会延迟其他中断），
而回显逻辑（printf、命令处理）又重又长，不能放 ISR。所以：

```
生产(ISR 上下文):  rxbuf[tail] = c;  tail = (tail+1) % 256;
消费(主循环):      c = rxbuf[head];  head = (head+1) % 256;
判空:  head == tail
判满:  (tail+1) % 256 == head      ← 为什么留一格? 见下
```

**为什么满判是 `(tail+1)%N == head` 而不是 `tail == head`？**
因为 `head == tail` 已经被用作"空"的判据。如果不留一格，
"满"也会满足 `head == tail`，空和满就分不清了。**留一格**让环形缓冲区
最多存 N-1 个字符，换来一个无歧义的空/满判据——经典的空间换简单。

**为什么不用加锁？** 三个事实：
1. 单 hart：中断只能插在**两条指令之间**，不会撕开一条指令
2. ISR 只写 `tail`，主循环只写 `head`——**各自只写自己的下标**
3. 下标是 32 位对齐量，读/写各是一条指令，天然原子

所以单生产者/单消费者/单 hart 下，这个缓冲区**无锁安全**。
（多核就需要原子操作或关中断了——M7 的信号量会正式处理这个问题。）

---

## 3. 技巧与细节

### 3.1 ISR 里排空接收（一次中断收多个字符）

```cpp
void uart_rx_irq_handler() {
    rx_irq_cnt++;
    while (*reg(REG_LSR) & 0x01) {     /* 有数据就取, 取到没有为止 */
        u32 next = (rx_tail + 1) % RXBUF_SIZE;
        char c = static_cast<char>(*reg(REG_RBR));
        if (next != rx_head) { rxbuf[rx_tail] = c; rx_tail = next; }
        /* 满了: 字符已读出, 直接丢弃 */
    }
}
```

实测喂入 `abci` 只触发 **1 次**中断——字符密集到达时被排空循环一次收完。
**原理**：中断是在"有数据"的瞬间触发的；处理完 claim 后，若还有字符
在途，中断线再次拉高会再触发一次。排空循环把"已就绪"的字符全部取走，
减少 trap 保存/恢复的开销。这是性能敏感驱动的通用手法。

### 3.2 初始化顺序（中断源就绪了才开门）

```cpp
uart_init();            /* ① 串口基础配置, IER=0 (先别开中断) */
plic_init();            /* ② PLIC 配好: 优先级/使能/阈值 + MEIE */
uart_enable_rx_irq();   /* ③ 打开 16550 的 IER.ERBFI —— 现在中断才会产生 */
timer_init();           /* ④ 最后开总闸 mstatus.MIE */
```

**为什么总闸最后开？** 如果先开 MIE，此时某个中断源的配置还没完成，
一旦中断到来，trap_handler 会拿到半初始化的状态（比如 claim 返回
未使能的源号）。**"所有中断源就绪，才开门迎客"** 是中断初始化的铁律。

### 3.3 本次排障全过程（调试方法论，值得反复看）

**现象**：输入无回显，uptime 正常 → 主循环活着，问题在中断路径。

**第 1 步：确认中断到底有没有发生**（区分"没发生"和"发生了没处理"）：
```bash
qemu ... -d int -D /tmp/qemu-int.log
grep m_external /tmp/qemu-int.log    # 结果: 一条都没有!
# 日志里只有 m_timer (cause=7) —— 外设中断从未到达 CPU
```
→ 排除 CPU/开关问题，嫌疑指向 PLIC 配置或**中断源号**。

**第 2 步：查硬件真相——设备树**（不能凭记忆，机器说了算）：
```bash
qemu-system-riscv64 -machine virt,dumpdtb=/tmp/virt.dtb
dtc -I dtb -O dts /tmp/virt.dtb | grep -A8 'uart@10000000'
# uart@10000000 { interrupts = <0x0a>; ... }   ← 源号 10!
# 旁边 rtc@101000 { interrupts = <0x0b>; }     ← 11, 交叉验证连续
```

**第 3 步：修正并验证**：`PLIC_UART_SOURCE = 10` → 重建 → 回显恢复。

**方法论总结**：
1. 先用 `-d int` 确认中断是否到达 CPU（现象分层）
2. 再用设备树/手册确认硬件事实（不要凭记忆）
3. 一次只改一个变量（源号），改完立刻验证

### 3.4 其他细节

- **claim 返回 0 不 complete**：0 表示"没有挂起源"，写回 0 无意义
- **ISR 依然不 printf**：字符只进缓冲区，输出留主循环（M3 原则延续）；
  若在 ISR 里 printf，会打断主循环正在进行的 printf → 输出交错
- **PLIC 源号从 1 开始**：源 0 保留（`riscv,ndev = 53` 表示 1..53 有效）
- **电平触发 vs 边沿触发**：16550 是电平触发（数据在就拉高）；
  PLIC 的 pending 由 claim 清除。所以"早到的字符"不会丢

---

## 4. 怎么验证（对照实验）

```bash
# 正向验证: 中断驱动回显
printf 'abci' | timeout 4 qemu-system-riscv64 -M virt -smp 2 -m 128M \
    -nographic -bios none -kernel build/riscvPrj.elf
# 预期: abc 回显; i 打印 rx_irq=1 (IRQ-driven ✓)

# 对照实验 1: 感受"没有中断"的世界
#   把 main.cpp 的 uart_rx_pop() 换成 uart_getc_nonblock() 再编译:
#   输入依然工作(轮询), 但 i 显示 rx_irq=0 —— 中断路径被旁路了

# 对照实验 2: 故意漏掉 complete
#   注释掉 plic_complete(source): 只回显第一个字符, 之后全部丢失

# 对照实验 3: 错误的源号 (本次的坑)
#   把 PLIC_UART_SOURCE 改回 1: 输入完全无反应, -d int 日志无 m_external
```

---

## 5. 下一步

- **M5：OpenSBI + S 模式** —— 同一套 PLIC 逻辑搬到 S 模式
  （context 1、`seip`、S 模式 trap 用 stvec）；OpenSBI 会接管 M 模式的
  中断初始化，你会第一次看到"固件帮你干活"
- **M7 前置知识**：环形缓冲区 = 生产者-消费者模型；M7 移植 myos 的
  信号量/消息队列时，这里就是它们的"物理原型"

---

## 附录 A：kernel/plic.cpp（全文）

```cpp
/* PLIC (Platform-Level Interrupt Controller) 驱动
 *
 * QEMU virt 的 PLIC 内存布局 (32 位寄存器):
 *   0x0c000000  基址
 *   0x0c000000 + 4×src   源优先级 (0=禁用, 1..7)
 *   0x0c001000 + ...     挂起位 (硬件写, 只读)
 *   0x0c002000 + ctx×0x1000 + ...   使能位
 *   0x0c200000 + ctx×0x2000         阈值
 *   0x0c200004 + ctx×0x2000         claim(读)/complete(写)
 * context 0 = hart 0 的 M 模式。 */
#include "kernel/plic.hpp"

constexpr u64 PLIC_BASE      = 0x0c000000;
constexpr u64 PLIC_PRIORITY  = 0x000000;
constexpr u64 PLIC_ENABLE    = 0x002000;
constexpr u64 PLIC_THRESHOLD = 0x200000;
constexpr u64 PLIC_CLAIM     = 0x200004;

static inline volatile u32* plic_reg(u64 offset) {
    return reinterpret_cast<volatile u32*>(PLIC_BASE + offset);
}

void plic_init() {
    *plic_reg(PLIC_PRIORITY + 4 * PLIC_UART_SOURCE) = 1;  /* ① 优先级 */
    *plic_reg(PLIC_ENABLE) |= (1u << PLIC_UART_SOURCE);   /* ② 使能   */
    *plic_reg(PLIC_THRESHOLD) = 0;                        /* ③ 阈值   */
    asm volatile("csrs mie, %0" :: "r"(1UL << 11));       /* ④ MEIE  */
}

u32 plic_claim() {
    return *plic_reg(PLIC_CLAIM);
}

void plic_complete(u32 source) {
    *plic_reg(PLIC_CLAIM) = source;
}
```

## 附录 B：drivers/uart.cpp（全文）

```cpp
#include "drivers/uart.hpp"

constexpr u64 UART_BASE = 0x10000000;
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
    *reg(REG_FCR) = 0x00;   /* 关 FIFO */
    *reg(REG_IER) = 0x00;   /* 先关中断 */
}

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');
    while (!(*reg(REG_LSR) & (1 << 5)))   /* 等 THRE */
        ;
    *reg(REG_THR) = static_cast<u8>(c);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

char uart_getc() {
    while (!(*reg(REG_LSR) & 0x01))
        ;
    return static_cast<char>(*reg(REG_RBR));
}

int uart_getc_nonblock() {
    if (!(*reg(REG_LSR) & 0x01)) return -1;
    return *reg(REG_RBR) & 0xFF;
}

/* ---- M4: 中断驱动接收 ---- */
constexpr u32 RXBUF_SIZE = 256;
static char rxbuf[RXBUF_SIZE];
static volatile u32 rx_head = 0;   /* 消费者(主循环)写 */
static volatile u32 rx_tail = 0;   /* 生产者(ISR)写   */
static volatile u32 rx_irq_cnt = 0;

void uart_enable_rx_irq() {
    *reg(REG_IER) |= 0x01;         /* IER bit0: RX 数据可用中断 */
}

void uart_rx_irq_handler() {       /* ISR 上下文: 只生产, 不打印 */
    rx_irq_cnt++;
    while (*reg(REG_LSR) & 0x01) {
        u32 next = (rx_tail + 1) % RXBUF_SIZE;
        char c = static_cast<char>(*reg(REG_RBR));
        if (next != rx_head) { rxbuf[rx_tail] = c; rx_tail = next; }
    }
}

int uart_rx_pop() {                /* 主循环上下文: 只消费 */
    if (rx_head == rx_tail) return -1;
    char c = rxbuf[rx_head];
    rx_head = (rx_head + 1) % RXBUF_SIZE;
    return c & 0xFF;
}

u32 uart_rx_irq_count() {
    return rx_irq_cnt;
}
```

## 附录 C：kernel/trap.cpp 中断分发（M4 相关部分）

```cpp
#include "kernel/trap.hpp"
#include "kernel/timer.hpp"
#include "kernel/plic.hpp"
#include "drivers/uart.hpp"
#include "lib/printf.hpp"
// ...

extern "C" void trap_handler(TrapFrame* f) {
    bool is_interrupt = (f->mcause >> 63) != 0;
    u64 cause = f->mcause & 0x7FFFFFFFFFFFFFFFULL;

    if (is_interrupt) {
        if (cause == 7) {                    /* 机器定时器 */
            timer_handler();
            return;
        }
        if (cause == 11) {                   /* 机器外设 (PLIC) */
            u32 source = plic_claim();
            if (source == PLIC_UART_SOURCE) {
                uart_rx_irq_handler();
            }
            if (source != 0) {
                plic_complete(source);
            }
            return;
        }
        printf("[trap] interrupt: %s (cause=%d), mepc=%p\n",
               interrupt_name(cause), (int)cause, (void*)f->mepc);
        return;
    }
    /* 异常路径 ... */
}
```

## 附录 D：kernel/main.cpp（M4 相关部分）

```cpp
extern "C" void kernel_main() {
    uart_init();            /* ① 串口基础 */
    plic_init();            /* ② PLIC 四步配置 + MEIE */
    uart_enable_rx_irq();   /* ③ UART 接收中断使能 */
    timer_init();           /* ④ 定时器 + 总闸 MIE (最后开门) */

    /* 横幅 ... */

    u64 last_print = 0;
    for (;;) {
        int c = uart_rx_pop();               /* 从环形缓冲区取 (ISR 塞的) */
        if (c >= 0) {
            char ch = (char)c;
            if (ch == '\r')      { uart_putc('\n'); printf("> "); }
            else if (ch == 0x03) { printf("\nbye (uptime %u ms)\n", (u32)uptime_ms());
                                   for (;;) asm volatile("wfi"); }
            else if (ch == 'e')  { asm volatile(".word 0xffffffff"); }
            else if (ch == 'i')  { printf("[info] rx_irq=%u\n", (u32)uart_rx_irq_count()); printf("> "); }
            else                 { uart_putc(ch); }
        }
        if (uptime_ms() - last_print >= 1000) {
            last_print = uptime_ms();
            printf("[tick] uptime = %u ms\n", (u32)last_print);
        }
    }
}
```
