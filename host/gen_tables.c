/* gen_tables.c：host 端表格產生器，輸出 RV32I 組語的資料檔（tables.s），
 * 並可同時輸出內容相同的 C 標頭檔（tables.h），給 c/solver_ref.c 使用
 *
 * 輸出的表格（標籤名稱 / 每格大小 / 格數）：
 *   perm_qt_R, perm_qt_B, perm_qt_D   2 bytes  5,040   排列索引 p 轉一次 quarter turn 後的 p'
 *   ori_qt_R,  ori_qt_B,  ori_qt_D    2 bytes    729   方向索引 o 轉一次 quarter turn 後的 o'
 *   bucket_start                      2 bytes  5,041   perimeter 第 p 個桶的起點（-k 0 時只輸出 2 格）
 *   bucket_entry                      2 bytes  周界大小 低 10 bits 是 o，高 6 bits 是距離（-k 0 時只有還原狀態）
 *   h_perm                            1 byte   5,040   只看排列時到還原狀態的距離
 *   h_ori                             1 byte     729   只看方向時到還原狀態的距離
 *
 * -k 0（只用 PDB 的 IDA*）：Ripes 不支援 .if，組語無法用條件組譯拿掉 perimeter 查詢，
 * 所以查詢一律保留，只在 h <= K 時執行。K = 0 時只有 h = 0（還原狀態，p = 0）會查表，
 * 因此只需要 bucket_start[0..1] 和一個項目（o = 0、距離 0）。
 *
 * 寫檔前的檢查：
 *   轉移表：每個面的表都是 0..n-1 的排列（一對一），而且套用 4 次等於不動
 *   H2：PDB 全部填滿、還原狀態為 0；perimeter 的狀態數、桶的起點遞增、桶內 o 遞增
 *   H1：組合後的 heuristic 在全部 3,674,160 個狀態都不高估，perimeter 內的值等於真實距離
 * 寫檔後的檢查：把輸出的 .s 檔（以及 -c 的 .h 檔）讀回來，逐一比對每個標籤的每個數值
 *
 * 用法：gen_tables [-k K] [-s SECTION] [-o FILE] [-c HEADER]
 *   -k K        perimeter 半徑 0~5，預設 5；0 表示只用 PDB 的 IDA*（perimeter 只有還原狀態）
 *   -s SECTION  輸出的 section 指令，預設 .data（見下方說明）
 *   -o FILE     組語資料檔的檔名，預設 tables.s
 *   -c HEADER   另外輸出 C 標頭檔（同樣的表格、同樣的名稱與順序）；不給就不輸出
 * 狀態碼：0 成功，1 檢查失敗或檔案錯誤，2 參數錯誤
 *
 * section：作業要求表格以唯讀資料連結，但 Ripes v2.2.6-106-g5b8a616 實測不支援
 * .rodata（Unknown directive '.rodata'），.section .rodata 也會失敗
 * （Illegal multiple directives），所以預設用 .data。兩者都算在 128 KiB 的預算內。
 *
 * 對齊：同一個 Ripes 版本實測 .align 2 是對齊 2 bytes（不是 GNU 的 2^2 = 4），
 * .half 只需要 2 bytes 對齊，所以足夠。總大小也依 2 bytes 對齊計算。
 */
#include "cube_common.h"

#define MAX_K 5
#define MAX_PERIMETER 12224 /* 半徑 5 的狀態數 */
#define O_MASK 0x3FF
#define D_SHIFT 10
#define VALUES_PER_LINE 16

/* ---- perimeter：以排列 p 分桶，桶內依朝向 o 遞增排序 ---- */

static int K;
static uint16_t bucket_start[PERMUTATIONS + 1];
static uint16_t bucket_entry[MAX_PERIMETER];
static uint32_t perimeter_count;

static void build_perimeter(const uint8_t *dist)
{
    perimeter_count = 0;
    for (uint32_t p = 0; p < PERMUTATIONS; p++) {
        bucket_start[p] = (uint16_t) perimeter_count;
        for (uint32_t o = 0; o < ORIENTATIONS; o++) {
            uint8_t d = dist[p * ORIENTATIONS + o];
            if (d <= K && perimeter_count < MAX_PERIMETER)
                bucket_entry[perimeter_count++] =
                    (uint16_t) (o | (uint32_t) d << D_SHIFT);
        }
    }
    bucket_start[PERMUTATIONS] = (uint16_t) perimeter_count;
}

static int lookup(uint16_t p, uint16_t o)
{
    for (uint32_t i = bucket_start[p]; i < bucket_start[p + 1]; i++) {
        uint16_t eo = bucket_entry[i] & O_MASK;
        if (eo >= o)
            return eo == o ? bucket_entry[i] >> D_SHIFT : -1;
    }
    return -1;
}

