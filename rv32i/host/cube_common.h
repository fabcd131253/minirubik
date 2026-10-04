/* IDA* 與 BIDA* 參考實作的共用部分：
 *   - 方塊模型（FROM/SPIN，與 solver_simple.c 相同）
 *   - 排列索引 p（0~5039）與方向索引 o（0~728）的編碼與解碼
 *   - 轉移表、pattern database、完整距離表（只在 host 驗證時使用）
 *   - 不使用遞迴的 IDA* 搜尋引擎，heuristic 由各程式提供
 *   - 命令列介面與 host 端驗證（H1、H2、H3）
 *
 * 搜尋時狀態一律保持為 (p, o) 兩個索引，不合成 p * 729 + o，
 * 所以搜尋迴圈中沒有乘法與除法；乘除法只出現在建表與輸入解析。
 */
#ifndef CUBE_COMMON_H
#define CUBE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUBIES 7
#define PERMUTATIONS 5040 /* 7! */
#define ORIENTATIONS 729  /* 3^6 */
#define STATES (PERMUTATIONS * ORIENTATIONS)
#define NUM_MOVES 9
#define MAX_DEPTH 11 /* HTM 直徑 */
#define NO_FACE 3    /* 根節點沒有「上一步的面」 */

static const char *MOVE_NAME[NUM_MOVES] = {"R", "R2", "R'", "B", "B2",
                                           "B'", "D", "D2", "D'"};

/* 與 solver_simple.c 相同的方塊模型 */
static const uint8_t FROM[3][CUBIES] = {
    {1, 4, 2, 0, 3, 5, 6}, /* R */
    {0, 1, 2, 4, 5, 6, 3}, /* B */
    {0, 2, 5, 3, 1, 4, 6}, /* D */
};
static const uint8_t SPIN[3][CUBIES] = {
    {1, 2, 0, 2, 1, 0, 0}, /* R */
    {0, 0, 0, 1, 2, 1, 2}, /* B */
    {0, 0, 0, 0, 0, 0, 0}, /* D */
};

typedef struct {
    uint8_t pos[CUBIES];
    uint8_t ori[CUBIES];
} Cube;

static Cube quarter_turn(Cube c, int face)
{
    Cube out;
    for (int i = 0; i < CUBIES; i++) {
        int from = FROM[face][i];
        out.pos[i] = c.pos[from];
        out.ori[i] = (c.ori[from] + SPIN[face][i]) % 3;
    }
    return out;
}

/* ---- 排列與方向各自的編碼（與 solver_simple.c 的 encode/decode 相同，只是拆開） ---- */

static uint16_t perm_rank(const uint8_t *pos)
{
    uint32_t r = 0;
    for (int i = 0; i < CUBIES; i++) {
        int smaller = 0;
        for (int j = i + 1; j < CUBIES; j++)
            if (pos[j] < pos[i])
                smaller++;
        r = r * (CUBIES - i) + smaller;
    }
    return (uint16_t) r;
}

static uint16_t ori_rank(const uint8_t *ori)
{
    uint32_t r = 0;
    for (int i = 0; i < 6; i++)
        r = r * 3 + ori[i];
    return (uint16_t) r;
}

static void perm_unrank(uint32_t r, uint8_t *pos)
{
    uint8_t remaining[CUBIES] = {0, 1, 2, 3, 4, 5, 6};
    uint32_t base = 720;
    for (int i = 0; i < CUBIES; i++) {
        int choice = r / base;
        r %= base;
        pos[i] = remaining[choice];
        for (int j = choice; j + 1 < CUBIES - i; j++)
            remaining[j] = remaining[j + 1];
        if (i < 5)
            base /= (6 - i);
    }
}

static void ori_unrank(uint32_t r, uint8_t *ori)
{
    int sum = 0;
    for (int i = 5; i >= 0; i--) {
        ori[i] = r % 3;
        sum += ori[i];
        r /= 3;
    }
    ori[6] = (3 - sum % 3) % 3;
}

/* ---- 轉移表：quarter turn 對 p、o 的作用，共 3*5040*2 + 3*729*2 = 34,614 bytes ---- */

