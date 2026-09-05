# M5 笔记 — OpenSBI + S 模式

> 目标读者：未来的自己。约定：盒子=寄存器，仓库=内存。
> 全程在 QEMU virt (`-M virt -smp 2 -m 128M`)，OpenSBI v0.9 (QEMU 6.2 自带)。

## 1. 这一阶段做了什么

**一句话**：把内核从"QEMU 直接扔进 M 模式的裸机程序"升级为"由 OpenSBI 固件载入、
跑在 **S 模式**的真正内核"——机器资源（mtimecmp、mhartid）从此碰不到，一切特权
操作改走 SBI 调用，trap 全家桶换 S 前缀 CSR，PLIC 换 S-context。这是 M8（跑 Linux）
的同款架构预演。

### 改动清单

| 文件 | 改动 | 关键点 |
|------|------|--------|
| `run.sh` / Makefile | 去掉 `-bios none` | QEMU 自动先跑 OpenSBI (fw_dynamic) |
| `linker.ld` | `0x80000000` → `0x80200000` | OpenSBI 占前 2MB |
| `arch/boot.S` | M→S CSR + 原子守卫 | `stvec`/`sscratch`；hartid 从 a0 拿 |
| `arch/trap.S` | M→S CSR | `sepc/scause/stval/sstatus`，`sret` |
| `include/kernel/trap.hpp` | 字段改名 | 同上 |
| `lib/sbi.hpp/.cpp` **(新)** | SBI ecall 封装 | 第一个系统调用接口 |
| `kernel/timer.cpp` | CLINT MMIO → SBI | `rdtime` + `sbi_set_timer` + `sie.STIE` |
| `kernel/plic.cpp` | context 0 → hartid*2+1 | + ★stride 坑修复 (0x80/0x1000) |
| `kernel/trap.cpp` | 中断号 7/11 → 5/9 | S 模式只见委派来的中断 |
| `kernel/main.cpp` | `kernel_main(hartid, dtb)` | + `s` 命令看 S CSR |

### 实测输出（回归测试，输入 `a b i s` + Ctrl+C）

```
> a                                    ← IRQ 驱动 echo
[riscvPrj] triggering breakpoint (ebreak)...
[trap] exception: Breakpoint (cause=3)
  sepc    = 0x8020026a
  stval   = 0x0
  sstatus = 0x6120
  [trap] recoverable: breakpoint hit, skipping (sepc += 2)   ← c.ebreak 压缩指令!
[riscvPrj] back from breakpoint
> [info] rx_irq=1 (IRQ-driven RX works)                     ← 中断路径统计
> [csrs] sstatus=0x6022 (SIE=bit1, SPP=bit8)
       sie=0x220 (SSIE=1 STIE=5 SEIE=9)                     ← STIE+SEIE 都开了
       sip=0x0 (pending bits)
> [riscvPrj] bye, halting (uptime 3697 ms)                  ← tick + 停机
```

输入 `e`（非法指令，经 OpenSBI 重定向）：

```
[trap] exception: Illegal instruction (cause=2)
  sepc    = 0x80200208
  stval   = 0xffffffff        ← 我们嵌入的 .word 0xffffffff, 一字不差
  [trap] recoverable: skipping illegal instruction (sepc += 4)
```

## 2. 原理

### 2.1 新的启动链路（和 M4 对比）

```
M4 (-bios none):                          M5 (OpenSBI):
QEMU ROM → 0x80000000 (内核, M 模式)      QEMU ROM → OpenSBI@0x80000000 (M 模式)
                │                             │ 打印 banner, 建 PMP/委派/SBI 运行时
                ▼                             ▼ sret-like 切换
        内核就是机器的主人              内核@0x80200000 (S 模式, "经理"层)
                                        a0=hartid  a1=dtb(0x87000000)
```

OpenSBI banner 里的关键行（每个都是信息源）：

