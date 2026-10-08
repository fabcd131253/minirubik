# render_gui.s：LED matrix renderer（GUI 版）
#
# 把方塊畫成展開圖，接在 solver.s 後面組譯：
#   cat tables.s solver.s render_gui.s > solver_full_gui.s
# 需要在 Ripes 的 I/O 分頁加入 LED Matrix（寬 35、高 25），組譯器才會有 LED_MATRIX_0_* 符號。
# CLI 版改接 render_cli.s（只有一個空的 render_input）；兩個版本只差在這個檔案。
#
# 介面：solver.s 印出解答後呼叫 render_input，s1 = 解答步數（print_solution 留下的值），
#   moves[0..s1) = 解答。renderer 自己從 input 字串讀出打亂狀態，先畫一張，
#   然後每套用解答的一步就重畫一張，最後一張是還原狀態。
#
# 展開圖（像素）：每張貼紙 4 寬 × 3 高，面內 2 × 2 張貼紙相連；
#   面的格位 9 像素寬、7 像素高（含 1 像素的黑色間隔）：
#         U                 U 在 (9, 0)
#     L   F   R   B         L (0, 7)、F (9, 7)、R (18, 7)、B (27, 7)
#         D                 D 在 (9, 14)
#   共 35 × 20 像素，下方 5 列不使用。LED 是 row-major：位址 = BASE + (y × WIDTH + x) × 4。
#
# 顏色：角塊 c 在位置 P、方向 k 時，位置 P 的第 j 張貼紙（j = 0 為 U/D 面，1、2 依外側看順時針）
#   顯示角塊 c 的第 (j + k) mod 3 種顏色。這條公式與座標表由
#   一個獨立的 3D 貼紙模擬程式逐張比對驗證。
#
# 轉動：與 solver_simple.c 相同的 FROM/SPIN 模型，多一個固定不動的位置 7（UFL）。

    .equ RENDER_DELAY, 200000           # 每張圖之後的延遲迴圈次數（每次 3 條指令），依模擬速度調整
    .equ RENDER_DUMP, 0                 # 1：每張圖之後把 LED 內容印成文字（測試用）

    .data
# r_from[face][i]：轉動後位置 i 的角塊來自哪個位置；r_spin[face][i]：方向的增量
r_from: .byte 1, 4, 2, 0, 3, 5, 6, 7    # R
        .byte 0, 1, 2, 4, 5, 6, 3, 7    # B
        .byte 0, 2, 5, 3, 1, 4, 6, 7    # D
r_spin: .byte 1, 2, 0, 2, 1, 0, 0, 0    # R
        .byte 0, 0, 0, 1, 2, 1, 2, 0    # B
        .byte 0, 0, 0, 0, 0, 0, 0, 0    # D
# 索引 P 是程式中的編號（code）：code 0..6 = cube 1..7，與 solver.s 的 pos、oris 相同；
#   固定不動的 cube 0（UFL）不在 solver 的狀態中，renderer 把它放在 code 7。
# 每個位置 3 張貼紙的順序 j：先 U/D 面，再依「從角的外側看順時針」排列，
#   不是位置名稱的字母順序（例如 UFR 的順序是 U、R、F）。方向 k 以順時針定義，
#   所以只有這個順序能讓顏色公式 (j + k) mod 3 成立。
# r_xy[P][j]：位置 P 第 j 張貼紙左上角的 (x, y)
r_xy:   .byte 13, 3,  18, 7,  13, 7     # code 0 = cube 1，位置 UFR，貼紙順序 U R F
        .byte 13, 14, 13, 10, 18, 10    # code 1 = cube 2，位置 DFR，貼紙順序 D F R
        .byte 9, 14,  4, 10,  9, 10     # code 2 = cube 3，位置 DFL，貼紙順序 D L F
        .byte 13, 0,  27, 7,  22, 7     # code 3 = cube 4，位置 UBR，貼紙順序 U B R
        .byte 13, 17, 22, 10, 27, 10    # code 4 = cube 5，位置 DBR，貼紙順序 D R B
        .byte 9, 17,  31, 10, 0, 10     # code 5 = cube 6，位置 DBL，貼紙順序 D B L
        .byte 9, 0,   0, 7,   31, 7     # code 6 = cube 7，位置 UBL，貼紙順序 U L B
        .byte 9, 3,   9, 7,   4, 7      # code 7 = cube 0，位置 UFL（固定），貼紙順序 U F L
r_letters: .byte 87, 89, 71, 66, 82, 79 # "WYGBRO"：RENDER_DUMP 用，與 r_palette 對應
r_line: .zero 40                        # RENDER_DUMP 用的一列文字
    .align 4
