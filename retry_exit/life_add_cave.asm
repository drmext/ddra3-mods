; WORLD-style Quick Fail cave.
; Original at gamemdx+0x549F1: add dword ptr [rbx+90h], edi
; Then the native life<=0 shutter check. Zero life after that add
; while START is held, same as WORLD 0x181270BC8.

PUBLIC life_add_cave
EXTERN apply_quick_fail:PROC
EXTERN g_life_add_cont:QWORD

_TEXT SEGMENT

life_add_cave PROC
    add     dword ptr [rbx+90h], edi
    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    sub     rsp, 28h
    mov     rcx, rbx
    call    apply_quick_fail
    add     rsp, 28h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    jmp     qword ptr [g_life_add_cont]
life_add_cave ENDP

_TEXT ENDS
END
