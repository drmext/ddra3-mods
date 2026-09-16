; A3 32-bit mid-function caves for expand note colors and solidify.
; Atlas rows 0-15 stay stock A3. Rows 16-20 are WORLD-only extra quants
; (12th / 24th / 32nd / 48th / 64th). Receptor, freeze dim, and freeze glow
; keep their original rows.
;
; Packed V is (row+0.5)/21*255. Stock used /16; pack_scale and spot_pack
; replace that fmul so tap, freeze head, freeze body, and spots all match.

.686
.model flat, C
.xmm
option casemap:none

PUBLIC classify_cave
PUBLIC bake_cave
PUBLIC freeze_extra_cave
PUBLIC freeze_uv_cave
PUBLIC freeze_patch_cave
PUBLIC pack_scale_cave
PUBLIC spot_pack_cave
PUBLIC solidify_border_cave
PUBLIC solidify_fill_cave
PUBLIC other_border_detour
PUBLIC other_fill_detour
PUBLIC call_orig_other_fill
PUBLIC call_bake_fill3

EXTERN classify_row:PROC
EXTERN bake_map_index:PROC
EXTERN bake_fill_arg:PROC
EXTERN bake_invoke_fill_reg:PROC
EXTERN g_bake_fill_fn:DWORD
EXTERN patch_freeze_head_vertex:PROC
EXTERN detour_other_border_c:PROC
EXTERN detour_other_fill_c:PROC
EXTERN g_classify_cont:DWORD
EXTERN g_bake_cont:DWORD
EXTERN g_freeze_extra_cont:DWORD
EXTERN g_freeze_uv_cont:DWORD
EXTERN g_freeze_patch_cont:DWORD
EXTERN g_pack_scale_cont:DWORD
EXTERN g_spot_pack_cont:DWORD
EXTERN g_solidify_fill_cont:DWORD
EXTERN g_freeze_vtx:DWORD
EXTERN g_freeze_extra:DWORD
EXTERN g_freeze_remain:DWORD
EXTERN g_freeze_lane_progress:DWORD
EXTERN g_freeze_state:DWORD
EXTERN g_plus20_epilogue:DWORD
EXTERN g_plus18_epilogue:DWORD
EXTERN g_solidify:DWORD
EXTERN g_bake_quant:DWORD
EXTERN g_in_bake:DWORD

_TEXT SEGMENT
ALIGN 8
kInv21 QWORD 3FA8618618618618h   ; 1/21

classify_cave PROC
    and     eax, 3FFh
    push    ecx
    push    dword ptr [ebx + 98h]
    push    eax
    call    classify_row
    add     esp, 8
    mov     edx, eax
    pop     ecx
    jmp     dword ptr [g_classify_cont]
classify_cave ENDP

bake_cave PROC
    mov     eax, dword ptr [eax]
    push    ecx
    push    edx
    push    eax
    push    edi
    call    bake_map_index
    add     esp, 8
    pop     edx
    pop     ecx
    mov     ebx, dword ptr [ebp - 4]
    mov     ecx, dword ptr [ecx + eax*4]
    mov     edx, dword ptr [ecx]
    push    ebx
    push    ecx
    push    edx
    push    eax
    push    edi
    call    bake_fill_arg
    add     esp, 8
    mov     ebx, eax
    pop     edx
    pop     ecx
    ; Stock leaves [ebp-4] on the stack as the 3rd thiscall arg (ret 0Ch).
    ; Do not pop it into edi — edi is live across the baker loop.
    mov     eax, dword ptr [edx + 4]
    mov     dword ptr [g_bake_fill_fn], eax
    push    esi
    push    ebx
    push    ecx
    call    bake_invoke_fill_reg
    add     esp, 16
    jmp     dword ptr [g_bake_cont]
bake_cave ENDP

freeze_extra_cave PROC
    mov     dword ptr [g_freeze_extra], eax
    mov     edx, dword ptr [eax + 4]
    mov     dword ptr [g_freeze_remain], edx
    mov     edx, dword ptr [eax + 8]
    mov     dword ptr [g_freeze_state], edx
    mov     edx, dword ptr [eax + ebx*4 + 10h]
    mov     dword ptr [g_freeze_lane_progress], edx
    mov     edx, dword ptr [g_freeze_remain]
    test    edx, edx
    jmp     dword ptr [g_freeze_extra_cont]
freeze_extra_cave ENDP

