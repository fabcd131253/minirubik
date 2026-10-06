# The Mini-Rubik and Its C99 Solver

This report describes the 2×2×2 Rubik’s Cube as a finite state graph and explains the design, algorithmic complexity, and formal validation of the C99 solver in `solver.c`.

## Result

The Mini-Rubik is the 2×2×2 Rubik’s Cube (also known as the Pocket Cube). With one corner fixed to eliminate whole-cube rotations, its reachable state space contains exactly:

$$
7! \times 3^6 = 5{,}040 \times 729 = 3{,}674{,}160
$$

configurations. Counting the 24 whole-cube rotations as distinct positions instead gives $8! \times 3^7 = 88{,}179{,}840$, which is exactly $24 \times 3{,}674{,}160$; fixing one corner divides that factor out, and the solver works in the quotient.

In the half-turn metric (HTM), where quarter turns ($90^\circ$), inverse quarter turns ($-90^\circ$), and half turns ($180^\circ$) each count as a single move, the Cayley graph diameter is exactly 11. Every valid state can be solved in at most 11 moves. In the quarter-turn metric, where a half turn costs two moves, the diameter is 14; this solver measures and reports the HTM value.

```diagram
     ┌──────────────────────────────────┐
     │ Fix corner 0 at front-upper-left │
     └──────────────────────────────────┘
                       │
        ┌──────────────▾──────────────┐
        │ 7 positions, 7 orientations │
        └─────────────────────────────┘
                       │
  ┌────────────────────▾───────────────────┐
  │ Permutation and orientation invariants │
  └────────────────────────────────────────┘
                       │
      ┌────────────────▾────────────────┐
      │ Lehmer rank x 729 + base-3 rank │
      └─────────────────────────────────┘
                       │
          ┌────────────▾───────────┐
          │ 3,674,160 dense states │
          └────────────────────────┘
```

## 1. From a Physical Puzzle to a State Graph

Ernő Rubik built the first cube prototype in 1974 as an architectural model to demonstrate 3D kinematic movement: pieces move independently without the structure falling apart. The standard 3×3×3 cube has 8 corner cubies, 12 edge cubies, and 6 fixed centers, resulting in:

$$
\frac{8! \times 3^7 \times 12! \times 2^{11}}{2} \approx 4.33 \times 10^{19}
$$

reachable configurations. Enumerating or exploring that state space directly via uniform search is computationally infeasible.