/* 組合後的 heuristic；*exact 為 1 表示是真實距離。
 * 判斷流程和 solver.s 的 heuristic 相同（K = 0 時也走同一條路），H1 檢查的就是它 */
static int heuristic(uint16_t p, uint16_t o, int *exact)
{
    int h = h_perm[p] > h_ori[o] ? h_perm[p] : h_ori[o];
    *exact = h == 0;
    if (h > K)
        return h;
    int d = lookup(p, o);
    if (d >= 0) {
        *exact = 1;
        return d;
    }
    return K + 1;
}

/* ---- 寫檔前的檢查 ---- */

/* 轉移表：一對一，而且套用 4 次回到原值 */
static int check_transitions(void)
{
    static uint8_t seen[PERMUTATIONS];
    for (int face = 0; face < 3; face++) {
        const uint16_t *qt[2] = {perm_qt[face], ori_qt[face]};
        const uint32_t n[2] = {PERMUTATIONS, ORIENTATIONS};
        for (int t = 0; t < 2; t++) {
            memset(seen, 0, n[t]);
            for (uint32_t i = 0; i < n[t]; i++) {
                if (qt[t][i] >= n[t] || seen[qt[t][i]]++)
                    return 0;
                uint32_t x = i;
                for (int k = 0; k < 4; k++)
                    x = qt[t][x];
                if (x != i)
                    return 0;
            }
        }
    }
    return 1;
}

/* H2：perimeter 的結構 */
static int check_perimeter(const uint8_t *dist)
{
    uint32_t expected = 0;
    for (uint32_t r = 0; r < STATES; r++)
        if (dist[r] <= K)
            expected++;
    if (perimeter_count != expected || bucket_start[0] != 0 ||
        bucket_entry[0] != 0)
        return 0;
    for (uint32_t p = 0; p < PERMUTATIONS; p++) {
        if (bucket_start[p] > bucket_start[p + 1])
            return 0;
        for (uint32_t i = bucket_start[p]; i + 1 < bucket_start[p + 1]; i++)
            if ((bucket_entry[i] & O_MASK) >= (bucket_entry[i + 1] & O_MASK))
                return 0;
    }
    return 1;
}

/* H1：不高估；exact 的值等於真實距離 */
static int check_admissible(const uint8_t *dist, uint32_t *over,
                            uint32_t *wrong_exact)
{
    *over = *wrong_exact = 0;
    for (uint32_t r = 0; r < STATES; r++) {
        int exact;
        int h = heuristic(r / ORIENTATIONS, r % ORIENTATIONS, &exact);
        if (h > dist[r])
            (*over)++;
        if (exact && h != dist[r])
            (*wrong_exact)++;
    }
    return *over == 0 && *wrong_exact == 0;
}

/* ---- 輸出 ---- */

typedef struct {
    const char *label;
    const char *comment;
    int width; /* 1 = .byte，2 = .half */
    const void *data;
    uint32_t count;
} Table;

static uint32_t table_value(const Table *t, uint32_t i)
{
    return t->width == 2 ? ((const uint16_t *) t->data)[i]
                         : ((const uint8_t *) t->data)[i];
}

/* Ripes 的 .align 2 對齊 2 bytes（見檔頭說明） */
static uint32_t align2(uint32_t x)
{
    return (x + 1) & ~1U;
}

static int write_tables(const char *path, const char *section,
                        const Table *tables, int ntables, uint32_t *bytes)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "# %s: generated by gen_tables -k %d -s %s (do not edit)\n",
            path, K, section);
    fprintf(f, "# perimeter radius K = %d%s\n", K,
            K ? "" : " (PDB-only IDA*: perimeter holds the solved state only)");
    fprintf(f, "\n.equ PERM_COUNT, %d\n", PERMUTATIONS);
    fprintf(f, ".equ ORI_COUNT, %d\n", ORIENTATIONS);
    fprintf(f, ".equ PERIMETER_K, %d\n", K);
    fprintf(f, ".equ PERIMETER_COUNT, %u\n", perimeter_count);
    fprintf(f, ".equ ENTRY_O_MASK, 0x%X\n", O_MASK);
    fprintf(f, ".equ ENTRY_D_SHIFT, %d\n", D_SHIFT);
    fprintf(f, "\n%s\n", section);

    uint32_t offset = 0;
    for (int k = 0; k < ntables; k++) {
        const Table *t = &tables[k];
        offset = align2(offset); /* 每張表都從偶數位址開始 */
        fprintf(f, "\n# %s: %u x %d bytes\n", t->comment, t->count, t->width);
        fprintf(f, "    .align 2\n%s:\n", t->label);
        for (uint32_t i = 0; i < t->count; i++) {
            if (i % VALUES_PER_LINE == 0)
                fprintf(f, "    %s ", t->width == 2 ? ".half" : ".byte");
            fprintf(f, "%u", table_value(t, i));
            fputs(i % VALUES_PER_LINE == VALUES_PER_LINE - 1 ||
                          i + 1 == t->count
                      ? "\n"
                      : ", ",
                  f);
        }
        offset += t->count * (uint32_t) t->width;
    }
    *bytes = offset; /* 最後一張表之後不需要補齊 */
    fprintf(f, "\n# total: %u bytes including alignment\n", *bytes);
    return fclose(f) == 0;
}

