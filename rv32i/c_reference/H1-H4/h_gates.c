/* h_gates.c：對 c_reference/solver_ref.c 執行作業的 host 端關卡 H1–H4
 *
 * 直接 #include "solver_ref.c"，呼叫它自己的 heuristic()、lookup()、search()、verify()，
 * 檢查的就是 solver_ref.c 本身，而不是另一份照同樣邏輯寫的程式。
 *
 * 標準答案獨立於 tables.h：從方塊模型（FROM、SPIN）重新建立轉移表、PDB 與完整的 BFS 距離表。
 * 若 tables.h 有錯，標準答案不會跟著錯。
 *
 *   H1：heuristic() 對全部 3,674,160 個狀態都不高估；回報為精確值時等於真實距離
 *   H2：tables.h 的每張表都完整：轉移表與標準答案相同；PDB 填滿、最大值與還原狀態正確；
 *       perimeter 的狀態數、桶的起點遞增、桶內 o 遞增、每個項目的距離正確
 *   H3：search() 對全部狀態回傳的長度等於真實距離，而且 verify() 確認回到還原狀態
 *   H4：perimeter 項目（o | 距離 << 10）的存取：對每個項目（偶數、奇數索引分開統計）
 *       以及每個狀態，lookup() 的結果都和未壓縮的距離表相同
 *
 * 編譯（-I 指向 solver_ref.c 與 tables.h 所在的資料夾）：
 *   cc -O2 -std=c99 -I<c_reference> h_gates.c -o h_gates               使用 tables.h
 *   cc -O2 -std=c99 -I<c_reference> -DTABLES_IDA h_gates.c -o h_gates  使用 tables_ida.h
 * 狀態碼：0 全部通過，1 有關卡失敗
 */
#define main solver_ref_main /* 把 solver_ref.c 的 main 改名，避免衝突 */
#include "solver_ref.c"
#undef main

#include <string.h>
#include <time.h>

#define STATES (PERM_COUNT * ORI_COUNT)

/* ---- 標準答案：從方塊模型重新建立（不使用 tables.h） ---- */

static const uint8_t MODEL_FROM[3][7] = {
    {1, 4, 2, 0, 3, 5, 6}, /* R */
    {0, 1, 2, 4, 5, 6, 3}, /* B */
    {0, 2, 5, 3, 1, 4, 6}, /* D */
};
static const uint8_t MODEL_SPIN[3][7] = {
    {1, 2, 0, 2, 1, 0, 0},
    {0, 0, 0, 1, 2, 1, 2},
    {0, 0, 0, 0, 0, 0, 0},
};

static uint16_t oracle_perm_qt[3][PERM_COUNT];
static uint16_t oracle_ori_qt[3][ORI_COUNT];
static uint8_t oracle_h_perm[PERM_COUNT];
static uint8_t oracle_h_ori[ORI_COUNT];
static uint8_t *oracle_dist; /* 完整的 BFS 距離表，3,674,160 bytes */

static uint32_t model_perm_rank(const uint8_t *pos)
{
    uint32_t r = 0;
    for (int i = 0; i < 7; i++) {
        uint32_t smaller = 0;
        for (int j = i + 1; j < 7; j++)
            smaller += pos[j] < pos[i];
        r = r * (uint32_t) (7 - i) + smaller;
    }
    return r;
}

static void model_perm_unrank(uint32_t r, uint8_t *pos)
{
    uint8_t remaining[7] = {0, 1, 2, 3, 4, 5, 6};
    uint32_t base = 720;
    for (int i = 0; i < 7; i++) {
        uint32_t choice = r / base;
        r %= base;
        pos[i] = remaining[choice];
        for (uint32_t j = choice; j + 1 < (uint32_t) (7 - i); j++)
            remaining[j] = remaining[j + 1];
        if (i < 5)
            base /= (uint32_t) (6 - i);
    }
}

