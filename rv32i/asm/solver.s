# solver.s：2x2x2 魔術方塊最短解（RV32I，Ripes）——參考實作
#
# 演算法：IDA*，heuristic = max(h_perm[p], h_ori[o])，h <= PERIMETER_K 時查 perimeter：
#   查到 -> 精確距離（exact）；查不到 -> PERIMETER_K + 1
# 搜尋到 exact 且 g + d <= bound 的狀態就停止，沿 perimeter 補完剩下的步數。
# PERIMETER_K = 0（tables_ida.s）時只有還原狀態會查表，等於只用 PDB 的 IDA*。
#
# 組譯：Ripes 一次只讀一個檔案，而且 .equ 要先定義才能使用，所以先接表格，再接本檔，
# 最後接 renderer（Ripes 不支援 .if，所以 renderer 的開關是換檔案）：
#   cat tables.s solver.s render_cli.s > solver_full.s     CLI 版，量測 --iret
#   cat tables.s solver.s render_gui.s > solver_gui.s      GUI 版，LED matrix 動畫
# 兩個版本只差在 renderer：CLI 版的 render_input 只有一條 ret。
# 輸入：修改下方 input 的 14 個字元（組譯時寫入）。
# 輸出：解答（例如 "B' R' D2"）與換行；結束碼 0 成功、1 搜尋或驗證失敗、2 輸入不合法。
#
# 測試模式（RUN_TESTS = 1，預設）：先依序執行 tests 表中的測試案例，在程式內比對
#   預期的最短步數（或預期被拒絕），每個案例印一行 PASS／FAIL，然後照常處理 input。
#   任何測試失敗時，結束碼為 1。
# 一般模式（RUN_TESTS = 0）：只處理 input。量測 --iret（例如回報 21345671111111 的指令數）
#   時使用這個模式，數字才只包含一次查詢。兩種模式只差開頭的一次判斷。
# 兩種模式都在印出 input 的解答後呼叫 render_input（見 render_gui.s）。
# Ripes 不支援 .if，所以模式是在執行時用 RUN_TESTS 判斷，而不是條件組譯。
    .equ RUN_TESTS, 1
#
# 只用 RV32I：沒有 mul/div/rem；乘以小常數用加法或 shift/add。
# 這個 Ripes 版本不支援 .if，所以沒有條件組譯。
#
# 搜尋時的暫存器（search 內部）：
#   s0 d：目前這一層的深度        s1 limit = bound - g（g = d + 1）   s2 這一輪最小的超出量
#   s3 目前這一層 frame 的位址    s4 moves 陣列        s5 p、s6 o：這一層的狀態
#   s7 face：正在試的面（-1 表示還沒開始）              s8 turn：這一面已轉幾次（3 = 換面）
#   s9 last_face：走到這一層的那一面（根節點為 3）        s10 tp、s11 to：轉動後的狀態
#   a0 = 3    a1 = PERIMETER_K    a2 = PERIMETER_K + 1    a3 = bound
#   a6、a7：目前這一面的 perm_qt、ori_qt 基底位址          ra：face_tables 基底
#   gp：h_perm 基底    tp：h_ori 基底    a4：bucket_start 基底    a5：bucket_entry 基底
#   （gp、tp、ra 在搜尋迴圈中沒有其他用途，所以拿來放常用的基底位址）

    .data
input:  .string "21345671111111"        # PPPPPPPOOOOOOO

# 內建測試案例：每筆 16 bytes = 狀態（.string，15 bytes 含 NUL）+ 預期結果（1 byte）
#   預期結果 0~11：預期的最短步數；255：預期被拒絕（輸入不合法）
#   預期步數來自 host 的完整 BFS 距離表（c_reference/H1-H4 的標準答案）
#   以第一個 byte 為 0 的一筆作為結尾
tests:  .string "12345671111111"        # 已還原
        .byte   0
        .string "24173562322133"        # 短打亂：R B' D2，真實距離 3
        .byte   3
        .string "21345671111111"        # 距離 11（tests/solutions.txt 的樣本）
        .byte   11
        .string "11345671111111"        # 不合法：角塊 1 重複
        .byte   255
        .byte   0                       # 結尾
