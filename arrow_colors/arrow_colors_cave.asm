; A3 64-bit mid-function caves for expand note colors and solidify.
; Atlas rows 0-15 stay stock A3. Rows 16-20 are WORLD-only extra quants
; (12th / 24th / 32nd / 48th / 64th). Receptor, freeze dim, and freeze glow
; keep their original rows.

PUBLIC classify_cave
PUBLIC bake_cave
PUBLIC freeze_extra_cave
PUBLIC tap_pack_cave
PUBLIC body_pack_cave
PUBLIC freeze_uv_cave
PUBLIC common_pack_cave
PUBLIC freeze_patch_cave
PUBLIC solidify_border_cave
PUBLIC solidify_fill_cave

EXTERN classify_row:PROC
EXTERN bake_map_index:PROC
EXTERN bake_fill_arg:PROC
EXTERN bake_invoke_fill_reg:PROC
EXTERN g_bake_fill_fn:QWORD
EXTERN patch_freeze_head_vertex:PROC
EXTERN pack_row_byte_c:PROC
EXTERN g_classify_cont:QWORD
EXTERN g_bake_cont:QWORD
EXTERN g_freeze_extra_cont:QWORD
EXTERN g_tap_pack_cont:QWORD
EXTERN g_body_pack_cont:QWORD
EXTERN g_common_pack_cont:QWORD
EXTERN g_freeze_uv_cont:QWORD
EXTERN g_freeze_patch_cont:QWORD
EXTERN g_freeze_vtx:QWORD
EXTERN g_freeze_extra:QWORD
EXTERN g_freeze_remain:DWORD
EXTERN g_freeze_lane_progress:DWORD
EXTERN g_freeze_state:DWORD
EXTERN g_plus20_epilogue:QWORD
EXTERN g_plus18_epilogue:QWORD
EXTERN g_solidify:DWORD
EXTERN g_bake_quant:DWORD
EXTERN g_in_bake:DWORD

_TEXT SEGMENT

classify_cave PROC
    and     r11d, 3FFh
    push    rax
    push    rcx
    push    r8
    push    r9
    push    r10
    sub     rsp, 28h
    mov     ecx, r11d
    mov     edx, dword ptr [r9+0C8h]
    call    classify_row
    add     rsp, 28h
    mov     edx, eax
    pop     r10
    pop     r9
    pop     r8
    pop     rcx
    pop     rax
    jmp     qword ptr [g_classify_cont]
classify_cave ENDP

bake_cave PROC
    mov     r8, qword ptr [rax]
    push    rax
    push    rcx
    push    rdx
    push    r9
    push    r10
    push    r11
    sub     rsp, 20h
    mov     ecx, ebx
    mov     rdx, r8
    call    bake_map_index
    add     rsp, 20h
    mov     r8, rax
    pop     r11
    pop     r10
    pop     r9
    pop     rdx
    pop     rcx
    pop     rax
    mov     rcx, qword ptr [rdx+r8*8]
    mov     rax, qword ptr [rcx]
    push    rax
    push    rcx
    push    r8
    push    r9
    push    r10
    push    r11
    sub     rsp, 20h
    mov     ecx, ebx
    mov     edx, r8d
    call    bake_fill_arg
    add     rsp, 20h
    mov     edx, eax
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rcx
    pop     rax
    mov     r8d, r13d
    ; Same register footprint as stock call [rax+8]; fn via global (no 5th stack arg).
    mov     rax, qword ptr [rax+8]
    mov     qword ptr [g_bake_fill_fn], rax
    call    bake_invoke_fill_reg
    jmp     qword ptr [g_bake_cont]
bake_cave ENDP

freeze_extra_cave PROC
    mov     qword ptr [g_freeze_extra], rdx
    mov     r9d, dword ptr [rdx+8]
    mov     dword ptr [g_freeze_remain], r9d
    mov     eax, dword ptr [rdx+0Ch]
    mov     dword ptr [g_freeze_state], eax
    mov     eax, dword ptr [rdx+rbx*4+14h]
    mov     dword ptr [g_freeze_lane_progress], eax
    test    r9d, r9d
    jmp     qword ptr [g_freeze_extra_cont]
freeze_extra_cave ENDP

tap_pack_cave PROC
    push    rax
    push    rcx
    push    rdx
    sub     rsp, 20h
    mov     ecx, edx
    call    pack_row_byte_c
    add     rsp, 20h
    lea     rdx, [rsp+78h]
    mov     word ptr [rsp+79h], 0
    mov     qword ptr [rsp+58h], rdx
    mov     byte ptr [rsp+78h], al
    pop     rdx
    pop     rcx
    pop     rax
    mov     rdx, r8
    movaps  xmm2, xmm14
    jmp     qword ptr [g_tap_pack_cont]