[Philo Li’s formula-free tutorial](https://philoli.com/zh/blog/solve-rubiks-cube-without-formulas/) presents an algebraic mental model rather than rote sequence memorization. Face turns are permutations of cubies and orientations governed by three group-theoretic properties:

- Composition: turns compose to produce subsequent permutations ($g_1 \circ g_2 \in G$).
- Inverses: every turn can be inverted ($g \circ g^{-1} = e$).
- Order sensitivity: cube moves generally do not commute ($R U \neq U R$).

Human solvers exploit these properties via commutators, $[A, B] = A B A^{-1} B^{-1}$. Moving a target piece into a working area with $A$, modifying that area with $B$, and applying $A^{-1} B^{-1}$ leaves the rest of the cube invariant while applying local changes. Philo Li applies this intuition to human 3×3×3 Roux-style solving: block building, corner resolution, and edge orientation and placement.

The 2×2×2 solver addresses a simpler permutation group. A 2×2×2 cube contains no edge cubies and no centers. Instead of human multi-phase heuristics or commutator generation, this solver represents the complete Cayley graph of the 2×2×2 cube and solves any position optimally using precomputed retrograde search, the classical approach to small permutation groups set out by Cooperman and Finkelstein [2].

The two columns describe different puzzles, human 3×3×3 solving against a 2×2×2 program, so the comparison is one of method rather than of move counts:

| View | Human formula-free solving | This C99 solver |
| :--- | :--- | :--- |
| State representation | Visual features and partially solved blocks | Dense 32-bit integer rank |
| Move selection | Target tracking and protecting solved blocks | Single table lookup pointing toward solved |
| Strategy | Composing local commutators $[A, B]$ | Retrograde breadth-first search (BFS) |
| Optimality guarantee | None | Strictly optimal (at most 11 moves in HTM) |

## 2. Mini-Rubik Model

The 2×2×2 cube consists of 8 corner cubies. Fixing one corner, specifically cubie 0 at the front-upper-left (FUL) position, removes the 24 whole-cube rotational symmetries. The remaining 7 physical positions and cubies are indexed 1 through 7 in this report; `solver.c` indexes the same seven slots 0 through 6 in the arrays `state_t.p` and `state_t.o`, so report position $i$ is array index $i - 1$.

Orientations are cyclic twists in $\mathbb{Z}_3$:

- `1` (internal `0`): solved orientation.
- `2` (internal `1`): clockwise twist ($+120^\circ$).
- `3` (internal `2`): counterclockwise twist ($+240^\circ \equiv -120^\circ$).

Two symbols are used for these throughout the report: $d_i \in \{1, 2, 3\}$ is the input digit at position $i$, and $o_i = d_i - 1 \in \{0, 1, 2\}$ is the internal value stored in `state_t.o`.

### Unfolded Face Net

The fixed corner is marked as `0`. Each corner appears on three adjacent faces. This is an exterior unfold: every face is drawn as seen from outside the cube, so the back face reads `4 7` from left to right. `README.md` draws the back layer the other way, `7 4`, because it looks through the cube from the front; both describe the same corners.

```diagram
                         UP
                      ┌───┬───┐
                      │ 7 │ 4 │
                      ├───┼───┤
                      │ 0 │ 1 │
                      └───┴───┘

       LEFT              FRONT             RIGHT              BACK
    ┌───┬───┐         ┌───┬───┐         ┌───┬───┐         ┌───┬───┐
    │ 7 │ 0 │         │ 0 │ 1 │         │ 1 │ 4 │         │ 4 │ 7 │
    ├───┼───┤         ├───┼───┤         ├───┼───┤         ├───┼───┤
    │ 6 │ 3 │         │ 3 │ 2 │         │ 2 │ 5 │         │ 5 │ 6 │
    └───┴───┘         └───┴───┘         └───┴───┘         └───┴───┘

                        DOWN
                      ┌───┬───┐
                      │ 3 │ 2 │
                      ├───┼───┤
                      │ 6 │ 5 │
                      └───┴───┘
```

The solved state vector across moving positions $1 \dots 7$ is:

```text
positions:     1 2 3 4 5 6 7
orientations:  1 1 1 1 1 1 1
```

### Generators and Move Cycles

Because corner 0 is fixed at the intersection of faces Up ($U$), Front ($F$), and Left ($L$), turning any of those three faces would move corner 0. Every transformation of the quotient can therefore be expressed through the three opposite faces: Right ($R$), Back ($B$), and Down ($D$).

In the half-turn metric, each face has 3 non-trivial rotations ($90^\circ$, $180^\circ$, $270^\circ$), yielding 9 generator moves:

```text
R, R2, R'     B, B2, B'     D, D2, D'
```

A bare letter is a $90^\circ$ turn clockwise as seen from outside that face, `'` is its inverse, and `2` is a half turn. Each quarter turn performs a 4-cycle on corner positions and leaves the other three moving corners alone. The arrows below trace where a cubie travels; the number in parentheses is the twist added to whatever lands in that position, matching `source[face][]` and `twist[face][]` in `solver.c`:

```diagram
  R: right face           B: back face            D: down face

  1(+1) ──▸ 4(+2)         4(+1) ──▸ 7(+2)         2 ──▸ 5
    ▴         │             ▴         │           ▴     │
    │         ▾             │         ▾           │     ▾
  2(+2) ◂── 5(+1)         5(+2) ◂── 6(+1)         3 ◂── 6
```

### Orientation Changes under Quarter Turns

1. Right turn ($R$):
   - Positions 1 (FUR) and 5 (BDR) increment orientation: $+1 \pmod 3$.
   - Positions 4 (BUR) and 2 (FDR) decrement orientation: $+2 \pmod 3$.
   - Net orientation delta: $1 + 2 + 1 + 2 = 6 \equiv 0 \pmod 3$.
2. Back turn ($B$):
   - Positions 4 (BUR) and 6 (BDL) increment orientation: $+1 \pmod 3$.
   - Positions 5 (BDR) and 7 (BUL) decrement orientation: $+2 \pmod 3$.
   - Net orientation delta: $1 + 2 + 1 + 2 = 6 \equiv 0 \pmod 3$.
3. Down turn ($D$):
   - All 4 corners rotate within the down plane without changing their relation to the Up/Down reference axis ($+0 \pmod 3$).
   - Net orientation delta: $0 \equiv 0 \pmod 3$.

Every generator turn preserves the total twist modulo 3: every turn leaves $\sum_{i=1}^{7} o_i \bmod 3$ unchanged, and the solved state has $\sum o_i = 0$, so every reachable state satisfies $\sum_{i=1}^{7} o_i \equiv 0 \pmod 3$. That is the constraint `valid` checks and the reason the seventh orientation carries no information.

## 3. Compact State Representation

This section indexes cubies by the internal indices 0 through 6, and $o_i$ keeps the meaning fixed in section 2: the internal orientation `state_t.o[i]`. A physical state is valid if and only if it satisfies two invariants:

1. Permutation validity: the seven entries of `state_t.p` form a bijection of the internal cubie indices $\{0, 1, 2, 3, 4, 5, 6\}$, which are the report's cubies 1 through 7 shifted down by one.

2. Orientation constraint: every internal orientation $o_i \in \{0, 1, 2\}$, and the sum across all cubies is divisible by 3:

   $$
   \sum_{i=0}^6 o_i \equiv 0 \pmod 3
   $$

Because the orientation sum is constrained modulo 3, the orientation of the 7th cubie is uniquely determined by the first 6:

$$
o_6 = (3 - (\sum_{i=0}^5 o_i \bmod 3)) \bmod 3
$$

### Bijective Indexing

The state is encoded into a dense integer in the range $[0, 3{,}674{,}160)$ using two rank components:

1. Permutation Lehmer rank ($`p \in [0, 7!)`$), computed with the factoradic numeral system:

   $$p = \sum_{i=0}^6 c_i \times (6 - i)!, \quad c_i = \sum_{j=i+1}^6 [s.p[j] < s.p[i]]$$

   `rank_state` evaluates this by Horner's rule, `p = p * (7 - i) + c_i`, which needs no factorial table and keeps every partial value below 5,040.

2. Orientation rank ($`o \in [0, 3^6)`$), evaluated as a base-3 integer over the first 6 orientations:

   $$o = \sum_{i=0}^5 s.o[i] \times 3^{5 - i}$$

3. Composite rank:

   $$\text{rank} = p \times 729 + o$$

This encoding is a bijection between valid physical configurations and array indices in $[0, 3{,}674{,}160)$. Valmari [3] treats this same puzzle as a case study in how far a dense encoding can shrink the table, and reaches the same conclusion that the orientation of the last cubie is redundant. `unrank_state` inverts it: it peels factoradic digits off $p$ against a shrinking list of unused cubies, reads the six base-3 digits of $o$, and recomputes the seventh orientation from the parity constraint. The executable self-test checks the round trip on every one of the 3,674,160 ranks.

## 4. Solver Construction

The solver uses retrograde breadth-first search starting from the solved state (rank 0). All 9 generators cost one move, so a FIFO queue dequeues states in nondecreasing distance from solved and the first visit to a state is along a shortest path. Because every move $m$ has an inverse $m^{-1}$ with $m \circ m^{-1} = e$, when the search steps from an already-settled state $u$ to an unvisited $v$ via $m$, the move $m^{-1}$ takes $v$ back to $u$, one step closer to solved. The table stores that inverse, one byte per state, and following it from any state walks a shortest path home.

Before BFS, the implementation builds separate quarter-turn transition tables for the 5,040 permutation ranks and the 729 orientation ranks. This factoring is exact rather than an approximation: a quarter turn of face $f$ sets $p'[i] = p[\text{source}[f][i]]$ and $o'[i] = (o[\text{source}[f][i]] + \text{twist}[f][i]) \bmod 3$, so the new permutation depends only on the old permutation and the new orientation vector only on the old orientation vector. The composite rank therefore splits, advances componentwise by table lookup, and recombines as $p' \times 729 + o'$. Applying each face transition one, two, or three times produces the quarter, half, and inverse turns without reconstructing or re-ranking a `state_t` in the hot loop, which is what keeps 33 million edge expansions cheap.

```diagram
           ┌──────────────────────┐
           │ solved state, rank 0 │
           └──────────────────────┘
                       │
   ┌───────────────────▾──────────────────┐
   │ factor quarter turns into two tables │
   └──────────────────────────────────────┘
                       │
  ┌────────────────────▾──────────────────┐
  │ BFS level by level: pop rank, apply 9 │
  │            generator moves            │
  └───────────────────────────────────────┘
                       │
   ┌───────────────────▾─────────────────┐
   │ first visit: toward_solved[there] = │
   │            inverse move             │
   └─────────────────────────────────────┘
                       │
       ┌───────────────▾───────────────┐
       │ queue empty: 3,674,160 states │
       │      over depths 0 to 11      │
       └───────────────────────────────┘
                       │
    ┌──────────────────▾────────────────┐
    │ free queue, retain 3.50 MiB table │
    └───────────────────────────────────┘
```

A query then walks the table:

```diagram
             ┌───────────────────┐
             │ 14-digit argument │
             └───────────────────┘
                       │
  ┌────────────────────▾────────────────────┐
  │ parse_state: digits, permutation, twist │
  │                   sum                   │
  └─────────────────────────────────────────┘
                       │
          ┌────────────▾─────────────┐
          │ rank_state: dense rank r │
          └──────────────────────────┘
                       │
  ┌────────────────────▾─────────────────────┐
  │ r nonzero: emit move table[r], apply it, │
  │                 re-rank                  │
  └──────────────────────────────────────────┘
                       │
        ┌──────────────▾───────────────┐
        │ r = 0 after at most 11 moves │
        └──────────────────────────────┘
```

`toward_solved[0]` is written as 0 when the search starts, but the solve loop exits on rank 0, so that byte is never read.

### Memory Footprint and Performance

`build_table` holds three allocations simultaneously, and their sum is the high-water mark for the whole program:

| Allocation | Size | Storage |
| :--- | ---: | :--- |
| `toward_solved`, one move byte per state | 3,674,160 B, 3.504 MiB | heap |
| `queue`, one 4-byte rank per state | 14,696,640 B, 14.016 MiB | heap |
| Transition tables, $3 \times (5{,}040 + 729)$ `uint16_t` | 34,614 B, 33.80 KiB | automatic, in `build_table` |
| Computed peak | 18,405,414 B, 17.553 MiB | |

The queue accounts for 79.8% of that. It is sized to the entire state space rather than to an estimated frontier, and the sizing is exact rather than pessimistic: BFS enqueues each reachable state exactly once, every one of the 3,674,160 states is reachable, so `tail` finishes at precisely `STATES`. That identity is what `build_table` tests to decide the search was complete. A rank needs only 22 bits, since $3{,}674{,}160 < 2^{22}$, so three-byte entries would save 3.5 MiB, but `uint32_t` keeps the indexing a single shift-free load and the queue is freed before its size matters to anything else.

### Computed Against Measured

Peak resident set size on the development machine (Apple silicon, `cc -O3 -std=c99`), measured with `/usr/bin/time -l`:

| Run | Peak RSS | Computed | Difference |
| :--- | ---: | ---: | ---: |
| `./solver 21345671111111` | 19,808,256 B, 18.89 MiB | 17.553 MiB | 1.338 MiB |
| `./solver --self-test` | 19,808,256 B, 18.89 MiB | 17.553 MiB | 1.338 MiB |
| `./mini 21345671111111` | 56,524,800 B, 53.91 MiB | 52.559 MiB | 1.347 MiB |

Each figure is the maximum over eight runs on an idle machine, four in the case of `--self-test`. Under load the readings scatter downward, by as much as 15 MiB for `mini`, because the high-water mark counts only pages actually resident and the kernel reclaims under pressure; occasional readings land a single page higher. The maximum on an idle machine is the figure worth quoting, and it is the one that matches the arithmetic.

The gap is a fixed process baseline, not allocator overhead that scales with the request. A control program that allocates and touches exactly 100 MiB measures 106,233,856 B, which is 1.313 MiB above its own arithmetic, and the three rows above sit within 10 KiB of each other on that same margin despite differing by 35 MiB in what they allocate. The computed column is therefore the one to reason about when changing the design; add roughly 1.34 MiB to predict what the process will occupy.

The peak is also transient. Once BFS completes, the queue is freed and the transition tables leave scope, so the program holds 3.504 MiB for the entire query phase and exits from there. Nothing in the design requires the peak to persist, which is what makes the queue worth removing; section 7 describes how to do that and bring the peak to within 34 KiB of the retained figure.

Timing on the same machine, reported as the minimum of seven runs on an idle machine, since every figure here moves by half again under load: `./solver 21345671111111` takes 0.065 s wall clock and `./solver 12345671111111`, an already-solved cube needing zero moves, takes the same 0.065 s, so table construction is effectively the entire runtime and the query itself is unmeasurable beside it. `./solver --self-test` takes 0.145 s, since it additionally unranks, validates, and re-ranks every one of the 3,674,160 states before building the table. An invalid argument returns in 0.002 s, because validation precedes any allocation.

### Exact Distance Distribution (God's Algorithm in HTM)

Exhaustive breadth-first search of the complete state space reveals the exact distance distribution:

| Distance ($d$) | States at distance $d$ | Cumulative states | Percentage |
| :---: | ---: | ---: | ---: |
| 0 | 1 | 1 | < 0.001% |
| 1 | 9 | 10 | < 0.001% |
| 2 | 54 | 64 | 0.001% |
| 3 | 321 | 385 | 0.009% |
| 4 | 1,847 | 2,232 | 0.050% |
| 5 | 9,992 | 12,224 | 0.272% |
| 6 | 50,136 | 62,360 | 1.365% |
| 7 | 227,536 | 289,896 | 6.193% |
| 8 | 870,072 | 1,159,968 | 23.681% |
| 9 | 1,887,748 | 3,047,716 | 51.379% |
| 10 | 623,800 | 3,671,516 | 16.978% |
| 11 | 2,644 | 3,674,160 | 0.072% |
| Total | 3,674,160 | — | 100.0% |

The maximum distance to solved is 11, confirming God's Number for the 2×2×2 Rubik's Cube in the half-turn metric. Over 51% of all states require exactly 9 moves, and 92.0% require between 8 and 10 moves. The distribution is sharply peaked: a uniformly random scramble is almost never easy, and the 2,644 hardest states are 0.072% of the space. Only the diameter is checked by the program itself; the per-level counts above come from the same BFS instrumented to record levels.

## 5. C99 Interface

### Build and Verification

```sh
make          # builds solver and mini
make check    # executable tests, about five seconds
make prove    # Frama-C WP on solver.c, about eight seconds warm
```

`make check` runs three things: the exhaustive self-test, a set of solution vectors, and a set of rejection cases.

The vectors live in `tests/solutions.txt` as `state|solution` pairs and cover distances 0, 8, 8, 8, 9, 9, 10, and 11, the last being the sample state `21345671111111`. Both binaries must reproduce every one. The pairs were not taken on trust from the solver that produced them: each was checked against an independent model of `source[][]` and `twist[][]`, confirming that the printed moves solve the state and that the move count equals the true BFS distance, so the vectors pin optimality and not merely stability. That independent model also reproduced 3,674,160 reachable states at diameter 11 from the same tables.

The comparison is byte-exact, `cmp` against a file holding the expected line and its terminating newline, so trailing whitespace and stray blank lines fail the target as surely as a wrong move does; a shell string comparison would not catch either, because command substitution strips trailing newlines. Exit status is checked separately. Mutation testing confirms the gate bites: a reintroduced trailing space, an extra newline, a corrupted entry in `move_names`, a divergence in `mini` alone, and a nonzero exit are each caught.

The rejection cases drive one input per validation path, short, long, cubie digit below and above range, orientation digit below and above range, a non-digit, a duplicate cubie, and a parity violation, plus the no-argument and two-argument cases, and require status 2 from both binaries. Having `mini` run the same list is what keeps its silent, independent validation path honest, since it shares no code with `parse_state`.

`make prove` is kept separate because it needs Frama-C and Alt-Ergo installed, which the runtime tests do not, and it costs about eight seconds against `make check`'s five. Neither target depends on the other; CI runs `make check` on every push and `make prove` as a second job.

### Test Coverage

Measured with `llvm-cov` over exactly what `make check` executes:

| Metric | Self-test only | Full `make check` |
| :--- | ---: | ---: |
| Region coverage | 67.3% | 88.9% |
| Line coverage | 73.8% | 88.5% |
| Branch coverage | 59.6% | 84.6% |
| Function coverage | 90.0% | 100% |

The self-test drives the model directly and never calls `parse_state` at all, which is why the rejection cases matter disproportionately. What remains uncovered is deliberate, and it divides into three kinds. Only the `malloc` failure return in `build_table` and the `main` path reporting it need allocation failure to reach; interposing on `malloc` is more machinery than that risk justifies. The incompleteness return, the diameter check, and the `self_test` failure returns all need the code to be already broken, which is the same category as an assertion that never fires. The range-check rejection inside `valid` is unreachable from the CLI because `parse_state` validates digit ranges before calling it; that branch is not dead code but the thing that makes `valid`'s contract self-contained, and WP proves it rather than leaving it to a test. The `argv[0]` null guard needs an `exec` with an empty argument vector, which no hosted shell produces.

The failure side of `output_failed` is not in any of those categories and is now tested directly: `make check` runs both binaries and the self-test with stdout closed and requires status 1. That case was the one genuinely reachable gap, and a mutant that reverts `output_failed` to an unconditional success is caught by it.

### The Compact Variant

`mini.c` solves the same inputs as `solver.c` and prints the same line. The two were checked against each other on 1,210 argument strings, 400 of them valid scrambles and the rest random bytes, edge-case lengths, and malformed digits: identical exit codes and byte-identical stdout on every one. `make check` enforces that agreement on the 8 solution vectors and the full rejection set; the 1,210-string run is the wider, unenforced sample. It exists as a readability contrast, not as a faster or smaller program, and it differs in four ways worth stating:

- No `--self-test`. `mini` rejects the flag with status 2.
- No diagnostics. Where `solver` writes a usage line or an allocation failure to stderr, `mini` is silent and communicates only through its exit status.
- Slower by roughly eight times, 0.51 s against 0.065 s, because it re-ranks a full `state_t` on every one of the 33 million expansions instead of using the factored transition tables of section 4.
- Larger by roughly three times at peak, 52.56 MiB computed and 53.91 MiB measured against `solver`'s 17.553 MiB and 18.89 MiB, because its queue holds 14-byte `state_t` values rather than 4-byte ranks: $3{,}674{,}160 \times 14 = 49.06$ MiB of queue where `solver` needs 14.016 MiB.

"Compact" therefore refers to source size alone.

### CLI Scramble Input

The program takes one argument containing exactly 14 digits:

```text
PPPPPPP OOOOOOO
positions orientations
```

The space above is explanatory and is not present in the actual argument.

- Digits 1–7 (`P`) identify which cubie occupies physical positions 1–7 and must form a permutation of `1234567`.
- Digits 8–14 (`O`) describe the orientation at those same positions: `1` means solved, `2` means $+120^\circ$, and `3` means $-120^\circ$.
- The sum of the internal orientation values (`digit - 1`) must be divisible by 3.

The solved state is `12345671111111`; solving it prints an empty line, since zero moves are needed. A direct rank would be shorter but would expose the implementation's Lehmer and base-3 encoding and would be difficult to read off a physical cube; the digit form keeps the CLI compact and inspectable.

For example, a scrambled position:

```sh
./solver 21345671111111
```

Produces the optimal 11-move solution:

```text
B' R' D2 R' B R B' R D2 B R'
```

That sample swaps two adjacent corners. A single transposition is an odd permutation, so it is unreachable on a physical 3×3×3 corner set without also disturbing the edges; on the 2×2×2 quotient it is a perfectly ordinary position, and it happens to be one of the hardest, sitting at distance 11.

### Input Validation

Invalid input exits with status 2. `main` rejects anything that is not exactly one argument; `parse_state` rejects the rest:

- Out-of-bounds cubie digit (outside `1`–`7`) or orientation digit (outside `1`–`3`).
- An argument that is not exactly 14 characters: a shorter one hits the terminating NUL inside the digit-range check, and a longer one fails the final `input[14] == '\0'` test.
- Non-permutation input, caught by the duplicate scan inside `valid`.
- Invalid orientation parity, $\sum_{i=1}^7 o_i = \sum_{i=1}^7 (d_i - 1) \not\equiv 0 \pmod 3$.

| Status | Meaning |
| :---: | :--- |
| 0 | Solution printed, or self-test passed, and stdout was written successfully |
| 1 | Self-test failure, the BFS diameter was not 11, the state table could not be built, or the output could not be written |
| 2 | Usage or input validation error |

Both output paths end in `output_failed`, which calls `fflush(stdout)` and then tests `ferror(stdout)`. The check belongs at the flush rather than at the call that produced the text. stdout is fully buffered when it is not a terminal, so a `puts` or `printf` that merely queues bytes into the buffer returns success even when the eventual write cannot happen, and the real error surfaces at the implicit flush after `main` returns, where nothing is left to observe it. Testing only the return value of `puts` misses every such failure: before this check existed, both programs exited 0 having written nothing when stdout was closed. `mini.c` performs the same flush test before returning.

The usage message reads `argv[0]` through a guard, because C99 5.1.2.2.1 permits `argv[0]` to be a null pointer when `argc` is 0.

## 6. Formal Verification and Testing

### Executable Self-Test

Running `./solver --self-test` conducts an exhaustive verification of the entire domain:

1. Move inversion: for all 9 moves $m$, verifies that $m$ followed by $m^{-1}$ restores the exact solved state.
2. Bijection invariant: loops through all $3{,}674{,}160$ ranks, where `unrank_state(rank, &t)` generates a state, `valid(&t)` must hold, and `rank_state(&t)` must return the original rank.
3. Graph exploration: builds the complete BFS table, verifying that exactly $3{,}674{,}160$ unique states are visited and that the computed graph diameter equals 11.

```text
3674160 states; diameter 11
```

The three steps cover different things, and step 1 carries more weight than its two lines suggest. Because `inverse_move` pairs a turn of $t$ quarter turns with one of $4 - t$, the check applies exactly four quarter turns of each face and requires the solved state back. That holds only if every `source` row is a permutation whose order divides 4 and every `twist` row cancels over four applications, which is a real constraint on the cube model and not a restatement of the tables. Step 2 is what rules out an invalid state slipping into the encoding: it unranks, validates, and re-ranks all 3,674,160 indices. Step 3 establishes connectivity, that the 9 generators reach every encoded state from solved, so no valid state is left unsolvable, and it pins the diameter. None of the three steps checks that a stored move is one level closer to solved; that property comes from the FIFO order of the search, as argued in section 4, and is not verified mechanically anywhere.

### Deductive Verification with Frama-C

The `make prove` target executes deductive verification with Frama-C 33.0 (Arsenic) using the WP (weakest precondition) plugin with RTE (runtime error annotation) and the Alt-Ergo SMT solver:

```sh
frama-c -wp -wp-fct quarter_turn,rank_state,valid,parse_state \
  -wp-rte -rte-verbose 0 -wp-prover alt-ergo -wp-timeout 20 \
  -wp-cache none solver.c
```

Frama-C proves all 164 generated goals for `quarter_turn`, `rank_state`, `valid`, and `parse_state`: 102 directly with Qed and 56 with Alt-Ergo 2.6.3, plus three termination and three unreachable-path goals. The contracts establish quarter-turn semantics, ranking bounds, exact state validation, and the exact mapping from a successful 14-digit parse to a valid state, together with their runtime-error obligations. A fresh, uncached run reports 164/164 valid goals with no timeout or failed goal, in roughly 8 s wall clock on an idle development machine with the toolchain warm. The duration is machine dependent and strongly load sensitive: readings taken during a parallel build ran past 18 s for the same 164 goals. Elapsed time is not stable enough to gate on; the proved-equals-generated identity is, which is what `make prove` actually asserts.

Only functions with meaningful behavioral postconditions are selected. `apply_move` previously contributed runtime-safety and termination goals but had no postcondition, so counting it as deductive coverage overstated what was proved; move inversion remains checked by the executable self-test. `unrank_state`, `build_table`, and `main` sit outside the verification target, so the ACSL contract `unrank_state` already carries is neither proved nor consumed by any verified caller. That gap stays open because proving the full ranking bijection and BFS completeness requires substantially larger mathematical invariants, while the exhaustive executable check covers every rank and the complete graph.

What the proof does not say is as important as what it does. WP establishes that these four functions are arithmetic-overflow free, terminating, free of out-of-bounds access and invalid dereference, and faithful to their contracts for every input satisfying their preconditions. Memory safety holds modulo the two RTE categories Frama-C 33 cannot express, described below; neither bites here, since none of these functions makes an indirect call or depends on pointer alignment. It says nothing about the cube model itself. That `source` and `twist` describe real face turns rests on two checks that cover different failures, and neither is redundant. The 9 generators reach exactly 3,674,160 states at a half-turn-metric diameter of exactly 11, the published group order and God's number for this puzzle; and move inversion requires four quarter turns of each face to be the identity. The second is not implied by the first, which is easy to demonstrate: replacing the `R` row of `source` with the 3-cycle `{1, 2, 0, 3, 4, 5, 6}` and deleting the move-inversion loop still prints `3674160 states; diameter 11` and exits 0, because the mutated generators still reach the whole space, yet the resulting binary never terminates on a real query, emitting `R'` forever, since a 3-cycle applied three times is the identity and the solve loop never advances. With the loop present that mutation is rejected, as is a `twist` row of `{1, 1, 1, 0, 0, 0, 0}` whose entries do sum to 0 mod 3. A one-row typo therefore reaches past the state count and the diameter, and move inversion is what stops it. That the table yields shortest paths rests on the standard FIFO argument for unit-weight BFS plus the check that all 3,674,160 states were reached; `build_table` is outside both the WP target and any independent exhaustive check of its output.

Frama-C 33 reports its unsupported alignment and indirect-call RTE categories as warnings even though these functions make no indirect calls and all generated obligations are proved. `make prove` filters exactly those two known capability messages, leaving every other warning visible. It requires a nonzero proof summary whose proved and generated counts match, and rejects any Timeout, Unknown, or Failed count, because WP itself may exit successfully after an incomplete proof. This avoids treating a harmless change in goal count as a regression. `-rte-verbose 0` removes progress chatter only.

## 7. Implementation Notes and Possible Improvements

The current code is correct and fast enough; the following are the changes that would actually pay for themselves, roughly in order of value.

1. Drop the queue and sweep the table by level. The 14.016 MiB `queue` is 79.8% of peak memory and exists only to name the current frontier. Since the depth of a state never exceeds 11 and a move number never exceeds 8, both fit in a nibble: store `depth << 4 | move` in the existing byte, keep `0xFF` as the unvisited marker, and expand level $d$ by scanning the table for entries whose high nibble is $d$. Peak memory drops from 17.553 MiB to 3.537 MiB computed, or from 18.89 MiB to roughly 4.87 MiB measured, which brings the search peak within 34 KiB of the 3.504 MiB retained footprint; the remaining gap is the transition tables, which are still live during the search and only disappear if they are eliminated too. The sweep also replaces `tail`, so it has to keep a running count of discovered states for the completeness check `build_table` performs today. The cost is one expanding pass per level, $d = 0 \dots 11$, the last of which finds a non-empty frontier of 2,644 states and discovers nothing new, so 12 linear passes over 3.5 MiB, about 44 million sequential byte comparisons, against the 33 million random-access expansions the search already performs. The solve loop then reads the move as `table[rank] & 0x0F`, and the per-level histogram in section 4 becomes a by-product of the run instead of an external claim.

2. Harden the solve loop. `for (uint32_t rank = rank_state(&state); rank; rank = rank_state(&state))` trusts the table completely. A byte above 8 indexes `move_names` and, through `apply_move`, `source` out of bounds, and even a well-formed byte could in principle cycle forever. Checking `move < MOVES` before either lookup and bounding the loop at 11 iterations costs one comparison and one counter, and turns two invariants that currently hold by construction into runtime checks.

3. Report why the table could not be built. `build_table` returns `NULL` both when `malloc` fails and when the search fails to reach all 3,674,160 states, and `main` prints a single message for both. These are a resource failure and a logic failure; separating them costs an out-parameter and makes a future regression diagnosable.

4. Move the transition tables off the stack. `permutation` and `orientation` occupy 33.8 KiB of automatic storage. That is unremarkable on a hosted platform with an 8 MiB main-thread stack, but it exceeds the default stack of some embedded and thread configurations. Allocating them on the heap, or making them `static` if the one-time 33.8 KiB of resident data is acceptable, removes the constraint.

5. Extend the proof to `unrank_state` incrementally. The full bijection, `rank_state(unrank_state(r)) == r`, needs a factoradic invariant out of proportion to this program. A useful intermediate is `ensures valid_state(state)`. Its orientation half is easy, since the six base-3 digits are below 3 by construction and the seventh is built from the sum. Its permutation half is the real work: the loop needs an invariant saying that the first `i` entries are distinct, that `available[0..CUBIES-i-1]` still holds exactly the unused cubies, and that `f == (6 - i)!` with `p < f * (CUBIES - i)`, which is what bounds the factoradic digit `q = p / f` below `CUBIES - i` and keeps the index into `available` in range. That postcondition alone does not yet discharge the `rank_state` calls in `build_table`, because those run on `quarter_turn(state, face)`; closing the gap also needs a lemma that each row of `source` is a permutation, so a quarter turn preserves distinctness. Both steps are far smaller than the bijection, but they only pay off once `build_table` also enters the `-wp-fct` list, since WP generates call-site obligations only for the functions it analyzes. That much is attainable without proving BFS completeness: `build_table` would need loop invariants strong enough to bound its ranks and array indices, nothing about which states are reached.

Two changes that look attractive and are not worth making: tracking the rank incrementally in the solve loop with the same factored tables, which would remove a handful of `rank_state` calls from a loop that runs at most 11 times; and replacing the table with a meet-in-the-middle search, which would answer a single query in well under a millisecond against the 0.065 s the table costs. The speed argument for keeping the table is weak, since nothing here queries more than once per process, and the constant-time lookup buys a library entry point that does not exist. The real argument is that the table is the verification artifact. Building it exhaustively is what establishes that the 9 generators reach exactly 3,674,160 states at diameter 11, which is the only evidence that `source` and `twist` model a real cube at all, as section 6 discusses. A meet-in-the-middle search would answer faster and prove nothing, and the 0.065 s is the price of a program that checks its own model on every run.

## References

1. Philo Li, [“How to Solve a Rubik’s Cube Without Memorizing Algorithms”](https://philoli.com/zh/blog/solve-rubiks-cube-without-formulas/), 2026.
2. Gene Cooperman and Larry Finkelstein, “New Methods for Using Cayley Graphs in Interconnection Networks,” *Discrete Applied Mathematics* 37–38 (1992), 95–118.
3. Antti Valmari, “What the Small Rubik’s Cube Taught Me about Data Structures, Information Theory, and Randomisation,” *International Journal on Software Tools for Technology Transfer* 8(3) (2006), 180–194.