static uint16_t perm_qt[3][PERMUTATIONS];
static uint16_t ori_qt[3][ORIENTATIONS];

static void build_transition_tables(void)
{
    Cube c;
    for (uint32_t r = 0; r < PERMUTATIONS; r++) {
        perm_unrank(r, c.pos);
        memset(c.ori, 0, sizeof c.ori);
        for (int face = 0; face < 3; face++) {
            Cube n = quarter_turn(c, face);
            perm_qt[face][r] = perm_rank(n.pos);
        }
    }
    for (uint32_t r = 0; r < ORIENTATIONS; r++) {
        for (int i = 0; i < CUBIES; i++)
            c.pos[i] = (uint8_t) i;
        ori_unrank(r, c.ori);
        for (int face = 0; face < 3; face++) {
            Cube n = quarter_turn(c, face);
            ori_qt[face][r] = ori_rank(n.ori);
        }
    }
}

/* 用轉移表套用一個 move（0~8）：同一面的 quarter turn 做 1~3 次 */
static void apply_move_idx(uint16_t *p, uint16_t *o, int move)
{
    int face = move / 3;
    for (int t = 0; t <= move % 3; t++) {
        *p = perm_qt[face][*p];
        *o = ori_qt[face][*o];
    }
}

/* ---- pattern database：只看排列、只看方向的精確距離 ---- */

static uint8_t h_perm[PERMUTATIONS]; /* 5,040 bytes */
static uint8_t h_ori[ORIENTATIONS];  /*   729 bytes */

/* 在 n 個抽象狀態上，從 0 做 BFS；qt 是 [3][n] 的轉移表。
 * 回傳走訪到的狀態數，*max_h 回傳最大距離。 */
static uint32_t build_pdb(uint8_t *h, const uint16_t *qt, uint32_t n,
                          int *max_h)
{
    static uint16_t queue[PERMUTATIONS];
    uint32_t head = 0, tail = 0;
    memset(h, 0xFF, n);
    h[0] = 0;
    queue[tail++] = 0;
    *max_h = 0;
    while (head < tail) {
        uint16_t cur = queue[head++];
        for (int face = 0; face < 3; face++) {
            uint16_t x = cur;
            for (int t = 0; t < 3; t++) {
                x = qt[face * n + x];
                if (h[x] == 0xFF) {
                    h[x] = (uint8_t) (h[cur] + 1);
                    if (h[x] > *max_h)
                        *max_h = h[x];
                    queue[tail++] = x;
                }
            }
        }
    }
    return tail;
}

/* H2：表格必須填滿，還原狀態那一格為 0 */
static int build_pdbs(int verbose)
{
    int max_p, max_o;
    uint32_t np = build_pdb(h_perm, &perm_qt[0][0], PERMUTATIONS, &max_p);
    uint32_t no = build_pdb(h_ori, &ori_qt[0][0], ORIENTATIONS, &max_o);
    if (verbose) {
        printf("H2 h_perm: %u/%u filled, solved=%d, max=%d\n", np,
               PERMUTATIONS, h_perm[0], max_p);
        printf("H2 h_ori : %u/%u filled, solved=%d, max=%d\n", no,
               ORIENTATIONS, h_ori[0], max_o);
    }
    return np == PERMUTATIONS && no == ORIENTATIONS && h_perm[0] == 0 &&
           h_ori[0] == 0;
}

/* ---- 完整距離表：只在 host 端當作驗證用的標準答案（3.5 MB），不屬於搜尋本身 ---- */

static uint8_t *build_distance_oracle(void)
{
    uint8_t *dist = malloc(STATES);
    uint32_t *queue = malloc((size_t) STATES * sizeof(uint32_t));
    if (!dist || !queue) {
        free(dist);
        free(queue);
        return NULL;
    }
    memset(dist, 0xFF, STATES);
    uint32_t head = 0, tail = 0;
    dist[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        uint32_t here = queue[head++];
        uint16_t p = here / ORIENTATIONS, o = here % ORIENTATIONS;
        for (int face = 0; face < 3; face++) {
            uint16_t np = p, no = o;
            for (int t = 0; t < 3; t++) {
                np = perm_qt[face][np];
                no = ori_qt[face][no];
                uint32_t there = (uint32_t) np * ORIENTATIONS + no;
                if (dist[there] == 0xFF) {
                    dist[there] = (uint8_t) (dist[here] + 1);
                    queue[tail++] = there;
                }
            }
        }
    }
    free(queue);
    if (tail != STATES) {
        free(dist);
        return NULL;
    }
    return dist;
}

