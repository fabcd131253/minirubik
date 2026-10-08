CC ?= cc
CFLAGS ?= -O3 -std=c99 -Wall -Wextra -Wpedantic
FRAMA_C ?= frama-c
CLANG_FORMAT := $(shell command -v clang-format-20 2>/dev/null || \
	command -v clang-format 2>/dev/null)
C_SOURCES := $(wildcard *.c *.h)
SAMPLE_STATE := 21345671111111
SAMPLE_SOLUTION := B' R' D2 R' B R B' R D2 B R'
VECTORS := tests/solutions.txt
# One per rejection path: short, long, cubie digit low, cubie digit high,
# orientation digit low, orientation digit high, non-digit, duplicate, parity.
INVALID_STATES := 1234567111111 123456711111111 02345671111111 82345671111111 \
	12345671111110 12345671111114 1234567111111a 11345671111111 12345671111112

.PHONY: all check prove clean indent

all: solver mini

solver: solver.c
	$(CC) $(CFLAGS) $< -o $@

mini: mini.c
	$(CC) $(CFLAGS) $< -o $@

check: solver mini $(VECTORS)
	./solver --self-test
	@expected=$$(mktemp); actual=$$(mktemp); \
		trap 'rm -f "$$expected" "$$actual"' 0 1 2 15; \
		count=0; \
		while IFS='|' read -r state solution; do \
			case "$$state" in ""|\#*) continue ;; esac; \
			printf '%s\n' "$$solution" >"$$expected"; \
			for binary in ./solver ./mini; do \
				$$binary "$$state" >"$$actual"; \
				status=$$?; \
				test $$status -eq 0 || { \
					echo "$$binary $$state: exit status $$status"; exit 1; }; \
				cmp -s "$$actual" "$$expected" || { \
					echo "$$binary $$state: output mismatch"; \
					echo "  expected: $$solution"; \
					printf '  got:      '; cat "$$actual"; \
					echo "  ($$(wc -c <"$$expected") bytes expected, \
$$(wc -c <"$$actual") produced)"; exit 1; }; \
			done; \
			count=$$((count + 1)); \
		done <$(VECTORS); \
		echo "$$count solution vectors matched by solver and mini"
	@for binary in ./solver ./mini; do \
		for bad in $(INVALID_STATES); do \
			$$binary "$$bad" >/dev/null 2>&1; \
			status=$$?; \
			test $$status -eq 2 || { \
				echo "$$binary $$bad: expected status 2, got $$status"; exit 1; }; \
		done; \
		$$binary >/dev/null 2>&1; \
		status=$$?; \
		test $$status -eq 2 || { \
			echo "$$binary with no argument: expected status 2, got $$status"; \
			exit 1; }; \
		$$binary $(SAMPLE_STATE) $(SAMPLE_STATE) >/dev/null 2>&1; \
		status=$$?; \
		test $$status -eq 2 || { \
			echo "$$binary with two arguments: expected status 2, got $$status"; \
			exit 1; }; \
		$$binary $(SAMPLE_STATE) >&- 2>/dev/null; \
		status=$$?; \
		test $$status -eq 1 || { \
			echo "$$binary with stdout closed: expected status 1, got $$status"; \
			exit 1; }; \
	done
	@./solver --self-test >&- 2>/dev/null; \
		status=$$?; \
		test $$status -eq 1 || { \
			echo "solver --self-test with stdout closed: expected 1, got $$status"; \
			exit 1; }
	@echo "invalid input rejected with status 2, unwritable stdout with status 1"

prove: solver.c
	@log=$$(mktemp); trap 'rm -f "$$log"' 0 1 2 15; \
		$(FRAMA_C) -wp -wp-fct quarter_turn,rank_state,valid,parse_state \
		-wp-rte -rte-verbose 0 -wp-prover alt-ergo -wp-timeout 20 \
		-wp-cache none solver.c >"$$log" 2>&1; rc=$$?; \
		grep -Fvx -e '[wp] Warning: Skipped RTE guards: unaligned pointers (\aligned not supported)' \
		-e '[wp] Warning: Skipped RTE guards: invalid function pointer calls (\valid_function not supported)' "$$log"; \
		test $$rc -eq 0 && awk '$$1 == "[wp]" && $$2 == "Proved" && $$3 == "goals:" && $$4 > 0 && $$4 == $$6 { ok = 1 } END { exit !ok }' "$$log" && \
		! grep -Eq '(^|[[:space:]])(Timeout|Unknown|Failed):' "$$log"

