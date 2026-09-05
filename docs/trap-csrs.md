# RISC-V 异常/中断寄存器详解（trap 机制）

> 依据：RISC-V Privileged Spec v1.12 (20211203) 第 3 章（Machine-Level CSRs）
> 与第 4 章（Trap Handling）。本文以 RV64 + M 模式为主线，S 模式对照见文末。
> 配套阅读：`docs/m3-notes.md`（实际应用）、`docs/asm-cheatsheet.md`（指令）。

---

## 1. 体系结构背景

### 1.1 术语：异常、中断、trap

| 术语 | 英文 | 含义 | 特点 |
|------|------|------|------|
| 异常 | Exception | 由**当前指令**引起（非法指令、页错误、ecall） | 同步（synchronous），精确（precise） |
| 中断 | Interrupt | 由**外部事件**异步引起（定时器、外设） | 异步（asynchronous），在指令边界响应 |
| trap | Trap | 异常 + 中断的统称（规范用语） | 统一处理入口 |

**体系结构要点**：
- 同步异常由当前指令触发，**立即**进入处理（优先于任何挂起的中断）
- 中断只能在**指令边界**被响应（精确中断模型——处理完当前指令才进 trap）
- 这套"精确异常"模型保证：trap 返回后能无缝恢复被中断的指令流

### 1.2 特权模式与 CSR 访问规则

RISC-V 有三级特权：**M（机器）> S（监督）> U（用户）**。

- M 模式可访问**全部** M 级 CSR
- S 模式访问 M 级 CSR → **非法指令异常**（mcause=2）
- 只读 CSR（如 `mhartid`）写入被忽略
- **WARL 字段**（Write Any values, Reads Legal values）：写入不支持的值被忽略，
  读回的一定是合法值——给低成本的硬件实现留了自由度
- **WPRI 字段**（Write Preserve, Read Ignore）：保留位，写入保持原值，读出忽略

### 1.3 设计哲学（为什么这样设计）

1. **硬件只做最小事，现场保存全归软件**：
   RISC-V 进 trap 时硬件只记录 4 个寄存器（mepc/mcause/mtval/MPIE），
   **不自动保存任何通用寄存器**（对比 aarch64 有部分硬件现场保存）。
   好处：软件可按需保存（快速路径只存用到的寄存器），trap 开销可控
2. **精确异常**：mepc 指向出错指令、mcause/mtval 记录原因与细节，
   这是虚拟化、调试器、恢复机制的地基
3. **委派机制**（medeleg/mideleg）：让 M 固件、S 内核、Hypervisor
   各层只处理属于自己的 trap，互不干扰
4. **WARL**：规范定义语义，实现可裁剪（如不支持的 cause 码），
   软件必须能容忍读回值与写入值不同

---

## 2. 寄存器总览（M 模式）

| CSR | 地址 | 名称 | 权限 | 谁写 |
|-----|------|------|------|------|
| `mstatus` | 0x300 | Machine Status | 读/写 | 软件（硬件改其中 3 位） |
| `mtvec` | 0x305 | Machine Trap Vector | 读/写 | 软件（启动时一次） |
| `mscratch` | 0x340 | Machine Scratch | 读/写 | 软件（trap 暂存） |
| `mepc` | 0x341 | Machine Exception PC | 读/写 | **硬件写，软件可改** |
| `mcause` | 0x342 | Machine Cause | 读（WARL） | 硬件 |
| `mtval` | 0x343 | Machine Trap Value | 读（WARL） | 硬件 |
| `mie` | 0x304 | Machine Interrupt Enable | 读/写 | 软件 |
| `mip` | 0x344 | Machine Interrupt Pending | 读 | 硬件 |
| `mhartid` | 0xF14 | Hart ID | 只读 | 硬件 |
| `medeleg` | 0x302 | Machine Exception Delegation | 读/写 | 软件（M5 用） |
| `mideleg` | 0x303 | Machine Interrupt Delegation | 读/写 | 软件（M5 用） |

**分类记忆**：
- `mtvec/mepc/mcause/mtval` = **trap 四件套**（去哪、哪被打断、为什么、细节）
- `mstatus/mie/mip` = **状态与开关**（总闸、分闸、来电显示）
- `mscratch` = 软件暂存盒；`mhartid` = 我是谁
- `medeleg/mideleg` = 委派（把 trap 交给 S 模式处理）

---

## 3. 逐个详解

### 3.1 `mtvec`（0x305）— trap 入口地址

```
63                      2  1  0
┌────────────────────────┬────┐
│ BASE (trap 入口地址)    │MODE│
└────────────────────────┴────┘
```

