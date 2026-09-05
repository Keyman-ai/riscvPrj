# RISC-V 学习计划：从 OS 开发者到 RISC-V 专家

> 你的起点：已经用 aarch64 从零写过裸机 OS（myos：UART、异常、GICv3+定时器、
> 多任务、信号量、Shell、MMU）。本计划**不是从零教起**，而是帮你把已有能力
> 迁移到 RISC-V，再逐层深入直到专家级。
>
> 学习载体：本目录 `riscvPrj` —— 一个从零手写的 riscv64 裸机内核，每个阶段
> 都有可运行的 QEMU 演示。全部代码手写，无 libc，与 myos 同风格。

---

## 总体路线（约 12 个月）

| 阶段 | 主题 | 周期 | 里程碑（riscvPrj） |
|------|------|------|-------------------|
| 0 | 环境与生态入门 | 1 周 | 工具链 + QEMU 跑通 hello |
| 1 | 指令集 ISA 精通 | 2–3 周 | 手写汇编练习、手算指令编码 |
| 2 | 裸机启机（M 模式） | 3–4 周 | M1–M4：UART/异常/定时器/PLIC |
| 3 | 特权架构 + 虚拟内存 | 3–4 周 | M5–M6：S 模式 + Sv39 |
| 4 | OS 移植 + Linux 内核 | 4–6 周 | M7：riscvPrj 完整 OS + Linux 启动 |
| 5 | 专家进阶方向 | 持续 | 选择一个方向深挖 + 社区贡献 |

原则：**70% 动手，30% 读规范**。每个阶段结束必须能在 QEMU 上演示成果，
并在 `docs/` 里写一篇总结（迁移 myos 时你已经证明了这是最快的学法）。

---

## Phase 0 — 环境与生态入门（1 周）

**目标**：建立 RISC-V 全局观，工具链就绪，能读懂规范结构。

**内容**
- 安装：`gcc-riscv64-unknown-elf`、`qemu-system-riscv64`（qemu-system-misc）、
  `gdb-multiarch`、`spike`（riscv-isa-sim）、`riscv-tests`
- RISC-V 历史与生态：起源（Berkeley 2010）、RISC-V International、
  指令集**模块化**思想（I/M/A/F/D/C/Zicsr/Zifencei/Zba/Zbb…）
- 规范体系：Unprivileged Spec（20191213 → 20240411）、Privileged Spec
  （20211203 → 20231205）、ABI 规范（riscv-elf-psabi-doc）、SBI 规范
- 与 aarch64/x86 的三大差异：
  1. 指令集是**扩展模块化**的，不同 CPU 可裁剪组合
  2. 无标志位寄存器（无 NZCV），比较靠寄存器写回，分支直接测寄存器
  3. 特权模式 M/S/U 是规范明文规定的，模式切换靠 `ecall/mret`

**动手**
- 写 `hello.c`，`riscv64-unknown-elf-gcc -march=rv64gc -O2` 编译，
  `objdump -d` 反汇编，逐条读懂
- 用 `qemu-system-riscv64` 以 user 模式或裸机跑通它

**检验**：能向别人讲清 RISC-V 模块化指令集与 aarch64 固定指令集的区别。

---

## Phase 1 — 指令集 ISA 精通（2–3 周）

**目标**：把 RV64GC 指令集变成"母语"，看汇编如看 C。

**内容**
- RV64I 基础指令全集：算术/逻辑/移位、访存（LB/LH/LW/LD/SB/SH/SW/SD）、
  分支（BEQ/BNE/BLT/BGE + U 型 JAL/JALR）、CSR 指令族
- **指令编码格式**：R / I / S / B / U / J 六种，立即数编码规则
  （重点是 B/J 型的立即数拆分、S 型与 I 型的差异）——这是专家基本功
- 伪指令：`li`（扩展为多条）、`la`（AUIPC+ADDI）、`mv`、`not`、`neg`、`call`、`tail`、`nop`
- ABI 寄存器约定：x0–x31 与 a0–a7 / t0–t6 / s0–s11 / ra / sp / gp / tp 映射，
  栈帧布局、调用约定（参数/返回值/被调用者保存）
