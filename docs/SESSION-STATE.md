# 会话状态快照（供新会话/压缩后恢复上下文用）

> 更新时间：M5 完成后。新会话开始时让 AI 先读这个文件即可无缝继续。

## 1. 用户与目标

- **目标**：学习 RISC-V 成为专家；载体是 riscvPrj（从零手写 riscv64 OS）
- **背景**：写过 myos（aarch64 裸机 OS，见 `/home/cat/os/myos`）；汇编基础在本次学习中从零补起
  （已学会：寄存器/内存概念、读反汇编、六种指令格式、trap 机制、M/S 两级特权）
- **偏好**：中文交流；笔记要详细（模板见 §6）；逐条指令讲解用"盒子(寄存器)/仓库(内存)"类比；
  大动作先问再做；每周节奏见 learning-plan.md

## 2. 环境

- 远程主机：`root@192.168.137.19`，工作目录 `/home/cat/os/riscvPrj`（本地镜像已同步）
- 工具链已装：`riscv64-unknown-elf-gcc` 10.2 / `qemu-system-riscv64` 6.2（自带 OpenSBI v0.9）/
  gdb-multiarch / dtc
- 测试命令（★M5 起必须延迟喂入，启动瞬间灌字符会丢）：
  `(sleep 4; printf 'abis\x03') | timeout 12 qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic -kernel build/riscvPrj.elf`
- QEMU virt 关键地址：OpenSBI 0x80000000 / 内核 0x80200000 / UART 0x10000000 / CLINT 0x02000000 / PLIC 0x0c000000 / dtb 0x87000000
- UART = PLIC 中断源 **10**；PLIC stride（QEMU 特有）enable=0x80 / context=0x1000（§6 坑 9）
- **设备失联自救（WoL）**：SSH 超时时，Windows 侧（本机 ICS 网段 IP `192.168.137.10`）先
  `arp -a` 查设备 MAC（实测 `2e-1b-88-69-2c-92`，有缓存说明二层还通），向广播地址 9 端口
  发 WoL 魔术包即可远程唤醒（PowerShell UdpClient 6×0xFF + MAC×16）；唤醒后 ping 通 22
  端口通，直接 `rw_connect` 即可。设备休眠过一次，此法已实测有效
- 代码已推送 GitHub：https://github.com/Keyman-ai/riscvPrj（推送路径：远程主机无外网 →
  `rw_sync` 拉到本地镜像 → 本地 Windows git push；远程 `.git` 废弃，以本地镜像为推送出口）

## 3. 里程碑状态

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M1 | 启动 + UART + printf + trap 现场 | ✅ |
| M2 | 异常三档分类（非法指令/断点跳过、ecall 占位、其余停机）+ instr_len() | ✅ |
| M3 | CLINT 定时器中断 + uptime（1ms tick） | ✅ |
| M4 | PLIC + UART 中断驱动接收（环形缓冲区） | ✅ |
| M5 | OpenSBI + S 模式（stvec/sret/SBI 调用/S-context/委派） | ✅ |
| M6 | Sv39 虚拟内存 | ⬅ 下一步（用户此前推荐过 M5→M6 顺延） |
| M7 | 移植 myos 调度器/信号量/Shell/kmalloc | |
| M8 | QEMU 启动 Linux riscv64 | |

交互命令：`e`=非法指令(经 OpenSBI 重定向) `b`=断点(直接委派) `i`=RX 统计
`s`=读 sstatus/sie/sip（M5 新增） `Ctrl+C`=停机

## 4. 代码结构（全部已推送到远程）

```
arch/boot.S       S 模式启动(.option norelax! amoswap 原子守卫选 boot hart → 清BSS → sp/gp → stvec/sscratch → kernel_main(a0=hartid,a1=dtb))
arch/trap.S       S 模式 trap 入口(sscratch csrrw 交换技巧, 保存31GPR+sepc/scause/stval/sstatus, sret)
kernel/main.cpp   主程序(kernel_main(hartid,dtb) + 非阻塞echo + uptime + a/e/b/i/s 命令)
kernel/trap.cpp   S 模式 trap 分发(中断 1/5/9; 异常三档分类+instr_len; 非法指令经 OpenSBI 重定向)
kernel/timer.cpp  SBI 定时器(rdtime 读时间, sbi_set_timer 排闹钟, sie.STIE+sstatus.SIE)
kernel/plic.cpp   PLIC 驱动(S-context=hartid*2+1, 优先级/使能/阈值+claim/complete, stride 0x80/0x1000)
lib/sbi.cpp       SBI ecall 封装(通用 sbi_ecall + sbi_set_timer, EID "TIME"=0x54494D45)  ← M5 新增
drivers/uart.cpp  16550(轮询+RX环形缓冲区256B, SPSC无锁)
lib/printf.cpp    迷你printf(%d %u %x %X %c %s %p —— 不支持宽度格式%02X!)
include/          types.hpp + 各驱动头文件 (trap.hpp 字段已改 sepc/scause/stval/sstatus)
asm/              ex1-formats.S(六种格式) ex2-c-to-asm.c(C↔汇编练习)
docs/             见 §5
Makefile          rv64gc/lp64/medany, -MMD -MP, .DEFAULT_GOAL:=all, run 目标无 -bios none
run.sh            qemu 启动脚本(OpenSBI → 内核, 无 -bios none)
linker.ld         0x80200000(OpenSBI 之后 2MB), .text.boot 优先, __global_pointer$=.+0x800
```