```
Domain0 Next Address      : 0x0000000080200000   ← 内核会被跳到这 (决定 linker.ld)
Domain0 Next Mode         : S-mode               ← 内核进门就是 S 模式
Domain0 Next Arg1         : 0x0000000087000000   ← a1 = dtb 物理地址
Boot HART ID              : 1                    ← 冷启动 hart (但两个 hart 都会进 payload!)
Boot HART MIDELEG         : 0x0000000000000222   ← 委派给 S 的中断
Boot HART MEDELEG         : 0x000000000000b109   ← 委派给 S 的异常
```

### 2.2 委派位图逐位解读（M5 的地图）

**MIDELEG = 0x222 = 0b10_0010_0010** → bit 1, 5, 9 = SSIP, STIP, SEIP。
即**软件/定时器/外设**三种中断委派给 S 模式：发生时直接进我们的 `stvec`，
M 模式（OpenSBI）不掺和。这就是 trap.cpp 里中断号从 7/11 变成 5/9 的原因。

**MEDELEG = 0xb109 = 0b1011_0001_0000_1001**：

| bit | 异常 | 委派? | 走法 |
|-----|------|-------|------|
| 0 | 取指地址不对齐 | ✅ | 直达 stvec |
| 2 | **非法指令** | ❌ | → OpenSBI M trap → 模拟失败 → **重定向**回 stvec |
| 3 | **断点 ebreak** | ✅ | 直达 stvec (所以 'b' 和 'e' 路径不同!) |
| 8 | ecall from U | ✅ | 直达 stvec (M7 用户态系统调用用) |
| 9 | **ecall from S** | ❌ | → OpenSBI 当 SBI 调用处理, mret 回来 (不经过 stvec!) |
| 12/13/15 | 页错误 | ✅ | 直达 stvec (M6 用) |

非法指令的重定向机制（OpenSBI `sbi_trap_redirect`）：OpenSBI 在 M 模式把
`scause/stval/sepc` 写进 S 的 CSR，然后把 `mepc` 改成 **stvec 的值**，
`mstatus.MPP=S`，最后 mret —— CPU 就"回到"了 S 模式，且 PC 落在我们的
trap 入口上。等于 M 模式固件把 trap **转寄**给了 S 模式内核。

### 2.3 M↔S CSR 对照表（trap.S 全部改动 = 换名字）

| 用途 | M 模式 | S 模式 | 备注 |
|------|--------|--------|------|
| trap 向量 | mtvec | stvec | 低 2 位都是模式位 |
| 暂存陷阱帧指针 | mscratch | sscratch | csrrw 交换技巧不变 |
| 返回指令 | mret | sret | S 模式执行 mret 非法, 反之亦然 |
| 出错 PC | mepc | sepc | |
| trap 原因 | mcause | scause | 最高位=中断的约定相同 |
| 附加信息 | mtval | stval | |
| 状态 | mstatus | sstatus | **位编排不同!** |
| 中断使能 | mie.MTIE(7)/MEIE(11) | sie.STIE(5)/SEIE(9) | 位位置变了 |
| 全局总闸 | mstatus.MIE(bit3) | sstatus.SIE(bit1) | ★别想当然写 bit3 |
| 读 hartid | csrr mhartid | **不可能** | S 模式访问 = 非法指令 |
| 读时间 | CLINT mtime MMIO | `rdtime` 指令 | CSR 单指令原子读, 天然防撕裂 |
| 排定时器 | 写 CLINT mtimecmp MMIO | `ecall` → OpenSBI 代写 | M 模式专用 MMIO |

### 2.4 S 模式定时器的"两跳"路径（M5 最绕的地方）

```
我们 (S 模式)                OpenSBI (M 模式)                硬件
────────────                 ────────────────                ────
sbi_set_timer(t) ──ecall──▶ 拿到 EID=0x54494D45("TIME") FID=0
                             写本 hart mtimecmp = t ────────▶ mtimecmp=t
   (继续跑主循环)                                            mtime 追上 t
                                            MTIP 置起 ◀──────┘
                             trap 进 M 模式 (OpenSBI 的 mtvec)
                             "该给 S 模式注入 STIP 了"
                             软件置 mip.STIP ──────────────▶ S 模式看见
scause=5 trap ◀━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ sip.STIP=1
timer_handler(): 再排闹钟 + tick++
```