r_pos:  .zero 8                         # 目前的排列（含位置 7）
r_ori:  .zero 8                         # 目前的方向（r_pos + 8）
r_tmp:  .zero 16                        # 轉動時的暫存（排列 8 bytes + 方向 8 bytes）
r_off:  .zero 96                        # r_off[P*3+j]：貼紙左上角相對 BASE 的 byte 位移
# 6 種顏色：U 白、D 黃、F 綠、B 藍、R 紅、L 橘
r_palette: .word 0xFFFFFF, 0xFFFF00, 0x00C000, 0x0000FF, 0xFF0000, 0xFF8000
# r_color[c][s]：角塊 c（code，同 r_xy 的索引）的第 s 種顏色。
#   顏色依照角塊原位置的貼紙順序（同 r_xy：U/D 面開始，外側看順時針），不是位置名稱的順序。
r_color: .word 0xFFFFFF, 0xFF0000, 0x00C000   # code 0 = cube 1，UFR，U R F：白 紅 綠
         .word 0xFFFF00, 0x00C000, 0xFF0000   # code 1 = cube 2，DFR，D F R：黃 綠 紅
         .word 0xFFFF00, 0xFF8000, 0x00C000   # code 2 = cube 3，DFL，D L F：黃 橘 綠
         .word 0xFFFFFF, 0x0000FF, 0xFF0000   # code 3 = cube 4，UBR，U B R：白 藍 紅
         .word 0xFFFF00, 0xFF0000, 0x0000FF   # code 4 = cube 5，DBR，D R B：黃 紅 藍
         .word 0xFFFF00, 0x0000FF, 0xFF8000   # code 5 = cube 6，DBL，D B L：黃 藍 橘
         .word 0xFFFFFF, 0xFF8000, 0x0000FF   # code 6 = cube 7，UBL，U L B：白 橘 藍
         .word 0xFFFFFF, 0x00C000, 0xFF8000   # code 7 = cube 0，UFL（固定），U F L：白 綠 橘

    .text
# ---------------------------------------------------------------- render_input
# s1 = 解答步數。會改動所有暫存器（之後 main 只剩結束），ra 存在堆疊。
render_input:
    addi sp, sp, -4
    sw   ra, 0(sp)
    # LED matrix 至少要 35 × 20，否則不畫
    li   t0, LED_MATRIX_0_WIDTH
    li   t1, 35
    blt  t0, t1, render_return
    li   t0, LED_MATRIX_0_HEIGHT
    li   t1, 20
    blt  t0, t1, render_return
    # r_off[i] = (y × WIDTH + x) × 4；乘法用 y 次加法
    li   t5, LED_MATRIX_0_WIDTH
    slli t5, t5, 2                      # t5：一列的 bytes
    la   t0, r_xy
    la   t1, r_off
    li   t2, 24
render_off_loop:
    lbu  t3, 0(t0)                      # x
    lbu  t4, 1(t0)                      # y
    slli t3, t3, 2
render_off_row:
    beqz t4, render_off_store
    add  t3, t3, t5
    addi t4, t4, -1
    j    render_off_row
render_off_store:
    sw   t3, 0(t1)
    addi t0, t0, 2
    addi t1, t1, 4
    addi t2, t2, -1
    bnez t2, render_off_loop
    # 從 input 讀出打亂狀態：r_pos[i] = input[i] - '1'，r_ori[i] = input[i + 7] - '1'
    la   t0, input
    la   t1, r_pos
    li   t2, 7
render_read:
    lbu  t3, 0(t0)
    addi t3, t3, -49
    sb   t3, 0(t1)
    lbu  t3, 7(t0)
    addi t3, t3, -49
    sb   t3, 8(t1)
    addi t0, t0, 1
    addi t1, t1, 1
    addi t2, t2, -1
    bnez t2, render_read
    li   t3, 7                          # 位置 7（UFL）固定是角塊 7、方向 0
    sb   t3, 0(t1)
    sb   zero, 8(t1)
    jal  ra, render_frame               # 第 0 張：打亂狀態
    li   s2, 0                          # s2：已套用的步數
render_move:
    beq  s2, s1, render_return
    la   t0, moves                      # from solver.s
    add  t0, t0, s2
    lbu  t1, 0(t0)                      # move（0..8）
    li   s3, 0                          # s3 = move / 3（面），用減法
    li   t0, 3
render_div:
    blt  t1, t0, render_div_done
    addi t1, t1, -3
    addi s3, s3, 1
    j    render_div
render_div_done:
    addi s4, t1, 1                      # s4 = 轉幾次 quarter turn
render_turn:
    mv   a0, s3
    jal  ra, render_quarter
    addi s4, s4, -1
    bnez s4, render_turn
    jal  ra, render_frame
    addi s2, s2, 1
    j    render_move
render_return:
    lw   ra, 0(sp)
    addi sp, sp, 4
    ret

# ---------------------------------------------------------------- render_quarter
# 對 r_pos、r_ori 套用 a0 面的一次 quarter turn：
#   tmp_pos[i] = pos[from[i]]，tmp_ori[i] = (ori[from[i]] + spin[i]) mod 3，再複製回去。
render_quarter:
    slli t0, a0, 3                      # face × 8
    la   t1, r_from
    add  t1, t1, t0
    la   t2, r_spin
    add  t2, t2, t0
    la   t3, r_pos
    la   t4, r_tmp
    li   t5, 0
    li   a5, 3
    li   a6, 8
