/* ============================================================
 * ex2-c-to-asm.c — "看汇编还原 C" 练习
 *
 * 用法:  make asm-c2asm     → 生成 build/asm/ex2.s
 *   等价于:
 *     riscv64-unknown-elf-gcc -march=rv64gc -mabi=lp64 \
 *         -O2 -S asm/ex2-c-to-asm.c -o build/asm/ex2.s
 *
 * 练习步骤 (每次 30 分钟):
 *   1. 先看 C 代码, 心里预测会生成什么指令序列
 *   2. 编译出 ex2.s, 逐行给汇编写注释
 *   3. 换 -O0 / -O2 / -Os 重新编译, 对比指令数量与栈帧差异
 *   4. 把汇编"还原"回 C (隔天再做, 检验是否真懂)
 *   5. 进阶: objdump -d 看机器码, 手算其中几条的编码
 * ============================================================ */

/* 1. 算术: 预期 addi / slli / mul / sub 等 */
int square(int x) {
    return x * x;
}

/* 2. 分支: 预期 blt / bge / 跳转指令, 注意编译器可能用无分支技巧 */
int max3(int a, int b, int c) {
    int m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

/* 3. 循环: 预期 beq/bne + 地址自增, 看编译器怎么消除 i 变量 */
int sum_array(const int* a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

/* 4. 递归: 观察序言/尾声, ra 的保存与恢复, 参数传 a0-a7 */
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

/* 5. 指针/访存: 预期 ld/sd + 偏移寻址 */
void swap(long* a, long* b) {
    long t = *a;
    *a = *b;
    *b = t;
}

/* 6. 64 位除法/取模: rv64gc 有 M 扩展, 用 divu/remu 硬件指令 */
unsigned long divmod(unsigned long a, unsigned long b) {
    return a / b + a % b;
}
