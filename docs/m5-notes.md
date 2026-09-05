# M5 笔记 — OpenSBI + S 模式（教学版）

> 读法：按顺序读，每节只依赖前面的节。
> 类比约定：**盒子 = 寄存器，仓库 = 内存**；M5 新增一对：**户主/住户/物业**。
> CSR 名字先别背 —— §3 告诉你"只记 5 个名字"就够了。

## 0. 一句话总结

**M5 = 从"独占整台机器的户主"搬进"有物业的小区当住户"。**
M4 的代码 90% 原样平移，变的不是逻辑，是你住进了**中间层**。

## 1. 这一阶段做了什么

物理上改了 10 个文件，但性质只有三种：

| 文件 | 性质 | 为什么 |
|------|------|--------|
| `arch/boot.S` `arch/trap.S` `kernel/trap.cpp` `include/kernel/trap.hpp` | **换名字** | M 开头 CSR → S 开头，逻辑一字不动 |
| `run.sh` `Makefile` `linker.ld` | **换门牌** | 内核从 0x80000000 搬到 0x80200000（物业占了前 2MB） |
| `lib/sbi.cpp`(新) `kernel/timer.cpp` `kernel/plic.cpp` `kernel/main.cpp` | **走新流程** | 摸不到的机器资源改走 SBI 申请单；门铃插座换到 S-context |

### 实测输出（回归证据，输入 `a b i s` + Ctrl+C）

```
> a                                          ← echo，走 UART 中断
[trap] exception: Breakpoint (cause=3)
  sepc    = 0x8020026a                       ← 注意：是 sepc 不是 mepc 了
  [trap] recoverable: breakpoint hit, skipping (sepc += 2)   ← c.ebreak 压缩指令
> [info] rx_irq=1 (IRQ-driven RX works)
> [csrs] sstatus=0x6022  sie=0x220  sip=0x0  ← 's' 命令看 S 模式开关
> [riscvPrj] bye, halting (uptime 3697 ms)   ← SBI 定时器在后台走了 3.7 秒
```

输入 `e`（非法指令，注意它比 'b' 多一跳，§4 讲为什么）：

```
[trap] exception: Illegal instruction (cause=2)
  stval   = 0xffffffff                       ← 我们故意嵌的坏指令，一字不差
  [trap] recoverable: skipping illegal instruction (sepc += 4)
```

## 2. 为什么需要 OpenSBI —— 世界的样子变了

### 2.1 M4 的世界（你已经懂的）

```
QEMU 开机 ──▶ 0x80000000: 你的内核 (M 模式)
                 │
                 ├── 自己摸 CLINT 定时器
                 ├── 自己接 PLIC 门铃
                 └── 所有 trap 直接到你手里
```

你是**独栋小屋的户主**：水电表（mtimecmp）、户口本（mhartid）都在你自己抽屉里，
随便翻。门铃（trap）只有一种：全响到你耳朵。

### 2.2 M5 的世界

```
QEMU 开机 ──▶ 0x80000000: OpenSBI (M 模式) ← 物业
                  │  把楼收拾好：水表电表归它管，决定哪些门铃线接给你
                  ▼ 敲门："入住吧"（a0=你的房号, a1=小区图纸=dtb）
              0x80200000: 你的内核 (S 模式) ← 住户
                  │
                  ├── 要动水电？填申请单（ecall → SBI）
                  └── 门铃只响物业接给你的那几条（委派）
```

**你降级了，但这是刻意的** —— 见 2.4。

### 2.3 学 M5 的意义：你正在变成 Linux

QEMU 上启动 Linux riscv64 就是这条完全相同的链路：
**OpenSBI (M) → Linux 内核 (S) → 应用 (U)**。M5 之后我们和 Linux 只差一个"规模"。
M8 里程碑你能亲眼看 Linux 走完你走过的路。

### 2.4 为什么要分层（设计动机）

如果应用/内核/机器资源不分层：任何一段代码都能关掉闹钟、读到别的程序的内存、
摸硬盘控制器 —— 一个 bug 就能搞死整台机器。
分层的本质：**把"危险的事"集中在最 trustworthy 的一层（M/物业）**，
中间层（S/你）只能通过"申请单"（ecall）间接办事，最外层（U/应用，M7 再见）
连申请单都得递给你。每一层都是上一层的护栏。