render_quarter_loop:
    add  t6, t1, t5
    lbu  t6, 0(t6)                      # from[i]
    add  t6, t3, t6
    lbu  a1, 0(t6)                      # pos[from[i]]
    lbu  a2, 8(t6)                      # ori[from[i]]
    add  a3, t2, t5
    lbu  a3, 0(a3)                      # spin[i]
    add  a2, a2, a3
    blt  a2, a5, render_quarter_store
    addi a2, a2, -3
render_quarter_store:
    add  a3, t4, t5
    sb   a1, 0(a3)
    sb   a2, 8(a3)
    addi t5, t5, 1
    blt  t5, a6, render_quarter_loop
    lw   t0, 0(t4)                      # 16 bytes 複製回 r_pos、r_ori
    sw   t0, 0(t3)
    lw   t0, 4(t4)
    sw   t0, 4(t3)
    lw   t0, 8(t4)
    sw   t0, 8(t3)
    lw   t0, 12(t4)
    sw   t0, 12(t3)
    ret

# ---------------------------------------------------------------- render_frame
# 依 r_pos、r_ori 畫出 24 張貼紙（每張 4 × 3 個 LED），然後延遲；RENDER_DUMP = 1 時印出文字。
render_frame:
    li   a0, LED_MATRIX_0_BASE
    li   t5, LED_MATRIX_0_WIDTH
    slli t5, t5, 2                      # t5：一列的 bytes
    la   t0, r_pos
    la   t1, r_off
    la   t2, r_color
    li   t3, 0                          # t3：位置 P
    li   a7, 3
render_frame_pos:
    add  t4, t0, t3
    lbu  a1, 0(t4)                      # 角塊 c
    lbu  a2, 8(t4)                      # 方向 k
    slli a3, a1, 3                      # &r_color[c][0] = r_color + c × 12
    slli a1, a1, 2
    add  a3, a3, a1
    add  a3, t2, a3
    li   a4, 0                          # a4：貼紙 j
render_frame_slot:
    add  a5, a4, a2                     # s = (j + k) mod 3
    blt  a5, a7, render_frame_color
    addi a5, a5, -3
render_frame_color:
    slli a5, a5, 2
    add  a5, a3, a5
    lw   a5, 0(a5)                      # 顏色
    lw   a6, 0(t1)                      # 位移
    add  a6, a0, a6
    sw   a5, 0(a6)                      # 第 1 列的 4 個 LED
    sw   a5, 4(a6)
    sw   a5, 8(a6)
    sw   a5, 12(a6)
    add  a6, a6, t5
    sw   a5, 0(a6)                      # 第 2 列
    sw   a5, 4(a6)
    sw   a5, 8(a6)
    sw   a5, 12(a6)
    add  a6, a6, t5
    sw   a5, 0(a6)                      # 第 3 列
    sw   a5, 4(a6)
    sw   a5, 8(a6)
    sw   a5, 12(a6)
    addi t1, t1, 4
    addi a4, a4, 1
    blt  a4, a7, render_frame_slot
    addi t3, t3, 1
    li   t4, 8
    blt  t3, t4, render_frame_pos
    li   t0, RENDER_DELAY               # 延遲，讓每一步停留在畫面上
render_frame_delay:
    beqz t0, render_frame_dump
    addi t0, t0, -1
    j    render_frame_delay
render_frame_dump:
    li   t0, RENDER_DUMP
    beqz t0, render_frame_return
    # 印出前 20 列 × 35 個 LED：顏色對應 r_letters 的字母，其他（黑色）印 '.'
    li   t0, 0                          # t0：y
render_dump_row:
    li   t1, 0                          # t1：x
    la   t2, r_line
render_dump_col:
    lw   t3, 0(a0)                      # a0 逐一走過這一列的 LED
    li   t4, 46                         # '.'
    la   a1, r_palette
    la   a2, r_letters
    li   a3, 6
render_dump_find:
    lw   a4, 0(a1)
    bne  a4, t3, render_dump_next
    lbu  t4, 0(a2)
    j    render_dump_put
render_dump_next:
    addi a1, a1, 4
    addi a2, a2, 1
    addi a3, a3, -1
    bnez a3, render_dump_find
render_dump_put:
    sb   t4, 0(t2)
    addi t2, t2, 1
    addi a0, a0, 4
    addi t1, t1, 1
    li   t4, 35
    blt  t1, t4, render_dump_col
    li   t4, 10                         # 換行與結尾
    sb   t4, 0(t2)
    sb   zero, 1(t2)
    li   t4, LED_MATRIX_0_WIDTH         # 跳到下一列的開頭
    addi t4, t4, -35
    slli t4, t4, 2
    add  a0, a0, t4
    mv   a1, a0
    la   a0, r_line
    li   a7, 4
    ecall
    mv   a0, a1
    addi t0, t0, 1
    li   t4, 20
    blt  t0, t4, render_dump_row
render_frame_return:
    ret
