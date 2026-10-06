# render_cli.s：LED matrix renderer（CLI 版，不畫圖）
#
# Ripes 的 CLI 沒有 I/O 周邊，引用 LED_MATRIX_0_BASE 的程式無法組譯，
# 所以量測 --iret 的 CLI 版接這個檔案：render_input 什麼都不做。
#   cat tables.s solver.s render_cli.s > solver_full.s
# GUI 版改接 render_gui.s；兩個版本只差在這個檔案。
# Ripes 不支援 .if，所以用兩個檔案取代作業建議的 .equ RENDER 加上 .if RENDER。

    .text
render_input:
    ret
