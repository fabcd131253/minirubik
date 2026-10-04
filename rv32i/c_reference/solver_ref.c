/* solver_ref.c：asm/solver.s 的 C 參考版
 *
 * 和 solver.s 做完全相同的事：相同的資料、相同的函式切分、相同的搜尋順序，
 * 所以對同一個輸入會印出相同的解答、回傳相同的結束碼。
 * 用途是以 riscv64-unknown-elf-gcc -O2 -march=rv32i -mabi=ilp32 編譯，
 * 作為手寫組語要比較的基準（作業 Stage 4 的要求）。
 *
 * 演算法：IDA*，h = max(h_perm[p], h_ori[o])；h <= PERIMETER_K 時查 perimeter，
 * 查到為精確距離，查不到為 PERIMETER_K + 1。碰到精確且 g + d <= bound 的狀態就停止，
 * 沿 perimeter 補完剩下的步數。表格由 host/gen_tables 產生（-c 輸出的 tables.h）。
 *
 * 只用 RV32I 能直接做的運算：程式中沒有對變數做 *、/、%，
 * 否則 gcc 會呼叫 __mulsi3、__udivsi3、__umodsi3（作業不允許）。
 *   p * (7 - i)  -> times_small()：依乘數的位元加上 x、x << 1、x << 2
 *   sum mod 3    -> 依序減 12、6、3（sum 最多 14）
 *   move / 3     -> 減兩次 3（move 最多 8）
 *   x * 3        -> (x << 1) + x
 * solver.s 中對應的寫法是重複加法、迴圈減法；結果相同，C 這樣寫是為了避免編譯器
 * 把迴圈辨識成乘法或除法。
 *
 * 編譯：
 *   host：cc -O2 -std=c99 solver_ref.c -o solver_ref
 *         -DTABLES_IDA          使用 tables_ida.h（只用 PDB 的 IDA*）
 *         -DINPUT='"…"'        改變輸入（預設與 solver.s 相同）
 *   RV32I：見 1_submission/Makefile 的 rv32 目標（本機沒有工具鏈，未驗證）
 * 輸出：解答與換行；結束碼 0 成功、1 搜尋或驗證失敗、2 輸入不合法（與 solver.s 相同）
 */
#include <stdint.h>

#ifdef TABLES_IDA
#include "tables_ida.h"
#else
#include "tables.h"
#endif

#ifndef INPUT
#define INPUT "21345671111111" /* PPPPPPPOOOOOOO */
#endif

/* ---- 資料（對應 solver.s 的 .data） ---- */

static const char input[16] = INPUT; /* 固定 16 bytes：讀 input[14] 不會越界 */
static uint16_t root_p, root_o;
static uint8_t pos[8];   /* 解析後的排列 */
static uint8_t oris[8];  /* 解析後的方向 */
static uint8_t moves[16]; /* 解答，每步一個 move（0..8） */

typedef struct {         /* 搜尋堆疊的一層，和 solver.s 相同的 16 bytes 配置 */
    uint16_t p, o, tp, to;
    uint8_t face, turn, last_face, pad[5];
} Frame;
static Frame frames[12];

static const char names[9][4] = {"R", "R2", "R'", "B", "B2",
                                 "B'", "D", "D2", "D'"};

/* ---- 輸出與結束（對應 solver.s 的 ecall） ---- */

#ifdef __riscv
/* Ripes 的 ecall：a7 = 4 印出 a0 指向的字串；a7 = 93 以 a0 為結束碼結束 */
static void print_str(const char *s)
{
    register const char *a0 __asm__("a0") = s;
    register int a7 __asm__("a7") = 4;
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
}

static void exit_with(int code)
{
    register int a0 __asm__("a0") = code;
    register int a7 __asm__("a7") = 93;
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    for (;;)
        ;
}
#else
#include <stdio.h>
#include <stdlib.h>

static void print_str(const char *s)
{
    fputs(s, stdout);
}

static void exit_with(int code)
{
    fflush(stdout);
    exit(code);
}
#endif

/* ---- 小常數乘法：x * k，k 為 0..7 ---- */

static uint32_t times_small(uint32_t x, uint32_t k)
{
    uint32_t r = 0;
    if (k & 1)
        r += x;
    if (k & 2)
        r += x << 1;
    if (k & 4)
        r += x << 2;
    return r;
}

/* ---- parse：解析 input、檢查合法性，算出排列索引 p 與方向索引 o ----
 * 回傳 0 成功，1 不合法。檢查的順序和 solver.s 相同。 */