msg_test:   .string "test "
msg_colon:  .string ": "
msg_pass:   .string " PASS "
msg_fail:   .string " FAIL "
msg_moves:  .string " moves: "
msg_reject: .string "rejected"
msg_nl:     .byte 10, 0
    .align 4
test_ptr:   .word 0                     # 目前的測試案例（search 會用掉所有暫存器，所以放在記憶體）
test_num:   .word 0                     # 目前的編號（1 起算）
test_fail:  .word 0                     # 失敗的個數
    .align 2
root_p: .half 0
root_o: .half 0
pos:    .zero 8                         # 解析後的排列 pos[0..6]
oris:   .zero 8                         # 解析後的方向 oris[0..6]
moves:  .zero 16                        # 解答，每步一個 byte（0..8）
frames: .zero 96                        # 搜尋堆疊：12 層，每層 8 bytes，只存這一層自己的值
                                        #   +0 p  +2 o（.half）  +4 face  +5 turn  +6 last_face（.byte）
    .align 2
face_tables:                            # face_tables[face]：該面的 perm_qt、ori_qt 位址
        .word perm_qt_R, ori_qt_R
        .word perm_qt_B, ori_qt_B
        .word perm_qt_D, ori_qt_D
names:  .byte 82, 0, 0, 0               # "R"   move 名稱，每個 4 bytes，索引 = move * 4
        .byte 82, 50, 0, 0              # "R2"
        .byte 82, 39, 0, 0              # "R'"
        .byte 66, 0, 0, 0               # "B"
        .byte 66, 50, 0, 0              # "B2"
        .byte 66, 39, 0, 0              # "B'"
        .byte 68, 0, 0, 0               # "D"
        .byte 68, 50, 0, 0              # "D2"
        .byte 68, 39, 0, 0              # "D'"
space:  .byte 32, 0
newline: .byte 10, 0

    .text
# ---------------------------------------------------------------- main
main:
    li   t0, RUN_TESTS
    bnez t0, main_tests
    # 一般模式：只處理 input。不經過 solve、不檢查 test_fail，
    # 讓量測 --iret 時只多開頭這兩條判斷（gcc 編譯 C 參考版時，RUN_TESTS = 0 是編譯時常數）
    la   a0, input
    jal  ra, parse              # a0 = 0 成功；a1 = p，a2 = o
    bnez a0, exit_invalid
    la   t0, root_p
    sh   a1, 0(t0)
    la   t0, root_o
    sh   a2, 0(t0)
    jal  ra, search             # a0 = 解答長度，失敗為 -1
    bltz a0, exit_fail
    mv   s0, a0
    jal  ra, verify             # a0 = 0 表示套用解答後回到還原狀態
    bnez a0, exit_fail
    mv   a0, s0
    jal  ra, print_solution     # 之後 s1 = 解答步數
    jal  ra, render_input       # LED 動畫（CLI 版是空的）
    li   a0, 0
    j    exit
main_tests:                     # 測試模式：先跑內建測試，失敗個數記在 test_fail
    jal  ra, run_tests
    la   a0, input              # 再照常處理 input
    jal  ra, solve              # a0 = 0 成功（a1 = 步數）、1 失敗、2 不合法
    bnez a0, main_tests_exit
    mv   a0, a1
    jal  ra, print_solution     # 之後 s1 = 解答步數
    jal  ra, render_input       # LED 動畫（CLI 版是空的）
    li   a0, 0
main_tests_exit:                # a0 = input 的結束碼；任何測試失敗時改為 1
    la   t0, test_fail
    lw   t0, 0(t0)
    beqz t0, exit
    li   a0, 1
    j    exit
exit_invalid:
    li   a0, 2
    j    exit
exit_fail:
    li   a0, 1
exit:
    li   a7, 93                 # Ripes ecall：以 a0 為結束碼結束
    ecall