/* ---- 不使用遞迴的 IDA* 搜尋引擎 ----
 * heuristic(p, o, &exact) 回傳不高估的估計值；exact 為 1 表示估計值就是真實距離。
 * finish(p, o, out) 在 exact 狀態上補完剩下的路徑，回傳補上的步數。
 * 對 IDA* 來說只有還原狀態是 exact，finish 補 0 步；
 * 對 BIDA* 來說周界內的狀態都是 exact，finish 沿著周界走回還原狀態。
 */
typedef int (*heuristic_fn)(uint16_t p, uint16_t o, int *exact);
typedef int (*finish_fn)(uint16_t p, uint16_t o, uint8_t *out);

typedef struct {
    uint16_t p, o;   /* 這一層的狀態 */
    uint16_t tp, to; /* 目前這一面已轉 turn 次之後的狀態 */
    int8_t last_face; /* 走到這一層的那一步是哪一面 */
    int8_t face;      /* 正在嘗試的面 */
    int8_t turn;      /* 這一面已經轉了幾次（3 表示要換下一面） */
} Frame;

typedef struct {
    uint64_t generated;     /* 產生並估計過的子節點數 */
    uint32_t iterations;    /* IDA* 的輪數 */
    uint64_t pruned;        /* g + h > bound 被剪掉的子節點數 */
    uint64_t descents;      /* 往下一層（push 一個 frame）的次數 */
    uint64_t pops;          /* 一層試完、回到上一層的次數 */
    uint64_t face_switches; /* 換到下一個面的次數 */
} SearchStats;

/* 周界查表的計數器（IDA* 用不到）：查詢次數、迴圈步數（二分搜尋的輪數或桶內比對次數） */
static uint64_t g_lookup_calls, g_lookup_steps;

static int ida_engine(uint16_t p0, uint16_t o0, uint8_t *moves,
                      heuristic_fn heuristic, finish_fn finish,
                      SearchStats *stats)
{
    Frame st[MAX_DEPTH + 1];
    int exact;
    memset(stats, 0, sizeof *stats);
    g_lookup_calls = g_lookup_steps = 0;
    int bound = heuristic(p0, o0, &exact);
    if (exact) /* 根節點的距離已知：直接補完 */
        return finish(p0, o0, moves);

    for (;;) {
        int next_bound = 255;
        int d = 0;
        stats->iterations++;
        st[0] = (Frame){p0, o0, p0, o0, NO_FACE, -1, 3};
        while (d >= 0) {
            Frame *f = &st[d];
            if (f->turn == 3) { /* 換下一面，跳過與上一步同一面的 */
                do
                    f->face++;
                while (f->face == f->last_face);
                if (f->face >= 3) { /* 這一層試完了：回到上一層 */
                    stats->pops++;
                    d--;
                    continue;
                }
                stats->face_switches++;
                f->tp = f->p;
                f->to = f->o;
                f->turn = 0;
            }
            /* 接著上一次的結果再轉一次：依序得到 X、X2、X' */
            f->tp = perm_qt[f->face][f->tp];
            f->to = ori_qt[f->face][f->to];
            f->turn++;
            moves[d] = (uint8_t) (f->face * 3 + f->turn - 1);
            stats->generated++;

            int g = d + 1;
            int h = heuristic(f->tp, f->to, &exact);
            if (g + h > bound) { /* 剪枝，記下最小的超出值 */
                stats->pruned++;
                if (g + h < next_bound)
                    next_bound = g + h;
                continue;
            }
            if (exact) /* 剩下的距離已知且不超過 bound：找到最短解 */
                return g + finish(f->tp, f->to, moves + g);
            st[d + 1] = (Frame){f->tp, f->to, f->tp, f->to, f->face, -1, 3};
            stats->descents++;
            d++;
        }
        if (next_bound == 255)
            return -1; /* 不應發生：圖是連通的 */
        bound = next_bound;
    }
}