| MODE | 值 | 行为 |
|------|----|------|
| Direct（直接） | 00 | 所有 trap 都跳 BASE |
| Vectored（向量） | 01 | 异常跳 BASE；**中断**跳 BASE + 4×cause（按原因码跳表） |

- BASE 必须按 `IALIGN` 对齐（有 C 扩展 = 2 字节，无 = 4 字节）；
  直接模式要求 BASE 4 字节对齐（低 2 位是 MODE 字段）
- 本工程用**直接模式**：单一入口，靠 mcause 在软件里分发（M3 笔记 2.5）
- **向量模式**在"每个中断源要独立快速入口"时才有优势（Linux 早期启动用）

### 3.2 `mepc`（0x341）— 被打断的位置

- **异常**：出错指令的地址（ecall/ebreak 指向该指令本身）
- **中断**：被中断的那条指令的地址（返回后从这条继续）
- **软件可改**：`mepc += 4` 跳过非法指令（我们的 M1 演示）；
  任务切换时改 mepc 实现"从另一任务继续"
- 对齐要求：与 IALIGN 一致，写入非法值被忽略（WARL）
- **易错**：mret 用的是硬件 CSR 值——修改后必须写回（M1 坑 2）

### 3.3 `mcause`（0x342）— 原因码

```
63                    62                      0
┌──────────────────────┬──────────────────────┐
│ Interrupt (1=中断)    │ Exception Code       │
└──────────────────────┴──────────────────────┘
```

- bit 63（RV64 的最高位 XLEN-1）：**1 = 中断**，0 = 异常
- 其余位：原因码。注意**原因码字段是 63 位宽**，标准原因码只用到低 12 位，
  高位为平台自定义留白
- 完整表见第 5 节

### 3.4 `mtval`（0x343）— 补充信息（按 cause 不同而不同）

| 场景 | mtval 内容 |
|------|-----------|
| 取指/访存地址错位、访问故障、页错误 | 出错（虚拟）地址 |
| 非法指令 | 该指令的编码（帮助诊断/模拟） |
| 断点 | 断点指令地址 |
| 其他（含 ecall、多数中断） | 0（或未定义） |

**易错**：别假设 mtval 总是地址——按 mcause 查表解释。

### 3.5 `mscratch`（0x340）— 软件暂存盒

- 硬件不读不写它，纯粹给 trap 代码当"第一桶金"
- **经典用法**（我们的 trap.S）：预置为 `&trap_frame`，
  入口用 `csrrw t0, mscratch, t0` 原子交换——t0 变帧指针、
  原 t0 进 mscratch，不破坏任何用户寄存器（M3 笔记 3.5）
- Linux/OpenSBI 用它存 hart 的 per-CPU 结构指针

### 3.6 `mstatus`（0x300）— 状态与开关（RV64）

```
63    35:34 33:32     22 21 20 19 18 17 16:15 14:13 12:11 10:9 8  7  6  5     3     1
┌─────┬─────┬─────┬───┬──┬──┬──┬──┬──┬──┬────┬────┬────┬───┬──┬──┬──┬──┬───┬───┬───┐
│ SD  │UXL  │SXL  │TSR│TW│TVM│MXR│SUM│MPRV│XS  │FS  │MPP │VS │SPP│MPIE│UBE│SPIE│MIE│SIE│
└─────┴─────┴─────┴───┴──┴──┴──┴──┴──┴────┴────┴────┴───┴──┴──┴──┴──┴───┴───┴───┘
```

**本工程相关的核心 3 位**：

| 位 | 字段 | 含义 |
|----|------|------|
| 3 | MIE | 机器模式**全局中断总闸**（0=所有机器中断被屏蔽） |
| 7 | MPIE | trap 前的 MIE 值——硬件自动存，mret 自动恢复 |
| 12:11 | MPP | trap 前的特权模式（11=M，01=S，00=U） |

**trap 发生时硬件对这 3 位做的事**（原子）：

```
MPIE ← MIE        (记住"刚才开着吗")
MIE  ← 0          (关总闸: 禁止嵌套)
MPP  ← 进入 trap 前的特权模式
```

**mret 做的事**（镜像）：

```
MIE ← MPIE        (恢复总闸)
MPIE ← 1
特权模式 ← MPP;  MPP ← U (最低特权)
```

> 这就是 M3 实测 `mstatus=0x1880` 的来历：
> `0x1800`(MPP=11) + `0x80`(MPIE=1，进 trap 前开着) + MIE=0（被硬件关了）。

**其余位（了解即可，后面阶段会用到）**：