## 3. CSR 换名对照 —— 只记 5 个名字

M4 学过的 CSR，M5 全部"前缀换 m→s"，**逻辑一模一样**：

| 你会的 | 现在叫 | 干什么（一句话） |
|--------|--------|------------------|
| mtvec | **stvec** | 门铃总机：trap 进来跳哪 |
| mscratch | **sscratch** | 抽屉里藏着 trap_frame 指针（csrrw 交换技巧不变） |
| mret | **sret** | 处理完门铃，回去继续干活 |
| mepc / mcause / mtval | **sepc / scause / stval** | 出事地点 / 事由 / 证据 |
| mie.MTIE(bit7) / MEIE(bit11) | **sie.STIE(bit5) / SEIE(bit9)** | 定时器/外设门铃开关 |

两个**必须小心的位编排陷阱**（名字换了，位子也挪了）：

```
M4: mstatus.MIE 在 bit3   ──▶  M5: sstatus.SIE 在 bit1   ← 别写 bit3！
```

's' 命令的实测值可以自己解码验证：
`sie=0x220` = 0b10_0010_0000 = bit5(STIE) + bit9(SEIE) ✓ 两个门铃开关都开了；
`sstatus=0x6022` = bit1(SIE 总闸开) + bit5(SPIE) + bit8(SPP，上次 trap 来自 S 模式)。

## 4. 三条新规则（S 模式"不能干什么"）

住户的自由比户主少，规矩就三条：

### 规则 1：M 开头的 CSR 摸不得 → 直接非法指令

最常用的一条：`csrr mhartid` 在 S 模式 = 非法指令。
那 hartid 哪来的？**物业敲门时递的**：进门那一刻 `a0` 寄存器里就是 hartid，
`a1` 是设备树（dtb）地址。boot.S 一开始就把 a0 当 hartid 用。

### 规则 2：定闹钟只能填申请单 —— 这就是系统调用

M4 你自己写 `mtimecmp`（CLINT 的一个盒子）。现在那个盒子在物业手里，
你写它 = 非法指令。怎么办？**填单**：

```
你 (S 模式)                    物业 (OpenSBI, M 模式)
    │
    │ a7=0x54494D45("TIME") a6=0  a0=闹钟时间
    ├──── ecall ────────────▶   看到 ecall → 查表 → 替你写 mtimecmp
    │◀──── mret 回来 ─────────   单子办完了
    ▼
继续跑主循环（1ms 后闹钟响，见 §5）
```

`ecall` = "我要请上层办事"的指令。**这就是系统调用的原型**：
M7 里用户态应用找你要内存/要读文件，递给你的也是 `ecall`，
只不过收单的人是你。M5 你把"物业视角"和"未来内核视角"一次学完了。

### 规则 3：门铃线只有物业接给你的才直接响 —— 委派

物业手里两张接线表（位图），决定哪些 trap 直通你：

**中断表 `mideleg = 0x222`** = 0b10_0010_0010 → bit 1, 5, 9 接给你：

| 门铃 | M4 的号 | M5 的号 | 说明 |
|------|---------|---------|------|
| 软件 IPI | 3 | 1 | 物业核间通信用的，你没开 |
| 定时器 | 7 | **5** | tick 就是它 |
| 外设 (PLIC) | 11 | **9** | UART 接收就是它 |

→ 所以 trap.cpp 里 `cause == 7` 变成 `cause == 5`，`cause == 11` 变成 `cause == 9`。

**异常表 `medeleg = 0xb109`**，翻译成人话：

| 异常 | 接给你？ | 'e'/'b' 命令的完整路径 |
|------|---------|------------------------|
| 断点 ebreak (3) | ✅ 直通 | 按键 → **直接响你家** → scause=3 |
| 非法指令 (2) | ❌ 没接 | 按键 → **先响物业** → 物业不认识这条指令 → 转寄到你家（"重定向"）→ scause=2 |
| 页错误 (12/13/15) | ✅ 直通 | M6 页表配错时你会天天见它们 |
| ecall from S (9) | ❌ | 就是你填的 SBI 申请单，物业收单，不响门铃 |