# ---------------------------------------------------------------- solve
# 求解一個狀態：輸入 a0 = 14 字元字串的位址。
# 輸出 a0 = 0 成功（a1 = 解答步數，moves 已填好且已驗證回到還原狀態）、
#      a0 = 1 搜尋或驗證失敗、a0 = 2 輸入不合法。
# search 會改動幾乎所有暫存器；verify 保留 s0，所以步數放在 s0。
solve:
    addi sp, sp, -4
    sw   ra, 0(sp)
    jal  ra, parse              # a0 = 0 成功；a1 = p，a2 = o
    bnez a0, solve_invalid
    la   t0, root_p
    sh   a1, 0(t0)
    la   t0, root_o
    sh   a2, 0(t0)
    jal  ra, search             # a0 = 解答長度，失敗為 -1
    bltz a0, solve_fail
    mv   s0, a0
    jal  ra, verify             # a0 = 0 表示套用解答後回到還原狀態（T5）
    bnez a0, solve_fail
    li   a0, 0
    mv   a1, s0
    j    solve_return
solve_invalid:
    li   a0, 2
    j    solve_return
solve_fail:
    li   a0, 1
solve_return:
    lw   ra, 0(sp)
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- run_tests
# 依序執行 tests 表中的每個案例，在程式內比對預期結果，每個案例印一行：
#   test N: STATE PASS M moves: 解答        （預期步數 M，且 solve 成功、步數相同）
#   test N: STATE PASS rejected             （預期被拒絕，solve 回傳 2）
#   失敗時把 PASS 換成 FAIL，並把 test_fail 加 1。
# solve 會改動幾乎所有暫存器，所以目前的案例與編號都存在記憶體（test_ptr、test_num）。
run_tests:
    addi sp, sp, -4             # 
    sw   ra, 0(sp)
    la   t0, tests
    la   t1, test_ptr
    sw   t0, 0(t1)
    la   t1, test_num
    sw   zero, 0(t1)
    la   t1, test_fail
    sw   zero, 0(t1)
run_tests_loop:
    la   t1, test_ptr
    lw   t0, 0(t1)
    lbu  t2, 0(t0)
    beqz t2, run_tests_done     # 第一個 byte 為 0：沒有更多案例
    la   t1, test_num
    lw   t2, 0(t1)
    addi t2, t2, 1
    sw   t2, 0(t1)
    la   a0, msg_test           # "test N: STATE"
    li   a7, 4
    ecall
    mv   a0, t2
    li   a7, 1                  # Ripes ecall：印出整數
    ecall
    la   a0, msg_colon
    li   a7, 4
    ecall
    la   t1, test_ptr
    lw   a0, 0(t1)
    li   a7, 4
    ecall
    la   t1, test_ptr
    lw   a0, 0(t1)
    jal  ra, solve              # a0 = 狀態碼，a1 = 步數
    la   t1, test_ptr
    lw   t0, 0(t1)
    lbu  t2, 15(t0)             # 預期結果
    li   t3, 255
    beq  t2, t3, run_tests_expect_reject
    bnez a0, run_tests_fail     # 預期有解：solve 必須成功
    bne  a1, t2, run_tests_fail # 而且步數必須等於預期的最短步數
    mv   s0, a1                 # 通過：印出 " PASS M moves: 解答"
    la   a0, msg_pass
    li   a7, 4
    ecall
    mv   a0, s0
    li   a7, 1
    ecall
    la   a0, msg_moves
    li   a7, 4
    ecall
    mv   a0, s0
    jal  ra, print_solution     # 印出解答與換行
    j    run_tests_next
run_tests_expect_reject:
    li   t3, 2
    bne  a0, t3, run_tests_fail # 預期被拒絕：solve 必須回傳 2
    la   a0, msg_pass
    li   a7, 4
    ecall
    la   a0, msg_reject
    li   a7, 4
    ecall
    la   a0, msg_nl
    li   a7, 4
    ecall
    j    run_tests_next
run_tests_fail:
    la   t1, test_fail
    lw   t2, 0(t1)
    addi t2, t2, 1
    sw   t2, 0(t1)
    la   a0, msg_fail
    li   a7, 4
    ecall
    la   a0, msg_nl
    li   a7, 4
    ecall
run_tests_next:
    la   t1, test_ptr
    lw   t0, 0(t1)
    addi t0, t0, 16             # 下一筆案例
    sw   t0, 0(t1)
    j    run_tests_loop