; Save head vertex and replay the stolen prologue. Packing is done in
; sub_1001A300 via pack_scale_cave (/21).
freeze_uv_cave PROC
    mov     dword ptr [g_freeze_vtx], esi
    mov     eax, dword ptr [esp + 24h]
    push    ecx
    mov     ecx, dword ptr [esp + 2Ch]
    jmp     dword ptr [g_freeze_uv_cont]
freeze_uv_cave ENDP

freeze_patch_cave PROC
    fld     dword ptr [eax + edx + 8]
    fsub    dword ptr [esp + 38h]
    fstp    dword ptr [esp + 30h]
    push    eax
    push    ecx
    push    edx
    mov     ecx, dword ptr [g_freeze_extra]
    test    ecx, ecx
    jz      freeze_patch32_done
    cmp     dword ptr [g_freeze_vtx], 0
    jz      freeze_patch32_done
    push    ecx
    push    edi
    push    dword ptr [g_freeze_vtx]
    call    patch_freeze_head_vertex
    add     esp, 12
freeze_patch32_done:
    mov     dword ptr [g_freeze_extra], 0
    mov     dword ptr [g_freeze_vtx], 0
    mov     dword ptr [g_freeze_remain], 0
    mov     dword ptr [g_freeze_lane_progress], 0
    mov     dword ptr [g_freeze_state], 0
    pop     edx
    pop     ecx
    pop     eax
    jmp     dword ptr [g_freeze_patch_cont]
freeze_patch_cave ENDP

; Replace stock fmul 1/16 with 1/21. ST0 already holds (row+0.5).
pack_scale_cave PROC
    fmul    qword ptr [kInv21]
    jmp     dword ptr [g_pack_scale_cont]
pack_scale_cave ENDP

spot_pack_cave PROC
    fmul    qword ptr [kInv21]
    jmp     dword ptr [g_spot_pack_cont]
spot_pack_cave ENDP

solidify_border_cave PROC
    cmp     esi, edx
    mov     edx, dword ptr [ecx]
    push    eax
    jl      solidify_plus20
    cmp     dword ptr [g_solidify], 1
    jae     solidify_plus20
    jmp     dword ptr [g_plus18_epilogue]
solidify_plus20:
    jmp     dword ptr [g_plus20_epilogue]
solidify_border_cave ENDP

solidify_fill_cave PROC
    cmp     dword ptr [g_solidify], 3
    jge     solidify_complete
    cmp     dword ptr [g_solidify], 2
    jl      solidify_fill_nophase
    mov     eax, 0FFFFFC72h
    jmp     solidify_fill_push
solidify_fill_nophase:
    add     eax, edi
solidify_fill_push:
    push    edi
    push    eax
    add     esi, -5
    push    esi
    call    edx
    jmp     dword ptr [g_solidify_fill_cont]
solidify_complete:
    push    0FFFFFC72h
    mov     eax, dword ptr [ebx + 10h]
    call    eax
    jmp     dword ptr [g_solidify_fill_cont]
solidify_fill_cave ENDP

other_border_detour PROC
    push    dword ptr [esp + 4]
    push    ecx
    call    detour_other_border_c
    add     esp, 8
    ret     4
other_border_detour ENDP

other_fill_detour PROC
    push    dword ptr [esp + 12]
    push    dword ptr [esp + 12]
    push    dword ptr [esp + 12]
    push    ecx
    call    detour_other_fill_c
    add     esp, 16
    ret     12
other_fill_detour ENDP

call_orig_other_fill PROC
    push    ebp
    mov     ebp, esp
    push    dword ptr [ebp + 24]
    push    dword ptr [ebp + 20]
    push    dword ptr [ebp + 16]
    mov     ecx, dword ptr [ebp + 12]
    call    dword ptr [ebp + 8]
    pop     ebp
    ret
call_orig_other_fill ENDP

; Stock baker thiscall: ecx=self, 3 stack args, callee ret 0Ch.
; cdecl call_bake_fill3(fn, self, a2, a3, a4)
call_bake_fill3 PROC
    push    ebp
    mov     ebp, esp
    mov     eax, dword ptr [ebp + 8]
    mov     ecx, dword ptr [ebp + 12]
    push    dword ptr [ebp + 24]
    push    dword ptr [ebp + 20]
    push    dword ptr [ebp + 16]
    call    eax
    pop     ebp
    ret
call_bake_fill3 ENDP

_TEXT ENDS
END
