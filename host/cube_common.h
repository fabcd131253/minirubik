/* gen_tables.c 使用的方塊模型與 host 端的表格：
 *   - 方塊模型（FROM/SPIN，與 solver_simple.c 相同）
 *   - 排列索引 p（0~5039）與方向索引 o（0~728）的編碼與解碼
 *   - 轉移表與 pattern database（寫進 tables.s、tables.h）
 *   - 完整距離表：只在 host 上當作驗證用的標準答案，不會寫進目標程式
 *
 * 搜尋時狀態一律保持為 (p, o) 兩個索引，不合成 p * 729 + o，
 * 所以目標上的搜尋迴圈中沒有乘法與除法。
 */
#ifndef CUBE_COMMON_H
#define CUBE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUBIES 7
#define PERMUTATIONS 5040 /* 7! */
#define ORIENTATIONS 729  /* 3^6 */
#define STATES (PERMUTATIONS * ORIENTATIONS)

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

#endif /* CUBE_COMMON_H */