static uint32_t parse(uint32_t *p_out, uint32_t *o_out)
{
    uint32_t seen = 0, sum = 0;
    for (uint32_t i = 0; i < 7; i++) {
        /* 比 '1' 小的字元（含 NUL）減完會變成很大的無號數 */
        uint32_t c = (uint32_t) (uint8_t) input[i] - 49u;
        if (c >= 7)
            return 1;
        uint32_t bit = 1u << c;
        if (seen & bit) /* 角塊重複 */
            return 1;
        seen |= bit;
        pos[i] = (uint8_t) c;
        uint32_t d = (uint32_t) (uint8_t) input[i + 7] - 49u;
        if (d >= 3)
            return 1;
        sum += d;
        oris[i] = (uint8_t) d;
    }
    if (input[14] != 0) /* 長度必須剛好 14 */
        return 1;
    if (sum >= 12) /* sum mod 3（sum <= 14） */
        sum -= 12;
    if (sum >= 6)
        sum -= 6;
    if (sum >= 3)
        sum -= 3;
    if (sum != 0) /* 方向總和必須是 3 的倍數 */
        return 1;

    uint32_t p = 0; /* p = p * (7 - i) + （後面比 pos[i] 小的個數） */
    for (uint32_t i = 0; i < 7; i++) {
        uint32_t smaller = 0;
        for (uint32_t j = i + 1; j < 7; j++)
            if (pos[j] < pos[i])
                smaller++;
        p = times_small(p, 7 - i) + smaller;
    }
    uint32_t o = 0; /* 前 6 個方向當作三進位數 */
    for (uint32_t i = 0; i < 6; i++)
        o = (o << 1) + o + oris[i];
    *p_out = p;
    *o_out = o;
    return 0;
}

/* ---- load_face：依面（0 = R、1 = B、2 = D）選擇兩張轉移表 ---- */
static void load_face(uint32_t face, const uint16_t **pq, const uint16_t **oq)
{
    if (face == 0) {
        *pq = perm_qt_R;
        *oq = ori_qt_R;
    } else if (face == 1) {
        *pq = perm_qt_B;
        *oq = ori_qt_B;
    } else {
        *pq = perm_qt_D;
        *oq = ori_qt_D;
    }
}

/* ---- heuristic：回傳 h，*exact = 1 表示 h 是精確距離 ---- */
static uint32_t heuristic(uint32_t p, uint32_t o, uint32_t *exact)
{
    uint32_t h = h_perm[p], ho = h_ori[o];
    if (h < ho)
        h = ho;
    *exact = h == 0; /* h = 0 只有還原狀態 */
    if (h > PERIMETER_K) /* 不可能在 perimeter 內，不必查 */
        return h;
    for (uint32_t i = bucket_start[p], end = bucket_start[p + 1]; i < end; i++) {
        uint32_t e = bucket_entry[i], eo = e & ENTRY_O_MASK;
        if (eo < o) /* 桶內 o 遞增：還沒到就往後找 */
            continue;
        if (eo != o) /* 已經超過：不在 perimeter 內 */
            break;
        *exact = 1; /* 找到：精確距離 */
        return e >> ENTRY_D_SHIFT;
    }
    *exact = 0;
    return PERIMETER_K + 1; /* 不在 perimeter 內：距離至少 K + 1 */
}

/* ---- lookup：perimeter 查詢，回傳距離，不在 perimeter 內為 -1 ---- */
static int32_t lookup(uint32_t p, uint32_t o)
{
    for (uint32_t i = bucket_start[p], end = bucket_start[p + 1]; i < end; i++) {
        uint32_t e = bucket_entry[i], eo = e & ENTRY_O_MASK;
        if (eo < o)
            continue;
        if (eo != o)
            break;
        return (int32_t) (e >> ENTRY_D_SHIFT);
    }
    return -1;
}

/* ---- finish：從 perimeter 內的狀態走回還原狀態，每一步找距離少 1 的鄰居 ----
 * 寫入 out[0..)，回傳步數。和 solver.s 一樣假設一定找得到（只在 exact 時呼叫）。 */
static uint32_t finish(uint32_t p, uint32_t o, uint8_t *out)
{
    const uint16_t *pq, *oq;
    uint32_t total = (uint32_t) lookup(p, o), remaining = total;
    while (remaining != 0) {
        remaining--; /* 下一步要到達的距離 */
        uint32_t found = 0;
        for (uint32_t face = 0; !found; face++) {
            load_face(face, &pq, &oq);
            uint32_t tp = p, to = o;
            for (uint32_t turn = 1; turn <= 3; turn++) {
                tp = pq[tp];
                to = oq[to];
                if (lookup(tp, to) == (int32_t) remaining) {
                    *out++ = (uint8_t) ((face << 1) + face + turn - 1);
                    p = tp;
                    o = to;
                    found = 1;
                    break;
                }
            }
        }
    }
    return total;
}

