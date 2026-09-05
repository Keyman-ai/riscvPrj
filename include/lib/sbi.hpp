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