- M 扩展：MUL/MULH/MULHSU/MULHU/DIV/DIVU/REM/REMU 及除法溢出语义
- A 扩展：LR/SC（原子读改写原语）、AMOSWAP/AMOADD 等，与 aarch64
  LDXR/STXR 对比
- F/D 浮点：f0–f31、NaN-boxing 规则、浮点 CSR（frm/fcsr）
- C 压缩指令：16 位编码、指令对齐规则、哪些指令被压缩

**动手（riscvPrj/asm/）**
- 手写汇编：递归（阶乘/斐波那契）、字符串函数（strlen/strcmp）、
  位操作（popcount/clz）
- 手算 10 条指令的机器码，再用 `objdump` 验证
- 用 `objdump -d` 反汇编自己的 C 代码，练习"看汇编还原 C"

**资源**
- Unprivileged Spec 第 2–12 章（逐章读）
- RISC-V 汇编 Cheat Sheet（five-embeddev.github.io/riscv-isa-manual）
- 《RISC-V 手册》（Patterson & Waterman，中文版）
- 寄存器约定表：riscv-elf-psabi-doc 的 calling convention 章节

**检验**：随机给一条指令（如 `bge x5, x6, label`）能写出编码；
给一段汇编能还原出等价的 C。

---

## Phase 2 — 裸机启机：M 模式（3–4 周）★ riscvPrj 核心

**目标**：riscvPrj 在 QEMU virt 机器上从加电到完整跑起来，这一阶段
把 myos 的"UART + 异常 + 定时器"全部在 RISC-V 上重写一遍。

**内容**
- QEMU virt 内存布局（背下来）：
  - DRAM：`0x8000_0000`（复位入口，`-bios none` 时）
  - UART 16550：`0x1000_0000`
  - CLINT（定时器）：`0x0200_0000`（mtime `0x200_4000`、mtimecmp）
  - PLIC（外设中断）：`0x0c00_0000`
  - `-machine virt` 文档：`docs/system/riscv/virt.rst`
- 启动流程：链接脚本（链接到 0x80000000）→ 启动汇编（初始化各 hart 栈、
  清 BSS）→ 跳 C 入口
- UART 16550 驱动 + 自己的 `printf`（从 myos 移植，寄存器不同、逻辑相同）
- 异常/中断（对比 aarch64）：
  - `mtvec`（VS. VBAR_EL1）、`mcause`、`mepc`、`mstatus`、`mtval`、`mret`
  - **RISC-V 没有向量表，只有一个 trap 入口**，靠 mcause 分发
  - 完整现场保存/恢复（对比 myos 的 16 项异常向量表保存全部寄存器）
- 定时器：CLINT `mtime/mtimecmp`，`mie.MTIE` + `mstatus.MIE`，中断处理
- PLIC：外设中断控制器（对比 GICv3），UART RX 中断 + claim/complete 流程

**动手（riscvPrj 里程碑）**
- **M1**：UART 驱动 + printf，打印启动横幅
- **M2**：trap 处理：非法指令触发异常，保存现场并打印 mcause/mepc/寄存器
- **M3**：机器定时器中断：uptime 毫秒计数
- **M4**：PLIC + UART 接收中断：echo 回显

**与 aarch64 对照表**（写进你的笔记）

| 概念 | aarch64 (myos) | RISC-V |
|------|---------------|--------|
| 异常入口 | VBAR_EL1 向量表（16 项） | mtvec 单一入口 + mcause 分发 |
| 中断控制器 | GICv3 | CLINT(定时器) + PLIC(外设) |
| 定时器 | ARM Generic Timer | CLINT mtime/mtimecmp |
| 保存现场 | 异常代码保存 x0–x30 | trap 代码保存 x1–x31 |
| 模式切换 | EL0/EL1 异常级别 | U/S/M + ecall/mret |
| 状态寄存器 | NZCV/PSTATE | mstatus/mcause/mepc |

**资源**
- Privileged Spec 第 3 章（M 模式）、第 4 章（CSR）
- QEMU 源码 `hw/riscv/virt.c`（看内存布局真相）
- 《RISC-V 体系结构编程与实践》（奔跑吧 Linux 内核系列，第 1–6 章）
- myos 源码直接对照移植（你最熟的材料）

