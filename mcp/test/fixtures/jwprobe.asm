    .model small
    .stack 100h

Vtx STRUCT
    vx  SWORD ?
    vy  SWORD ?
    tag SDWORD ?
Vtx ENDS

    .data
jw_home  Vtx <3, 4, 100>
jw_nodes Vtx 4 dup(<>)
jw_count SWORD 4
jw_total SDWORD 0
jw_name  BYTE "jwprobe", 0

    .code
jw_scale PROTO FASTCALL :SWORD, :SWORD
jw_fill PROC near
    LOCAL i:SWORD
    mov  i, 0
fill_loop:
    mov  bx, i
    cmp  bx, jw_count
    jge  fill_done
    mov  ax, bx
    shl  bx, 1
    shl  bx, 1
    shl  bx, 1
    inc  ax
    mov  jw_nodes[bx].vx, ax
    shl  ax, 1
    mov  jw_nodes[bx].vy, ax
    mov  cx, i
    inc  cx
    mov  ax, 10
    imul cx
    mov  word ptr jw_nodes[bx].tag, ax
    mov  word ptr jw_nodes[bx].tag+2, dx
    inc  i
    jmp  fill_loop
fill_done:
    ret
jw_fill ENDP

jw_sum PROC near
    LOCAL acc:SDWORD, j:SWORD
    mov  word ptr acc, 0
    mov  word ptr acc+2, 0
    mov  j, 0
sum_loop:
    mov  bx, j
    cmp  bx, jw_count
    jge  sum_done
    shl  bx, 1
    shl  bx, 1
    shl  bx, 1
    mov  ax, jw_nodes[bx].vx
    cwd
    add  word ptr acc, ax
    adc  word ptr acc+2, dx
    inc  j
    jmp  sum_loop
sum_done:
    mov  ax, word ptr acc
    mov  dx, word ptr acc+2
    mov  word ptr jw_total, ax
    mov  word ptr jw_total+2, dx
    ret
jw_sum ENDP

jw_scale PROC FASTCALL k:SWORD, m:SWORD
    mov  ax, k
    imul m
    ret
jw_scale ENDP

start:
    mov  ax, @data
    mov  ds, ax
    call jw_fill
    call jw_sum
    INVOKE jw_scale, 6, 7
    mov  ax, 4C00h
    int  21h
    END start