| 位 | 字段 | 含义 |
|----|------|------|
| 17 | MPRV | Load/Store 按 MPP 的特权执行（模拟用户访存用） |
| 18 | SUM | 允许 S 模式访问 U 模式页（M6 开 MMU 后用） |
| 19 | MXR | 允许从数据页取指 |
| 20/21/22 | TVM/TW/TSR | S 模式执行 sfence.vma/wfi/sret 时是否陷入 M（虚拟化用） |
| 14:13 | FS | 浮点单元状态（Off/Initial/Clean/Dirty） |
| 16:15 | XS | 扩展状态汇总 |
| 63 | SD | FS/XS/VS 的"脏"汇总（只读） |
| 33:32, 35:34 | SXL/UXL | S/U 模式的 XLEN（RV64 上可配成 32/64，M6 前保持 64） |
| 8, 5, 1 | SPP/SPIE/SIE | S 模式的对应位（M 模式可读写，M5 后 S 内核会用） |

### 3.7 `mie` / `mip`（0x304 / 0x344）— 中断分闸与来电显示

```
63           11       7       3       1
┌────────────┬───────┬───────┬───────┬──────┐
│ (其他/平台)  │MEIP/  │MTIP/  │MSIP/  │SSIP/ │
│            │MEIE   │MTIE   │MSIE   │SSIE  │ ...
└────────────┴───────┴───────┴───────┴──────┘
```

| 位 | 中断 | mie（软件写） | mip（硬件写） |
|----|------|-------------|--------------|
| 3 | 机器软件中断 | MSIE | MSIP（核间通信用） |
| 7 | 机器定时器 | MTIE | MTIP（CLINT 置位） |
| 11 | 机器外设 | MEIE | MEIP（PLIC 置位，M4 用） |
| 1/5/9 | S 模式对应 | SSIE/STIE/SEIE | ...（M5 后 S 内核用） |

**中断响应条件（层层"与"）**：

```
mip 某位 = 1  (事件发生)
  且 mie 对应位 = 1  (分闸开着)
  且 mstatus.MIE = 1  (总闸开着)
  → 触发 trap
```

### 3.8 `mhartid`（0xF14）— 我是第几个核

- 只读；每个 hart 一个，0 起始连续编号
- 启动时用来决定谁引导（boot.S 第一行）、以后 per-CPU 数据索引

### 3.9 `medeleg` / `mideleg`（0x302/0x303）— 委派（M5 预告）

- 按位对应 cause：某位 = 1 → 该 trap **直接由 S 模式处理**
  （硬件写 sepc/scause/stval，跳 stvec，不再进 M 模式）
- 为什么需要：OpenSBI（M 模式）只想管 M 的事，把常见异常/中断
  **委派**给 S 模式内核——减少 M↔S 往返开销
- 规则：委派只能往**更低特权**走（M→S→U）；S 模式有对应的 sdeleg（v1.12 起）
- 易错：**S 模式开中断前必须先设置委派位**，否则中断全砸向 M 模式

---

## 4. 硬件 trap 流程（规范定义的精确序列）

### 4.1 异常/中断发生时（未委派，M 模式处理）

```
① mepc   ← 当前 PC（异常=出错指令；中断=被中断指令）
② mstatus.MPIE ← MIE;  mstatus.MIE ← 0;  mstatus.MPP ← 当前特权级
③ mcause ← 原因码（中断时 bit63=1）
④ mtval  ← 按 cause 的补充信息（无则 0）
⑤ PC     ← mtvec.BASE（向量模式且为中断时: BASE + 4×cause）
```

### 4.2 返回：`mret`

```
① PC     ← mepc
② 特权级 ← MPP;  mstatus.MPP ← U
③ mstatus.MIE ← MPIE;  mstatus.MPIE ← 1
④ 实现相关: 冲刷流水线/指令缓存（保证新指令流可见）
```

**注意**：`mret` 是"特权返回指令"——S 模式的内核用 `sret`，
U 模式代码执行 `mret` 会触发非法指令异常。

### 4.3 中断优先级与嵌套

- **固定优先级**（规范定义，从高到低）：
  `MEI > MSI > MTI > SEI > SSI > STI > UEI > USI > UTI`
  （机器级 > 监督级 > 用户级；同级内 外部 > 软件 > 定时器）
- 同时挂起多个中断时，取最高优先级者（QEMU 遵循此顺序）
- **嵌套**：硬件进 trap 时关 MIE，天然禁止嵌套；软件可在 handler 里
  重新开 MIE 实现嵌套，但必须保证：每个嵌套层有独立栈帧、现场保存区，
  否则返回时现场被覆盖——本工程（及大多数教学内核）不做嵌套

---

## 5. 原因码全表（mcause）

### 5.1 异常（bit63 = 0）