为什么要两跳？mtimecmp 只能 M 模式写（我们写直接非法指令），而定时器到期
先产生的是 M 模式中断（MTIP）。OpenSBI 的定时器中断处理函数干的事就是
"把这次到期**转发**成 S 模式可见的 STIP"。Sstc 扩展（新硬件）才有 stimecmp
让 S 模式直接写，QEMU 6.2 的 virt 还没有，走的就是这条经典路径——
**这恰好是老 Linux 的真实写照**。

### 2.5 PLIC context 模型

```
                PLIC (0x0c000000)
中断源 0..126 ──┤ 优先级[128]     (每源一个 word, 0=禁用)
                │ pending[128]    (硬件写, 只读)
                │ ┌─ context 0 = hart0 M  (OpenSBI 自己用)
                ├─┤  context 1 = hart0 S  ← 我们 (内核跑 hart 0)
                │ └─ context 2 = hart1 M
                └── context 3 = hart1 S  (闲着)
每个 context 一份: enable[128 bit] + threshold(3bit) + claim/complete
```

- context 号 = `hartid*2 + (M?0:1)`，"M 在前 S 在后"
- 投递条件（每个 context 独立判断）：`pending[i] && enable[i] && priority[i] > threshold`
- 满足则该 context 对应 CPU 的对应模式外部中断线拉高（hart0-S → mip.SEIP）
- claim：读 `0x0c20_0000 + ctx*0x1000 + 4` 拿最高优先级源号（同时清挂起）；
  处理完写回同一地址

### 2.6 为什么 S 模式是"真正的内核模式"

M4 的内核是机器的主人但没有任何"属下"；M5 之后三层职责清晰：
**M 模式 = 硬件管家**（OpenSBI：开机、委派、代管 mtimecmp、未来 CPU 核间唤醒），
**S 模式 = 内核**（我们：trap、内存管理、调度），**U 模式 = 应用**（M7）。
Linux 在 QEMU riscv 上就是这套：OpenSBI → Image (S 模式)。M5 之后我们和
Linux 之间只差一个"规模"。

## 3. 技巧（实现细节 + 坑 + 调试方法论）

### 3.1 ★ 本里程碑最大的坑：PLIC stride 写错（隐蔽性满分）

QEMU virt 的 PLIC 是 **SiFive PLIC 的简化模型**，stride 与真实硬件不同：

| 参数 | SiFive FU540 手册 | **QEMU virt (qtree 实测)** |
|------|-------------------|---------------------------|
| enable stride | 0x100 | **0x80** |
| context stride | 0x2000 | **0x1000** |

Linux 内核 `drivers/irqchip/irq-sifive-plic.c` 的
`ENABLE_PER_HART 0x80` / `CONTEXT_PER_HART 0x1000` 是旁证。

**症状**（为什么隐蔽）：写 `enable@0x0c002100` 以为在配 hart0-S，
实际写进了 hart1-M 的使能区；而 `0x0c002100` 这个地址本身是合法 MMIO，
**读回值就是你写的值**——自检全过、逻辑全对，就是中断永远不来。
探针链最终定位：`pend=0x400`（PLIC 有挂起）+ `ctx1_en/thr 看似正确` +
**`sip=0`**（SEIP 没注入）→ 轮询 4 个 context 的 claim，发现挂起源在
"另一个地址"下 → `info qtree` 看到 `enable-stride = 128`。
**教训**：外设寄存器布局查设备树/模型源码/内核驱动，别照抄芯片手册。

### 3.2 启动 hart 的原子守卫（amoswap 初体验）

实测发现 OpenSBI 会把**两个 hart 都送进 payload**（冷启动 hart 1 先到，
hart 0 后到），谁先到谁当内核，否则两个 hart 并发清 BSS 直接乱套：

```asm
la   t0, boot_flag
li   t1, 1
amoswap.w.aqrl t2, t1, (t0)   /* 原子交换: t2=旧值, boot_flag=1 */
bnez t2, .Lpark               /* 旧值非 0 → 别的 hart 已在跑 → 去停机 */
```

`.aqrl` = acquire+release 语义（保证前后内存操作不越界重排），A 扩展
入门正合适——M7 做多核调度时还要深入。顺便： hartid 别再用
`csrr mhartid` 读（S 模式非法指令），用 OpenSBI 传入的 a0。

