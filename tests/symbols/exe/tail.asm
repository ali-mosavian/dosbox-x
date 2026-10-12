; SYMTAIL.EXE: prints, as two hex digits, the byte of memory just past the end of its own image, then waits for a key.
; The loader must leave it as it found it (zero): the bytes of the file beyond the image (a linker's debug information) are no part of it.
.8086
.model small
.stack 100h
.data
pad db 1200 dup(0)
hexdigits db '0123456789ABCDEF'
marker db 'END'
.code
start:
    mov ax,@data
    mov ds,ax
    mov al,[marker+3]
    mov bl,al
    mov cl,4
    shr al,cl
    mov bh,0
    mov bl,al
    mov dl,[hexdigits+bx]
    mov ah,2
    int 21h
    mov al,[marker+3]
    and al,0Fh
    mov bl,al
    mov dl,[hexdigits+bx]
    mov ah,2
    int 21h
    mov ah,8
    int 21h
    mov ax,4C00h
    int 21h
end start