/* 把輸出的檔案讀回來，逐一比對每個標籤的每個數值 */
static int verify_file(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    uint32_t got[16] = {0};
    int cur = -1, ok = 1;
    char line[1024];
    while (ok && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t len = strcspn(s, "\r\n");
        s[len] = '\0';
        if (len && s[len - 1] == ':') { /* 標籤 */
            s[len - 1] = '\0';
            cur = -1;
            for (int k = 0; k < ntables; k++)
                if (!strcmp(s, tables[k].label))
                    cur = k;
            if (cur < 0)
                ok = 0;
            continue;
        }
        int width = !strncmp(s, ".half", 5) ? 2 : !strncmp(s, ".byte", 5) ? 1 : 0;
        if (!width)
            continue;
        if (cur < 0 || width != tables[cur].width) {
            ok = 0;
            break;
        }
        char *q = s + 5;
        for (;;) {
            char *end;
            long v = strtol(q, &end, 0);
            if (end == q)
                break;
            const Table *t = &tables[cur];
            if (got[cur] >= t->count || (uint32_t) v != table_value(t, got[cur]))
                ok = 0;
            got[cur]++;
            q = end;
            while (*q == ' ' || *q == ',')
                q++;
        }
    }
    fclose(f);
    for (int k = 0; k < ntables; k++)
        if (got[k] != tables[k].count)
            ok = 0;
    return ok;
}

/* C 標頭檔：和 .s 檔相同的表格、名稱與順序，給 c/solver_ref.c 使用 */
static int write_c_header(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "/* %s: generated by gen_tables -k %d (do not edit) */\n", path, K);
    fprintf(f, "#ifndef CUBE_TABLES_H\n#define CUBE_TABLES_H\n\n#include <stdint.h>\n\n");
    fprintf(f, "#define PERM_COUNT %d\n", PERMUTATIONS);
    fprintf(f, "#define ORI_COUNT %d\n", ORIENTATIONS);
    fprintf(f, "#define PERIMETER_K %d\n", K);
    fprintf(f, "#define PERIMETER_COUNT %u\n", perimeter_count);
    fprintf(f, "#define ENTRY_O_MASK 0x%X\n", O_MASK);
    fprintf(f, "#define ENTRY_D_SHIFT %d\n", D_SHIFT);
    for (int k = 0; k < ntables; k++) {
        const Table *t = &tables[k];
        fprintf(f, "\n/* %s: %u x %d bytes */\n", t->comment, t->count, t->width);
        fprintf(f, "static const %s %s[%u] = {\n",
                t->width == 2 ? "uint16_t" : "uint8_t", t->label, t->count);
        for (uint32_t i = 0; i < t->count; i++) {
            if (i % VALUES_PER_LINE == 0)
                fputs("    ", f);
            fprintf(f, "%u,", table_value(t, i));
            fputs(i % VALUES_PER_LINE == VALUES_PER_LINE - 1 || i + 1 == t->count
                      ? "\n"
                      : " ",
                  f);
        }
        fputs("};\n", f);
    }
    fputs("\n#endif /* CUBE_TABLES_H */\n", f);
    return fclose(f) == 0;
}

/* 把 C 標頭檔讀回來，逐一比對：遇到 "static const uintN_t name[" 切換目前的表，
 * 數字開頭的行逐一解析，"};" 結束這張表 */
