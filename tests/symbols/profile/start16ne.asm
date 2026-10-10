; A 16-bit entry for an NE under HX: the loader has set DS.
.model small
.386
extrn _run@3:far
.stack 400h
.data
ready db 'ready',13,10,'$'
finished db 'done',13,10,'$'
.code
start:
    mov dx,offset ready
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    call _run@3
    mov dx,offset finished
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    mov ah,4ch
    int 21h
end start
