; A 32-bit entry for LE (DOS/32A) and PE (HX): prints, waits for a key, runs, prints, waits again.
.386
.model flat
.stack 4000h
extrn run_:near
.data
ready db 'ready',13,10,'$'
finished db 'done',13,10,'$'
.code
start:
    mov edx,offset ready
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    call run_
    mov edx,offset finished
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    mov ax,4c00h
    int 21h
end start