### 3.3 OpenSBI 改变了 QEMU 的输入时序（测试命令要改）

- `-bios none` 时代：管道里的字符在内核启动瞬间就能进 RBR（实测
  boot 时 `LSR=0x61`，DR=1，字符已在等）
- OpenSBI 时代：boot 时 RBR 是空的（`LSR=0x60`），**启动瞬间灌进管道的
  字符会丢**；等内核起来（~2s）再喂一切正常

→ **测试命令统一改成延迟喂入**：

```bash
(sleep 4; printf 'abis\x03') | timeout 12 qemu-system-riscv64 -M virt \
    -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
```

（交互时无影响——人手速远慢于 2 秒。）

### 3.4 其他值得记的实现细节

- **printf 不支持 `%02X`**（宽度/补零格式没实现）——调试时用它输出会
  打印字面量 `<%02X>`，且极易被 grep 的模式过滤掉，**曾让三组对照实验
  全部误判**。要补格式请改 printf.cpp（已列入 M6 前的小任务）。
- `rdtime` 读 time CSR：单指令原子，M4 手写的防撕裂读法整体退役。
- trap.S 的保存/恢复逻辑**一字未改**，只换 CSR 名——特权模式切换对
  "现场保存"这个抽象层是透明的，这验证了当初把 trap.S 写成纯模板的正确性。
- `sie=0x220` 实测 = bit5(STIE)+bit9(SEIE)；`sstatus=0x6022` = SIE(1) +
  SPIE(5) + SPP(8)。读 CSR 对照位图是排除配置错误的快速手段（`s` 命令）。
- Makefile：`run` 目标同步去掉 `-bios none`；新文件 sbi.cpp 记得加进
  `CXX_SOURCES`（显式列表，不会自动发现）。

### 3.5 调试方法论复盘（本次的真实破案路径）

中断不来的排查顺序（从便宜到贵）：

1. **读使能位**：`sie`、sstatus.SIE（`s` 命令 / csrr）——配置层
2. **读 pending**：PLIC pending 寄存器 —— 设备层（这次卡在 pend=0 时
   其实是字符根本没来，见 3.3）
3. **逐段断链**：把链路 UART→IER→PLIC→SEIP→stvec 拆开，每段一个探针。
   本次加了只读探针（sip/pend/en/thr/IER 一次打印），立刻看到
   `pend=0x400 但 sip=0`，断点锁死在 PLIC→CPU 注入
4. **质疑地址假设**：轮询 4 个 context 的 claim 找挂起源 →
   `claim ctx1=0xA`（用 0x2000 stride 时算出来的"ctx1"地址里居然能读到）——
   说明 stride 语义和假设不符
5. **问模型本体**：QEMU monitor `info qtree` 直接读设备参数，一锤定音

以及最重要的：**对照实验的观测手段本身要先校准**（%02X 假阴性教训）。

## 4. 验证（可复现实验）

```bash
# 0) 构建
make clean && make

# 1) 冒烟: OpenSBI 链路 + S 模式入口 (readelf 确认 entry = 0x80200000)
riscv64-unknown-elf-readelf -h build/riscvPrj.elf | grep Entry
(sleep 4; printf '') | timeout 8 qemu-system-riscv64 -M virt -smp 2 -m 128M \
    -nographic -kernel build/riscvPrj.elf
# 期望: OpenSBI banner → [riscvPrj - riscv64 OS (M1-M5, S-mode)] → tick

# 2) 全功能回归: echo/断点/rx统计/CSR查看/停机
(sleep 4; printf 'abis\x03') | timeout 12 qemu-system-riscv64 -M virt \
    -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
# 期望: 'a' 回显; cause=3 断点 sepc+=2; rx_irq>0; sie=0x220; bye,halting

# 3) 非法指令 (OpenSBI 重定向路径)
(sleep 4; printf 'e\x03') | timeout 10 qemu-system-riscv64 -M virt \
    -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
# 期望: cause=2, stval=0xffffffff, sepc+=4, "back from trap"

# 4) S 模式权限边界 (对照): S 模式下碰 mhartid 会怎样? 把 boot.S 里
#    临时加一句 csrr t0, mhartid 再跑 —— 观察 OpenSBI 的反应 (留作实验)

# 5) PLIC stride 反证: 把 plic.cpp 的 stride 改回 0x100/0x2000 重跑 #2
#    —— 观察中断失效, 复现本里程碑最大坑
```

