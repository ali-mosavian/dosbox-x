; Writes 11h to 5000:0000, then 42h to 5000:0010, then exits.
.model tiny
.code
org 100h
start:
    mov ax, 5000h
    mov es, ax
    mov byte ptr es:[0], 11h
    mov byte ptr es:[10h], 42h
    mov ax, 4c00h
    int 21h
end start
