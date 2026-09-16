; A3 32-bit Quick Fail cave and song-end thunk.
; Original at gamemdx+0x445ED: add dword ptr [esi+70h], edi
; Then the native life<=0 shutter check. Zero life after that add
; while START is held, same as WORLD 0x181270BC8 / A3 64-bit 0x549F1.
; song_end at 0x320A0 uses esi as this (not ecx).

.686
.model flat, C
option casemap:none

PUBLIC life_add_cave
PUBLIC song_end_detour
PUBLIC call_orig_song_end

EXTERN apply_quick_fail:PROC
EXTERN detour_song_end_c:PROC
EXTERN g_life_add_cont:DWORD

_TEXT SEGMENT

; Steal add + mov eax,[esi+70h] (6 bytes). Continuation is test eax,eax,
; so reload eax after Quick Fail may have zeroed life.
life_add_cave PROC
    add     dword ptr [esi + 70h], edi
    push    eax
    push    ecx
    push    edx
    push    esi
    call    apply_quick_fail
    add     esp, 4
    pop     edx
    pop     ecx
    pop     eax
    mov     eax, dword ptr [esi + 70h]
    jmp     dword ptr [g_life_add_cont]
life_add_cave ENDP

; Incoming: esi = actor. Return al.
song_end_detour PROC
    push    esi
    call    detour_song_end_c
    add     esp, 4
    ret
song_end_detour ENDP

; cdecl call_orig_song_end(fn, seq): set esi=seq, call the MinHook trampoline.
call_orig_song_end PROC
    push    ebp
    mov     ebp, esp
    push    esi
    mov     esi, dword ptr [ebp + 12]
    call    dword ptr [ebp + 8]
    pop     esi
    pop     ebp
    ret
call_orig_song_end ENDP

_TEXT ENDS
END