'重定向'的机制（知道原理就行）：物业收到转不出去的 trap 时，把
`scause/stval/sepc` 三个盒子填好，然后把返回地址**改成你家的门铃总机
（stvec）**再回去 —— CPU 就"恰好"落进你的 trap_entry，拿着填好的单子处理。
**结论：'e' 和 'b' 的 trap 号一样能到你手里，只是 'e' 多绕物业一跳。**

## 5. 定时器的两跳（最容易懵的地方，单独讲）

M4 的一跳：`mtime >= mtimecmp` → 门铃直接响你（cause=7）。
M5 变两跳，因为闹钟开关在物业手里：

```
你(S)                    物业(M)                     硬件(闹钟)
 │ sbi_set_timer(t)       │                            │
 ├─ecall────────────────▶│ 写你房间的 mtimecmp = t ──▶│ 开关拨好
 │ (继续跑主循环)          │                            │ 1ms 后...
 │                        │ ◀── 闹钟响(cause=7 那种) ──┤ 但响的是物业！
 │                        │ 物业看单：哦这是住户的闹钟    │
 │ ◀─ 转告: STIP 置位 ────│ (软件注入, 称"转发")         │
 │ 门铃响你: scause=5      │                            │
 ▼                        │                            │
timer_handler(): 再填一张单 + tick++（下个 1ms 循环往复）
```

为什么非得两跳？**闹钟先响物业是物理事实**（mtimecmp 是 M 模式资源），
物业能做的只是"听到后再捅你一下"（把 STIP 位写进你的 ip 盒子）。
新硬件的 Sstc 扩展才允许住户自己摸闹钟，QEMU 6.2 还没有 ——
所以这恰好是**老版 Linux 的真实路径**，你学的是历史同款。

顺带一个白送的好处：读时间用 `rdtime` 指令（读 time CSR），
单指令原子，M4 手写的防撕裂读法整体退役。

## 6. PLIC：同一个门铃系统，换了插座

PLIC 上每个"门铃插座"叫 **context**。编号规则：`hartid × 2 + (M?0:S)`：

```
              PLIC (0x0c000000)
  源 10=UART ──┤
               ├── context 0 = hart0 的 M 模式 ← 物业自用
               ├── context 1 = hart0 的 S 模式 ← ★你（M4→M5 换的就是这个）
               ├── context 2 = hart1 的 M 模式
               └── context 3 = hart1 的 S 模式
     每个插座独立一份：使能位图 + 阈值 + claim/complete 信箱
```

M4→M5 的实质变化就一行：`ctx = hartid*2 + 1`（M4 是 `+0`）。
claim/complete 地址跟着 ctx 算：`0x0c200000 + ctx×0x1000`。
注意 stride 是 0x1000 不是手册上的 0x2000 —— 这是 M5 最大一个坑，
破案过程在附录 B，正文你只需记住"QEMU 布局 ≠ 芯片手册"。

## 7. 启动流程（boot.S 的三个新东西）

```asm
_start:
    /* 新东西①: a0 就是 hartid —— 物业门口递的信封，不用再读 mhartid */
    la   t0, boot_flag
    li   t1, 1
    amoswap.w.aqrl t2, t1, (t0)   /* 新东西②: 原子交换(A扩展) —— 谁先到谁跑内核 */
    bnez t2, .Lpark
    ...
    csrw stvec, t0                /* 换名: mtvec → stvec */
    csrw sscratch, t0
    call kernel_main              /* a0/a1 原样传下去 = hartid/dtb */
```

- **① 信封**：进门时 a0=hartid、a1=dtb，这是 OpenSBI 的交楼仪式
- **② 原子守卫**：实测物业会把两个 hart 都送进来（先来的先跑），
  两个 hart 并发清 BSS 就乱套了，所以用 `amoswap`（读旧值+写新值一条指令完成，
  谁读到 0 谁获胜）保证只有一个跑。A 扩展第一次见面，M7 多核还要深交
- **③ 门牌 0x80200000**：物业本体占 0x80000000 起前 2MB，你家门牌往后挪
  （Linux 的 Image 也是 0x80200000，同款惯例）

