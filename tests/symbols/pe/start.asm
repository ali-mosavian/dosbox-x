; Entry for a 32-bit PE under a DPMI loader: prints, waits for a key, calls the C code, exits.
.386
.model flat
extrn run_:near
.data
ready db 'ready',13,10,'$'
.code
start:
    mov edx,offset ready
    mov ah,9
    int 21h
    mov ah,8
    int 21h
    call run_
    mov ax,4c00h
    int 21h
end start
