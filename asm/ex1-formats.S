/* ============================================================
 * ex1-formats.S — RISC-V 六种指令格式速查练习 (RV64GC)
 *
 * 用法:  make asm-formats
 *   等价于:
 *     riscv64-unknown-elf-gcc -march=rv64gc -mabi=lp64 \
 *         -c asm/ex1-formats.S -o build/asm/ex1.o
 *     riscv64-unknown-elf-objdump -d build/asm/ex1.o
 *
 * 练习方法:
 *   1. 看下面每条指令的注释(格式/字段含义)
 *   2. 用 objdump 看输出的 32 位机器码
 *   3. 自己按格式布局把机器码拆开, 核对是否与注释一致 —— 手算 5~10 条
 *   4. 改改立即数/寄存器, 再编译, 观察机器码哪些位变了
 *
 * 六种格式布局 (MSB → LSB, 每格一个字段):
 *   R型: funct7 | rs2 | rs1 | funct3 | rd | opcode
 *   I型: imm[11:0] | rs1 | funct3 | rd | opcode
 *   S型: imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode
 *   B型: imm[12] | imm[10:5] | rs2 | rs1 | funct3 | imm[4:1] | imm[11] | opcode
 *   U型: imm[31:12] | rd | opcode
 *   J型: imm[20] | imm[10:1] | imm[11] | imm[19:12] | rd | opcode
 *
 * 记忆点:
 *   - opcode 固定 7 位: 0x13 算数I / 0x33 算数R / 0x03 加载 / 0x23 存储
 *     / 0x63 分支 / 0x6F jal / 0x67 jalr / 0x17 auipc / 0x37 lui
 *   - 立即数一律符号扩展; B/J 型立即数第 0 位恒为 0(2 字节对齐), 不编码
 *   - S 型和 B 型的立即数字段故意"错位"排列, 是为了与 I 型共用译码逻辑
 * ============================================================ */

.section .text

/* ---------- R 型: 双寄存器算术 ---------- */
.globl ex_r_type
ex_r_type:
    add  x5, x6, x7        /* R型 opcode=0x33 funct3=0   funct7=0x00 */
    sub  x5, x6, x7        /* R型 opcode=0x33 funct3=0   funct7=0x20 (与add区分) */
    mul  x5, x6, x7        /* R型 M扩展 opcode=0x33 funct3=0 funct7=0x01 */
    div  x5, x6, x7        /* R型 M扩展 funct3=4 */
    and  x5, x6, x7        /* R型 funct3=7 */
    xor  x5, x6, x7        /* R型 funct3=4 */
    sll  x5, x6, x7        /* R型 funct3=1 */
    slt  x5, x6, x7        /* R型 funct3=2 (set less than) */
    ret                    /* 伪指令: jalr x0, 0(ra) — 见下方 jalr */

/* ---------- I 型: 立即数算术 / 加载 / jalr ---------- */
.globl ex_i_type
ex_i_type:
    addi x5, x6, -16       /* I型 opcode=0x13 funct3=0, imm 符号扩展 */
    slli x5, x6, 3         /* I型移位: 注意 funct3=1, imm 低6位是移位量 */
    ld   x5, 8(x6)         /* I型加载 opcode=0x03 funct3=3 (doubleword) */
    lw   x5, 8(x6)         /* I型加载 funct3=2 (word) */
    lbu  x5, 8(x6)         /* I型加载 funct3=4 (byte, 无符号) */
    jalr x0, 0(x1)         /* I型 opcode=0x67: 跳转到 x1+0, x0 不存返回地址 */
    csrr x5, mhartid       /* I型 CSR 读: 伪指令 = csrrs x5, mhartid, x0 */

/* ---------- S 型: 存储 ---------- */
.globl ex_s_type
ex_s_type:
    sd   x5, 8(x6)         /* S型 opcode=0x23 funct3=3 */
    sw   x5, 8(x6)         /* S型 funct3=2 */
    sb   x5, 8(x6)         /* S型 funct3=0 */
    /* 注意: sd x5, 8(x6) 与 ld x5, 8(x6) 的偏移 8 编码位置不同:
     *  I型: 8 在高 12 位; S型: 拆成 imm[11:5] + imm[4:0] */

/* ---------- B 型: 分支 (注意立即数错位) ---------- */
.globl ex_b_type
ex_b_type:
    beq  x5, x6, 1f        /* B型 opcode=0x63 funct3=0 */
    bne  x5, x6, 1f        /* B型 funct3=1 */
    blt  x5, x6, 1f        /* B型 funct3=4 (有符号小于) */
    bge  x5, x6, 1f        /* B型 funct3=5 */
    bltu x5, x6, 1f        /* B型 funct3=6 (无符号小于) */
1:  ret

/* ---------- U 型: 高位立即数 ---------- */
.globl ex_u_type
ex_u_type:
    lui   x5, 0x12345      /* U型 opcode=0x37: 把 0x12345000 装入 x5 */
    auipc x5, 0x12345      /* U型 opcode=0x17: x5 = PC + 0x12345000 */
    /* 立即数高 20 位, 低 12 位补 0 —— 拼完整大常数要靠 lui+addi 两条 */
    ret

/* ---------- J 型: 无条件跳转 ---------- */
.globl ex_j_type
ex_j_type:
    jal  x1, 1f            /* J型 opcode=0x6F: 跳转并存返回地址到 ra */
1:  jal  x0, 2f            /* J型: x0 作 rd 等价于无条件跳(不保存) */
2:  ret

/* ---------- 伪指令: 编译器魔术, 反汇编后看真身 ---------- */
.globl ex_pseudo
ex_pseudo:
    li   x5, 0x12345       /* 伪指令: 可能展开成 lui x5, 0x12 + addi x5, x5, 0x345 */
    li   x5, 100           /* 小常数: 展开成单条 addi */
    mv   x5, x6            /* = addi x5, x6, 0 */
    not  x5, x6            /* = xori x5, x6, -1 */
    neg  x5, x6            /* = sub  x5, x0, x6 */
    nop                    /* = addi x0, x0, 0 */
    ret

/* ---------- 调用约定: 函数序言/尾声 ---------- */
/* 传参 a0-a7; 返回值 a0; ra 由调用者保存; s0-s11/sp 由被调用者保存 */
.globl ex_frame
ex_frame:
    addi sp, sp, -32       /* 分配 32 字节栈帧 (16 字节对齐) */
    sd   ra, 24(sp)        /* 非叶子函数必须保存返回地址 */
    sd   s0, 16(sp)        /* 保存被调用者保存寄存器 */
    addi s0, sp, 32        /* 帧指针 s0 = 旧 sp (调试用, -O0 常见) */
    /* ... 函数体 ... */
    ld   ra, 24(sp)        /* 恢复 */
    ld   s0, 16(sp)
    addi sp, sp, 32
    ret