indent:
ifeq ($(CLANG_FORMAT),)
	$(error clang-format 20 not found)
endif
	@$(CLANG_FORMAT) --version | grep -q 'version 20' || \
		{ echo "error: clang-format version 20 required"; exit 1; }
	$(CLANG_FORMAT) -i $(C_SOURCES)

# ---------------------------------------------------------------------------
# RV32I port (Ripes): host/ generates the tables, assembly/ holds the
# hand-written solver, c/ holds the C reference that gcc -O2 is measured on.
# Files under ida/ are the PDB-only variant (perimeter radius K = 0), kept to
# measure what the perimeter table buys.

SECTION ?= .data
# gcc for the RV32I reference build: riscv64-unknown-elf-gcc, else the xPack
# riscv-none-elf-gcc; override with make RV_CC=/path/to/gcc.
RV_CC ?= $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null || \
	command -v riscv-none-elf-gcc 2>/dev/null || echo riscv64-unknown-elf-gcc)
RV_FLAGS ?= -O2 -march=rv32i -mabi=ilp32 -ffreestanding -nostdlib
# There is no crt0 to set gp, so keep the linker from relaxing to gp-relative
# addressing.
RV_LDFLAGS ?= -Wl,--no-relax
# Ripes command line. On a machine without a display use for example
#   make run RIPES="xvfb-run -a /path/to/Ripes.AppImage"
RIPES ?= Ripes
PROC ?= RV32_ISS
# Optional overrides for run and run-ref: the 14-character input state, and
# RUN_TESTS=0 to skip the built-in test cases (as for measuring --iret).
INPUT ?=
RUN_TESTS ?=

GEN := host/gen_tables
GATES := c/H1-H4/h_gates c/H1-H4/h_gates_ida
RIPES_CLI = $(RIPES) --mode cli --proc $(PROC) --iret

.PHONY: rv32i tables full gui cref rv32 elf gates run run-ida run-ref distclean

rv32i: tables full gui cref

# ---- host: table generator ----
$(GEN): host/gen_tables.c host/cube_common.h
	$(CC) $(CFLAGS) $< -o $@

# One run writes both the assembly data file and the identical C header.
tables: assembly/tables.s assembly/ida/tables_ida.s

assembly/tables.s: $(GEN)
	./$(GEN) -k 5 -s $(SECTION) -o $@ -c c/tables.h

assembly/ida/tables_ida.s: $(GEN)
	./$(GEN) -k 0 -s $(SECTION) -o $@ -c c/ida/tables_ida.h

c/tables.h: assembly/tables.s ;
c/ida/tables_ida.h: assembly/ida/tables_ida.s ;

# ---- assembly: Ripes reads one file and needs .equ before use, so the tables
# come first and the renderer last. Ripes has no .if, so the renderer is
# switched by the file appended: render_cli.s for measuring, render_gui.s for
# the LED matrix animation. The two builds differ only in that file.
full: assembly/solver_full.s assembly/ida/solver_ida_full.s
gui: assembly/solver_full_gui.s

assembly/solver_full.s: assembly/tables.s assembly/solver.s assembly/render_cli.s
	cat $^ > $@

assembly/ida/solver_ida_full.s: assembly/ida/tables_ida.s assembly/solver.s assembly/render_cli.s
	cat $^ > $@

assembly/solver_full_gui.s: assembly/tables.s assembly/solver.s assembly/render_gui.s
	cat $^ > $@

# ---- c: host builds of the C reference, compared against solver.s ----
cref: c/solver_ref c/ida/solver_ref_ida

c/solver_ref: c/solver_ref.c c/tables.h
	$(CC) $(CFLAGS) $< -o $@

c/ida/solver_ref_ida: c/solver_ref.c c/ida/tables_ida.h
	$(CC) $(CFLAGS) -DTABLES_IDA $< -o $@

# ---- c: RV32I reference build (needs a RISC-V gcc) ----
# rv32: the gcc -O2 assembly; elf: that assembly linked, runnable on Ripes.
rv32: c/solver_ref_rv32i.s
elf: c/solver_ref.elf c/ida/solver_ref_ida.elf