/* ---- 輸入、輸出與 host 端驗證 ---- */

/* 解析 14 字元輸入並檢查合法性（重複角塊、方向總和），與 solver.c 的 parse_state 相同 */
static int parse_state(const char *s, uint16_t *p, uint16_t *o)
{
    Cube c;
    unsigned seen = 0, sum = 0;
    if (strlen(s) != 14)
        return 0;
    for (int i = 0; i < CUBIES; i++) {
        unsigned a = (unsigned) (s[i] - '1');
        unsigned b = (unsigned) (s[i + CUBIES] - '1');
        if (a >= CUBIES || b >= 3 || (seen >> a & 1))
            return 0;
        seen |= 1U << a;
        sum += b;
        c.pos[i] = (uint8_t) a;
        c.ori[i] = (uint8_t) b;
    }
    if (sum % 3)
        return 0;
    *p = perm_rank(c.pos);
    *o = ori_rank(c.ori);
    return 1;
}

static void state_string(uint16_t p, uint16_t o, char *buf)
{
    Cube c;
    perm_unrank(p, c.pos);
    ori_unrank(o, c.ori);
    for (int i = 0; i < CUBIES; i++) {
        buf[i] = (char) ('1' + c.pos[i]);
        buf[i + CUBIES] = (char) ('1' + c.ori[i]);
    }
    buf[14] = '\0';
}

static void print_moves(const uint8_t *moves, int n)
{
    for (int i = 0; i < n; i++)
        printf("%s%s", i ? " " : "", MOVE_NAME[moves[i]]);
    putchar('\n');
}

typedef struct {
    const char *name;
    int (*init)(int verbose); /* 建立該程式需要的額外表格（H2） */
    heuristic_fn heuristic;
    finish_fn finish;
    /* 成本模型（--cost）用的參數，單位是 RV32I 指令數的估計值 */
    int filter;      /* 每個子節點多一次「h > K 就不查周界」的比較：1 有、0 無 */
    int lookup_base; /* 每次周界查詢的固定成本 */
    int lookup_step; /* 周界查詢每一步迴圈的成本 */
    uint32_t static_bytes; /* 靜態資料大小（init 之後才確定的程式請在 init 中更新） */
} Solver;

/* H1、H3：only_d11 為 1 時，H3 只檢查 2,644 個距離 11 的狀態 */
static int run_check(const Solver *s, int only_d11)
{
    uint8_t *dist = build_distance_oracle();
    if (!dist) {
        fputs("could not build distance oracle\n", stderr);
        return 1;
    }

    /* H1：所有狀態 h(s) <= d(s) */
    uint32_t over = 0, exact_wrong = 0;
    for (uint32_t r = 0; r < STATES; r++) {
        int exact;
        int h = s->heuristic(r / ORIENTATIONS, r % ORIENTATIONS, &exact);
        if (h > dist[r])
            over++;
        if (exact && h != dist[r])
            exact_wrong++;
    }
    printf("H1 h > d on %u states, exact-but-wrong on %u states\n", over,
           exact_wrong);

    /* H3：搜尋結果長度等於真實距離，且套用後回到還原狀態 */
    uint64_t sum_nodes[MAX_DEPTH + 1] = {0}, max_nodes[MAX_DEPTH + 1] = {0};
    uint32_t count[MAX_DEPTH + 1] = {0}, wrong = 0, worst_rank = 0;
    clock_t start = clock();
    for (uint32_t r = 0; r < STATES; r++) {
        int d = dist[r];
        if (only_d11 && d != MAX_DEPTH)
            continue;
        uint16_t p = r / ORIENTATIONS, o = r % ORIENTATIONS;
        uint8_t moves[MAX_DEPTH + 1];
        SearchStats st;
        int len = ida_engine(p, o, moves, s->heuristic, s->finish, &st);
        uint16_t tp = p, to = o;
        for (int i = 0; i < len; i++)
            apply_move_idx(&tp, &to, moves[i]);
        if (len != d || tp != 0 || to != 0)
            wrong++;
        count[d]++;
        sum_nodes[d] += st.generated;
        if (st.generated > max_nodes[d]) {
            max_nodes[d] = st.generated;
            if (d == MAX_DEPTH)
                worst_rank = r;
        }
    }
    double secs = (double) (clock() - start) / CLOCKS_PER_SEC;
    printf("H3 %s states checked, wrong length or not solved: %u (%.1f s)\n",
           only_d11 ? "distance-11" : "all", wrong, secs);
    printf(" d    states   avg nodes   max nodes\n");
    for (int d = 0; d <= MAX_DEPTH; d++)
        if (count[d])
            printf("%2d %9u %11.1f %11llu\n", d, count[d],
                   (double) sum_nodes[d] / count[d],
                   (unsigned long long) max_nodes[d]);
    char buf[15];
    state_string(worst_rank / ORIENTATIONS, worst_rank % ORIENTATIONS, buf);
    printf("worst distance-11 state: %s (%llu nodes)\n", buf,
           (unsigned long long) max_nodes[MAX_DEPTH]);
    free(dist);
    return over || exact_wrong || wrong;
}

