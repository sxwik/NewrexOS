bits 32

; This is a standalone flat binary. It has no OS headers.
; It will be executed directly from the heap.

start:
    ; Hardware hack: Write a bright pink 'X' directly to VGA memory
    ; Address 0xB8000 + (row 12 * 160) + (col 78 * 2) = 0xB87BC
    mov byte [0xB87BC], 'X'  ; Character
    mov byte [0xB87BD], 0x0D ; Color (Light Magenta on Black)
    
    ; Return control to the Newrex kernel shell
    ret