c/solver_ref_rv32i.s: c/solver_ref.c c/tables.h
	$(RV_CC) $(RV_FLAGS) -S $< -o $@

c/solver_ref.elf: c/solver_ref_rv32i.s
	$(RV_CC) $(RV_FLAGS) $(RV_LDFLAGS) $< -o $@

c/ida/solver_ref_ida.elf: c/solver_ref.c c/ida/tables_ida.h
	$(RV_CC) $(RV_FLAGS) $(RV_LDFLAGS) -DTABLES_IDA $< -o $@

# ---- c/H1-H4: host gates H1-H4 on solver_ref.c against an exact BFS table.
# H3 searches all 3,674,160 states and takes minutes.
gates: $(GATES)
	./c/H1-H4/h_gates
	./c/H1-H4/h_gates_ida

c/H1-H4/h_gates: c/H1-H4/h_gates.c c/solver_ref.c c/tables.h
	$(CC) $(CFLAGS) -Ic $< -o $@

c/H1-H4/h_gates_ida: c/H1-H4/h_gates.c c/solver_ref.c c/ida/tables_ida.h
	$(CC) $(CFLAGS) -Ic -DTABLES_IDA $< -o $@

# ---- run on Ripes (CLI) ----
# Without INPUT or RUN_TESTS the committed files are run as they are; with
# either, a copy with those values substituted is built under build/.
SED_INPUT = $(if $(INPUT),-e 's/^input:[[:space:]]*\.string[[:space:]]*"[^"]*"/input:  .string "$(INPUT)"/')
COMMA := ,
SED_TESTS = $(if $(RUN_TESTS),-e 's/^\([[:space:]]*\)\.equ RUN_TESTS$(COMMA) [01]/\1.equ RUN_TESTS$(COMMA) $(RUN_TESTS)/')
REF_DEFS = $(if $(INPUT),-DINPUT='"$(INPUT)"') $(if $(RUN_TESTS),-DRUN_TESTS=$(RUN_TESTS))

ifeq ($(INPUT)$(RUN_TESTS),)
RUN_SRC := assembly/solver_full.s
RUN_IDA_SRC := assembly/ida/solver_ida_full.s
REF_ELF := c/solver_ref.elf
else
RUN_SRC := build/solver_full.s
RUN_IDA_SRC := build/solver_ida_full.s
REF_ELF := build/solver_ref.elf
endif

build/solver_full.s: assembly/solver_full.s FORCE
	@mkdir -p build
	sed -e '' $(SED_INPUT) $(SED_TESTS) $< > $@

build/solver_ida_full.s: assembly/ida/solver_ida_full.s FORCE
	@mkdir -p build
	sed -e '' $(SED_INPUT) $(SED_TESTS) $< > $@

build/solver_ref_rv32i.s: c/solver_ref.c c/tables.h FORCE
	@mkdir -p build
	$(RV_CC) $(RV_FLAGS) $(REF_DEFS) -S $< -o $@

build/solver_ref.elf: build/solver_ref_rv32i.s
	$(RV_CC) $(RV_FLAGS) $(RV_LDFLAGS) $< -o $@

# Ripes prints NUL bytes around program output; tr removes them.
run: $(RUN_SRC)
	$(RIPES_CLI) --src $< -t asm 2>&1 | tr -d '\000'

run-ida: $(RUN_IDA_SRC)
	$(RIPES_CLI) --src $< -t asm 2>&1 | tr -d '\000'

run-ref: $(REF_ELF)
	$(RIPES_CLI) --src $< -t elf 2>&1 | tr -d '\000'

.PHONY: FORCE
FORCE:

clean:
	$(RM) solver mini
	$(RM) $(GEN) c/solver_ref c/ida/solver_ref_ida c/solver_ref.elf \
		c/ida/solver_ref_ida.elf $(GATES)
	$(RM) -r build

# Removes the generated files that are committed as well; make rv32i rebuilds
# them, and make rv32 rebuilds c/solver_ref_rv32i.s.
distclean: clean
	$(RM) assembly/tables.s assembly/ida/tables_ida.s assembly/solver_full.s \
		assembly/ida/solver_ida_full.s assembly/solver_full_gui.s \
		c/tables.h c/ida/tables_ida.h c/solver_ref_rv32i.s