tap_pack_cave ENDP

body_pack_cave PROC
    lea     r8, [r11+38h]
    mov     qword ptr [r11-18h], r8
    mov     word ptr [r11+39h], 0
    push    rax
    push    rcx
    push    rdx
    push    r11
    sub     rsp, 38h
    movups  xmmword ptr [rsp+20h], xmm2
    movups  xmmword ptr [rsp+10h], xmm3
    mov     ecx, dword ptr [rsp+0E8h]
    call    pack_row_byte_c
    movups  xmm3, xmmword ptr [rsp+10h]
    movups  xmm2, xmmword ptr [rsp+20h]
    add     rsp, 38h
    pop     r11
    pop     rdx
    mov     byte ptr [r11+38h], al
    pop     rcx
    pop     rax
    movss   xmm1, dword ptr [rsp+88h]
    jmp     qword ptr [g_body_pack_cont]
body_pack_cave ENDP

common_pack_cave PROC
    push    rax
    push    rcx
    push    rdx
    sub     rsp, 20h
    xor     ecx, ecx
    call    pack_row_byte_c
    add     rsp, 20h
    movd    xmm3, eax
    cvtdq2ps xmm3, xmm3
    pop     rdx
    pop     rcx
    pop     rax
    mov     qword ptr [rsp+0A8h], rbp
    mov     qword ptr [rsp+58h], r12
    mov     qword ptr [rsp+50h], r14
    mov     dword ptr [rsp+0A0h], edi
    mov     word ptr [rsp+91h], di
    lea     r14, [rbx+54h]
    mov     ebp, edi
    lea     r12, [rbx+98h]
    jmp     qword ptr [g_common_pack_cont]
common_pack_cave ENDP

; Save the head vertex pointer only. Do NOT touch r12d — the body uses
; native freeze rows throughout, exactly as WORLD's 0xE40 cave does.
freeze_uv_cave PROC
    mov     qword ptr [g_freeze_vtx], rdi
    push    rax
    push    rcx
    sub     rsp, 28h
    mov     ecx, r12d
    call    pack_row_byte_c
    add     rsp, 28h
    mov     byte ptr [rsp+6Ch], al
    pop     rcx
    pop     rax
    movaps  xmm3, xmm10
    movaps  xmm2, xmm14
    lea     rdx, [rsp+5Ch]
    mov     qword ptr [rsp+40h], rdx
    mov     word ptr [rsp+5Dh], 0
    mov     rdx, rdi
    jmp     qword ptr [g_freeze_uv_cont]
freeze_uv_cave ENDP

freeze_patch_cave PROC
    subss   xmm8, xmm10
    mov     rax, qword ptr [g_freeze_extra]
    test    rax, rax
    jz      freeze_patch_done
    mov     rcx, qword ptr [g_freeze_vtx]
    test    rcx, rcx
    jz      freeze_patch_done
    sub     rsp, 30h
    movaps  xmmword ptr [rsp+20h], xmm8
    mov     r8, rax
    mov     rdx, r13
    call    patch_freeze_head_vertex
    movaps  xmm8, xmmword ptr [rsp+20h]
    add     rsp, 30h
freeze_patch_done:
    mov     qword ptr [g_freeze_extra], 0
    mov     qword ptr [g_freeze_vtx], 0
    mov     dword ptr [g_freeze_remain], 0
    mov     dword ptr [g_freeze_lane_progress], 0
    mov     dword ptr [g_freeze_state], 0
    jmp     qword ptr [g_freeze_patch_cont]
freeze_patch_cave ENDP

solidify_border_cave PROC
    cmp     eax, ecx
    mov     rax, qword ptr [r11]
    mov     rcx, r11
    jl      solidify_plus20
    cmp     dword ptr [g_solidify], 1
    jae     solidify_plus20
    jmp     qword ptr [g_plus18_epilogue]
solidify_plus20:
    jmp     qword ptr [g_plus20_epilogue]
solidify_border_cave ENDP

solidify_fill_cave PROC
    add     r8d, ebx
    cmp     dword ptr [g_solidify], 2
    jl      solidify_fill_nophase
    mov     r8d, 0FFFFFC72h
solidify_fill_nophase:
    add     rsp, 20h
    pop     rbx
    cmp     dword ptr [g_solidify], 3
    jl      solidify_do_fill
    jmp     qword ptr [r10+20h]
solidify_do_fill:
    jmp     qword ptr [r10+28h]
solidify_fill_cave ENDP

_TEXT ENDS
END
