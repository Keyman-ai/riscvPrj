/* UART 16550 驱动 (QEMU virt 机器) */
#include "drivers/uart.hpp"

/* MMIO 基地址: QEMU virt 的 standard UART (16550 兼容) */
constexpr u64 UART_BASE = 0x10000000;

/* 寄存器偏移 (16550, 字节访问) */
constexpr u64 REG_THR = 0x00;   /* 发送保持寄存器 (写) */
constexpr u64 REG_RBR = 0x00;   /* 接收缓冲寄存器 (读) */
constexpr u64 REG_IER = 0x01;   /* 中断使能寄存器 */
constexpr u64 REG_FCR = 0x02;   /* FIFO 控制寄存器 */
constexpr u64 REG_LCR = 0x03;   /* 线路控制寄存器 */
constexpr u64 REG_LSR = 0x05;   /* 线路状态寄存器 */

static inline volatile u8* reg(u64 offset) {
    return reinterpret_cast<volatile u8*>(UART_BASE + offset);
}

void uart_init() {
    *reg(REG_LCR) = 0x03;   /* 8N1: 8 数据位, 无校验, 1 停止位 */
    *reg(REG_FCR) = 0x00;   /* 关闭 FIFO (轮询模式) */
    *reg(REG_IER) = 0x00;   /* 先关所有中断, M4 阶段再开 RX 中断 */
}

void uart_putc(char c) {
    /* '\n' 先补 '\r', 避免终端只换行不回车 */
    if (c == '\n') {
        uart_putc('\r');
    }
    /* 轮询 LSR bit5 (THRE): 发送保持寄存器空才能写 */
    while (!(*reg(REG_LSR) & (1 << 5)))
        ;
    *reg(REG_THR) = static_cast<u8>(c);
}

void uart_puts(const char* s) {
    while (*s) {
        uart_putc(*s++);
    }
}

char uart_getc() {
    /* 轮询 LSR bit0 (DR): 接收数据就绪 */
    while (!(*reg(REG_LSR) & 0x01))
        ;
    return static_cast<char>(*reg(REG_RBR));
}

int uart_getc_nonblock() {
    if (!(*reg(REG_LSR) & 0x01)) {
        return -1;   /* 无数据 */
    }
    return *reg(REG_RBR) & 0xFF;
}

/* ================= M4: 中断驱动接收 =================
 * 环形缓冲区: ISR 是生产者(写 tail), 主循环是消费者(写 head)。
 * 单生产者/单消费者 + 单 hart: 两个下标各自只被一方写, 无需加锁。
 * 满时丢新字符 (tail 追上 head 就不写)。 */

constexpr u32 RXBUF_SIZE = 256;
static char rxbuf[RXBUF_SIZE];
static volatile u32 rx_head = 0;   /* 消费者: 主循环读/写 */
static volatile u32 rx_tail = 0;   /* 生产者: ISR 读/写 */
static volatile u32 rx_irq_cnt = 0;

void uart_enable_rx_irq() {
    *reg(REG_IER) |= 0x01;   /* IER bit0: RX 数据可用中断 */
}

/* ISR 上下文调用 (此时 MIE=0, 不会被再次打断; 绝不在这里 printf!) */
void uart_rx_irq_handler() {
    rx_irq_cnt++;
    /* 有数据就取 (FIFO 关闭时每字节触发一次, 循环是保险) */
    while (*reg(REG_LSR) & 0x01) {
        u32 next = (rx_tail + 1) % RXBUF_SIZE;
        char c = static_cast<char>(*reg(REG_RBR));
        if (next != rx_head) {      /* 缓冲区没满 */
            rxbuf[rx_tail] = c;
            rx_tail = next;
        }
        /* 满了: 字符已被 RBR 读出, 直接丢弃 */
    }
}

/* 主循环调用: 非阻塞取一个字符 */
int uart_rx_pop() {
    if (rx_head == rx_tail) {
        return -1;                  /* 空 */
    }
    char c = rxbuf[rx_head];
    rx_head = (rx_head + 1) % RXBUF_SIZE;
    return c & 0xFF;
}

u32 uart_rx_irq_count() {
    return rx_irq_cnt;
}
