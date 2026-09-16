; A3 32-bit option_copy is EDI=dst / ESI=src usercall (not stack args).
; MinHook trampoline is invoked with EDI/ESI restored.

.686
.model flat, C
option casemap:none

PUBLIC option_copy_detour
PUBLIC call_orig_option_copy

EXTERN detour_option_copy_c:PROC

_TEXT SEGMENT

; Entry: EDI = dst Option*, ESI = src Option*
; Save ESI/EDI/EBX so the game caller always gets them back (usercall), then
; push copies as cdecl args (right-to-left: src then dst).
option_copy_detour PROC
    push    ebx
    push    esi
    push    edi
    push    esi                 ; src arg
    push    edi                 ; dst arg
    call    detour_option_copy_c
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    ret
option_copy_detour ENDP

; cdecl call_orig_option_copy(fn, dst, src): set EDI/ESI, call trampoline.
call_orig_option_copy PROC
    push    ebp
    mov     ebp, esp
    push    edi
    push    esi
    mov     edi, dword ptr [ebp + 12]
    mov     esi, dword ptr [ebp + 16]
    call    dword ptr [ebp + 8]
    pop     esi
    pop     edi
    pop     ebp
    ret
call_orig_option_copy ENDP

_TEXT ENDS
END
