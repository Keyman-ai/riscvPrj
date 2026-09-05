/* riscvPrj 链接脚本：QEMU virt 机器, DRAM 基址 0x80000000
 * M5 起由 OpenSBI 固件加载: OpenSBI(fw_dynamic) 占据 0x80000000 起的前 2MB,
 * 内核链接/加载到 0x80200000 (QEMU 默认 payload 地址, Linux Image 同款惯例) */
OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)
ENTRY(_start)                        /* 入口: arch/boot.S 的 _start */

SECTIONS
{
    . = 0x80200000;                  /* OpenSBI 之后: 0x80000000 + 2MB */

    . = ALIGN(16);
    .text : {
        *(.text.boot)                /* boot.S 的启动代码优先 */
        *(.text*)                    /* 其余代码 */
    }

    . = ALIGN(16);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(16);
    .data : {
        __global_pointer$ = . + 0x800;  /* gp 指向 data 段中间, 覆盖 ±2KB 小数据 */
        *(.data*)
    }

    . = ALIGN(16);
    .bss : {
        __bss_start = .;             /* 给 boot.S 清零用 */
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;               /* 给 boot.S 清零用 */
    }

    . = ALIGN(16);
    . = . + 0x10000;                 /* 64KB 内核栈 (单 hart; 多核阶段再改 per-hart) */
    __stack_top = .;                 /* 给 boot.S 用 */

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.eh_frame_hdr)
    }
}