static uint32_t model_ori_rank(const uint8_t *ori)
{
    uint32_t r = 0;
    for (int i = 0; i < 6; i++)
        r = r * 3 + ori[i];
    return r;
}

static void model_ori_unrank(uint32_t r, uint8_t *ori)
{
    uint32_t sum = 0;
    for (int i = 5; i >= 0; i--) {
        ori[i] = (uint8_t) (r % 3);
        sum += ori[i];
        r /= 3;
    }
    ori[6] = (uint8_t) ((3 - sum % 3) % 3);
}

static void build_oracle_transitions(void)
{
    uint8_t a[7], b[7];
    for (uint32_t r = 0; r < PERM_COUNT; r++) {
        model_perm_unrank(r, a);
        for (int f = 0; f < 3; f++) {
            for (int i = 0; i < 7; i++)
                b[i] = a[MODEL_FROM[f][i]];
            oracle_perm_qt[f][r] = (uint16_t) model_perm_rank(b);
        }
    }
    for (uint32_t r = 0; r < ORI_COUNT; r++) {
        model_ori_unrank(r, a);
        for (int f = 0; f < 3; f++) {
            for (int i = 0; i < 7; i++)
                b[i] = (uint8_t) ((a[MODEL_FROM[f][i]] + MODEL_SPIN[f][i]) % 3);
            oracle_ori_qt[f][r] = (uint16_t) model_ori_rank(b);
        }
    }
}

/* 在 n 個抽象狀態上做 BFS（每一面轉 1~3 次）；回傳走訪到的狀態數 */
static uint32_t oracle_pdb(uint8_t *h, const uint16_t *qt, uint32_t n)
{
    static uint16_t queue[PERM_COUNT];
    uint32_t head = 0, tail = 0;
    memset(h, 0xFF, n);
    h[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        uint16_t cur = queue[head++];
        for (int f = 0; f < 3; f++) {
            uint16_t x = cur;
            for (int t = 0; t < 3; t++) {
                x = qt[f * n + x];
                if (h[x] == 0xFF) {
                    h[x] = (uint8_t) (h[cur] + 1);
                    queue[tail++] = x;
                }
            }
        }
    }
    return tail;
}

static uint32_t build_oracle_distance(void)
{
    uint32_t *queue = malloc((size_t) STATES * sizeof *queue);
    oracle_dist = malloc(STATES);
    if (!queue || !oracle_dist)
        return 0;
    memset(oracle_dist, 0xFF, STATES);
    uint32_t head = 0, tail = 0;
    oracle_dist[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        uint32_t here = queue[head++];
        uint32_t p = here / ORI_COUNT, o = here % ORI_COUNT;
        for (int f = 0; f < 3; f++) {
            uint32_t np = p, no = o;
            for (int t = 0; t < 3; t++) {
                np = oracle_perm_qt[f][np];
                no = oracle_ori_qt[f][no];
                uint32_t there = np * ORI_COUNT + no;
                if (oracle_dist[there] == 0xFF) {
                    oracle_dist[there] = (uint8_t) (oracle_dist[here] + 1);
                    queue[tail++] = there;
                }
            }
        }
    }
    free(queue);
    return tail;
}

/* ---- 關卡 ---- */

static int gate_h1(void)
{
    uint32_t over = 0, exact_wrong = 0, exact_count = 0;
    for (uint32_t r = 0; r < STATES; r++) {
        uint32_t exact;
        uint32_t h = heuristic(r / ORI_COUNT, r % ORI_COUNT, &exact); /* solver_ref.c */
        if (h > oracle_dist[r])
            over++;
        if (exact) {
            exact_count++;
            if (h != oracle_dist[r])
                exact_wrong++;
        }
    }
    printf("H1 heuristic(): h > d on %u states, exact-but-wrong on %u "
           "(exact on %u states)\n",
           over, exact_wrong, exact_count);
    return over == 0 && exact_wrong == 0;
}

