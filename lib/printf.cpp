/* 迷你 printf: %d %i %u %x %X %c %s %p %%
 * 移植自 myos (aarch64), 去掉信号量锁 (M1 无调度器)。
 * 无 libc: 用 __builtin_va_* 处理变参。 */
#include "lib/printf.hpp"
#include "drivers/uart.hpp"

/* INT64_MIN 取反会溢出, 这里用 u64 直接表示其绝对值 */
static constexpr u64 INT64_MIN_ABS = (u64)1 << 63;

static void print_unsigned(u64 num, u32 base, bool upper) {
    char buf[32];   /* 数字字符缓冲, 先倒序生成再反打 */
    int i = 0;
    do {
        u64 digit = num % base;
        if (digit < 10) {
            buf[i++] = static_cast<char>('0' + digit);
        } else {
            buf[i++] = upper ? static_cast<char>('A' + digit - 10)
                             : static_cast<char>('a' + digit - 10);
        }
        num /= base;
    } while (num > 0);
    while (--i >= 0) {
        uart_putc(buf[i]);
    }
}

static void print_signed(s64 num, u32 base) {
    if (num < 0) {
        uart_putc('-');
        if (static_cast<u64>(num) == INT64_MIN_ABS) {
            print_unsigned(INT64_MIN_ABS, base, false);   /* 避免取反溢出 */
        } else {
            num = -num;
            print_unsigned(static_cast<u64>(num), base, false);
        }
    } else {
        print_unsigned(static_cast<u64>(num), base, false);
    }
}

void printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++;   /* 跳过 '%' */
        switch (*fmt) {
            case 'd': case 'i': {
                int val = __builtin_va_arg(ap, int);
                print_signed(val, 10);
                break;
            }
            case 'u': {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                print_unsigned(val, 10, false);
                break;
            }
            case 'x': {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                print_unsigned(val, 16, false);
                break;
            }
            case 'X': {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                print_unsigned(val, 16, true);
                break;
            }
            case 'c': {
                int val = __builtin_va_arg(ap, int);
                uart_putc(static_cast<char>(val));
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                while (*s) uart_putc(*s++);
                break;
            }
            case 'p': {
                void* p = __builtin_va_arg(ap, void*);
                uart_puts("0x");
                print_unsigned(reinterpret_cast<u64>(p), 16, false);
                break;
            }
            case '%': {
                uart_putc('%');
                break;
            }
            default: {
                /* 未知格式: 原样输出 % + 字符 */
                uart_putc('%');
                uart_putc(*fmt);
                break;
            }
        }
        fmt++;
    }

    __builtin_va_end(ap);
}