/* ---- 成本模型：把搜尋事件的次數換算成 RV32I 指令數的「估計值」 ----
 * 每個常數是假設熱路徑的值都放在暫存器、表格基底位址已經在暫存器中時，
 * 手寫組語大概需要的指令數。這只是用來比較不同方案的模型，不是量測；
 * 真正的數字必須用 Ripes 的 --iret 量測。
 */
enum {
    COST_GEN = 20,     /* 產生一個子節點：兩次轉移表（slli/add/lhu ×2 = 6）、
                          兩次 PDB（add/lbu ×2 = 4）、max（3）、g + h 與 bound 比較（2）、
                          記錄 move（2）、turn 計數與迴圈（3） */
    COST_PRUNE = 3,    /* 被剪掉時更新 next_bound：比較、分支、搬移 */
    COST_DESCEND = 10, /* 往下一層：存 p、o、last_face，重設 face、turn，g++ */
    COST_POP = 8,      /* 回到上一層：g--，重新載入該層的 p、o、tp、to、face、turn */
    COST_FACE = 8,     /* 換下一面：face++、跳過 last_face、比較 3、tp = p、to = o、
                          載入該面兩張轉移表的基底位址 */
    COST_ITER = 10     /* 每一輪 IDA* 的初始化 */
};

static uint64_t estimate_cost(const Solver *s, const SearchStats *st)
{
    return st->generated * (uint64_t) (COST_GEN + s->filter) +
           st->pruned * COST_PRUNE + st->descents * COST_DESCEND +
           st->pops * COST_POP + st->face_switches * COST_FACE +
           (uint64_t) st->iterations * COST_ITER +
           g_lookup_calls * (uint64_t) s->lookup_base +
           g_lookup_steps * (uint64_t) s->lookup_step;
}

static void print_cost_row(const char *label, const Solver *s,
                           const SearchStats *st)
{
    printf("%-16s %9llu %9llu %8llu %8llu %8llu %8llu %9llu %11llu\n", label,
           (unsigned long long) st->generated,
           (unsigned long long) st->pruned,
           (unsigned long long) st->descents, (unsigned long long) st->pops,
           (unsigned long long) st->face_switches,
           (unsigned long long) g_lookup_calls,
           (unsigned long long) g_lookup_steps,
           (unsigned long long) estimate_cost(s, st));
}