static int gate_h2(void)
{
    int ok = 1;
    const uint16_t *perm[3] = {perm_qt_R, perm_qt_B, perm_qt_D};
    const uint16_t *ori[3] = {ori_qt_R, ori_qt_B, ori_qt_D};
    uint32_t perm_diff = 0, ori_diff = 0;
    for (int f = 0; f < 3; f++) {
        for (uint32_t i = 0; i < PERM_COUNT; i++)
            perm_diff += perm[f][i] != oracle_perm_qt[f][i];
        for (uint32_t i = 0; i < ORI_COUNT; i++)
            ori_diff += ori[f][i] != oracle_ori_qt[f][i];
    }
    printf("H2 transitions: perm_qt differs on %u entries, ori_qt on %u\n",
           perm_diff, ori_diff);
    ok &= perm_diff == 0 && ori_diff == 0;

    uint32_t np = oracle_pdb(oracle_h_perm, &oracle_perm_qt[0][0], PERM_COUNT);
    uint32_t no = oracle_pdb(oracle_h_ori, &oracle_ori_qt[0][0], ORI_COUNT);
    uint32_t pd = 0, od = 0, pmax = 0, omax = 0, pfill = 0, ofill = 0;
    for (uint32_t i = 0; i < PERM_COUNT; i++) {
        pd += h_perm[i] != oracle_h_perm[i];
        pfill += h_perm[i] != 0xFF;
        if (h_perm[i] > pmax)
            pmax = h_perm[i];
    }
    for (uint32_t i = 0; i < ORI_COUNT; i++) {
        od += h_ori[i] != oracle_h_ori[i];
        ofill += h_ori[i] != 0xFF;
        if (h_ori[i] > omax)
            omax = h_ori[i];
    }
    printf("H2 h_perm: %u/%u filled, solved=%u, max=%u, differs from oracle on %u\n",
           pfill, PERM_COUNT, h_perm[0], pmax, pd);
    printf("H2 h_ori : %u/%u filled, solved=%u, max=%u, differs from oracle on %u\n",
           ofill, ORI_COUNT, h_ori[0], omax, od);
    ok &= np == PERM_COUNT && no == ORI_COUNT && pd == 0 && od == 0 &&
          pfill == PERM_COUNT && ofill == ORI_COUNT && h_perm[0] == 0 &&
          h_ori[0] == 0;

    /* perimeter：K = 0（tables_ida.h）時只輸出 bucket_start[0..1]，只有 p = 0 會被查 */
    uint32_t nbuckets = PERIMETER_K ? PERM_COUNT : 1;
    uint32_t expected = 0, monotonic = 1, sorted = 1, wrong_d = 0, dmax = 0;
    for (uint32_t r = 0; r < STATES; r++)
        expected += oracle_dist[r] <= PERIMETER_K;
    for (uint32_t p = 0; p < nbuckets; p++) {
        if (bucket_start[p] > bucket_start[p + 1])
            monotonic = 0;
        for (uint32_t i = bucket_start[p]; i < bucket_start[p + 1]; i++) {
            uint32_t o = bucket_entry[i] & ENTRY_O_MASK;
            uint32_t d = bucket_entry[i] >> ENTRY_D_SHIFT;
            if (i + 1 < bucket_start[p + 1] &&
                o >= (bucket_entry[i + 1] & ENTRY_O_MASK))
                sorted = 0;
            if (o >= ORI_COUNT || d != oracle_dist[p * ORI_COUNT + o])
                wrong_d++;
            if (d > dmax)
                dmax = d;
        }
    }
    uint32_t count = bucket_start[nbuckets];
    printf("H2 perimeter (K=%d): %u entries (expected %u), solved entry=%u, "
           "max distance=%u, starts monotonic=%s, o sorted=%s, wrong entries=%u\n",
           PERIMETER_K, count, expected, bucket_entry[0], dmax,
           monotonic ? "yes" : "no", sorted ? "yes" : "no", wrong_d);
    ok &= count == expected && count == PERIMETER_COUNT && bucket_entry[0] == 0 &&
          dmax == PERIMETER_K && monotonic && sorted && wrong_d == 0;
    return ok;
}

