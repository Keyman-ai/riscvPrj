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
