; A 16-bit entry that just runs: for a dosrun job, which has no keyboard.
.model small
.386
extrn _run@3:far
.stack 400h
.data
.code
start:
    mov ax,@data
    mov ds,ax
    call _run@3
    mov ah,4ch
    int 21h
end start