/* ---- search：不使用遞迴的 IDA*，回傳解答長度（moves 已填好），失敗為 -1 ---- */
static int32_t search(void)
{
    const uint16_t *pq = perm_qt_R, *oq = ori_qt_R;
    uint32_t exact;
    uint32_t p = root_p, o = root_o;
    uint32_t bound = heuristic(p, o, &exact); /* 第一輪的 bound = h(root) */
    if (exact) /* 根節點已經是精確距離：直接補完 */
        return (int32_t) finish(p, o, moves);

    for (;;) { /* 每一輪：從根節點重新開始 */
        uint32_t next_bound = 255, d = 0;
        Frame *f = frames;
        uint32_t tp = 0, to = 0;
        uint32_t last = 3;           /* 根節點沒有上一步的面 */
        uint32_t face = (uint32_t) -1;
        uint32_t turn = 3;           /* 先換到第一個面 */
        p = root_p;
        o = root_o;

        for (;;) {
            if (turn == 3) {         /* 換下一面，跳過和上一步同一面的 */
                face++;
                if (face == last)
                    face++;
                if (face >= 3) {     /* 三個面都試完：回到上一層 */
                    if (d == 0)
                        break;       /* 這一輪結束 */
                    f--;
                    d--;
                    p = f->p;
                    o = f->o;
                    tp = f->tp;
                    to = f->to;
                    face = f->face;
                    turn = f->turn;
                    last = f->last_face;
                    load_face(face, &pq, &oq);
                    continue;
                }
                tp = p;
                to = o;
                turn = 0;
                load_face(face, &pq, &oq);
            }
            tp = pq[tp];             /* 接著上一次的結果再轉一次：X、X2、X' */
            to = oq[to];
            turn++;
            moves[d] = (uint8_t) ((face << 1) + face + turn - 1);
            uint32_t h = heuristic(tp, to, &exact);
            uint32_t fval = d + 1 + h; /* f = g + h，g = d + 1 */
            if (fval > bound) {      /* 剪枝，記下最小的超出值 */
                if (fval < next_bound)
                    next_bound = fval;
                continue;
            }
            if (exact) {             /* 精確距離且不超過 bound：找到最短解 */
                d++;
                return (int32_t) (d + finish(tp, to, moves + d));
            }
            f->p = (uint16_t) p;     /* 保存這一層，往下一層 */
            f->o = (uint16_t) o;
            f->tp = (uint16_t) tp;
            f->to = (uint16_t) to;
            f->face = (uint8_t) face;
            f->turn = (uint8_t) turn;
            f->last_face = (uint8_t) last;
            f++;
            d++;
            p = tp;
            o = to;
            last = face;
            face = (uint32_t) -1;
            turn = 3;
        }
        if (next_bound == 255)       /* 沒有任何節點被剪掉：不應發生 */
            return -1;
        bound = next_bound;          /* 下一輪的 bound */
    }
}

/* ---- verify：從 root 套用 moves[0..len)，回傳 0 表示回到還原狀態（T5） ---- */
static uint32_t verify(uint32_t len)
{
    const uint16_t *pq, *oq;
    uint32_t p = root_p, o = root_o;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t m = moves[i], face = 0;
        if (m >= 3) { /* face = move / 3（move <= 8） */
            m -= 3;
            face++;
        }
        if (m >= 3) {
            m -= 3;
            face++;
        }
        load_face(face, &pq, &oq);
        for (uint32_t turns = m + 1; turns != 0; turns--) { /* move % 3 + 1 次 */
            p = pq[p];
            o = oq[o];
        }
    }
    return p | o; /* p = 0 且 o = 0 時為 0 */
}

/* ---- print_solution：move 之間以空白分隔，最後換行 ---- */
static void print_solution(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (i != 0)
            print_str(" ");
        print_str(names[moves[i]]);
    }
    print_str("\n");
}

/* ---- main：對應 solver.s 的 main ---- */
static int solver_main(void)
{
    uint32_t p, o;
    if (parse(&p, &o))
        return 2;
    root_p = (uint16_t) p;
    root_o = (uint16_t) o;
    int32_t len = search();
    if (len < 0)
        return 1;
    if (verify((uint32_t) len))
        return 1;
    print_solution((uint32_t) len);
    return 0;
}

#ifdef __riscv
void _start(void)
{
    exit_with(solver_main());
}
#else
int main(void)
{
    exit_with(solver_main());
    return 0;
}
#endif