## 5. 下一步（M6 预告：Sv39 虚拟内存）

- satp 寄存器：MODE=8 (Sv39) + 页表基址；`sfence.vma`
- 三级页表 (VPN2→VPN1→VPN0)，512 项 × 8B = 4KB 页表页
- 页错误终于有用武之地（medeleg bit 12/13/15 已委派）
- 页分配器 + 恒等映射起步；M7 的调度器需要每进程页表
- 前置小任务：printf 补 `%02X`/`%08x` 等宽度格式

## 附录：全部改动源码

### A. arch/boot.S（S 模式启动）

```asm
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
```

### B. arch/trap.S（S 模式 trap 入口，与 M 模式版只差 CSR 名）

```asm
/* riscvPrj S 模式 trap 入口 (M5, 直接模式, stvec 低 2 位 = 0)。
 * 进入时把全部 GPR + sepc/scause/stval/sstatus 保存进 trap_frame (35 * 8 字节),
 * 调 C 处理函数 trap_handler(TrapFrame*), 返回后恢复现场并 sret。
 *
 * 与 M 模式版的唯一区别是 CSR 名字:
 *   mscratch -> sscratch, mepc->sepc, mcause->scause, mtval->stval,
 *   mstatus->sstatus, mret -> sret。保存/恢复逻辑一字不变 —— trap
 * 现场处理是"模式无关"的套路, 这正是 RISC-V 特权设计漂亮的地方。
 *
 * 关键技巧: 用 sscratch 与 t0 做一次 csrrw 交换 —— 进 trap 时 t0 变成
 * 帧指针, 原 t0 暂存进 sscratch; 退出时反向交换, 同时把 sscratch 恢复成
 * 帧指针, 供下一次 trap 使用。 */
.section .text
.globl trap_entry
.align 2                            /* stvec 直接模式要求 4 字节对齐 */

trap_entry:
    /* ---- 保存现场 ---- */
    csrrw t0, sscratch, t0          /* t0 = &trap_frame, sscratch = 原 t0 */
    sd x1,  0*8(t0)                 /* ra */
    sd x2,  1*8(t0)                 /* sp */
    sd x3,  2*8(t0)                 /* gp */
    sd x4,  3*8(t0)                 /* tp */
    /* x5 (t0) 的原值在 sscratch 里, 稍后补存 */
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
    csrr t1, sscratch
    sd   t1, 4*8(t0)                /* 原 t0 */
    csrr t1, sepc
    sd   t1, 31*8(t0)
    csrr t1, scause
    sd   t1, 32*8(t0)
    csrr t1, stval
    sd   t1, 33*8(t0)
    csrr t1, sstatus
    sd   t1, 34*8(t0)

    /* ---- 调 C 处理函数 ---- */
    mv   a0, t0                     /* 参数: TrapFrame* */
    call trap_handler

    /* handler 可能修改了 sepc (如异常恢复 sepc+=4) 或 sstatus, 写回 CSR */
    ld   t1, 31*8(t0)               /* sepc */
    csrw sepc, t1
    ld   t1, 34*8(t0)               /* sstatus */
    csrw sstatus, t1

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

    /* 反向交换: t0 = 原 t0 (sscratch 里), sscratch = &trap_frame (下次用) */
    csrrw t0, sscratch, t0
    sret

/* trap 现场保存区: 35 个 u64 (x1..x31 + sepc/scause/stval/sstatus) */
.section .bss
.align 3
.globl trap_frame
trap_frame:
    .skip 35 * 8
```

### C. include/lib/sbi.hpp + lib/sbi.cpp（SBI ecall 封装，新文件）