**检验**：riscvPrj 在 QEMU 上跑出 M1–M4；能徒手画出
`ecall → M 模式 trap → mret` 的完整寄存器流转。

---

## Phase 3 — 特权架构 + 虚拟内存（3–4 周）

**目标**：理解 M/S/U 三模式与虚拟内存，把内核从 M 模式挪到 S 模式
（贴近真实世界：Linux 跑在 S 模式，M 模式归 OpenSBI）。

**内容**
- 特权模式：M/S/U、`mstatus.MPP`、模式切换路径（ecall 进 trap、mret 返回）、
  `medeleg/mideleg` 异常委派（哪些异常交给 S 处理）
- CSR 访问指令：`csrr/csrw/csrrw/csrrwi/csrrsi/csrrci` 及原子读改写语义
- **SBI（Supervisor Binary Interface）**：OpenSBI 固件（qemu 自带
  `opensbi-riscv64-generic-fw_dynamic.bin`）
  - `sbi_console_putchar`、`sbi_set_timer`、`sbi_hart_start`、`sbi_ecall`
  - 了解 OpenSBI 源码 `platform/generic`（可选）
- 虚拟内存：`satp` CSR + Sv39（39 位虚拟地址，3 级页表）/ Sv48
  - 把 myos 的 MMU 页表代码移植过来（PTE 标志位不同，但思想相同）
  - `SFENCE.VMA`（TLB 失效，对比 aarch64 的 TLBI）
  - 页表遍历：虚拟地址 → VPN[2:0] → PPN

**动手（riscvPrj 里程碑）**
- **M5**：用 OpenSBI 固件启动 riscvPrj，内核跑在 S 模式
  （trap 走 `stvec`，定时器走 SBI 或 S 模式 timer 中断）
- **M6**：Sv39 页表 + 打开 MMU，内核在虚拟地址上运行
  （页分配器可直接从 myos 移植）
- （可选 M6.5）U 模式 + 用户态程序 + `ecall` 系统调用雏形

**资源**
- Privileged Spec 第 1–2 章（模式与地址翻译）、第 5 章（S 模式）、
  第 10 章（Sv39/Sv48）
- SBI Spec（github.com/riscv-non-isa/riscv-sbi-doc）
- OpenSBI 源码（github.com/riscv-software-src/opensbi）

**检验**：能画出一次 U→S→M 的中断完整路径（谁保存现场、谁委派、
谁处理、谁返回）；riscvPrj 带 MMU 在 S 模式运行。

---

## Phase 4 — OS 级掌握（4–6 周）

**目标**：riscvPrj 功能对齐 myos，并能在 QEMU 上启动真实 Linux。

**内容**
- **把 myos 全部特性移植到 RISC-V**：
  - 多任务调度（上下文切换：save/restore 换成 RISC-V 寄存器 + `mret`/`sret`）
  - 信号量（关中断原子性，CSR 读写 vs aarch64 的 DAIF）
  - 交互式 Shell、kmalloc（内存管理代码几乎可直接移植）
- **Linux 内核的 RISC-V 后端**：
  - `arch/riscv` 目录结构、启动流程（`head.S` → `start_kernel`）
  - 设备树（DTB）解析：`/soc/serial@10000000`、`/cpus`、`/memory`
  - 平台驱动：riscv 串口（8250/16550）驱动
  - `make ARCH=riscv defconfig` 编译，配合 OpenSBI 在 QEMU virt 上
    启动 Linux + initramfs（`-bios opensbi -kernel Image`）
- 阅读 Berkeley Boot Loader（riscv-pk）了解 S 模式启动细节（可选）

**动手（riscvPrj 里程碑）**
- **M7**：riscvPrj = 调度器 + 信号量 + Shell + MMU，功能与 myos 对齐
- **M8**：QEMU 启动编译出的 Linux riscv64 内核，进入 shell

**资源**
- Linux 源码 `arch/riscv/`
- 《奔跑吧 Linux 内核》riscv 相关章节
- QEMU `docs/system/riscv/virt.rst` 的启动示例命令

**检验**：riscvPrj 演示与 myos 同级别的功能；能解释
`OpenSBI → Linux head.S → start_kernel` 每一步发生了什么。

---

## Phase 5 — 专家进阶（持续，选 1–2 个方向）