run_tests_done:
    lw   ra, 0(sp)
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- parse
# 解析 a0 指向的 14 字元字串，檢查合法性，算出排列索引 p 與方向索引 o。
# 回傳 a0 = 0 成功（a1 = p、a2 = o），a0 = 1 不合法。
# 檢查的順序與內容不變；為了讓已還原、短打亂的輸入也比 gcc 快，改寫成：
#   指標遞增、常數放在迴圈外、mod 3 用位元遮罩、乘法展開成 shift/add。
parse:
    mv   t0, a0                 # t0：目前的排列字元（方向字元在 7(t0)）
    la   t1, pos                # t1：目前的 pos[i]（oris[i] 在 8(t1)，oris 緊接在 pos 後面）
    addi t3, t0, 7              # 迴圈結束的位置
    li   t4, 0                  # seen：第 c 個 bit 為 1 表示角塊 c 出現過
    li   t5, 0                  # 方向總和
    li   t6, 7
    li   a3, 3
    li   a4, 1
parse_loop:
    lbu  a1, 0(t0)              # 排列字元
    addi a1, a1, -49            # '1' -> 0；比 '1' 小的字元（含 NUL）會變成很大的無號數
    bgeu a1, t6, parse_bad
    sll  a2, a4, a1             # 角塊 c 對應的 bit
    and  a5, t4, a2
    bnez a5, parse_bad          # 角塊重複
    or   t4, t4, a2
    sb   a1, 0(t1)              # pos[i] = c
    lbu  a1, 7(t0)              # 方向字元
    addi a1, a1, -49
    bgeu a1, a3, parse_bad
    add  t5, t5, a1
    sb   a1, 8(t1)              # oris[i] = d
    addi t0, t0, 1
    addi t1, t1, 1
    bne  t0, t3, parse_loop
    lbu  a1, 7(t0)              # input[14] 必須是 NUL：長度剛好 14
    bnez a1, parse_bad
    li   a2, 0x1249             # 第 0、3、6、9、12 個 bit：總和（最多 14）是 3 的倍數
    srl  a2, a2, t5
    andi a2, a2, 1
    beqz a2, parse_bad          # 方向總和必須是 3 的倍數

    # 排列索引：先把每一位的 smaller（後面比 pos[i] 小的個數）寫回 pos[i]，
    # i 由小到大，之後只會讀到 pos[j]（j > i），所以覆寫是安全的。i = 6 的 smaller 一定是 0。
    addi t1, t1, -7             # t1 = &pos[0]
    addi t3, t1, 6              # 只算 i = 0..5
    addi t6, t1, 7              # 內層迴圈的結束位置 &pos[7]
parse_rank:
    lbu  a2, 0(t1)              # pos[i]
    li   a3, 0                  # smaller
    addi a4, t1, 1              # j = i + 1
parse_count:
    lbu  a5, 0(a4)
    sltu a5, a5, a2             # pos[j] < pos[i]：1，否則 0
    add  a3, a3, a5
    addi a4, a4, 1
    bne  a4, t6, parse_count
    sb   a3, 0(t1)              # pos[i] = smaller
    addi t1, t1, 1
    bne  t1, t3, parse_rank
    # p = ((((c0 × 6 + c1) × 5 + c2) × 4 + c3) × 3 + c4) × 2 + c5，乘法展開成 shift/add
    addi t1, t1, -6             # t1 = &pos[0]
    lbu  a1, 0(t1)              # p = c0
    slli a2, a1, 2
    slli a1, a1, 1
    add  a1, a1, a2             # × 6
    lbu  a2, 1(t1)
    add  a1, a1, a2             # + c1
    slli a2, a1, 2
    add  a1, a1, a2             # × 5
    lbu  a2, 2(t1)
    add  a1, a1, a2             # + c2
    slli a1, a1, 2              # × 4
    lbu  a2, 3(t1)
    add  a1, a1, a2             # + c3
    slli a2, a1, 1
    add  a1, a1, a2             # × 3
    lbu  a2, 4(t1)
    add  a1, a1, a2             # + c4
    slli a1, a1, 1              # × 2
    lbu  a2, 5(t1)
    add  a1, a1, a2             # + c5

    # 方向索引：前 6 個方向當作三進位數，o = o × 3 + oris[i]
    li   a2, 0
    addi t1, t1, 8              # t1 = &oris[0]
    addi t3, t1, 6