```cpp
/* include/lib/sbi.hpp */
/* SBI 调用封装 (M5)
 *
 * S 模式碰不到的机器资源 (mtimecmp、系统复位、控制台...) 统一走 SBI:
 * 执行 ecall 指令 -> CPU 切到 M 模式 -> OpenSBI 的 mtvec 处理 -> mret 回来。
 * 这就是"系统调用"的原型: 用户态 ecall -> 内核, 内核 ecall -> 固件, 同构!
 *
 * 调用约定 (SBI 规范):
 *   a7 = 扩展 ID (EID), a6 = 功能 ID (FID), a0..a5 = 参数
 *   返回: a0 = error code (0 = 成功), a1 = value */
#pragma once
#include "types.hpp"

struct SbiRet {
    u64 error;   /* 0 = SBI_SUCCESS */
    u64 value;
};

/* 通用 ecall 发射器: 8 个寄存器参数一一对应 a0..a7 */
SbiRet sbi_ecall(u64 ext, u64 fid,
                 u64 a0 = 0, u64 a1 = 0, u64 a2 = 0,
                 u64 a3 = 0, u64 a4 = 0, u64 a5 = 0);

/* Timer 扩展 (EID 0x54494D45 = ASCII "TIME", FID 0):
 * 在 stime 时刻向【本 hart】注入 STIP, 并清除当前挂起的 STIP。
 * 内部: OpenSBI 替我们写该 hart 的 CLINT mtimecmp (S 模式碰不到)。 */
void sbi_set_timer(u64 stime);
```

```cpp
/* lib/sbi.cpp */
/* SBI 调用实现: 纯内联汇编, 手工摆好 a0..a7 再发 ecall。
 *
 * 内联汇编要点:
 *   register u64 x asm("a0") = v   把 C 变量钉在指定物理寄存器上;
 *   "+r"(r_a0), "+r"(r_a1)         a0/a1 既是入参也是出参 (SBI 返回值);
 *   "memory" clobber               固件可能读写内存, 禁止编译器跨 ecall 缓存。 */
#include "lib/sbi.hpp"

SbiRet sbi_ecall(u64 ext, u64 fid,
                 u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    register u64 r_a0 asm("a0") = a0;
    register u64 r_a1 asm("a1") = a1;
    register u64 r_a2 asm("a2") = a2;
    register u64 r_a3 asm("a3") = a3;
    register u64 r_a4 asm("a4") = a4;
    register u64 r_a5 asm("a5") = a5;
    register u64 r_a6 asm("a6") = fid;
    register u64 r_a7 asm("a7") = ext;
    asm volatile("ecall"
                 : "+r"(r_a0), "+r"(r_a1)
                 : "r"(r_a2), "r"(r_a3), "r"(r_a4),
                   "r"(r_a5), "r"(r_a6), "r"(r_a7)
                 : "memory");
    return {r_a0, r_a1};
}

constexpr u64 SBI_EXT_TIME              = 0x54494D45;   /* "TIME" */
constexpr u64 SBI_EXT_TIME_FID_SETTIMER = 0;

void sbi_set_timer(u64 stime) {
    /* M5 阶段忽略返回值; OpenSBI v0.9 支持 Timer 扩展, 恒成功 */
    sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_FID_SETTIMER, stime);
}
```

### D. kernel/timer.cpp（SBI 化定时器）

```cpp
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
```

### E. kernel/plic.cpp（S-context + stride 修复）

