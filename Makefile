# riscvPrj 构建脚本 (风格与 myos 对齐)
# 工具链前缀
CROSS   = riscv64-unknown-elf-
CXX     = $(CROSS)g++

# 目标架构: rv64gc = I + M + A + F + D + C
ARCH     = rv64gc
ABI      = lp64

# 编译 flags
# -MMD -MP: 自动生成头文件依赖 (.d 文件), 改头文件会自动重编
CXXFLAGS = -ffreestanding -nostdlib \
           -fno-exceptions -fno-rtti -fno-stack-protector \
           -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany \
           -O2 -Wall -Wextra \
           -fno-pie -Iinclude -MMD -MP

# 链接 flags (-lgcc: 除法等少量内建函数, 纯硬件指令覆盖不到时兜底)
LDFLAGS = -T linker.ld -nostdlib -ffreestanding -static -no-pie -lgcc

# 源文件 (链接顺序: arch 第一, 其余任意)
CXX_SOURCES = kernel/main.cpp kernel/trap.cpp kernel/timer.cpp kernel/plic.cpp \
              drivers/uart.cpp \
              lib/printf.cpp lib/sbi.cpp
ASM_SOURCES = arch/boot.S arch/trap.S

# 编译产物目录: 对象与 elf 全部输出到 build/
OBJ_DIR = build
OBJECTS = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CXX_SOURCES)) \
          $(patsubst %.S,$(OBJ_DIR)/%.o,$(ASM_SOURCES))

# 默认目标 (必须放在 -include 之前!
# 否则 .d 文件里的规则会成为第一个目标, make 默认就只检查它)
.DEFAULT_GOAL := all
all: $(OBJ_DIR)/riscvPrj.elf

# 引入自动生成的依赖文件 (改头文件 → 自动重编)
-include $(OBJECTS:.o=.d)

# 编译 C++
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 编译汇编
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CXXFLAGS) -c $< -o $@

# 链接
$(OBJ_DIR)/riscvPrj.elf: $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

# 反汇编到文件 (Phase 1 学指令编码的好帮手)
disasm: $(OBJ_DIR)/riscvPrj.elf
	$(CROSS)objdump -d $(OBJ_DIR)/riscvPrj.elf > $(OBJ_DIR)/disasm.txt
	@echo "disassembly -> $(OBJ_DIR)/disasm.txt"

# 启动 QEMU (M5: OpenSBI 固件 -> S 模式内核, 不再 -bios none)
run: $(OBJ_DIR)/riscvPrj.elf
	qemu-system-riscv64 -M virt -smp 2 -m 128M -nographic \
	    -kernel $(OBJ_DIR)/riscvPrj.elf

# 清理
clean:
	rm -rf $(OBJ_DIR)

# ---------- Phase 1 学习工具: 指令集练习 (asm/) ----------

# 汇编 ex1 并反汇编: 看六种指令格式的真实机器码
asm-formats:
	@mkdir -p $(OBJ_DIR)/asm
	$(CROSS)gcc -march=$(ARCH) -mabi=$(ABI) -c asm/ex1-formats.S -o $(OBJ_DIR)/asm/ex1.o
	$(CROSS)objdump -d $(OBJ_DIR)/asm/ex1.o

# 把 ex2 编译成汇编: "看汇编还原 C" 练习 (-O0 与 -O2 各出一份对比)
asm-c2asm:
	@mkdir -p $(OBJ_DIR)/asm
	$(CROSS)gcc -march=$(ARCH) -mabi=$(ABI) -O2 -S asm/ex2-c-to-asm.c -o $(OBJ_DIR)/asm/ex2-O2.s
	$(CROSS)gcc -march=$(ARCH) -mabi=$(ABI) -O0 -S asm/ex2-c-to-asm.c -o $(OBJ_DIR)/asm/ex2-O0.s
	@echo "-> build/asm/ex2-O2.s (优化版)  build/asm/ex2-O0.s (朴素版)"

.PHONY: all run clean disasm asm-formats asm-c2asm
