# M2 笔记：异常分类处理

> 阅读顺序：**1. 做了什么** → **2. 原理**（异常如何分类、如何恢复）→
> **3. 技巧**（指令长度判定 + 构建系统坑）→ **4. 验证** → **附录**。
> 配套：`docs/trap-csrs.md`（mcause 全表）、`docs/asm-cheatsheet.md`。

---

## 1. 这一阶段做了什么

**一句话**：把 trap_handler 的异常路径从"非法指令特判"升级为
**三档分类处理**——可恢复的跳过继续（非法指令、断点），ecall 做
系统调用占位，其余（访存错误/页错误）打印现场后停机。

### 改动清单

| 文件 | 改动 | 作用 |
|------|------|------|
| `kernel/trap.cpp` | 异常路径重写 | 三档分类 + `instr_len()` 指令长度判定 + `ecall_mode()` |
| `kernel/main.cpp` | 新增 `b` 命令 | ebreak 断点演示（`mcause=3`） |
| `Makefile` | `.DEFAULT_GOAL := all` | 修复"默认目标被 .d 文件劫持"的坑（见技巧 3.2） |

### 运行效果（实测）

```
> e
[trap] exception: Illegal instruction (cause=2)
  [trap] recoverable: skipping illegal instruction (mepc += 4)   ← 32 位指令

> b
[trap] exception: Breakpoint (cause=3)
  [trap] recoverable: breakpoint hit, skipping (mepc += 2)       ← 压缩指令 c.ebreak!
```

第二个输出是亮点：编译器把 `ebreak` 压成了 **2 字节的 `c.ebreak`**，
`instr_len()` 按编码正确识别——C 扩展在实战中首次登场。

---

## 2. 原理：异常怎么分类、怎么恢复

### 2.1 分类的哲学：按"能否恢复"分档

异常处理的核心问题是：**出了这个异常，程序还能继续吗？**

| 档位 | 异常 | 能否继续 | 处理方式 |
|------|------|---------|---------|
| ① 可恢复 | 非法指令(2)、断点(3) | 能 | `mepc += 指令长度`，跳过出错指令 |
| ② 系统调用 | ecall(8/9/11) | 能（语义不同） | 跳过后由"系统调用处理"接管（还没实现） |
| ③ 不可恢复 | 对齐错(0/4/6)、访存错(1/5/7)、页错(12/13/15) | 不能 | 打印现场 + 停机 |

**为什么非法指令/断点"跳过"是合法的？** 这两类是软件**故意**制造的：
- 非法指令：模拟器/未实现指令的占位（我们的 `e` 演示）
- 断点：调试器把目标指令替换成 `ebreak`，命中后恢复执行 = **从断点
  下一条继续**——`mepc += len` 正是 gdb 等调试器的标准恢复动作

**为什么访存错误不跳过？** 出错指令的语义没完成（数据没读出来），
跳过 = 用垃圾值继续算，程序状态已损坏。正确的做法是杀掉任务
（真实 OS 发 SIGSEGV）——我们还没有任务，所以停机。

### 2.2 指令长度判定：RISC-V 的"自描述"编码

`mepc += 4` 是 M1 的老写法——但如果出错的是**压缩指令**（2 字节），
加 4 就跳过头了。RISC-V 的指令宽度是**编码自描述**的，看最低 2 位：

```
最低 2 位:  11          → 32 位标准指令 (4 字节)
            00/01/10    → 16 位压缩指令 (2 字节, C 扩展)
            (11111      → 48/64 位扩展指令, 将来)
```

所以：

```cpp
static u32 instr_len(const void* addr) {
    u16 half = *(const volatile u16*)addr;
    return (half & 0x3) == 0x3 ? 4 : 2;
}
```

**这就是 IALIGN=16 的意义**：rv64gc 允许 2 字节对齐的压缩指令，
任何"跳过一条指令"的逻辑都必须先判定宽度——gdb、反汇编器、
模拟器全是这么干的。

### 2.3 ecall：未来的系统调用入口

`ecall`（环境调用）是 RISC-V 的**软中断**：主动陷入更高特权模式。
`mcause` 8/9/11 分别对应 U/S/M 模式发出的 ecall。

真实 OS 的用法：U 模式程序 `ecall` → 陷入 S 模式内核 → 内核按
`a7`（系统调用号）+ `a0-a6`（参数）分发 → `sret` 返回。
M2 先留占位（跳过 + 提示），M6.5 做用户态时正式实现。

### 2.4 mcause 与三类异常的映射

完整原因码表见 `docs/trap-csrs.md` 第 5 节。本阶段实际用到的：

| cause | 名称 | 档位 |
|-------|------|------|
| 2 | 非法指令 | ① 跳过 |
| 3 | 断点 ebreak | ① 跳过 |
| 8/9/11 | ecall (U/S/M) | ② 占位跳过 |
| 0/4/6 | 地址未对齐 | ③ 停机 |
| 1/5/7 | 访问故障 | ③ 停机 |
| 12/13/15 | 页错误 | ③ 停机（M6 开 MMU 后变成"可恢复"——缺页换入!） |

> 预告：页错误在 M6 之后会从"停机"变成"缺页处理"——那是虚拟内存
> 的核心机制，分类表到时候要再改一档。

---

## 3. 技巧：实现细节与坑

### 3.1 `mepc += 指令长度` 而不是 `+= 4`

M1 只处理 32 位非法指令所以 `+= 4` 够用；M2 有了 `c.ebreak`
（2 字节）就不够了。**教训：任何"跳过指令"的代码都必须用
`instr_len()`，硬编码 4 是潜伏的 bug。**