```cpp
/* PLIC (Platform-Level Interrupt Controller) 驱动
 *
 * QEMU virt 的 PLIC 内存布局 (32 位寄存器, 按中断源/上下文编址):
 *   0x0c000000  基址
 *   0x0c000000 + 4×src   每个中断源的优先级 (0=禁用, 1..7 有效)
 *   0x0c001000 + ...     挂起位 (硬件写, 只读)
 *   0x0c002000 + ctx×0x80          每个 context 的使能位   ★ stride=0x80
 *   0x0c200000 + ctx×0x1000        每个 context 的优先级阈值 ★ stride=0x1000
 *   0x0c200004 + ctx×0x1000        每个 context 的 claim(读)/complete(写)
 *
 * context 编号 = hart 在前模式在后: 每个 hart 占两个 context,
 *   ctx = hartid*2 + 0 (M 模式), ctx = hartid*2 + 1 (S 模式)。
 * M4 时内核自己就是 M 模式, 用 context 0; M5 内核在 S 模式,
 * 用 hart 0 的 S-context = 1。M 模式那份留给 OpenSBI, 互不干扰。
 *
 * 协议: 中断来了 → claim 读到源号(同时清挂起) → 处理 → complete 写回源号。
 * 不 complete 的话, PLIC 认为还没处理完, 该源的中断不再上报。
 *
 * ★ M5 踩坑实录: stride 别照抄 SiFive FU540 手册 (enable 0x100 /
 *   context 0x2000)! QEMU virt 的 sifive_plic 模型是 0x80 / 0x1000
 *   (qtree 实测 enable-stride=128, context-stride=4096; Linux 内核
 *   plic.c 的 ENABLE_PER_HART=0x80 / CONTEXT_PER_HART=0x1000 同款)。
 *   写错 stride 的症状极隐蔽: 寄存器读回值"看起来对", 但全写进了
 *   别的 context —— PLIC pending 有, sip.SEIP 永远不来。 */
#include "kernel/plic.hpp"

constexpr u64 PLIC_BASE = 0x0c000000;
constexpr u64 PLIC_PRIORITY = 0x000000;   /* 源优先级区 (与 context 无关) */
constexpr u64 CTX_ENABLE_STRIDE = 0x80;   /* 使能区: 每 context 间隔 0x80 (QEMU!) */
constexpr u64 CTX_CONTEXT_STRIDE = 0x1000;/* 阈值/claim 区: 每 context 间隔 0x1000 (QEMU!) */

/* 本 hart 的 S 模式 context 号 (plic_init 里算好) */
static u32 s_context;

static inline volatile u32* plic_reg(u64 offset) {
    return reinterpret_cast<volatile u32*>(PLIC_BASE + offset);
}

void plic_init(u32 hartid) {
    s_context = hartid * 2 + 1;     /* hart 的 S-context: M 在前 S 在后 */

    /* ① UART 源优先级 = 1 (0 表示禁用该源; 与 context 无关, 写一次即可) */
    *plic_reg(PLIC_PRIORITY + 4 * PLIC_UART_SOURCE) = 1;

    /* ② 使能 UART 源: S-context 使能区 = 0x2000 + ctx*0x80 */
    *plic_reg(0x2000 + static_cast<u64>(s_context) * CTX_ENABLE_STRIDE)
        |= (1u << PLIC_UART_SOURCE);

    /* ③ 阈值 = 0: 所有优先级 > 0 的中断都放行 */
    *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE) = 0;

    /* ④ 开 S 外设中断: sie.SEIE (bit 9) —— 总闸 sstatus.SIE 由 timer_init 开 */
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));
}

u32 plic_claim() {
    return *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE + 4);
}

void plic_complete(u32 source) {
    *plic_reg(0x200000 + static_cast<u64>(s_context) * CTX_CONTEXT_STRIDE + 4) = source;
}
```

### F. kernel/trap.cpp（S 模式中断号 + OpenSBI 重定向注释）

（见仓库文件，关键差异：`interrupt_name` 只剩 1/5/9 三种 S 模式中断；
字段访问全部 `f->sepc/f->scause/f->stval/f->sstatus`；非法指令分支注明
"经 OpenSBI 重定向进来"；ecall 分支注明"我们自己的 SBI ecall 由 OpenSBI
在 M 模式处理完才返回，不会走到这里"。）

### G. kernel/main.cpp（S 模式主程序 + 's' 命令）

（见仓库文件，关键差异：`kernel_main(u64 hartid, u64 dtb)` 接 OpenSBI
的 a0/a1；`plic_init(hartid)`；横幅改为 S-mode/OpenSBI/SBI timer；
新增 `s` 命令读 sstatus/sie/sip。）

### H. linker.ld / run.sh（加载地址与启动方式）

```ld
/* linker.ld 关键行 */
    . = 0x80200000;   /* OpenSBI 之后: 0x80000000 + 2MB */
```

```bash
# run.sh: 去掉 -bios none, QEMU 自动加载自带 OpenSBI (fw_dynamic)
qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf
```