static int gate_h3(void)
{
    uint32_t wrong = 0, not_solved = 0, count[16] = {0}, diameter = 0;
    clock_t start = clock();
    for (uint32_t r = 0; r < STATES; r++) {
        root_p = (uint16_t) (r / ORI_COUNT); /* solver_ref.c 的全域變數 */
        root_o = (uint16_t) (r % ORI_COUNT);
        int32_t len = search();
        if (len != (int32_t) oracle_dist[r])
            wrong++;
        if (len < 0 || verify((uint32_t) len))
            not_solved++;
        if (len >= 0 && len < 16) {
            count[len]++;
            if ((uint32_t) len > diameter)
                diameter = (uint32_t) len;
        }
    }
    double secs = (double) (clock() - start) / CLOCKS_PER_SEC;
    printf("H3 search(): wrong length on %u states, verify() failed on %u "
           "(%.1f s)\n",
           wrong, not_solved, secs);
    printf("H3 length distribution:");
    for (uint32_t d = 0; d <= diameter; d++)
        printf(" %u:%u", d, count[d]);
    printf("\nH3 longest solution: %u moves (%u states)\n", diameter,
           count[diameter]);
    return wrong == 0 && not_solved == 0 && diameter == 11;
}

static int gate_h4(void)
{
    uint32_t nbuckets = PERIMETER_K ? PERM_COUNT : 1;
    uint32_t checked[2] = {0, 0}, bad[2] = {0, 0};
    for (uint32_t p = 0; p < nbuckets; p++)
        for (uint32_t i = bucket_start[p]; i < bucket_start[p + 1]; i++) {
            uint32_t o = bucket_entry[i] & ENTRY_O_MASK;
            int32_t got = lookup(p, o); /* solver_ref.c 的存取函式 */
            checked[i & 1]++;
            if (got != (int32_t) oracle_dist[p * ORI_COUNT + o])
                bad[i & 1]++;
        }
    /* 每個狀態：在 perimeter 內要回傳距離，不在要回傳 -1（K = 0 時只有 p = 0 會被查） */
    uint32_t states_checked = 0, states_bad = 0;
    for (uint32_t p = 0; p < nbuckets; p++)
        for (uint32_t o = 0; o < ORI_COUNT; o++) {
            uint32_t d = oracle_dist[p * ORI_COUNT + o];
            int32_t want = d <= PERIMETER_K ? (int32_t) d : -1;
            states_checked++;
            states_bad += lookup(p, o) != want;
        }
    printf("H4 lookup() per entry: even indices %u checked, %u wrong; "
           "odd indices %u checked, %u wrong\n",
           checked[0], bad[0], checked[1], bad[1]);
    printf("H4 lookup() per state: %u states checked, %u wrong\n",
           states_checked, states_bad);
    return bad[0] == 0 && bad[1] == 0 && states_bad == 0;
}

int main(void)
{
    printf("solver_ref.c with %s (PERIMETER_K = %d)\n",
#ifdef TABLES_IDA
           "tables_ida.h",
#else
           "tables.h",
#endif
           PERIMETER_K);
    build_oracle_transitions();
    uint32_t reached = build_oracle_distance();
    printf("oracle: %u states reached from the cube model\n", reached);
    if (reached != STATES)
        return 1;
    int h1 = gate_h1(), h2 = gate_h2(), h4 = gate_h4(), h3 = gate_h3();
    printf("result: H1 %s, H2 %s, H3 %s, H4 %s\n", h1 ? "pass" : "FAIL",
           h2 ? "pass" : "FAIL", h3 ? "pass" : "FAIL", h4 ? "pass" : "FAIL");
    free(oracle_dist);
    return h1 && h2 && h3 && h4 ? 0 : 1;
}
