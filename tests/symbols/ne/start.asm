; Entry for a 16-bit NE under a DPMI loader: prints, waits for a key, calls the C code, exits.
.model small
.386
extrn _run@3:far
.stack 400h
.data
ready db 'ready',13,10,'$'
.code
start:
    mov dx,offset ready
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    call _run@3
    mov ah,4ch
    int 21h
end start