### 3.2 构建系统坑：`-include` 劫持了默认目标（本次新踩！）

**现象**：改完代码 `make`，main.o 重编了但 **ELF 没链接**，跑的还是
旧内核。查 mtime 发现 elf 比所有 .o 都旧。

**原因**：上一阶段加的 `-include $(OBJECTS:.o=.d)` 放在 `all:` 规则
**之前**。`.d` 文件里的规则（`build/kernel/main.o: kernel/main.cpp ...`）
被展开后成了 makefile 里**第一个目标**——make 的默认目标就从 `all`
变成了 `main.o`！于是 `make` 只检查 main.o 就完事，永远不链接。

**为什么 `make clean && make` 能骗过我们？** clean 删掉 build/ 后
`.d` 文件不存在，`-include` 啥也没引入，第一个目标恢复为 `all`——
所以每次"干净构建"都正常，只有增量构建才中招。**这类坑最阴险**。

**修复**（Makefile）：

```make
.DEFAULT_GOAL := all     # 显式声明默认目标, 不依赖"第一个规则"的隐式规则
all: $(OBJ_DIR)/riscvPrj.elf

-include $(OBJECTS:.o=.d)   # 放在 all 之后, 双保险
```

**要点**：`-include` 依赖文件必须放在默认目标**之后**，或用
`.DEFAULT_GOAL` 显式指定——GNU make 的默认目标 = 第一个规则的目标，
这个隐式规则很容易被依赖文件打破。

### 3.3 打印现场保持"全寄存器"

异常时打印 `mepc/mtval/mstatus + ra/sp/gp/tp + a0-a3` 是 M1 的资产：
**现场信息是排障的第一手材料**。M2 的分类只是在打印之后加了
"能否恢复"的判断，打印本身没动。

---

## 4. 怎么验证（对照实验）

```bash
# 实验 1: 断点恢复 (注意 mepc += 2, 压缩指令!)
printf 'ab' | timeout 3 qemu-system-riscv64 -M virt -smp 2 -m 128M \
    -nographic -bios none -kernel build/riscvPrj.elf
# 预期: Breakpoint (cause=3), mepc += 2, 继续运行

# 实验 2: 非法指令恢复 (mepc += 4)
printf 'ae' | timeout 3 qemu... -kernel build/riscvPrj.elf
# 预期: Illegal instruction (cause=2), mepc += 4

# 实验 3: 故意把 instr_len 的判定写反 (>= 0x3 ? 2 : 4)
#   预期: 断点恢复后跳到指令中间 → 下一条指令非法 → 连环异常
#   这验证了"宽度判定错了会怎样"

# 实验 4: 复现 Makefile 坑
#   git 里去掉 .DEFAULT_GOAL 那行, 改个 .cpp 再 make:
#   观察 main.o 重编但 elf 不更新 (stat 对比 mtime)
```

---

## 5. 下一步

- **M5：OpenSBI + S 模式** —— 异常分类表会跟着换 CSR
  （`scause`/`sepc`），ecall 占位变成真正的 SBI 调用入口
- **M6**：页错误从"停机"升级为"缺页处理"——分类表的第三次进化

---

## 附录 A：kernel/trap.cpp 异常路径（M2 版核心）

```cpp
/* 判断 mepc 处指令长度: 低 2 位 = 11 → 32 位, 否则 16 位压缩指令 */
static u32 instr_len(const void* addr) {
    u16 half = *(const volatile u16*)addr;
    return (half & 0x3) == 0x3 ? 4 : 2;
}

static const char* ecall_mode(u64 cause) {
    switch (cause) {
        case 8:  return "U-mode";
        case 9:  return "S-mode";
        default: return "M-mode";
    }
}

/* trap_handler 异常部分: */
printf("\n[trap] exception: %s (cause=%d)\n", exception_name(cause), (int)cause);
printf("  mepc=%p mtval=%p mstatus=0x%X\n",
       (void*)f->mepc, (void*)f->mtval, (u32)f->mstatus);
printf("  ra=%p sp=%p gp=%p tp=%p\n",
       (void*)f->ra, (void*)f->sp, (void*)f->gp, (void*)f->tp);

/* ① 可恢复: 非法指令 / 断点 —— 跳过出错指令 */
switch (cause) {
    case 2: {
        u32 len = instr_len((void*)f->mepc);
        printf("  [trap] recoverable: skipping illegal instruction (mepc += %u)\n", len);
        f->mepc += len;
        return;
    }
    case 3: {
        u32 len = instr_len((void*)f->mepc);
        printf("  [trap] recoverable: breakpoint hit, skipping (mepc += %u)\n", len);
        f->mepc += len;
        return;
    }
}

/* ② ecall: 系统调用占位 */
if (cause == 8 || cause == 9 || cause == 11) {
    printf("  [trap] ecall from %s: no syscalls implemented, skipping\n",
           ecall_mode(cause));
    f->mepc += 4;   /* ecall 恒为 32 位, 无压缩版本 */
    return;
}

/* ③ 不可恢复: 打印后停机 */
printf("  [trap] unrecoverable, halting\n");
for (;;) asm volatile("wfi");
```

## 附录 B：kernel/main.cpp 断点演示

```cpp
} else if (ch == 'b') {
    /* M2: ebreak 断点 -> mcause=3, 分类为可恢复 */
    printf("\n[riscvPrj] triggering breakpoint (ebreak)...\n");
    asm volatile("ebreak");             /* 0x00100073 -> c.ebreak (2B) */
    printf("[riscvPrj] back from breakpoint\n");
    printf("> ");
}
```
