; VTINT.COM: the BIOS ticks that 1000 calls of interrupt VECTOR with AH = FUNCTION take (INT 10h/03h get cursor,
; INT 21h/2Ah get date), left at B800:0F00 with 55h after it. Built twice, see build.sh.
.model tiny
.386
.code
org 100h
start:
    mov ah,0
    int 1ah
    mov si,dx
    mov di,1000
again:
    mov ah,FUNCTION
    xor bh,bh
    int VECTOR
    dec di
    jnz again
    mov ah,0
    int 1ah
    sub dx,si
    mov ax,0b800h
    mov es,ax
    mov es:[0f00h],dl
    mov byte ptr es:[0f02h],55h
    mov ah,8
    int 21h
    mov ax,4c00h
    int 21h
end start