/* --cost：對全部 2,644 個距離 11 的狀態估計指令數，找出最壞情況 */
static int run_cost(const Solver *s)
{
    uint8_t *dist = build_distance_oracle();
    if (!dist) {
        fputs("could not build distance oracle\n", stderr);
        return 1;
    }
    printf("cost model: gen %d (+%d filter), prune %d, descend %d, pop %d, "
           "face %d, iter %d, lookup %d + %d/step\n",
           COST_GEN, s->filter, COST_PRUNE, COST_DESCEND, COST_POP, COST_FACE,
           COST_ITER, s->lookup_base, s->lookup_step);
    printf("static data: %u bytes (budget 131072)\n", s->static_bytes);

    uint64_t worst = 0, total = 0;
    uint32_t worst_rank = 0, count = 0, wrong = 0;
    for (uint32_t r = 0; r < STATES; r++) {
        if (dist[r] != MAX_DEPTH)
            continue;
        uint8_t moves[MAX_DEPTH + 1];
        SearchStats st;
        int len = ida_engine(r / ORIENTATIONS, r % ORIENTATIONS, moves,
                             s->heuristic, s->finish, &st);
        if (len != MAX_DEPTH)
            wrong++;
        uint64_t c = estimate_cost(s, &st);
        total += c;
        count++;
        if (c > worst) {
            worst = c;
            worst_rank = r;
        }
    }
    free(dist);

    printf("\n%-16s %9s %9s %8s %8s %8s %8s %9s %11s\n", "state", "generated",
           "pruned", "descend", "pop", "face", "lookups", "steps",
           "est. instr");
    char buf[15];
    uint8_t moves[MAX_DEPTH + 1];
    SearchStats st;
    state_string(worst_rank / ORIENTATIONS, worst_rank % ORIENTATIONS, buf);
    ida_engine(worst_rank / ORIENTATIONS, worst_rank % ORIENTATIONS, moves,
               s->heuristic, s->finish, &st);
    print_cost_row(buf, s, &st);
    uint16_t p, o;
    parse_state("21345671111111", &p, &o);
    ida_engine(p, o, moves, s->heuristic, s->finish, &st);
    print_cost_row("21345671111111", s, &st);

    printf("\ndistance-11 states: %u (wrong length: %u)\n", count, wrong);
    printf("est. instructions: worst %llu (%.2f%% of 5e7), average %.0f\n",
           (unsigned long long) worst, worst / 5e7 * 100, (double) total / count);
    return wrong != 0;
}

static int output_failed(void)
{
    return fflush(stdout) != 0 || ferror(stdout);
}

/* 用法：
 *   prog STATE           印出最短解（與 solver.c 相同格式）
 *   prog --stats STATE   另外印出節點數與輪數
 *   prog --check         H1、H2，以及對全部狀態做 H3（數分鐘）
 *   prog --check11       H1、H2，H3 只檢查距離 11 的狀態
 *   prog --cost          對全部距離 11 的狀態估計 RV32I 指令數（成本模型，不是量測）
 * 狀態碼：0 成功，1 失敗，2 輸入錯誤（與 solver.c 相同）
 */
static int cube_main(int argc, char **argv, const Solver *s)
{
    build_transition_tables();
    int checking = argc == 2 && (!strcmp(argv[1], "--check") ||
                                 !strcmp(argv[1], "--check11"));
    int costing = argc == 2 && !strcmp(argv[1], "--cost");
    if (!build_pdbs(checking) || !s->init(checking || costing)) {
        fputs("H2 failed: table not fully populated\n", stderr);
        return 1;
    }
    if (costing)
        return run_cost(s) || output_failed();
    if (checking) {
        int failed = run_check(s, !strcmp(argv[1], "--check11"));
        return failed || output_failed();
    }

    int stats_mode = argc == 3 && !strcmp(argv[1], "--stats");
    const char *input = stats_mode ? argv[2] : argv[1];
    uint16_t p, o;
    if ((argc != 2 && !stats_mode) || !parse_state(input, &p, &o)) {
        fprintf(stderr, "usage: %s [--stats] PPPPPPPOOOOOOO | --check | --check11\n",
                argc > 0 && argv[0] ? argv[0] : s->name);
        return 2;
    }
    uint8_t moves[MAX_DEPTH + 1];
    SearchStats st;
    int len = ida_engine(p, o, moves, s->heuristic, s->finish, &st);
    if (len < 0) {
        fputs("search failed\n", stderr);
        return 1;
    }
    print_moves(moves, len);
    if (stats_mode)
        printf("length=%d nodes=%llu iterations=%u\n", len,
               (unsigned long long) st.generated, st.iterations);
    return output_failed();
}

#endif /* CUBE_COMMON_H */