parse_ori:
    lbu  a5, 0(t1)
    slli a3, a2, 1
    add  a2, a3, a2             # o × 3
    add  a2, a2, a5
    addi t1, t1, 1
    bne  t1, t3, parse_ori
    li   a0, 0
    ret
parse_bad:
    li   a0, 1
    ret

# ---------------------------------------------------------------- load_face
# 依 s7（0 = R、1 = B、2 = D）載入該面的轉移表基底：a6 = perm_qt、a7 = ori_qt。改動 t0。
load_face:
    bnez s7, load_face_not_r
    la   a6, perm_qt_R
    la   a7, ori_qt_R
    ret
load_face_not_r:
    li   t0, 1
    bne  s7, t0, load_face_d
    la   a6, perm_qt_B
    la   a7, ori_qt_B
    ret
load_face_d:
    la   a6, perm_qt_D
    la   a7, ori_qt_D
    ret

# ---------------------------------------------------------------- heuristic
# 輸入 a0 = p、a1 = o；輸出 a0 = h、a1 = 1 表示 h 是精確距離。
# 改動 t0~t5；需要 gp、tp、a4、a5 已經載入基底位址。
heuristic:
    add  t0, gp, a0				# gp = h_perm
    lbu  t0, 0(t0)              # h_perm[p]
    add  t1, tp, a1				# tp = h_ori
    lbu  t1, 0(t1)              # h_ori[o]
    bgeu t0, t1, heuristic_max	# branch if (h_perm[p] >= h_ori[o])
    mv   t0, t1					# t0 will always be the bigger one
heuristic_max:                  # t0 = max(h_perm, h_ori)
    seqz t2, t0                 # h = 0 只有還原狀態：精確 . if t0 = 0 set t2 = 1
    li   t1, PERIMETER_K		# PERIMETER_K = 5
    bltu t1, t0, heuristic_out  # h > K：不可能在 perimeter 內，不必查
    slli t3, a0, 1				# a0 = p 
    add  t3, a4, t3				# a4 = bucket_start
    lhu  t4, 0(t3)              # 第 p 個桶的起點
    lhu  t5, 2(t3)              # 第 p 個桶的終點（下一個桶的起點）
heuristic_scan:
    bgeu t4, t5, heuristic_miss # not in the perimeter
    slli t3, t4, 1				# t4 = bucket_start[p] (2 bytes each)
    add  t3, a5, t3
    lhu  t3, 0(t3)              # 項目：o | 距離 << 10 . t3 = bucket_entry[bucket_start[p]]
    andi t1, t3, 1023			# mask lower 10 bits
    bltu t1, a1, heuristic_next # 桶內 o 遞增：還沒到就往後找 (the o in bucket must be sorted ascending)
    bne  t1, a1, heuristic_miss # 已經超過：不在 perimeter 內
    srli t0, t3, 10             # 找到：精確距離 . t0 = t3[15:10]
    li   t2, 1
    j    heuristic_out
heuristic_next:
    addi t4, t4, 1
    j    heuristic_scan
heuristic_miss:
    li   t0, PERIMETER_K
    addi t0, t0, 1              # 不在 perimeter 內：距離至少 K + 1
    li   t2, 0
heuristic_out:
    mv   a0, t0					# a0 = the distance
    mv   a1, t2					# a1 = 1 (found) 0 (not found)
    ret

# ---------------------------------------------------------------- lookup
# perimeter 查詢：輸入 a0 = p、a1 = o；輸出 a0 = 距離，不在 perimeter 內為 -1。
# 改動 t1、t3~t5。只在 finish 中使用（搜尋中的查詢已經寫在 heuristic 內）。
lookup:							# same logic as heuristic
    slli t3, a0, 1
    add  t3, a4, t3
    lhu  t4, 0(t3)
    lhu  t5, 2(t3)
lookup_scan:
    bgeu t4, t5, lookup_miss
    slli t3, t4, 1
    add  t3, a5, t3
    lhu  t3, 0(t3)
    andi t1, t3, 1023
    bltu t1, a1, lookup_next
    bne  t1, a1, lookup_miss
    srli a0, t3, 10
    ret
lookup_next:
    addi t4, t4, 1
    j    lookup_scan