## 8. 坑与技巧（一句话版，破案过程见附录）

| 坑 | 一句话 |
|----|--------|
| PLIC stride | QEMU 是 0x80/0x1000，别照抄 SiFive 手册的 0x100/0x2000；写错的症状是"配置读回全对但中断永不来" |
| printf 不支持 `%02X` | 会打印字面量且易被 grep 过滤，曾让三组对照实验全部误判 —— **调试手段本身要先校准** |
| 测试要延迟喂入 | `(sleep 4; printf 'abis\x03') \| timeout 12 qemu ...`，启动瞬间灌的字符会丢 |
| sstatus.SIE 在 bit1 | 不是 mstatus 的 bit3，抄位图前先对表 |

## 9. 验证（可复现）

```bash
make clean && make
# 全功能: echo/断点/rx统计/CSR查看/停机
(sleep 4; printf 'abis\x03') | timeout 12 qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
# 非法指令(经物业重定向)
(sleep 4; printf 'e\x03')    | timeout 10 qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
```

自检问题（合上笔记回答）：
1. 'b' 和 'e' 触发的 trap，路径差在哪一跳？为什么结果一样？
2. sbi_set_timer 之后到 scause=5 之间发生了什么（两跳）？
3. 为什么 'e' 的门铃线没接给你，你却还能收到？

## 10. 下一步（M6：Sv39）

satp 寄存器开分页 → 三级页表 → 页错误（12/13/15，已委派给你）终于有用武之地。
前置小任务：printf 补宽度格式（%02X/%08x）。

---

## 附录 A：源码导览

不在这里重复贴全文（文件自带逐行注释），每个文件读它学什么：

| 文件 | 精读点 |
|------|--------|
| `arch/boot.S` | 信封(a0/a1)、原子守卫、stvec/sscratch（60 行，最值得通读） |
| `arch/trap.S` | 和 M4 逐行对比 —— 除 CSR 名外零差异，体会"套路模式无关" |
| `lib/sbi.cpp` | ecall 内联汇编：寄存器钉扎 + "+r" 双向 + memory clobber |
| `kernel/timer.cpp` | 两跳的"你"侧：填单 + sie.STIE + sstatus.SIE |
| `kernel/plic.cpp` | context 计算 + stride 常量（注释里有完整踩坑记录） |
| `kernel/trap.cpp` | 5/9 换号 + 重定向注释 |
| `kernel/main.cpp` | kernel_main(hartid, dtb) 接信封 + 's' 命令 |

## 附录 B：PLIC stride 破案实录（选修，当侦探故事读）

**现象**：S 模式内核启动后 tick 正常（说明 trap 机制没坏），但 UART 中断
永不到达：`pend=0x400`（PLIC 挂起有）+ `sie.SEIE` 开了 + **`sip.SEIP=0`**
（门铃没捅到你）。

**破案链（从便宜到贵的排查顺序）**：

1. 探针：主循环定期打印 `sip / PLIC pending / context 使能 / 阈值 / IER`
   → 锁定 `pend 有、SEIP 无`，断点在 PLIC→CPU 注入线
2. 轮询 4 个 context 的 claim 信箱（读完 complete 还回）→ 挂起源挂在
   "另一个地址算出来的 context" 下 → 说明 stride 语义和假设不符
3. `qemu-system-riscv64 -monitor unix:/tmp/qmon` + `info qtree` 直接读
   模型参数 → **`enable-stride = 128 (0x80)`、`context-stride = 4096 (0x1000)`**

**根因**：照抄 SiFive FU540 手册的 0x100/0x2000。写 `enable@0x0c002100`
以为在配 hart0-S，实际写进了 hart1-M 的使能区 —— 而那个地址是合法 MMIO，
读回值就是你写的值，**自检全过、逻辑全对，中断就是不来**。

**旁证**：Linux 内核 `drivers/irqchip/irq-sifive-plic.c` 里
`ENABLE_PER_HART 0x80` / `CONTEXT_PER_HART 0x1000` —— 和 QEMU 一致。

**教训**：外设寄存器布局查设备树/模型参数/内核驱动，别照抄芯片手册；
以及对照实验的观测手段本身要先校准（%02X 教训）。