| 码 | 名称 | 触发者 |
|----|------|--------|
| 0 | 指令地址未对齐 | 跳转到非对齐地址 |
| 1 | 指令访问故障 | 取指地址不可访问 |
| 2 | 非法指令 | 未定义指令/非法 CSR 访问（我们的 `e` 演示） |
| 3 | 断点 | ebreak（调试器用） |
| 4 | 加载地址未对齐 | |
| 5 | 加载访问故障 | |
| 6 | 存储/AMO 地址未对齐 | |
| 7 | 存储/AMO 访问故障 | |
| 8 | ecall from U | 用户态系统调用 |
| 9 | ecall from S | 监督态系统调用（M5 后 OpenSBI 用它） |
| 10 | 保留 | |
| 11 | ecall from M | |
| 12 | 指令页错误 | M6 开 MMU 后 |
| 13 | 加载页错误 | |
| 14 | 保留 | |
| 15 | 存储/AMO 页错误 | |
| 16–23 | 保留 | 部分草案用 16/17/18 做 load/store/AMO 细分 |
| 24–31 | 保留 | |
| 32–47 | 自定义 | 实现自定义 |
| ≥64 | 平台 | 平台自定义 |

### 5.2 中断（bit63 = 1）

| 码 | 名称 | 缩写 | 来源 |
|----|------|------|------|
| 1 | 监督软件中断 | SSI | S 模式核间通信 |
| 3 | 机器软件中断 | MSI | M 模式核间通信（CLINT MSIP） |
| 5 | 监督定时器 | STI | S 模式定时器（M5 用 stimecmp/SBI） |
| 7 | **机器定时器** | **MTI** | **CLINT（我们 M3 用的）** |
| 9 | 监督外设中断 | SEI | PLIC 转给 S 模式 |
| 11 | **机器外设中断** | **MEI** | **PLIC（我们 M4 要用的）** |

---

## 6. 与 aarch64 对照（从 myos 迁移视角）

| 概念 | aarch64（myos 用过） | RISC-V | 差异要点 |
|------|---------------------|--------|---------|
| trap 入口 | VBAR_EL1 + 16 项向量表 | mtvec 单入口（可向量模式） | RISC-V 默认软件分发，更简单 |
| 返回地址 | ELR_EL1 | mepc | 语义相同，都要软件改/写回 |
| 原因 | ESR_EL1（编码不同） | mcause | RISC-V 最高位区分中断/异常 |
| 出错地址 | FAR_EL1 | mtval | 内容按 cause 变化 |
| 状态保存 | SPSR_EL1（NZCV/EL/DAIF） | mstatus（MIE/MPIE/MPP） | RISC-V 无标志位，更精简 |
| 中断总闸 | DAIF（PSTATE.I） | mstatus.MIE | 硬件进 trap 自动关，语义一致 |
| 现场保存 | 硬件自动存部分 + 软件 | **全软件**（mscratch 技巧） | RISC-V 更灵活、trap 更快 |
| 中断控制器 | GICv3 | CLINT（定时）+ PLIC（外设） | 分工不同，PLIC 更像简化 GIC |
| 特权层 | EL0/EL1/EL2/EL3 | U/S/M（H 扩展后 S+HS） | 层级数不同，思路相同 |
| 返回指令 | eret | mret/sret | 一致 |

---

## 7. 易错点清单（本工程踩过/将来会踩）

1. **改了 mepc 必须写回 CSR**——mret 用硬件值（M1 坑 2）
2. **开中断前必须配好 mtvec**——否则中断直接跳垃圾地址
3. **只开 mie 不开 mstatus.MIE**——总闸关着，中断永远不进来
4. **mtvec 对齐**：直接模式低 2 位必须 00
5. **mtval 别当地址用**——先查 mcause 再解释
6. **中断路径用不可重入代码**（printf）→ 输出交错/死锁（M3 设计规避）
7. **S 模式开中断前先设委派**（M5）
8. **多核**：mtimecmp/mie 是 per-hart 的，每个 hart 都要配（Phase 3 后）
9. **WARL**：读 mcause 高位别假设为 0（未来扩展可能用）
10. **mepc 对齐**：改 mepc 时保持 IALIGN 对齐，否则 mret 后行为未定义

---

## 8. 参考

- RISC-V Privileged Spec v1.12 (20211203)：
  https://github.com/riscv/riscv-isa-manual
  - §3.1 Machine-Level CSRs（mstatus/mtvec/mepc/mcause/mtval/mscratch/mie/mip）
  - Trap Handling 章节（本文第 4 节的权威来源）
  - §3.1 medeleg/mideleg（委派语义）
- RISC-V Unprivileged Spec（csr 指令的语义：csrr/csrw/csrrw/csrs/csrc）
- 本工程实测：`docs/m3-notes.md`（中断路径）、`docs/m1-notes.md`（坑 1/2）