lookup_miss:					# error handle (lookup is called when found a valid path, so it must be in perimeter)
    li   a0, -1
    ret

# ---------------------------------------------------------------- search
# 不使用遞迴的 IDA*。輸入：root_p、root_o；輸出 a0 = 解答長度（moves 已填好），失敗為 -1。
# 搜尋迴圈內不呼叫任何函式（heuristic、load_face 都已展開），所以 ra 拿來放 face_tables。
search:
    addi sp, sp, -4
    sw   ra, 0(sp)				# return address stored in stack
    la   gp, h_perm
    la   tp, h_ori
    la   a4, bucket_start
    la   a5, bucket_entry
    la   s4, moves
    la   t0, root_p				# encoded p
    lhu  s5, 0(t0)
    la   t0, root_o				# encoded o
    lhu  s6, 0(t0)
    mv   a0, s5					# a0 = p
    mv   a1, s6					# a1 = o
    jal  ra, heuristic          # 第一輪的 bound = h(root)
    mv   a3, a0					# a3 = bound
    beqz a1, search_setup		# a1 = 1：根節點已經在 perimeter 內
    li   s0, 0                  # 直接補完 (d = 0)
    mv   a0, s5
    mv   a1, s6
    mv   a2, s4
    jal  ra, finish
    j    search_return

search_setup:                   # 搜尋迴圈中固定不變的值
    li   a0, 3                  # 每一面轉 3 次、共 3 個面
    li   a1, PERIMETER_K
    addi a2, a1, 1              # 不在 perimeter 內時的 h = K + 1
    la   ra, face_tables        # 各面兩張轉移表的位址

search_iteration:               # 每一輪：從根節點重新開始
    li   s2, 255                # 這一輪被剪掉的節點中，最小的超出量 f - bound
    addi s1, a3, -1             # limit = bound - g，根節點的子節點 g = 1
    li   s0, 0
    la   s3, frames             # frame[0]
                                # s5、s6 是根節點的 p、o：第一輪在進入前載入；
                                # 之後每一輪結束時都已回到根節點，s5、s6 仍是根節點
    li   s9, 3                  # 根節點沒有上一步的面
    li   s7, -1
    li   s8, 3                  # turn = 3：先換到第一個面

search_loop:
    bne  s8, a0, search_turn	# 這一面還沒轉滿 3 次
    addi s7, s7, 1              # 換下一面，跳過和上一步同一面的
    bne  s7, s9, search_face_ok
    addi s7, s7, 1
search_face_ok:
    bgeu s7, a0, search_pop     # 三個面都試完了：回到上一層
    mv   s10, s5				# 從這一層的狀態開始轉
    mv   s11, s6
    li   s8, 0
    slli t0, s7, 3              # face_tables[face]：perm_qt、ori_qt 的位址
    add  t0, ra, t0
    lw   a6, 0(t0)
    lw   a7, 4(t0)

search_turn:                    # 接著上一次的結果再轉一次：依序得到 X、X2、X'
    slli t0, s10, 1
    add  t0, a6, t0
    lhu  s10, 0(t0)             # tp = perm_qt[face][tp]
    slli t0, s11, 1
    add  t0, a7, t0
    lhu  s11, 0(t0)             # to = ori_qt[face][to]
    addi s8, s8, 1				# turn++
    # heuristic（展開）：t0 = max(h_perm[tp], h_ori[to])
    add  t0, gp, s10
    lbu  t0, 0(t0)
    add  t1, tp, s11
    lbu  t1, 0(t1)
    bgeu t0, t1, search_hmax
    mv   t0, t1
search_hmax:
    bltu a1, t0, search_plain   # h > K：不可能在 perimeter 內，不必查
    slli t3, s10, 1             # 第 tp 個桶：[bucket_start[tp], bucket_start[tp + 1])
    add  t3, a4, t3
    lhu  t4, 0(t3)
    lhu  t5, 2(t3)
    bgeu t4, t5, search_miss    # 空桶
    slli t4, t4, 1
    add  t4, a5, t4             # 指向第一個項目
    slli t5, t5, 1
    add  t5, a5, t5             # 指向桶的結尾