## 5. 文档清单（docs/，均为详细版）

- `learning-plan.md` — 12 个月学习计划（Phase 0-5）
- `asm-cheatsheet.md` — 汇编指令小词典（用户随时查）
- `trap-csrs.md` — trap CSR 体系结构详解（位级、规范级；M5 起委派章节正式用上）
- `m1-notes.md` ~ `m5-notes.md` — 里程碑笔记（统一模板：做了什么→原理→技巧→验证→附录全文）

## 6. 已踩过的坑（重要！别再犯）

1. **boot 汇编必须 `.option norelax`**——否则 `la` 被松弛成 gp 相对寻址（gp 未初始化→死循环）
2. **trap 返回必须把 handler 改过的 mepc/mstatus 写回 CSR**——mret/sret 用硬件值
3. **外设中断源号查设备树**（`-machine dumpdtb=`），别凭记忆
4. **Makefile：`.d` 依赖文件 `-include` 必须放在默认目标之后**（或加 `.DEFAULT_GOAL := all`），
   否则默认目标被劫持 → 增量构建只编译不链接
5. **mtimecmp 先写低 32 位再写高**（高字生效）；mtime 读要防撕裂（M5 起 rdtime 单指令原子，此坑退役）
6. **ISR 里不 printf**（重入/交错）；ISR 尽量排空数据（一次中断收多字符）
7. **claim 返回 0 不要 complete**；处理完必须 complete 否则中断丢失
8. 运行时注意：`rw_push` 对 timer.cpp 有 mtime 假冲突（MD5 相同），force 覆盖即可；
   编辑本地文件前需重新 read（mtime 检查）
9. **★QEMU virt 的 PLIC stride ≠ SiFive 真实硬件**：enable stride=0x80（不是 0x100），
   context stride=0x1000（不是 0x2000）。写错症状极隐蔽：寄存器读回值"看起来对"，
   实际写进了别的 context —— pending 有、sip.SEIP 永远不来。破案工具：
   探针二分 + 轮询各 context 的 claim + QEMU monitor `info qtree`
   （Linux 内核 plic.c：ENABLE_PER_HART=0x80 / CONTEXT_PER_HART=0x1000 旁证）
10. **S 模式碰 M 模式 CSR = 非法指令**：mhartid 读不了（用 OpenSBI 传入的 a0）、
    mtimecmp 写不了（用 SBI）、mie/mstatus 别再碰（sie/sstatus 位编排还不同：
    全局总闸 SIE 在 bit1 不是 bit3！）
11. **OpenSBI 启动期灌进管道的字符会丢**：测试必须延迟喂入 `(sleep 4; printf 'abis\x03')`
    （-bios none 时代 QEMU 流控会把字符留在 OS 管道里等内核读，OpenSBI 时代不会）
12. **printf 不支持 %02X 等宽度格式**：会打印字面量且容易被 grep 过滤，调试时曾造成
    三组对照实验集体误判（观测手段要先校准！）
13. OpenSBI 会把所有 hart 都送进 payload（谁先到谁跑内核，boot.S 用 amoswap 原子守卫）；
    内核实际跑在 hart 0（实测），PLIC S-context = 1

## 7. 笔记模板（用户要求的标准结构）

```
1. 这一阶段做了什么（一句话 + 改动清单表 + 实测输出）
2. 原理（完整链路 + 寄存器位级 + 为什么这样设计 + ASCII 图）
3. 技巧（实现细节、坑、调试方法论）
4. 验证（可复现的对照实验）
5. 下一步
附录（全部源码逐行注释）
```

## 8. M6 备忘（下一步要点）

- satp 寄存器：MODE=8 (Sv39) + PPN(页表基址)；写完必须 `sfence.vma`
- 三级页表：va[38:30]→VPN2, [29:21]→VPN1, [20:12]→VPN0；每级 512 项 × 8B = 4KB 页
- PTE 位：V R W X A U D + PPN；U 位决定 U 模式能否访问（M7 用户态要用）
- 页错误（medeleg bit 12/13/15 已委派）终于能进我们自己的 handler（trap.cpp 已有 case 12/13/15 兜底）
- 需要一个简单的物理页分配器（4KB 粒度，从 __bss_end 之后切）
- 起步建议：恒等映射（virt=phys）跑通 satp 切换，再玩映射重排
- 前置小任务：printf 补宽度格式（%02X/%08x/%5d），m5-notes §3.4 有教训
- 参照：docs/trap-csrs.md；myos 若做过 ARM 页表可对照（VMSAv8-64 vs Sv39）
