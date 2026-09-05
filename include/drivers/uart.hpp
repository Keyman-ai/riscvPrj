/* UART 16550 驱动 (QEMU virt 机器, 基址 0x10000000, 字节寄存器) */
#pragma once
#include "types.hpp"

void uart_init();                  /* 初始化: 8N1, 无 FIFO, 关中断 */
void uart_putc(char c);            /* 输出一个字符 ('\n' 自动补 '\r') */
void uart_puts(const char* s);     /* 输出字符串 */
char uart_getc();                  /* 阻塞读取一个字符 */
int  uart_getc_nonblock();         /* 非阻塞读取: 无数据返回 -1, 否则返回 0-255 */

/* M4: 中断驱动接收 */
void uart_enable_rx_irq();         /* 打开 RX 数据可用中断 (IER bit0) */
void uart_rx_irq_handler();        /* ISR 调用: 把收到的字符塞进环形缓冲区 */
int  uart_rx_pop();                /* 主循环调用: 取一个字符, 空返回 -1 */
u32  uart_rx_irq_count();          /* RX 中断次数 (验证用) */