到这一步你已经具备"RISC-V 平台 OS 工程师"能力。专家之路按兴趣分叉：

**方向 A：CPU 与微架构**
- 用 Chisel/Verilog 写一个可运行的 RISC-V CPU
  - 《手把手教你设计 CPU：RISC-V 处理器》（胡振波）
  - 阅读 PicoRV32、siyuanchen/riscv-soc、香山（XiangShan，国产生态标杆）
- 流水线、分支预测、缓存与一致性基础

**方向 B：向量与计算加速**
- RVV 1.0（riscv-v-spec）：vlen、LMUL、vsetvli、mask 编程
- GCC/LLVM 自动向量化、手写 SIMD 优化
- 在 QEMU `-cpu rv64,v=true` 上实践

**方向 C：虚拟化、安全与可信执行**
- H 扩展（Hypervisor）、KVM/riscv、Xen/riscv
- PMP（物理内存保护）、IOMMU（Smmtt）、TEE：Keystone / TockOS
- 侧信道与 Spectre 在 RISC-V 的实现差异

**方向 D：工具链、形式化与验证（最"专家"的一条路）**
- LLVM RISC-V backend、GNU toolchain 的 linker relaxation（RVC）
- `riscv-tests`、`riscv-arch-test`（一致性测试套件）
- `sail-riscv`（形式化语义模型）、`riscv-dv`（随机指令生成器）
- 向 riscv-isa-manual / riscv-tests 提 PR —— 这是成为专家的标志

**社区参与（贯穿全程）**
- RISC-V International（riscv.org，参与 ISA 草案讨论与投票）
- CNRV（cnrv.io）：中文 RISC-V 社区、邮件列表、线下活动
- 泰晓科技（TinyLab）、《RISC-V 体系结构编程与实践》作者群
- GitHub：riscv-isa-manual、riscv-tests、opensbi、qemu、linux
- 年度活动：RISC-V Summit China / 玄铁杯（T-Head）等

**检验**：完成一个拿得出手的 mini 项目（自己写的 CPU / 优化过的
向量库 / 一个 upstream patch / 一篇深度技术文章）。

---

## 每周节奏建议

- 工作日：2–3 小时，**动手为主**（写代码、跑 QEMU、看反汇编）
- 周末：4–6 小时，读规范 + 写 `docs/` 总结
- 每完成一个里程碑：写一篇"我如何实现的"笔记（myos 的做法，很有效）
- 遇到不懂的：先查规范原文，再查中文资料（CNRV/泰晓），最后上
  riscv-tools / opensbi / qemu 的 GitHub issue

## 资源清单速查

**规范（必读，优先级最高）**
- Unprivileged Spec：https://github.com/riscv/riscv-isa-manual
- Privileged Spec：同上仓库 `src/priv.tex` 编译版
- ABI：https://github.com/riscv-non-isa/riscv-elf-psabi-doc
- SBI：https://github.com/riscv-non-isa/riscv-sbi-doc

**书**
- 《RISC-V 手册》（Patterson & Waterman）—— 入门
- 《RISC-V 体系结构编程与实践》（奔跑吧 Linux 内核系列）—— 实战
- 《手把手教你设计 CPU：RISC-V 处理器》—— CPU 方向
- 《RISC-V 开放架构设计论》（李德磊）—— 体系结构视角

**在线**
- riscv.org / CNRV（cnrv.io）/ 泰晓科技（tinylab.org）
- five-embeddev.github.io/riscv-isa-manual（规范速查）
- RISC-V Cheatsheet（指令编码速查）
- godbolt.org（选 riscv64 gcc 看汇编）

**工具链/验证**
- QEMU（riscv64-softmmu）、Spike（riscv-isa-sim）
- riscv-tests / riscv-arch-test / riscv-dv / sail-riscv
- gdb-multiarch + OpenOCD（后续接真机调试）

---

## 一句话总结

> 用 myos 的方法论（从零写、能跑、写笔记）在 riscvPrj 上重演一遍
> RISC-V 的启机之路，然后用 Linux/OpenSBI 验证理解，
> 最后选择一个方向进入社区 —— 这就是从"会写 aarch64 OS"
> 到"RISC-V 专家"的完整路径。