search_scan:
    lhu  t3, 0(t4)              # 項目：o | 距離 << 10
    andi t1, t3, 1023
    bltu t1, s11, search_scan_next # 桶內 o 遞增：還沒到就往後找
    bne  t1, s11, search_miss   # 已經超過：不在 perimeter 內
    srli t0, t3, 10             # 找到：精確距離
    bltu s1, t0, search_prune   # 距離 > limit：剪枝
    j    search_found           # 精確距離且不超過 bound：找到最短解
search_scan_next:
    addi t4, t4, 2
    bltu t4, t5, search_scan
search_miss:
    mv   t0, a2                 # 不在 perimeter 內：h = K + 1
search_plain:                   # h 不是精確值
    bltu s1, t0, search_prune   # h > limit（也就是 g + h > bound）：剪枝
    slli t1, s7, 1              # 不剪枝：記錄這一步，往下一層
    add  t1, t1, s7
    add  t1, t1, s8
    addi t1, t1, -1             # move = face * 3 + turn - 1
    add  t3, s4, s0
    sb   t1, 0(t3)              # moves[d] = move
    sh   s5, 0(s3)              # 保存這一層自己的值：p、o、face、turn、last_face
    sh   s6, 2(s3)              #（tp、to 不必存：回溯時就是子節點的 p、o，仍在 s5、s6）
    sb   s7, 4(s3)
    sb   s8, 5(s3)
    sb   s9, 6(s3)
    addi s3, s3, 8
    addi s0, s0, 1				# d++
    addi s1, s1, -1             # 下一層的 g 多 1：limit 少 1
    mv   s5, s10
    mv   s6, s11
    mv   s9, s7
    li   s7, -1
    li   s8, 3
    j    search_loop

search_prune:                   # 記下最小的超出量 h - limit（= f - bound，至少 1）
    sub  t1, t0, s1
    bgeu t1, s2, search_loop
    mv   s2, t1
    j    search_loop

search_pop:
    beqz s0, search_iteration_end
    addi s3, s3, -8             # 回到上一層，接著試它的下一個 move
    addi s0, s0, -1				# d--
    addi s1, s1, 1              # 上一層的 g 少 1：limit 多 1
    mv   s10, s5                # 這一層的 tp、to = 剛離開的子節點的 p、o
    mv   s11, s6                #（必須在下面載入 s5、s6 之前）
    lhu  s5, 0(s3)              # 這一層自己的 p、o、face、turn、last_face
    lhu  s6, 2(s3)
    lbu  s7, 4(s3)
    lbu  s8, 5(s3)
    lbu  s9, 6(s3)
    slli t0, s7, 3
    add  t0, ra, t0
    lw   a6, 0(t0)
    lw   a7, 4(t0)
    j    search_loop

search_iteration_end:
    li   t0, 255
    beq  s2, t0, search_fail    # 沒有任何節點被剪掉：不應發生
    add  a3, a3, s2             # 下一輪的 bound = bound + 最小的超出量
    j    search_iteration

search_found:                   # t0 = 剩下的精確距離
    slli t1, s7, 1
    add  t1, t1, s7
    add  t1, t1, s8
    addi t1, t1, -1             # move = face * 3 + turn - 1
    add  t3, s4, s0
    sb   t1, 0(t3)              # moves[d] = move
    addi s0, s0, 1              # g
    mv   a0, s10
    mv   a1, s11
    add  a2, s4, s0
    jal  ra, finish             # 補完剩下的步數
    add  a0, a0, s0				# a0 = g + (distance in perimeter)
    j    search_return
search_fail:
    li   a0, -1
search_return:
    lw   ra, 0(sp)
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- finish
# 從 perimeter 內的狀態走回還原狀態：每一步找一個距離少 1 的鄰居。
# 輸入 a0 = p、a1 = o、a2 = 寫入 move 的位址；輸出 a0 = 步數。
# 只在搜尋結束時呼叫一次，可以改動 s0 以外的 s 暫存器。
finish:
    addi sp, sp, -4
    sw   ra, 0(sp)
    mv   s5, a0					# p
    mv   s6, a1					# o
    mv   s1, a2					# moves[]
    jal  ra, lookup	
    mv   s3, a0                 # 總步數 . a0 = moves in perimeter
    mv   s2, a0                 # 剩下的步數
