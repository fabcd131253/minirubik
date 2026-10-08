# minirubik on RV32I

An optimal solver for the 2×2×2 Rubik's Cube, written in hand-written RV32I
assembly for the [Ripes](https://github.com/mortbopet/Ripes) simulator. This
fork of [sysprog21/minirubik](https://github.com/sysprog21/minirubik) is
Assignment 1 of Computer Architecture (Fall 2026):
<https://hackmd.io/@sysprog/2026-arch-homework1>.

## The problem

The upstream C program, [`solver.c`](solver.c), runs a breadth-first search
over all 3,674,160 states of the cube, stores one move toward solved for every
state, and then follows that table. On a hosted machine this takes 0.065 s. On
the target it does not fit:

| Constraint on the target | Upstream `solver.c` |
| :--- | ---: |
| Static data at most 128 KiB | 18,405,414 B peak (BFS queue 14.0 MiB, table 3.5 MiB) |
| At most 5 × 10⁷ retired instructions per distance-11 state | about 10⁹ to build the table |
| RV32I only: no `mul`, `div`, or `rem` | ranks are built with multiply and divide |

This port keeps optimality, a shortest solution for every state, while
replacing full enumeration with a heuristic search that runs on the simulated
processor.

## Results

Measured on Ripes v2.2.6-106-g5b8a616, `--mode cli --proc RV32_ISS --iret`,
with `RUN_TESTS = 0` and the CLI renderer. The reference is the C version of
the same algorithm, [`c/solver_ref.c`](c/solver_ref.c), compiled with
`gcc -O2 -march=rv32i -mabi=ilp32`.

| | Hand-written assembly | gcc -O2 reference |
| :--- | ---: | ---: |
| `21345671111111` (distance 11) | **849,005** | 1,134,043 |
| Worst distance-11 state, `54721631111111` | **2,342,150** (4.7% of 5 × 10⁷) | 3,125,116 |
| Solved cube `12345671111111` | 471 | 520 |
| `.text` | **2,276 B** | 2,740 B |
| `.data` | 75,264 B (57.4% of 128 KiB) | — |

All 2,644 distance-11 states and 100 random states give the same output on the
assembly and the C reference, and every solution length equals the exact
distance.

## Encoding

One corner, up-front-left, is held fixed, so only the faces `R`, `B`, and `D`
turn. The other seven corners give a state of two independent indices:

| Part | Range | Meaning |
| :--- | :--- | :--- |
| permutation `p` | 0 … 5,039 (7!) | which corner sits in each position, as a Lehmer-code rank |
| orientation `o` | 0 … 728 (3⁶) | twists of the first six corners in base 3; the seventh follows because the twist sum is 0 mod 3 |

A quarter turn changes `p` and `o` independently, so each has its own
transition table (`perm_qt`, `ori_qt`), and the search never combines them into
`p × 729 + o`. The search loop therefore has no multiply or divide; the only
multiplications, while parsing the input, are shift-and-add sequences.

The input is the same 14-character string as upstream: seven corner digits
`1`–`7` for positions 1–7, followed by seven orientation digits `1`–`3`. The
solved cube is `12345671111111`. See the upstream [`report.md`](report.md) for
the position map.

## Algorithm

Iterative-deepening A\* (IDA\*) with a perimeter, a simplified form of
Manzini's BIDA\*, run with a non-recursive frame stack:

- **Pattern databases.** `h_perm[p]` and `h_ori[o]` are the exact distances of
  the permutation alone and of the orientation alone. Any sequence that solves
  the cube also solves each projection, so both are lower bounds, and so is
  their maximum.
- **Perimeter table.** Every state within 5 moves of solved (12,224 states) is
  stored with its exact distance. A state inside the perimeter ends the
  search with its exact remaining distance; a state outside it is at least 6
  moves away, so the heuristic is `max(h_perm, h_ori, 6)`. The table is looked
  up only when both pattern databases are at most 5. We use only membership in
  the perimeter, not BIDA\*'s per-node minimum over perimeter states.
- **Bucket layout.** The perimeter is stored like a compressed sparse row
  matrix: `bucket_start[p]` points to the orientations of permutation `p`,
  sorted by `o`, each packed with its distance into 16 bits. A lookup indexes
  by `p` directly and reads 2.84 entries on average, against a binary search of
  about 14 steps over a sorted list that would also need `p × 729 + o`.
- **Pruning.** A face is never turned twice in a row, so after the first move
  only 6 of the 9 moves are tried. `X2` and `X'` are obtained by applying the
  quarter-turn table again, so no half-turn tables are needed.

The search terminates because each iteration raises the bound by at least one
and the bound cannot pass the true distance, which is at most 11. The first
solution found is optimal because every smaller bound has already failed.

For comparison, the same program with the pattern databases only
(`assembly/ida/`, perimeter radius 0) takes 6,336,661 instructions on
`21345671111111`: the perimeter costs 34,530 bytes and saves a factor of 7.5.

## Memory

All tables are generated on the host by [`host/gen_tables.c`](host/gen_tables.c)
and linked as data. Ripes rejects `.rodata`, so they live in `.data`.

| Table | Bytes |
| :--- | ---: |
| `perm_qt` (3 × 5,040 × 2) | 30,240 |
| `ori_qt` (3 × 729 × 2) | 4,374 |
| `bucket_start` (5,041 × 2) | 10,082 |
| `bucket_entry` (12,224 × 2) | 24,448 |
| `h_perm` | 5,040 |
| `h_ori` | 729 |
| Tables | 74,913 |
| Program data (input, test cases, messages, 96-byte frame stack) | 351 |
| `.data` total | 75,264 |

There is no heap and no `.bss`; the stack holds at most two return addresses.

## Correctness

- **On the host** (`make gates`): over all 3,674,160 states, the heuristic
  never overestimates (H1); every table is fully populated (H2); the search
  returns a solution of exactly the true distance (H3, about one minute); and
  the perimeter lookup agrees with the table at even and odd indices (H4).
- **On the target**: `solver.s` carries four test cases — solved, the 3-move
  scramble `R B' D2`, the distance-11 state `21345671111111`, and an invalid
  state — and checks inside the program that each solution returns to solved
  and has the expected optimal length. It prints `PASS` or `FAIL` per case and
  exits with status 1 if any case fails. They pass on `RV32_ISS` and `RV32_5S`.

## Build and run

Requirements: a host C compiler; a RISC-V gcc (`riscv64-unknown-elf-gcc`, or
xPack `riscv-none-elf-gcc`) for the reference build only; and a Ripes build
that provides `RV32_ISS` (a continuous prerelease, not v2.2.6).

```sh
make rv32i      # tables, assembly/solver_full*.s, and the host C reference
make rv32 elf   # c/solver_ref_rv32i.s and the ELF files (needs RISC-V gcc)
make gates      # H1 to H4 on the host (several minutes)
```

The `run` targets call the Ripes command line, given by `RIPES` (default
`Ripes`). The processor is `PROC` (default `RV32_ISS`), and `--iret` is always
reported. On a machine without a display, run Ripes under `xvfb-run`:

```sh
make run RIPES="xvfb-run -a /path/to/Ripes.AppImage"
```

### (a) The gcc -O2 reference, `c/solver_ref_rv32i.s`

```sh
make run-ref RIPES=/path/to/Ripes
```

This assembles and links `c/solver_ref_rv32i.s` into `c/solver_ref.elf` and
runs it on Ripes. To regenerate the assembly from `c/solver_ref.c` first, run
`make rv32`.

### (b) The hand-written solver, `assembly/solver_full.s`

```sh
make run RIPES=/path/to/Ripes
```

By default the program runs its four test cases and then solves the state in
the `input` line of `assembly/solver.s`:

```
test 1: 12345671111111 PASS 0 moves:
test 2: 24173562322133 PASS 3 moves: D2 B R'
test 3: 21345671111111 PASS 11 moves: R B' D2 R' B R' B' R D2 R B
test 4: 11345671111111 PASS rejected
R B' D2 R' B R' B' R D2 R B

Program exited with code: 0
===== instructions retired
1700106
```

The exit code is 0 on success, 1 if the search, the verification, or a test
case fails, and 2 for an invalid input.

### Choosing the input and the mode

Both targets accept `INPUT`, a 14-character state, and `RUN_TESTS=0`, which
skips the test cases so that `--iret` counts a single query. The committed
files are left unchanged; a substituted copy is built under `build/`.

```sh
make run     INPUT=54721631111111 RUN_TESTS=0   # 2,342,150 instructions
make run-ref INPUT=54721631111111 RUN_TESTS=0   # 3,125,116 instructions
make run-ida RUN_TESTS=0                        # pattern databases only
make run PROC=RV32_5S                           # a pipelined model
```

To change the input permanently, edit the `input` line in
`assembly/solver.s` (or `-DINPUT` for `c/solver_ref.c`) and run `make rv32i`.

### LED matrix animation

In the Ripes GUI, add an LED Matrix in the I/O tab with width 35 and height
25, then load `assembly/solver_full_gui.s`. The cube is drawn as an unfolded
net, first scrambled and then once after every move of the solution. Ripes has
no `.if`, so the renderer is chosen by the file appended last; the GUI and CLI
builds differ only in `render_gui.s` against `render_cli.s`, and the CLI build
spends two instructions on the empty renderer call.

## Upstream C solver

`solver.c` and `mini.c` are the upstream BFS solvers, unchanged:

```sh
make            # builds solver and mini
make check      # test vectors in tests/solutions.txt and invalid inputs
make prove      # optional Frama-C proof
./solver 21345671111111
```

See [`report.md`](report.md) for their model and verification.
