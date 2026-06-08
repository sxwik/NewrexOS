bits 32

start:
    mov edi, 0xB8000      ; Point EDI to the start of VGA video memory
    mov ecx, 2000         ; Loop counter (80 columns * 25 rows = 2000 characters)
    mov ah, 0x0A          ; Color attribute: Bright Green (0x0A) on Black

.draw_loop:
    ; Hardware hack: use the lowest bits of the memory address to pick a character
    ; This creates a cool pseudo-random visual effect on the screen
    mov al, cl            
    and al, 0x7F          ; Keep it in the standard ASCII range
    add al, 0x20          ; Shift it up to printable characters
    
    mov word [edi], ax    ; Write the character (AL) and color (AH) to the screen
    add edi, 2            ; Move to the next VGA slot (2 bytes per slot)
    
    dec ecx               ; Count down
    jnz .draw_loop        ; If ECX is not zero, jump back and draw the next character

    ret                   ; Safely return control to the Newrex kernel