finish_step:
    beqz s2, finish_done		
    addi s2, s2, -1             # 下一步要到達的距離
    li   s7, 0					# face R
finish_face:
    jal  ra, load_face			# get next state
    mv   s10, s5				# s10 = p
    mv   s11, s6				# s11 = o
    li   s8, 0
finish_turn:
    slli t0, s10, 1				
    add  t0, a6, t0				# a6 = perm_qt[]
    lhu  s10, 0(t0)				
    slli t0, s11, 1
    add  t0, a7, t0				# a7 = ori_qt[]
    lhu  s11, 0(t0)
    addi s8, s8, 1				# turn++
    mv   a0, s10				# s10 = new p
    mv   a1, s11				# s11 = new o
    jal  ra, lookup				#
    beq  a0, s2, finish_take	# a0 = distance found
    li   t0, 3					#
    bne  s8, t0, finish_turn	# branch if turn != 3 (turn = 0, 1, 2)
    addi s7, s7, 1              # 一定會在三個面之內找到 . face++
    j    finish_face
finish_take:
    slli t0, s7, 1				# s7 * 2
    add  t0, t0, s7				# s7 * 2 + s7
    add  t0, t0, s8				# s7 * 3 + s8 (face * 3 + turn)
    addi t0, t0, -1             # move = face * 3 + turn - 1
    sb   t0, 0(s1)				# s1 = moves[]
    addi s1, s1, 1				# moves++ (next index)
    mv   s5, s10				# s10 = p
    mv   s6, s11				# s11 = o
    j    finish_step
finish_done:
    mv   a0, s3					# s3 = moves in perimeter
    lw   ra, 0(sp)
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- verify
# 從 root 套用 moves[0..a0)，回傳 a0 = 0 表示回到還原狀態（T5）。保留 s0。
verify:
    addi sp, sp, -4
    sw   ra, 0(sp)
    mv   s1, a0                 # 剩下的步數 . a0 is the total distance from solved state
    la   s2, moves				# s2 = moves[]
    la   t0, root_p
    lhu  s5, 0(t0)
    la   t0, root_o
    lhu  s6, 0(t0)
verify_loop:
    beqz s1, verify_check		
    lbu  t1, 0(s2)              # move . s2 = moves[]
    li   s7, 0                  # face = move / 3：用減法
    li   t0, 3
verify_div:
    blt  t1, t0, verify_div_done	# t1 = move; t0 = 3
    addi t1, t1, -3				# move - 3
    addi s7, s7, 1				#
    j    verify_div
verify_div_done:
    addi s8, t1, 1              # 轉幾次 = move % 3 + 1
    jal  ra, load_face
verify_turn:
    slli t0, s5, 1
    add  t0, a6, t0			
    lhu  s5, 0(t0)				# s5 = perm_qt[s7(face)][root_p]
    slli t0, s6, 1
    add  t0, a7, t0
    lhu  s6, 0(t0)				# s6 = ori_qt[s7(face)][root_o]
    addi s8, s8, -1				# s8 is turns. turn until it reaches 0
    bnez s8, verify_turn
    addi s2, s2, 1				# moves++
    addi s1, s1, -1				# moves remaining
    j    verify_loop
verify_check:
    or   a0, s5, s6             # p = 0 且 o = 0 時為 0 . a0 must be all zeros since p = 0 and o = 0 because it's solved state
    lw   ra, 0(sp)				
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- print_solution
# 印出 moves[0..a0)，move 之間以空白分隔，最後換行。返回時 s1 = a0（render_input 使用）。
print_solution:
    mv   s1, a0					# a0 is distance
    la   s2, moves
    la   s3, names
    li   s4, 0
print_loop:
    beq  s4, s1, print_end
    beqz s4, print_name         # 第一個 move 前面不印空白
    la   a0, space				# 
    li   a7, 4                  # Ripes ecall：印出字串
    ecall
print_name:
    add  t0, s2, s4
    lbu  t0, 0(t0)				# moves[s4]
    slli t0, t0, 2				# move * 4
    add  a0, s3, t0				# s3 = names
    li   a7, 4
    ecall
    addi s4, s4, 1
    j    print_loop
print_end:
    la   a0, newline
    li   a7, 4
    ecall
    ret
