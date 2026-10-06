CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
SECTION ?= .data
# RV32I 編譯：作業要求以 gcc -O2 -march=rv32i 編譯自己的最終 C 演算法，和手寫組語比較。
# 在 PATH 中依序尋找 riscv64-unknown-elf-gcc、riscv-none-elf-gcc（xPack 版），
# 也可以用 make rv32 RV_CC=/path/to/gcc 指定。
RV_CC ?= $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null || \
	command -v riscv-none-elf-gcc 2>/dev/null || echo riscv64-unknown-elf-gcc)
RV_FLAGS ?= -O2 -march=rv32i -mabi=ilp32 -ffreestanding -nostdlib
# 沒有 crt0 設定 gp，所以連結時關掉 relaxation，避免連結器改用 gp 相對定址
RV_LDFLAGS ?= -Wl,--no-relax

GEN := host/gen_tables

.PHONY: all tables full gui cref rv32 elf gates clean

all: tables full gui cref

# ---- host：表格產生器 ----
$(GEN): host/gen_tables.c host/cube_common.h
	$(CC) $(CFLAGS) $< -o $@

# 同一次執行同時產生組語用的 .s 和 C 參考版用的 .h，兩者內容相同
tables: asm/tables.s asm/tables_ida.s

asm/tables.s: $(GEN)
	./$(GEN) -k 5 -s $(SECTION) -o $@ -c c_reference/tables.h

asm/tables_ida.s: $(GEN)
	./$(GEN) -k 0 -s $(SECTION) -o $@ -c c_reference/tables_ida.h

c_reference/tables.h: asm/tables.s ;
c_reference/tables_ida.h: asm/tables_ida.s ;

# ---- asm：Ripes 一次只讀一個檔案，而且 .equ 要先定義：表格在前、程式在後、renderer 最後 ----
# Ripes 不支援 .if，所以 renderer 的開關是換檔案：CLI 版接 render_cli.s（量測 --iret），
# GUI 版接 render_gui.s（需要在 Ripes 加入 35 × 25 的 LED Matrix）
full: asm/solver_full.s asm/solver_ida_full.s
gui: asm/solver_gui.s

asm/solver_full.s: asm/tables.s asm/solver.s asm/render_cli.s
	cat $^ > $@

asm/solver_ida_full.s: asm/tables_ida.s asm/solver.s asm/render_cli.s
	cat $^ > $@

asm/solver_gui.s: asm/tables.s asm/solver.s asm/render_gui.s
	cat $^ > $@

# ---- c_reference：host 上執行的版本（用來和 solver.s 比對輸出） ----
cref: c_reference/solver_ref c_reference/solver_ref_ida

c_reference/solver_ref: c_reference/solver_ref.c c_reference/tables.h
	$(CC) $(CFLAGS) $< -o $@

c_reference/solver_ref_ida: c_reference/solver_ref.c c_reference/tables_ida.h
	$(CC) $(CFLAGS) -DTABLES_IDA $< -o $@

# ---- c_reference：RV32I 版本（需要 RISC-V 的 gcc） ----
# rv32：gcc 產生的組語，用來閱讀和比較
rv32: c_reference/solver_ref.rv32.s

c_reference/solver_ref.rv32.s: c_reference/solver_ref.c c_reference/tables.h
	$(RV_CC) $(RV_FLAGS) -S $< -o $@

# elf：可以直接在 Ripes 執行（--src … -t elf），量測 gcc -O2 版本的 --iret
elf: c_reference/solver_ref.elf c_reference/solver_ref_ida.elf

c_reference/solver_ref.elf: c_reference/solver_ref.c c_reference/tables.h
	$(RV_CC) $(RV_FLAGS) $(RV_LDFLAGS) $< -o $@

c_reference/solver_ref_ida.elf: c_reference/solver_ref.c c_reference/tables_ida.h
	$(RV_CC) $(RV_FLAGS) $(RV_LDFLAGS) -DTABLES_IDA $< -o $@

# ---- c_reference/H1-H4：對 solver_ref.c 執行 host 端關卡 H1–H4 ----
# 標準答案由方塊模型重新建立，獨立於 tables.h；H3 對全部 3,674,160 個狀態搜尋，需要幾分鐘
GATES := c_reference/H1-H4/h_gates c_reference/H1-H4/h_gates_ida

gates: $(GATES)
	./c_reference/H1-H4/h_gates
	./c_reference/H1-H4/h_gates_ida

c_reference/H1-H4/h_gates: c_reference/H1-H4/h_gates.c c_reference/solver_ref.c c_reference/tables.h
	$(CC) $(CFLAGS) -Ic_reference $< -o $@

c_reference/H1-H4/h_gates_ida: c_reference/H1-H4/h_gates.c c_reference/solver_ref.c c_reference/tables_ida.h
	$(CC) $(CFLAGS) -Ic_reference -DTABLES_IDA $< -o $@

clean:
	$(RM) $(GEN) asm/tables.s asm/tables_ida.s asm/solver_full.s \
		asm/solver_ida_full.s asm/solver_gui.s c_reference/tables.h c_reference/tables_ida.h \
		c_reference/solver_ref c_reference/solver_ref_ida \
		c_reference/solver_ref.rv32.s c_reference/solver_ref.elf \
		c_reference/solver_ref_ida.elf $(GATES)