static int verify_c_header(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    uint32_t got[16] = {0};
    int cur = -1, ok = 1;
    char line[1024];
    while (ok && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        int width = !strncmp(s, "static const uint16_t ", 22) ? 2
                    : !strncmp(s, "static const uint8_t ", 21) ? 1
                                                               : 0;
        if (width) {
            char *name = s + (width == 2 ? 22 : 21);
            size_t len = strcspn(name, "[");
            cur = -1;
            for (int k = 0; k < ntables; k++)
                if (strlen(tables[k].label) == len &&
                    !strncmp(name, tables[k].label, len))
                    cur = k;
            if (cur < 0 || tables[cur].width != width)
                ok = 0;
            continue;
        }
        if (!strncmp(s, "};", 2)) {
            cur = -1;
            continue;
        }
        if (*s < '0' || *s > '9')
            continue;
        if (cur < 0) {
            ok = 0;
            break;
        }
        char *q = s;
        for (;;) {
            char *end;
            long v = strtol(q, &end, 10);
            if (end == q)
                break;
            const Table *t = &tables[cur];
            if (got[cur] >= t->count || (uint32_t) v != table_value(t, got[cur]))
                ok = 0;
            got[cur]++;
            q = end;
            while (*q == ' ' || *q == ',')
                q++;
        }
    }
    fclose(f);
    for (int k = 0; k < ntables; k++)
        if (got[k] != tables[k].count)
            ok = 0;
    return ok;
}

int main(int argc, char **argv)
{
    const char *out = "tables.s", *section = ".data", *header = NULL;
    K = MAX_K;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > MAX_K) {
                fprintf(stderr, "radius must be 0..%d\n", MAX_K);
                return 2;
            }
            K = (int) v;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            section = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            header = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: %s [-k 0..%d] [-s SECTION] [-o FILE] [-c HEADER]\n",
                    argv[0], MAX_K);
            return 2;
        }
    }

    build_transition_tables();
    if (!check_transitions()) {
        fputs("transition tables: not a bijection or order does not divide 4\n",
              stderr);
        return 1;
    }
    printf("transition tables: bijective, 4 quarter turns = identity\n");

    if (!build_pdbs(1)) {
        fputs("H2 failed: pattern database not fully populated\n", stderr);
        return 1;
    }

    uint8_t *dist = build_distance_oracle();
    if (!dist) {
        fputs("could not build distance oracle\n", stderr);
        return 1;
    }
    build_perimeter(dist); /* K = 0 時只有還原狀態 */
    if (!check_perimeter(dist)) {
        fputs("H2 failed: perimeter malformed\n", stderr);
        free(dist);
        return 1;
    }
    printf("H2 perimeter (K=%d): %u states, buckets well-formed\n", K,
           perimeter_count);
    uint32_t over, wrong_exact;
    int admissible = check_admissible(dist, &over, &wrong_exact);
    free(dist);
    printf("H1 h > d on %u states, exact-but-wrong on %u states\n", over,
           wrong_exact);
    if (!admissible)
        return 1;

    Table tables[] = {
        {"perm_qt_R", "R quarter turn on permutation index", 2, perm_qt[0], PERMUTATIONS},
        {"perm_qt_B", "B quarter turn on permutation index", 2, perm_qt[1], PERMUTATIONS},
        {"perm_qt_D", "D quarter turn on permutation index", 2, perm_qt[2], PERMUTATIONS},
        {"ori_qt_R", "R quarter turn on orientation index", 2, ori_qt[0], ORIENTATIONS},
        {"ori_qt_B", "B quarter turn on orientation index", 2, ori_qt[1], ORIENTATIONS},
        {"ori_qt_D", "D quarter turn on orientation index", 2, ori_qt[2], ORIENTATIONS},
        {"bucket_start", "perimeter bucket start, indexed by p", 2, bucket_start, PERMUTATIONS + 1},
        {"bucket_entry", "perimeter entries: o | distance << 10", 2, bucket_entry, perimeter_count},
        {"h_perm", "distance ignoring orientation", 1, h_perm, PERMUTATIONS},
        {"h_ori", "distance ignoring permutation", 1, h_ori, ORIENTATIONS},
    };
    int ntables = (int) (sizeof tables / sizeof tables[0]);
    if (!K) /* 只有 p = 0 會被查詢：bucket_start 只需要 [0]、[1] */
        tables[6].count = 2;

    uint32_t bytes;
    if (!write_tables(out, section, tables, ntables, &bytes)) {
        fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    if (!verify_file(out, tables, ntables)) {
        fprintf(stderr, "%s: read-back does not match the tables\n", out);
        return 1;
    }
    printf("wrote %s: %d tables, %u bytes (budget 131072), read-back verified\n",
           out, ntables, bytes);
    if (header) {
        if (!write_c_header(header, tables, ntables)) {
            fprintf(stderr, "could not write %s\n", header);
            return 1;
        }
        if (!verify_c_header(header, tables, ntables)) {
            fprintf(stderr, "%s: read-back does not match the tables\n", header);
            return 1;
        }
        printf("wrote %s: same %d tables, read-back verified\n", header, ntables);
    }
    return 0;
}
